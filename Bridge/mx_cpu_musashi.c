/* mx_cpu_musashi.c — mx_cpu_iface.h のMusashiバックエンド(P867、68000型)
 * ThirdParty/Musashi(kstenerud/Musashi 313ebf1)を mx_cpu_iface.h の契約に合わせて包む。
 * ★P868: MX68K_CPU_CORE=musashi のとき m68000_bridge.c の mx_cpu_* から呼ばれる(既定はc68k)。
 * ★Musashiのヘッダは必ず相対パスで include する。素の "m68k.h" と書くと、Xcodeのヘッダマップ経由で
 *   Core/px68k/m68000/Musashi/ の偽スタブ(レジスタ番号が本物と異なる)に解決されてしまう。 */
#include <stddef.h>
#include <stdlib.h>   /* P869: getenv */
#include <string.h>   /* P869: strcmp */
#include <stdio.h>    /* P887: snprintf(セルフテスト) */
#include "mx_cpu_iface.h"
#include "mx_cpu_musashi.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../ThirdParty/Musashi/m68kcpu.h"
#pragma clang diagnostic pop

/* 偽スタブ(ガード名 M68K_H / M68KCPU_H)を掴んでいないことをコンパイル時に確かめる */
#if defined(M68K_H) || defined(M68KCPU_H) || !defined(M68K__HEADER) || !defined(M68KCPU__HEADER)
#error "Musashiのヘッダが ThirdParty/Musashi 以外(Core内の偽スタブ等)に解決されている"
#endif
_Static_assert(M68K_REG_D0 == 0, "Musashiの本物の m68k.h ではない(偽スタブは M68K_REG_D0=6)");
#if M68K_EMULATE_INT_ACK != M68K_OPT_ON
#error "ThirdParty/Musashi/m68kconf.h の M68K_EMULATE_INT_ACK が ON になっていない"
#endif

/* c68k の「オートベクタ」戻り値(Core:c68k/c68k.h:111 C68K_INT_ACK_AUTOVECTOR)。
 * mx_cpu_bus.int_ack の戻り値規約は c68k のものなので、ここでMusashiの規約へ変換する。 */
#define MX_C68K_INT_ACK_AUTOVECTOR (-1)

static mx_cpu_bus s_bus;
static int s_in_execute;

/* P869: プローブ有効フラグ(68000型の既定経路では常に0)。観測フックはこれが真のときだけ動く */
static int s_p869_on;
static void p869_on_read16(unsigned int address, unsigned int v);
static void p869_on_read32(unsigned int address);

/* P887/P889: X68030ハイメモリ。NULL=無効。base/bytes は呼び出し側が指定する
 * (TS-6BE16相当=$01000000/16MB固定[XEiJ XEiJ.java:8056,8092]、
 *  060turbo相当=$10000000/16〜768MB可変[XEiJ XEiJ.java:8063,2766-2775])。
 * 有効時は CPU_ADDRESS_MASK=0xFFFFFFFF で32bitアドレスがそのまま届くので、[base, base+bytes) だけを
 * このバッファへ、それ以外は24bitミラー(a & 0x00FFFFFF)として s_bus へ渡す。
 * 無効時は従来どおり(アドレスに一切手を加えない)。 */
static uint32_t s_himem_base;
static uint32_t s_himem_bytes;
static uint8_t *s_himem;

static inline int himem_hit(uint32_t a) { return s_himem && (uint32_t)(a - s_himem_base) < s_himem_bytes; }

static inline unsigned int bus_read8(uint32_t a)
{
    if (!s_himem) return s_bus.read8(a) & 0xffu;
    if (himem_hit(a)) return s_himem[a - s_himem_base];
    return s_bus.read8(a & 0x00ffffffu) & 0xffu;
}
static inline unsigned int bus_read16(uint32_t a)
{
    if (!s_himem) return s_bus.read16(a) & 0xffffu;
    if (himem_hit(a)) {
        uint32_t o = a - s_himem_base;
        /* 末尾 byte の後半byteは base+bytes(ミラー側)へ落とす */
        unsigned int lo = himem_hit(a + 1) ? s_himem[o + 1] : (s_bus.read8((a + 1) & 0x00ffffffu) & 0xffu);
        return ((unsigned int)s_himem[o] << 8) | lo;
    }
    return s_bus.read16(a & 0x00ffffffu) & 0xffffu;
}
static inline void bus_write8(uint32_t a, unsigned int v)
{
    if (!s_himem) { s_bus.write8(a, v & 0xffu); return; }
    if (himem_hit(a)) { s_himem[a - s_himem_base] = (uint8_t)v; return; }
    s_bus.write8(a & 0x00ffffffu, v & 0xffu);
}
static inline void bus_write16(uint32_t a, unsigned int v)
{
    if (!s_himem) { s_bus.write16(a, v & 0xffffu); return; }
    if (himem_hit(a)) {
        uint32_t o = a - s_himem_base;
        s_himem[o] = (uint8_t)(v >> 8);
        if (himem_hit(a + 1)) s_himem[o + 1] = (uint8_t)v;
        else s_bus.write8((a + 1) & 0x00ffffffu, v & 0xffu);
        return;
    }
    s_bus.write16(a & 0x00ffffffu, v & 0xffffu);
}

/* --- Musashiが呼ぶメモリ関数(m68k.h で宣言、利用者が定義する) --- */
unsigned int m68k_read_memory_8(unsigned int address)  { return bus_read8(address); }
unsigned int m68k_read_memory_16(unsigned int address)
{
    unsigned int v = bus_read16(address);   /* バス読み出しは従来どおり1回だけ */
    if (s_p869_on && !(s_himem && himem_hit(address))) p869_on_read16(address, v);
    return v;
}
unsigned int m68k_read_memory_32(unsigned int address)
{
    uint32_t hi = bus_read16(address);
    uint32_t lo = bus_read16(address + 2);
    if (s_p869_on && !(s_himem && himem_hit(address))) p869_on_read32(address);
    return (hi << 16) | lo;
}
void m68k_write_memory_8(unsigned int address, unsigned int value)  { bus_write8(address, value); }
void m68k_write_memory_16(unsigned int address, unsigned int value) { bus_write16(address, value); }
void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    bus_write16(address, (value >> 16) & 0xffffu);
    bus_write16(address + 2, value & 0xffffu);
}

static int musashi_int_ack(int level)
{
    int32_t v = s_bus.int_ack(level);
    if (v == MX_C68K_INT_ACK_AUTOVECTOR) return (int)M68K_INT_ACK_AUTOVECTOR;
    return (int)v;
}

/* --- レジスタ読み出し --- */
uint32_t mx_cpu_musashi_get_dreg(int n) { return m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + (n & 7))); }
uint32_t mx_cpu_musashi_get_areg(int n) { return m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + (n & 7))); }
uint32_t mx_cpu_musashi_get_pc(void)  { return m68k_get_reg(NULL, M68K_REG_PC); }
uint32_t mx_cpu_musashi_get_sr(void)  { return m68k_get_reg(NULL, M68K_REG_SR); }
uint32_t mx_cpu_musashi_get_usp(void) { return m68k_get_reg(NULL, M68K_REG_USP); }
uint32_t mx_cpu_musashi_get_ssp(void) { return m68k_get_reg(NULL, M68K_REG_ISP); }

/* --- レジスタ書き込み --- */
void mx_cpu_musashi_set_dreg(int n, uint32_t v) { m68k_set_reg((m68k_register_t)(M68K_REG_D0 + (n & 7)), v); }
void mx_cpu_musashi_set_areg(int n, uint32_t v) { m68k_set_reg((m68k_register_t)(M68K_REG_A0 + (n & 7)), v); }
void mx_cpu_musashi_set_sr(uint32_t v)  { m68k_set_reg(M68K_REG_SR, v); }
void mx_cpu_musashi_set_usp(uint32_t v) { m68k_set_reg(M68K_REG_USP, v); }
void mx_cpu_musashi_set_pc(uint32_t v)  { m68k_set_reg(M68K_REG_PC, v & (s_himem ? 0xffffffffu : 0x00ffffffu)); }
void mx_cpu_musashi_set_ssp(uint32_t v) { m68k_set_reg(M68K_REG_ISP, v); }
/* P868: 32bitのまま設定する(マスクしない)。m68k_set_reg(PC) は m68ki_jump(MASK_OUT_ABOVE_32(value))
 * で32bitを保持し(m68kcpu.c:705)、命令フェッチ時だけ ADDRESS_68K(68000型は0x00FFFFFF)で
 * マスクする(m68kcpu.h:283、m68kcpu.c:796-798)ので、上位byteタグ付きPCでもホスト側は安全。 */
void mx_cpu_musashi_jump_raw32(uint32_t pc32) { m68k_set_reg(M68K_REG_PC, pc32); }

/* --- 初期化・リセット --- */
void mx_cpu_musashi_init(const mx_cpu_bus *bus)
{
    s_bus = *bus;   /* 呼出し側はスタック上の構造体を渡す(m68000_bridge.c の c68k_init)ので値でコピーする */
    s_in_execute = 0;
    m68k_init();    /* 全コールバックをNULLへ戻すので、int_ackの登録は必ずこの後 */
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(musashi_int_ack);
}

/* Musashiには直接フェッチ表が無い(命令も m68k_read_memory_* 経由で読む)ので何もしない */
void mx_cpu_musashi_map_fetch(uint32_t lo, uint32_t hi, const void *host) { (void)lo; (void)hi; (void)host; }

/* P897: FPU(ThirdParty/Musashi/m68kfpu.c、MAME m68kfpu.cpp 由来の移植版)。定義は m68kfpu.c */
extern void mx68k_m68kfpu_set_config(int present, int model);
extern int  mx68k_m68kfpu_get_present(void);
extern int  mx68k_m68kfpu_get_model(void);
extern void mx68k_m68kfpu_reset(void);
extern int  mx68k_m68kfpu_format_stats(char *out, size_t n);

/* P897: 環境変数 MX68K_MUSASHI_FPU / MX68K_MUSASHI_FPU_MODEL はプロセス中1回だけ評価する */
static int s_fpu_env_done;
static const char *s_fpu_env_raw;   /* NULL=未設定 */
static int s_fpu_env_on;            /* "1" と完全一致なら1 */
static int s_fpu_env_model;         /* 68881 または 68882(未指定・それ以外は68882) */

static void fpu_env_load(void)
{
    const char *m;
    if (s_fpu_env_done) return;
    s_fpu_env_done = 1;
    s_fpu_env_raw = getenv("MX68K_MUSASHI_FPU");
    if (s_fpu_env_raw != NULL && s_fpu_env_raw[0] == '\0') s_fpu_env_raw = NULL;
    s_fpu_env_on = (s_fpu_env_raw != NULL && strcmp(s_fpu_env_raw, "1") == 0);
    m = getenv("MX68K_MUSASHI_FPU_MODEL");
    s_fpu_env_model = (m != NULL && strcmp(m, "68881") == 0) ? 68881 : 68882;
}

/* EC030 のときだけ環境変数に従ってFPUを装着する(68000型では常に不在) */
static void fpu_apply_for_model(int ec030)
{
    fpu_env_load();
    mx68k_m68kfpu_set_config(ec030 && s_fpu_env_on, s_fpu_env_model);
}

void mx_cpu_musashi_set_fpu_config(int present, int model) { mx68k_m68kfpu_set_config(present, model); }
int  mx_cpu_musashi_get_fpu_present(void) { return mx68k_m68kfpu_get_present(); }
int  mx_cpu_musashi_get_fpu_model(void) { return mx68k_m68kfpu_get_model(); }
const char *mx_cpu_musashi_fpu_env_raw(void) { fpu_env_load(); return s_fpu_env_raw; }

void mx_cpu_musashi_p897_tick(int frame, mx_cpu_musashi_logf logf)
{
    char stats[512];
    if (logf == NULL || !mx68k_m68kfpu_get_present()) return;
    mx68k_m68kfpu_format_stats(stats, sizeof stats);
    logf("[P897-FPU] f=%d present=1 model=%d cpu_type=%u fpcr=%08X fpsr=%08X %s\n",
         frame, mx68k_m68kfpu_get_model(), m68ki_cpu.cpu_type, REG_FPCR, REG_FPSR, stats);
}

/* P897: FPUの初期化は m68k_pulse_reset() の後(上流 m68k_pulse_reset はFPUに触れない) */
void mx_cpu_musashi_reset(void) { m68k_pulse_reset(); mx68k_m68kfpu_reset(); }

/* --- 実行・割込み・サイクル操作 --- */
int32_t mx_cpu_musashi_execute(int32_t cycles)
{
    int32_t r;
    s_in_execute = 1;
    r = (int32_t)m68k_execute((unsigned int)cycles);
    s_in_execute = 0;
    return r;
}

/* Musashiは set_irq だけだと次回 execute まで受理しないので、実行中なら今の命令の後で切り上げる
 * (c68kの受理タイミングに揃える、ユーザー承認済み方針 2026-10-03) */
void mx_cpu_musashi_set_irq(int32_t level)
{
    m68k_set_irq((unsigned int)level);
    if (s_in_execute) m68k_end_timeslice();
}

void mx_cpu_musashi_end_timeslice(void) { if (s_in_execute) m68k_end_timeslice(); }
void mx_cpu_musashi_add_cycles(int32_t cycles) { if (s_in_execute) USE_CYCLES(cycles); }
int32_t mx_cpu_musashi_cycles_done(void) { return s_in_execute ? (int32_t)m68k_cycles_run() : -1; }

/* --- 実行状態 --- */
int32_t mx_cpu_musashi_get_irq_line(void) { return (int32_t)(CPU_INT_LEVEL >> 8); }
int     mx_cpu_musashi_is_halted(void)    { return CPU_STOPPED != 0; }

/* --- 実行状態の生値(ステート専用)。c68k の生値とは互換が無い --- */
uint32_t mx_cpu_musashi_state_get_run_status(void) { return (uint32_t)CPU_STOPPED; }
int32_t  mx_cpu_musashi_state_get_irq_line(void)   { return (int32_t)(CPU_INT_LEVEL >> 8); }
void mx_cpu_musashi_state_restore_run(uint32_t run_status, int32_t irq_line)
{
    CPU_STOPPED = run_status;
    CPU_INT_LEVEL = (uint)irq_line << 8;
}

/* ======================================================================
 * P869: 実験用CPUモデル(MX68K_MUSASHI_MODEL)と一時プローブ[P869-*]
 *   IPLROM30 のMPU判別ルーチン($FF0D3E-$FF0E38)を EC030型で通したときの
 *   例外・RTE・スタブ到達を実行時ログで確かめるための観測。
 *   撤去条件: X68030起動経路の正式実装サイクルでMPU判別結果を常設表示へ置き換えた時点
 *   (.mx68k_cycles/P869_plan.md「一時プローブの寿命」)。
 * ====================================================================== */

/* スタブ到達の種別。ThirdParty/Musashi/m68kfpu.c・m68kmmu.h の同名定数と一致させること */
#define MX68K_STUB_FPU_FLINE  1
#define MX68K_STUB_PMMU_NOOP  2
#define MX68K_STUB_PMMU_FLINE 3

/* 定義は ThirdParty/Musashi/m68kfpu.c。型は定義側と一字一句同じにすること(コンパイラは食い違いを検出しない) */
extern void (*mx68k_musashi_stub_hook)(int kind, unsigned int ppc, unsigned int ir, unsigned int ext);

#define P869_PC_MPUCHK_ENTER 0x00FF0D3Eu   /* MPU判別ルーチン先頭(link A6,#-8) */
#define P869_PC_MPUCHK_EXIT  0x00FF005Eu   /* bsr 復帰直後(move.l D0,D7) */
#define P869_WIN_MAX_INSN    20000u        /* ウィンドウ打切り命令数 */
#define P869_CAP_EXC         64u
#define P869_CAP_RTE         64u
#define P869_CAP_STUB        64u
#define P869_CAP_MARK        8u            /* ENTER/EXIT/TIMEOUT 各 */

static mx_cpu_musashi_logf s_logf;
static unsigned int s_insn;          /* 検出した命令フェッチ総数(分母) */
static unsigned int s_enter, s_exit, s_timeout;
static unsigned int s_exc_total;
static unsigned int s_vec_hist[256];
static unsigned int s_stub_fpu, s_stub_pmmu_noop, s_stub_pmmu_fline;
/* cpID 2-7・bit8=0 のコプロセッサ命令語(Musashi では無音 no-op)のフェッチ数と、そのうち $FExx 帯 */
static unsigned int s_cpnop, s_cpnop_fe;
static int s_win;                   /* MPU判別ウィンドウ中なら1 */
static unsigned int s_since_enter;
static unsigned int s_prev_op_ppc;
static unsigned int s_lines_exc, s_lines_rte, s_lines_stub, s_lines_timeout;

static const char *p869_kind_name(int kind)
{
    switch (kind) {
    case MX68K_STUB_FPU_FLINE:  return "fpu_fline";
    case MX68K_STUB_PMMU_NOOP:  return "pmmu_noop";
    case MX68K_STUB_PMMU_FLINE: return "pmmu_fline";
    default:                    return "unknown";
    }
}

/* 命令フェッチ判定: m68ki_read_imm_16 は REG_PC += 2 の後に REG_PC-2 を読む。
 * 実行ループは命令語の読み出し直前に REG_PPC = REG_PC とするので、命令語の読み出しだけが
 * address == PPC かつ PC == PPC+2 を満たす */
static void p869_on_read16(unsigned int address, unsigned int v)
{
    if (!(address == (REG_PPC & 0x00FFFFFFu) && REG_PC == REG_PPC + 2)) return;

    s_insn++;
    if ((v & 0xF100u) == 0xF000u && ((v >> 9) & 7u) >= 2u) {
        s_cpnop++;
        if ((v & 0xFF00u) == 0xFE00u) s_cpnop_fe++;
    }
    if (s_win) {
        s_since_enter++;
        if (s_since_enter > P869_WIN_MAX_INSN) {
            s_timeout++;
            if (s_lines_timeout < P869_CAP_MARK) {
                s_lines_timeout++;
                s_logf("[P869-MPUCHK-TIMEOUT] n=%u insn=%u ppc=%08X since_enter=%u\n",
                       s_timeout, s_insn, address, s_since_enter);
            }
            s_win = 0;
        }
    }
    /* REG_IR はまだ直前命令の語(この読み出しの戻り値で上書きされる前) */
    if (s_win && REG_IR == 0x4E73u && s_lines_rte < P869_CAP_RTE) {
        s_lines_rte++;
        s_logf("[P869-RTE] n=%u insn=%u from=%08X target=%08X op=%04X d0=%08X\n",
               s_lines_rte, s_insn, s_prev_op_ppc, address, v, REG_D[0]);
    }
    if (address == P869_PC_MPUCHK_ENTER) {
        s_enter++;
        if (s_enter <= P869_CAP_MARK)
            s_logf("[P869-MPUCHK-ENTER] n=%u insn=%u ppc=%08X op=%04X sr=%04X a7=%08X\n",
                   s_enter, s_insn, address, v, (unsigned int)m68ki_get_sr(), REG_A[7]);
        s_win = 1;
        s_since_enter = 0;
    }
    if (address == P869_PC_MPUCHK_EXIT) {
        s_exit++;
        if (s_exit <= P869_CAP_MARK)
            s_logf("[P869-MPUCHK-EXIT] n=%u insn=%u ppc=%08X op=%04X d0=%08X d1=%08X sr=%04X a7=%08X since_enter=%u\n",
                   s_exit, s_insn, address, v, REG_D[0], REG_D[1], (unsigned int)m68ki_get_sr(),
                   REG_A[7], s_since_enter);
        s_win = 0;
    }
    s_prev_op_ppc = REG_PPC;
}

/* ベクタフェッチ判定: m68ki_jump_vector は REG_PC = vector*4 + VBR の後でその番地を32bit読む。
 * 割込みのベクタ読み出しは REG_PC を経由しないので対象外 */
static void p869_on_read32(unsigned int address)
{
    unsigned int off = address - REG_VBR;
    unsigned int vec;
    if (!((REG_PC & 0x00FFFFFFu) == address && off < 0x400u && (off & 3u) == 0)) return;

    vec = (off >> 2) & 0xFFu;
    s_vec_hist[vec]++;
    s_exc_total++;
    if (s_win && s_lines_exc < P869_CAP_EXC) {
        s_lines_exc++;
        s_logf("[P869-EXC] n=%u insn=%u vec=%u ppc=%08X ir=%04X sr=%04X d0=%08X a0=%08X vbr=%08X inwin=%d\n",
               s_lines_exc, s_insn, vec, REG_PPC, REG_IR, (unsigned int)m68ki_get_sr(),
               REG_D[0], REG_A[0], REG_VBR, s_win);
    }
}

/* m68kfpu.c / m68kmmu.h のスタブから呼ばれる */
static void p869_stub_event(int kind, unsigned int ppc, unsigned int ir, unsigned int ext)
{
    switch (kind) {
    case MX68K_STUB_FPU_FLINE:  s_stub_fpu++; break;
    case MX68K_STUB_PMMU_NOOP:  s_stub_pmmu_noop++; break;
    case MX68K_STUB_PMMU_FLINE: s_stub_pmmu_fline++; break;
    default: break;
    }
    if (s_lines_stub < P869_CAP_STUB) {
        s_lines_stub++;
        s_logf("[P869-STUB] n=%u insn=%u kind=%s ppc=%08X ir=%04X ext=%04X inwin=%d\n",
               s_lines_stub, s_insn, p869_kind_name(kind), ppc, ir, ext, s_win);
    }
}

/* P872: 本番経路(設定画面の機種選択)用のCPU型設定。環境変数を読まず、P869プローブも有効にしない。
 * ec030=1 は P869 の "ec030" と同じ3点(型・24bitマスク・PMMU命令のno-opスタブ)。
 * ec030=0 は 68000型へ戻す(m68k_set_cpu_type が CPU_ADDRESS_MASK=0x00FFFFFF・HAS_PMMU=0 を再設定する)。 */
void mx_cpu_musashi_set_model(int ec030)
{
    if (ec030) {
        m68k_set_cpu_type(M68K_CPU_TYPE_68EC030);
        CPU_ADDRESS_MASK = 0x00ffffffu;
        HAS_PMMU = 1;
    } else {
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    }
    fpu_apply_for_model(ec030);   /* P897 */
}

/* P872: 現在の型の分類(ステートのCPU識別子の下位バイト)。
 * 0x00=68000型、0x01=EC030(24bitマスク+PMMU no-opスタブ=本番/P869 "ec030" と同構成)、0xFF=それ以外 */
uint32_t mx_cpu_musashi_get_model_id(void)
{
    if (CPU_TYPE == CPU_TYPE_000) return 0x00u;
    /* P887: 0x02=EC030+PMMU no-opスタブ+32bitマスク+ハイメモリ有効 */
    if (CPU_TYPE == CPU_TYPE_EC030 && CPU_ADDRESS_MASK == 0xffffffffu && HAS_PMMU && s_himem) return 0x02u;
    if (CPU_TYPE == CPU_TYPE_EC030 && CPU_ADDRESS_MASK == 0x00ffffffu && HAS_PMMU) return 0x01u;
    return 0xFFu;
}

/* P887: ハイメモリ用バッファを設定する(NULL=無効)。有効時は CPU_ADDRESS_MASK を32bitへ、
 * 無効時は24bitへ戻す。mx_cpu_musashi_set_model の後(set_model がマスクを24bitへ戻すため)、
 * かつCPU非実行のリセット区間(mx68k_reset_hard / mx68k_shutdown)からだけ呼ぶこと。
 * P889: base/bytes を呼び出し側から指定できるようにした(TS-6BE16=$01000000/16MB固定、
 * 060turbo相当=$10000000/可変)。buf=NULLのときbase/bytesは無視してよい(呼び出し側は0を渡すこと)。 */
void mx_cpu_musashi_set_highmem(uint8_t *buf, uint32_t base, uint32_t bytes)
{
    s_himem = buf;
    s_himem_base = buf ? base : 0;
    s_himem_bytes = buf ? bytes : 0;
    CPU_ADDRESS_MASK = buf ? 0xffffffffu : 0x00ffffffu;
}

uint32_t mx_cpu_musashi_get_address_mask(void) { return (uint32_t)CPU_ADDRESS_MASK; }

/* P887: [P887-HIMEM-SELFTEST] 用。Musashiのコールバックを直接呼び、ADDRESS_68K のマスクは
 * 手動で再現する(命令実行経路そのものは通らない)。base→$00000000 の順に退避・書込み・
 * 読戻しを行い、最後に逆順で元値へ戻す。無効時は両者が同じ番地なので二重復元で問題ない。
 * P889: プローブ対象は実際に有効な base/bytes(無効時は後方互換のため $01000000/16MB)。
 * 戻り値: himem(1=有効)。 */
int mx_cpu_musashi_highmem_selftest(char *out, size_t n)
{
    const uint32_t pat = 0xA5C35A3Cu;
    const uint32_t mask = (uint32_t)CPU_ADDRESS_MASK;
    const int himem = s_himem != NULL;
    const uint32_t probe_base  = s_himem ? s_himem_base  : 0x01000000u;
    const uint32_t probe_bytes = s_himem ? s_himem_bytes : 0x01000000u;
    uint32_t orig_00, orig_base, rd_base, rd_00, rd_end;
    unsigned int rd_last_b;
    const char *verdict;

    orig_00 = m68k_read_memory_32(0x00000000u & mask);
    orig_base = m68k_read_memory_32(probe_base & mask);
    m68k_write_memory_32(probe_base & mask, pat);
    rd_base = m68k_read_memory_32(probe_base & mask);
    rd_00 = m68k_read_memory_32(0x00000000u & mask);
    rd_last_b = m68k_read_memory_8((probe_base + probe_bytes - 1u) & mask);
    rd_end = m68k_read_memory_32((probe_base + probe_bytes) & mask);
    m68k_write_memory_32(probe_base & mask, orig_base);
    m68k_write_memory_32(0x00000000u & mask, orig_00);

    if (rd_base != pat) verdict = "BROKEN";
    else if (rd_00 == orig_00) verdict = "SEPARATE";
    else if (rd_00 == pat) verdict = "MIRROR";
    else verdict = "OTHER";

    snprintf(out, n, "[P887-HIMEM-SELFTEST] himem=%d mask=0x%08x base=0x%08x bytes=0x%08x pat=0x%08x "
             "orig_00=0x%08x orig_base=0x%08x rd_base=0x%08x rd_00=0x%08x rd_last_b=0x%02x rd_end=0x%08x verdict=%s\n",
             himem, mask, probe_base, probe_bytes, pat, orig_00, orig_base, rd_base, rd_00, rd_last_b, rd_end, verdict);
    return himem;
}

void mx_cpu_musashi_set_model_from_env(mx_cpu_musashi_logf logf)
{
    const char *env = getenv("MX68K_MUSASHI_MODEL");
    const char *model = "68000";
    int ec030 = 0, bare = 0, probe = 0, unrecognized = 0;

    if (env != NULL && env[0] != '\0') {
        if (strcmp(env, "68000-probe") == 0)     { model = "68000-probe"; probe = 1; }
        else if (strcmp(env, "ec030") == 0)      { model = "ec030"; ec030 = 1; probe = 1; }
        else if (strcmp(env, "ec030-bare") == 0) { model = "ec030-bare"; bare = 1; probe = 1; }
        else unrecognized = 1;
    }

    if (ec030 || bare) {
        m68k_set_cpu_type(M68K_CPU_TYPE_68EC030);
        /* ハイメモリ無しX68030の上位8bit無視ミラー(外部バス24bit相当) */
        CPU_ADDRESS_MASK = 0x00ffffffu;
        /* ec030: PMMU命令をMX作成no-opスタブ(m68kmmu.h)へ通す。PMMU_ENABLED は立てないので変換は起きない。
         * ec030-bare: 上流EC030のまま(PMMU命令は line-1111) */
        HAS_PMMU = ec030 ? 1 : 0;
    }
    fpu_apply_for_model(ec030 || bare);   /* P897 */

    if (probe) {
        s_logf = logf;
        mx68k_musashi_stub_hook = p869_stub_event;
        s_p869_on = (logf != NULL);
    }

    if (logf != NULL)
        logf("[P869-MODEL] model=%s env=%.32s%s cpu_type=%u addr_mask=%08X has_pmmu=%d probe=%d\n",
             model, env ? env : "(unset)", unrecognized ? " (unrecognized)" : "",
             m68ki_cpu.cpu_type, m68ki_cpu.address_mask, m68ki_cpu.has_pmmu, s_p869_on);
}

void mx_cpu_musashi_p869_tick(int frame)
{
    unsigned int vec_other;
    if (!s_p869_on) return;
    vec_other = s_exc_total - s_vec_hist[4] - s_vec_hist[11];
    s_logf("[P869-SUM] f=%d insn=%u enter=%u exit=%u timeout=%u exc_total=%u vec4=%u vec11=%u vec14=%u vec_other=%u "
           "stub_fpu=%u stub_pmmu_noop=%u stub_pmmu_fline=%u cpnop=%u cpnop_fe=%u pc=%08X halted=%d\n",
           frame, s_insn, s_enter, s_exit, s_timeout, s_exc_total, s_vec_hist[4], s_vec_hist[11], s_vec_hist[14], vec_other,
           s_stub_fpu, s_stub_pmmu_noop, s_stub_pmmu_fline, s_cpnop, s_cpnop_fe, REG_PC, CPU_STOPPED != 0);
}
