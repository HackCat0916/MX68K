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
#include "../ThirdParty/Musashi/m68kops.h"   /* P899: m68ki_instruction_jump_table / m68ki_cycles */

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

extern void debug_log(const char* fmt, ...);

/* D-84: Musashiの汎用コプロセッサハンドラ(cpgen/cpscc/cpdbcc/cptrapcc/cpbcc)は
 * EC020以降でcpIDを見ずに無音で戻る(上流の未実装部分)。実機はコプロセッサ不在なら
 * F-line例外(MC68030 UM §8.1.5/§10.5.2.2)なので、表の項目を差し替える。
 * 表は m68k_init() が1回だけ作り、機種切替・リセットでは作り直さないので、ここも1回だけ。 */
#define MX_MUSASHI_NUM_CPU_TYPES 5   /* m68kops.c:34378 の NUM_CPU_TYPES と一致させること */

static void mx_musashi_fix_cp_dispatch(void)
{
    static int s_done;
    void (**jt)(void) = m68ki_instruction_jump_table;
    void (*fline)(void), (*fpu0)(void), (*cp[5])(void);
    unsigned int op, i, j, k, n_fline = 0, n_fpu = 0, n_cpdup = 0;
    const char *abort_reason = NULL;

    if (s_done) return;
    s_done = 1;
    fline = jt[0xFF00];  /* m68k_op_1111 */
    fpu0  = jt[0xF200];  /* m68k_op_040fpu0_32 */
    cp[0] = jt[0xFE00]; cp[1] = jt[0xFE40]; cp[2] = jt[0xFE48];  /* cpgen, cpscc, cpdbcc */
    cp[3] = jt[0xFE78]; cp[4] = jt[0xFE80];                      /* cptrapcc, cpbcc */

    /* 正準ポインタ7つの総当たり比較(21組)。
     * (1) fline/fpu0/NULL との重なり(11組+NULL検査)は置換先と置換元の取り違えになるので中止する。
     * (2) cp*同士の重なり(10組)は、本体が同一の4関数(cpbcc/cpgen/cpscc/cpdbcc)をリンカが1つに
     *     畳んだ場合に起こりうる。どれも置換対象なので置換結果は変わらないが、件数でわかるよう数えて出す。 */
    if (fline == NULL || fpu0 == NULL) abort_reason = "null";
    else if (fline == fpu0) abort_reason = "fline==fpu0";
    for (i = 0; i < 5 && abort_reason == NULL; i++) {
        if (cp[i] == NULL) abort_reason = "cp_null";
        else if (cp[i] == fline) abort_reason = "cp==fline";
        else if (cp[i] == fpu0) abort_reason = "cp==fpu0";
    }
    if (abort_reason != NULL) {
        debug_log("[P899-CPFIX] ABORT reason=%s fline=%p fpu0=%p cp=%p %p %p %p %p\n", abort_reason,
                  (void *)fline, (void *)fpu0, (void *)cp[0], (void *)cp[1], (void *)cp[2], (void *)cp[3], (void *)cp[4]);
        return;
    }
    for (i = 0; i < 5; i++)
        for (j = i + 1; j < 5; j++)
            if (cp[i] == cp[j]) n_cpdup++;

    /* cpID2-7: 汎用cp*の項目だけをline-1111へ(move16 $F620-F627 等の別ハンドラはポインタが違うので残る) */
    for (op = 0xF000; op <= 0xFFFF; op++) {
        if (((op >> 9) & 7) < 2) continue;          /* cpID0(PMMU)・cpID1(FPU)は対象外/下で個別に扱う */
        for (i = 0; i < 5; i++) {
            if (jt[op] == cp[i]) { jt[op] = fline; n_fline++; break; }
        }
    }
    /* cpID1: FDBcc $F248-F24F・FTRAPcc/FScc絶対番地 $F278-F27F を040fpu0へ戻す。サイクル値も040fpu0に揃える */
    for (j = 0; j < 8; j++) {
        unsigned int ops[2] = { 0xF248u + j, 0xF278u + j };
        for (i = 0; i < 2; i++) {
            if (jt[ops[i]] == cp[2] || jt[ops[i]] == cp[3]) {
                jt[ops[i]] = fpu0;
                for (k = 0; k < MX_MUSASHI_NUM_CPU_TYPES; k++)
                    m68ki_cycles[k][ops[i]] = m68ki_cycles[k][0xF200];
                n_fpu++;
            }
        }
    }
    debug_log("[P899-CPFIX] fline=%u fpu=%u cpdup=%u (expect 1528/16)\n", n_fline, n_fpu, n_cpdup);
}

/* P910: 040型の実行カウンタ([P910-040EXEC])。プロセス内の累計で、リセットではクリアしない */
static unsigned int s_p910_movec_040, s_p910_caar_illegal, s_p910_cinv_exec, s_p910_cinv_user;
static unsigned int s_p910_first_caar_ppc, s_p910_first_cinv_ppc, s_p910_first_cinv_ir;

static void (*s_movec_cr_orig)(void), (*s_movec_rc_orig)(void);
static void (*s_fline_orig)(void);

/* P916: 060turbo(MC68060相当)の本番識別フラグ。mx_cpu_musashi_set_model(3) で1、他のモデルでは0。
 * P913 の s_p913_is_060(環境変数プローブ専用)とは別物。set_model_from_env では両者を揃える */
static int s_is_060;

/* P916: MC68060 の MOVEC 制御レジスタ(M68060UM 書籍p.D-22 / PDF p.427 の表、XEiJ:MC68060.java:8900/8920/9035/9055) */
#define MX_MOVEC_BUSCR      0x008u
#define MX_MOVEC_PCR        0x808u
/* PCR bit31-16=$0430(MC68060。EC/LC060は$0431)、bit15-8=リビジョン(M68060UM 書籍p.3-5 / PDF p.70、Figure 3-5) */
#define MX_060_PCR_ID       0x04300000u
/* リビジョンは一次資料では決まらない。XEiJ:XEiJ.java:5477 MPU_060_REV=7 と、XEiJ 0.26.10.08 の si 表示
 * ($0430/$07)に合わせる(P916 Step 4 ユーザー決定) */
#define MX_060_PCR_REV      7u
/* 書けるビット: PCR は EDEBUG(bit7)・DFP(bit1)・ESS(bit0)(M68060UM p.3-5、XEiJ:MC68060.java:9066)、
 * BUSCR は L/SL/LE/SLE(bit31-28)(M68060UM p.7-4〜7-5 Figure 7-5、XEiJ:MC68060.java:9037)。どちらも副作用無し */
#define MX_060_PCR_WMASK    0x00000083u
#define MX_060_BUSCR_WMASK  0xF0000000u

/* MOVEC ラッパの判定結果 */
#define MX_MOVEC_PASS       0   /* 元の Musashi ハンドラへ渡す */
#define MX_MOVEC_ILLEGAL    1   /* 不当命令例外にする */
#define MX_MOVEC_DONE       2   /* ラッパの中で処理を終えた */

/* P916: 060の影レジスタ(書けるビットだけを持つ。リセットで0、M68060UM 書籍p.8-16 / PDF p.253) */
static uint32_t s_060_pcr_w, s_060_buscr;
/* P916: 060の実行カウンタ([P916-060EXEC])。プロセス内の累計で、リセットではクリアしない */
static unsigned int s_p916_pcr_rd, s_p916_pcr_wr, s_p916_buscr_rd, s_p916_buscr_wr;
static unsigned int s_p916_absent[4];   /* 添字 0..3 = $802..$805 */
static unsigned int s_p916_absent_total, s_p916_first_absent_ppc, s_p916_first_absent_cr;
static unsigned int s_p916_first_pcr_ppc, s_p916_movep_exec;

static void p916_count_absent(unsigned int cr)
{
    if (s_p916_absent_total++ == 0) { s_p916_first_absent_ppc = REG_PPC; s_p916_first_absent_cr = cr; }
    if (cr >= 0x802u && cr <= 0x805u) s_p916_absent[cr - 0x802u]++;
}

/* 040型の MOVEC を、元のハンドラへ渡す/不当命令にする/ここで処理する の3つに分ける(to_cr: 1=Rn→cr、0=cr→Rn)。
 * 68040にCAARは無い(CAAR不在=040turbo書籍p.363)。指定時に不当命令例外とするのは
 * MC68030のMOVEC規則からの類推で、68040実機では未検証。060では M68060UM 書籍p.D-22 注1(表に無い番号は
 * 不当命令)が一次資料になる。040型・スーパーバイザ時だけ拡張語を先読みする。
 * 先読みは bus_read16 を直接使い、m68k_read_memory_16 の観測フック(p869_on_read16)を二重に発火させない。
 * プリフェッチOFF(m68kconf.h)なので REG_PC は拡張語を指している。先読みでは REG_PC は進めない */
static int movec_filter_040(int to_cr)
{
    unsigned int w2, cr;
    if (CPU_TYPE != CPU_TYPE_040) return MX_MOVEC_PASS;
    s_p910_movec_040++;
    if (!FLAG_S) return MX_MOVEC_PASS;   /* ユーザーモードは元のハンドラが特権違反にする(060でも同じ) */
    w2 = bus_read16(ADDRESS_68K(REG_PC));
    cr = w2 & 0xfffu;
    if (cr == 0x802u) {                  /* CAAR: 040にも060にも無い(P910の挙動のまま) */
        if (s_p910_caar_illegal++ == 0) s_p910_first_caar_ppc = REG_PPC;
        if (s_is_060) p916_count_absent(cr);
        return MX_MOVEC_ILLEGAL;
    }
    if (!s_is_060) return MX_MOVEC_PASS; /* 040はここまで(P910から変わらない) */
    if (cr == 0x803u || cr == 0x804u || cr == 0x805u) {   /* MSP/ISP/MMUSR: 060に無い */
        p916_count_absent(cr);
        return MX_MOVEC_ILLEGAL;
    }
    if (cr == MX_MOVEC_PCR || cr == MX_MOVEC_BUSCR) {
        uint *rn;
        /* 元のハンドラの OPER_I_16() と同じ方法で拡張語を読む(REG_PC+=2)。ゲストから見た読出し回数は元と同じ */
        w2 = m68ki_read_imm_16();
        m68ki_trace_t0();
        rn = &REG_DA[(w2 >> 12) & 15];
        if (cr == MX_MOVEC_PCR) {
            if (to_cr) {
                s_060_pcr_w = (uint32_t)*rn & MX_060_PCR_WMASK;
                s_p916_pcr_wr++;
            } else {
                *rn = MX_060_PCR_ID | (MX_060_PCR_REV << 8) | s_060_pcr_w;
                if (s_p916_pcr_rd++ == 0) s_p916_first_pcr_ppc = REG_PPC;
            }
        } else {
            if (to_cr) { s_060_buscr = (uint32_t)*rn & MX_060_BUSCR_WMASK; s_p916_buscr_wr++; }
            else { *rn = s_060_buscr; s_p916_buscr_rd++; }
        }
        return MX_MOVEC_DONE;
    }
    return MX_MOVEC_PASS;
}
/* $4E7A = movec cr→Rn(読出し)、$4E7B = movec Rn→cr(書込み)。m68k_in.c:6700 / :6831 */
static void mx_movec_cr(void)
{
    int r = movec_filter_040(0);
    if (r == MX_MOVEC_ILLEGAL) { m68ki_exception_illegal(); return; }
    if (r == MX_MOVEC_PASS) s_movec_cr_orig();
}
static void mx_movec_rc(void)
{
    int r = movec_filter_040(1);
    if (r == MX_MOVEC_ILLEGAL) { m68ki_exception_illegal(); return; }
    if (r == MX_MOVEC_PASS) s_movec_rc_orig();
}

/* CINV/CPUSH(040turbo書籍p.367)。MXはキャッシュをエミュレートしない(常にコヒーレント)ので無処理でよい。
 * ユーザーモードでの特権違反は[training knowledge](一次資料無し、P910 R-4) */
static void mx_cinv_cpush_040(void)
{
    if (!CPU_TYPE_IS_040_PLUS(CPU_TYPE)) { s_fline_orig(); return; }   /* 040以外は従来どおりF-line */
    if (!FLAG_S) { s_p910_cinv_user++; m68ki_exception_privilege_violation(); return; }
    if (s_p910_cinv_exec++ == 0) { s_p910_first_cinv_ppc = REG_PPC; s_p910_first_cinv_ir = REG_IR; }
}

/* P913: 060プローブ("060"、診断専用)。MC68060はMOVEPを持たず未実装整数命令例外(ベクタ61)になる
 * ([reference-implementation behaviour] XEiJ MC68060.java)。型は040のまま、MOVEPだけをベクタ61にする */
#define P913_VEC_UNIMPL_INT 61u
static int s_p913_is_060;
/* P914: "060nt"選択時のみ1。MOVEPをベクタ61化せず元のMusashiハンドラへ委譲する(素通り実験) */
static int s_p913_movep_passthrough;
static unsigned int s_p913_movep_total;
static int s_p913_frame;   /* 直近の [P869-SUM] のフレーム番号([P913-MOVEP] の鮮度表示用) */
static mx_cpu_musashi_logf s_logf;
static unsigned int s_insn;

/* MOVEP の基本4ハンドラ。添字は opmode(bit7-6): 0=16_er($0108) 1=32_er($0148) 2=16_re($0188) 3=32_re($01C8) */
static void (*s_movep_orig[4])(void);

static void mx_movep_060(void)
{
    /* P916: 060本番経路ではMOVEPをそのまま実行する(下の最初の分岐で元のハンドラへ行く)。ここでは数えるだけ。
     * P913 "060" プローブ(ベクタ61化)の時は実行されないので数えない */
    if (s_is_060 && CPU_TYPE == CPU_TYPE_040 && (!s_p913_is_060 || s_p913_movep_passthrough)) s_p916_movep_exec++;
    if (!s_p913_is_060 || CPU_TYPE != CPU_TYPE_040) { s_movep_orig[(REG_IR >> 6) & 3u](); return; }
    if (s_p913_movep_passthrough) {
        if (s_p913_movep_total++ == 0 && s_logf != NULL)
            s_logf("[P914-MOVEP-PASSTHROUGH] first PC=%08X IR=%04X insn=%u frame=%d\n", REG_PPC, REG_IR, s_insn, s_p913_frame);
        s_movep_orig[(REG_IR >> 6) & 3u]();
        return;
    }
    if (s_p913_movep_total++ == 0 && s_logf != NULL)
        s_logf("[P913-MOVEP] first PC=%08X IR=%04X insn=%u frame=%d\n", REG_PPC, REG_IR, s_insn, s_p913_frame);
    /* 積むフレーム形式は Musashi の040向け(format 2)。実機060の形式とは未照合(プローブ目的では不問) */
    m68ki_exception_trap(P913_VEC_UNIMPL_INT);
}

/* P913: MOVEP(mask=$F1F8、match=$0108/$0148/$0188/$01C8、各Dn×An=64、計256)を mx_movep_060 へ差し替える。
 * 正準ポインタと一致し、かつopcodeがmask/matchにも合う項目だけを数え、256/0でなければ一切差し替えない */
static void mx_musashi_fix_movep_dispatch(void)
{
    static int s_done;
    static const unsigned int match[4] = { 0x0108u, 0x0148u, 0x0188u, 0x01C8u };
    void (**jt)(void) = m68ki_instruction_jump_table;
    unsigned int op, i, j, n_hit = 0, n_mismatch = 0;
    const char *abort_reason = NULL;

    if (s_done) return;
    s_done = 1;
    for (i = 0; i < 4; i++) s_movep_orig[i] = jt[match[i]];
    for (i = 0; i < 4 && abort_reason == NULL; i++) {
        if (s_movep_orig[i] == NULL) abort_reason = "null";
        for (j = i + 1; j < 4 && abort_reason == NULL; j++)
            if (s_movep_orig[i] == s_movep_orig[j]) abort_reason = "dup";
    }
    if (abort_reason == NULL) {
        for (op = 0; op <= 0xFFFFu; op++)
            for (i = 0; i < 4; i++)
                if (jt[op] == s_movep_orig[i]) {
                    if ((op & 0xF1F8u) == match[i]) n_hit++; else n_mismatch++;
                    break;
                }
        if (n_hit != 256u || n_mismatch != 0u) abort_reason = "count";
    }
    if (abort_reason != NULL) {
        debug_log("[P913-INIT] ABORT reason=%s movep=%u mismatch=%u ptr=%p %p %p %p (expect 256/0)\n",
                  abort_reason, n_hit, n_mismatch, (void *)s_movep_orig[0], (void *)s_movep_orig[1],
                  (void *)s_movep_orig[2], (void *)s_movep_orig[3]);
        return;
    }
    n_hit = 0;
    for (op = 0; op <= 0xFFFFu; op++)
        for (i = 0; i < 4; i++)
            if (jt[op] == s_movep_orig[i]) { jt[op] = mx_movep_060; n_hit++; break; }
    debug_log("[P913-INIT] movep=%u mismatch=%u (expect 256/0)\n", n_hit, n_mismatch);
}

/* P910: MOVEC(CAAR)とCINV/CPUSH($F400-$F4FF、scope≠0)の表の項目を差し替える。
 * mx_musashi_fix_cp_dispatch の直後に1回だけ呼ぶ(表は m68k_init() が1回だけ作る)。
 * 件数がずれても中止せず実数を出す(smoke_test.sh が完全一致で照合する) */
static void mx_musashi_fix_040_dispatch(void)
{
    static int s_done;
    void (**jt)(void) = m68ki_instruction_jump_table;
    void (*fline)(void);
    unsigned int op, n_cinv = 0, n_skip = 0, n_movec = 0;
    const char *abort_reason = NULL;

    if (s_done) return;
    s_done = 1;
    fline = jt[0xFF00];  /* m68k_op_1111(P899と同じ正準ポインタ) */
    if (fline == NULL) abort_reason = "fline_null";
    else if (jt[0x4E7A] == NULL || jt[0x4E7B] == NULL) abort_reason = "movec_null";
    else if (jt[0xF4D8] != fline) abort_reason = "p899_not_applied";   /* P899がABORTした場合 */
    if (abort_reason != NULL) {
        debug_log("[P910-040FIX] ABORT reason=%s fline=%p movec_cr=%p movec_rc=%p f4d8=%p\n", abort_reason,
                  (void *)fline, (void *)jt[0x4E7A], (void *)jt[0x4E7B], (void *)jt[0xF4D8]);
        return;
    }

    s_fline_orig = fline;
    for (op = 0xF400; op <= 0xF4FF; op++) {
        if (((op >> 3) & 3) == 0) continue;   /* scope=00 は無効形式なのでF-lineのまま */
        if (jt[op] != fline) { n_skip++; continue; }
        jt[op] = mx_cinv_cpush_040;
        m68ki_cycles[4][op] = 4;              /* 040列のみ(m68kcpu.c:907 CYC_INSTRUCTION=m68ki_cycles[4]) */
        n_cinv++;
    }
    s_movec_cr_orig = jt[0x4E7A]; jt[0x4E7A] = mx_movec_cr; n_movec++;
    s_movec_rc_orig = jt[0x4E7B]; jt[0x4E7B] = mx_movec_rc; n_movec++;
    debug_log("[P910-040FIX] cinv=%u skipped=%u movec=%u (expect 192/0/2)\n", n_cinv, n_skip, n_movec);
    mx_musashi_fix_movep_dispatch();   /* P913 */
}

/* P917: 起動方法フラグ($0CBF)の確定命令 Seq (A0)(IPLROM30 $FF012E の $57D0)を観測する診断プローブ。
 * 元の処理より前に $0030/$0CC3/Zフラグを生で読んでログを出し、必ず元のハンドラへ渡す(挙動は不変)。
 * reset_seq が変わるたびに1行出す(リセット毎の分母を得るため)。
 * P918: 直前の cmp.l a1,d6 の入力そのもの(d6=ベクタ初期化前に読んだ $0030 の下位24bit、
 * a1=比較先 $FF0770)を生値で併記する。post_init_0030 は IPL がベクタを埋めた「後」の値であり、
 * 判定に使われた値ではない(旧名 old_0030 はこの点を取り違えていた) */
extern int g_mx68k_frame_num;
static unsigned int s_p917_reset_seq;
static unsigned int s_p917_logged_seq;
static void (*s_p917_seq_ai_orig)(void);

static void mx_p917_bootflag_hook(void)
{
    if (s_p917_logged_seq != s_p917_reset_seq) {
        /* 観測フック(p869_on_read*)を発火させないよう bus_read* を直接使う */
        uint32_t post_init_0030 = (bus_read16(0x000030u) << 16) | bus_read16(0x000032u);
        uint32_t raw_d6 = (uint32_t)REG_D[6];
        uint32_t raw_a1 = (uint32_t)REG_A[1];
        unsigned int raw_eq = COND_EQ() ? 1u : 0u;
        unsigned int raw_0cc3 = bus_read8(0x000CC3u);
        int will_be = (raw_0cc3 & 0x80u) ? -2 : (raw_eq ? -1 : 0);
        s_p917_logged_seq = s_p917_reset_seq;
        debug_log("[P917-BOOTFLAG] frame=%d reset_seq=%u pc=%08X d6=%08X a1=%08X post_init_0030=%08X "
                  "raw_eq=%u raw_0cc3=%02X new_0cbf_will_be=%d\n",
                  g_mx68k_frame_num, s_p917_reset_seq, REG_PPC, raw_d6, raw_a1, post_init_0030,
                  raw_eq, raw_0cc3, will_be);
    }
    s_p917_seq_ai_orig();
}

/* m68k_op_seq_8_ai は mask=$FFF8/match=$57D0(m68kops.c の表)で $57D0-$57D7 の8項目が共有する。
 * 正準ポインタが8件・範囲外0件であることを確かめてから $57D0(An=A0)の1項目だけ差し替える */
static void mx_musashi_p917_install_bootflag_hook(void)
{
    static int s_done;
    void (**jt)(void) = m68ki_instruction_jump_table;
    unsigned int op, n_hit = 0, n_outside = 0;

    if (s_done) return;
    s_done = 1;
    s_p917_seq_ai_orig = jt[0x57D0];
    if (s_p917_seq_ai_orig == NULL) {
        debug_log("[P917-INIT] ABORT reason=null\n");
        return;
    }
    for (op = 0; op <= 0xFFFFu; op++)
        if (jt[op] == s_p917_seq_ai_orig) {
            if ((op & 0xFFF8u) == 0x57D0u) n_hit++; else n_outside++;
        }
    if (n_hit != 8u || n_outside != 0u) {
        debug_log("[P917-INIT] ABORT reason=count seq_ai=%u outside=%u ptr=%p (expect 8/0)\n",
                  n_hit, n_outside, (void *)s_p917_seq_ai_orig);
        return;
    }
    jt[0x57D0] = mx_p917_bootflag_hook;
    debug_log("[P917-INIT] seq_ai=%u outside=%u replaced=1 (expect 8/0/1)\n", n_hit, n_outside);
}

/* P921: ABCD/SBCD/NBCD の補正式とフラグを XEiJ に合わせる(D-87)。
 * Musashi はキャリー閾値が res>0x99、SBCD の下位補正条件が res>9 で、非BCD入力のとき
 * 結果・X・C が違っていた(XEiJ:MC68000.java:13257 irpAbcd / :13307 irpSbcd / :5845 irpNbcd)。
 * N・V は一次資料では不定。XEiJ と instructiontest の期待値に合わせて3通りにする。
 * ★060判定(s_is_060)は必ず先に行う。060turbo は CPU_TYPE_040 なので EC020+ 判定にも当たる */
static unsigned int mx_bcd_finish(int t, int z)
{
    z &= 0xff;
    FLAG_Z |= (uint)z;                       /* 0 でなければ Z をクリア、0 なら変えない */
    if (s_is_060) {
        /* 060: N・V は変化しない(XEiJ:MC68060.java) */
    } else if (CPU_TYPE_IS_EC020_PLUS(CPU_TYPE)) {
        FLAG_N = NFLAG_8((uint)z);           /* EC030/040: N=最上位ビット、V=0(XEiJ:MC68EC030.java) */
        FLAG_V = VFLAG_CLEAR;
    } else {
        int a = z - t;                       /* 000/010: V=補正値の加算でのオーバーフロー(XEiJ:MC68000.java) */
        FLAG_N = NFLAG_8((uint)z);
        FLAG_V = (((t ^ z) & (a ^ z)) & 0x80) ? VFLAG_SET : VFLAG_CLEAR;
    }
    return (unsigned int)z;
}

static unsigned int mx_bcd_add(unsigned int dst, unsigned int src)
{
    int c = (int)XFLAG_AS_1();
    int t = (int)(dst & 0xffu) + (int)(src & 0xffu) + c;   /* 仮の結果 */
    int z = t;
    if ((int)(dst & 0x0fu) + (int)(src & 0x0fu) + c >= 0x0a) z += 0x10 - 0x0a;   /* ハーフキャリー */
    if (z >= 0xa0) { z += 0x100 - 0xa0; FLAG_X = XFLAG_SET; FLAG_C = CFLAG_SET; }
    else { FLAG_X = XFLAG_CLEAR; FLAG_C = CFLAG_CLEAR; }
    return mx_bcd_finish(t, z);
}

static unsigned int mx_bcd_sub(unsigned int dst, unsigned int src)
{
    int b = (int)XFLAG_AS_1();
    int t = (int)(dst & 0xffu) - (int)(src & 0xffu) - b;   /* 仮の結果 */
    int z = t;
    if ((int)(dst & 0x0fu) - (int)(src & 0x0fu) - b < 0) z -= 0x10 - 0x0a;   /* ハーフボロー */
    if (z < 0) {
        if (t < 0) z -= 0x100 - 0xa0;      /* 上位補正は補正前の生の差が負のときだけ */
        FLAG_X = XFLAG_SET; FLAG_C = CFLAG_SET;
    } else { FLAG_X = XFLAG_CLEAR; FLAG_C = CFLAG_CLEAR; }
    return mx_bcd_finish(t, z);
}

/* アドレッシングは元の m68k_op_abcd_8_{rr,mm,mm_ay7,mm_ax7,mm_axy7}(m68kops.c:46-173)と同じ順序・同じマクロ。
 * mm は src(-(Ay))を先に読み、次に dst の番地(-(Ax))を作る。A7 のバイト単位プリデクリメントは2減らす */
static void mx_abcd_op(void)
{
    if ((REG_IR & 0x08u) == 0) {
        uint *r_dst = &DX;
        uint res = mx_bcd_add(*r_dst, DY);
        *r_dst = MASK_OUT_BELOW_8(*r_dst) | res;
    } else {
        uint src = ((REG_IR & 7u) == 7u) ? OPER_A7_PD_8() : OPER_AY_PD_8();
        uint ea  = (((REG_IR >> 9) & 7u) == 7u) ? EA_A7_PD_8() : EA_AX_PD_8();
        uint dst = m68ki_read_8(ea);
        m68ki_write_8(ea, mx_bcd_add(dst, src));
    }
}

/* 元: m68k_op_sbcd_8_{rr,mm,mm_ay7,mm_ax7,mm_axy7}(m68kops.c:29496-29623) */
static void mx_sbcd_op(void)
{
    if ((REG_IR & 0x08u) == 0) {
        uint *r_dst = &DX;
        uint res = mx_bcd_sub(*r_dst, DY);
        *r_dst = MASK_OUT_BELOW_8(*r_dst) | res;
    } else {
        uint src = ((REG_IR & 7u) == 7u) ? OPER_A7_PD_8() : OPER_AY_PD_8();
        uint ea  = (((REG_IR >> 9) & 7u) == 7u) ? EA_A7_PD_8() : EA_AX_PD_8();
        uint dst = m68ki_read_8(ea);
        m68ki_write_8(ea, mx_bcd_sub(dst, src));
    }
}

/* NBCD = 0 − x − X の SBCD(XEiJ irpNbcd)。元: m68k_op_nbcd_8_{d,ai,pi,pi7,pd,pd7,di,ix,aw,al}(m68kops.c:25380-)。
 * 元は結果が$9Aになる場合に書込みを省いていたが、ここは常に書く(XEiJ busWb・実機のリード・モディファイ・ライト) */
static void mx_nbcd_op(void)
{
    unsigned int mode = (REG_IR >> 3) & 7u, reg = REG_IR & 7u;
    uint ea, dst;

    if (mode == 0) {
        uint *r = &DY;
        *r = MASK_OUT_BELOW_8(*r) | mx_bcd_sub(0, *r);
        return;
    }
    switch (mode) {
    case 2:  ea = EA_AY_AI_8(); break;
    case 3:  ea = (reg == 7u) ? EA_A7_PI_8() : EA_AY_PI_8(); break;
    case 4:  ea = (reg == 7u) ? EA_A7_PD_8() : EA_AY_PD_8(); break;
    case 5:  ea = EA_AY_DI_8(); break;
    case 6:  ea = EA_AY_IX_8(); break;
    default: ea = (reg == 1u) ? EA_AL_8() : EA_AW_8(); break;   /* mode 7: 差し替えるのは reg 0(aw)/1(al)のみ */
    }
    dst = m68ki_read_8(ea);
    m68ki_write_8(ea, mx_bcd_sub(0, dst));
}

/* P921: ABCD($C100系、mask=$F1F0)128・SBCD($8100系)128・NBCD($4800系、mask=$FFC0)50 の項目を差し替える。
 * 正準ポインタ(各族の全形式)が NULL でなく、族内・族間で重複せず、一致項目が全て族の範囲内で
 * 件数が 128/128/50・範囲外0 のときだけ差し替える。1つでも外れたら一切差し替えない(P913と同じ) */
#define P921_N_ABCD 5
#define P921_N_NBCD 10
static void mx_musashi_fix_bcd_dispatch(void)
{
    static int s_done;
    static const unsigned int abcd_ops[P921_N_ABCD] = { 0xC100u, 0xC108u, 0xC10Fu, 0xCF08u, 0xCF0Fu };
    static const unsigned int sbcd_ops[P921_N_ABCD] = { 0x8100u, 0x8108u, 0x810Fu, 0x8F08u, 0x8F0Fu };
    static const unsigned int nbcd_ops[P921_N_NBCD] = { 0x4800u, 0x4810u, 0x4818u, 0x481Fu, 0x4820u,
                                                        0x4827u, 0x4828u, 0x4830u, 0x4838u, 0x4839u };
    enum { P921_N_ALL = P921_N_ABCD * 2 + P921_N_NBCD };
    void (**jt)(void) = m68ki_instruction_jump_table;
    void (*ptr[P921_N_ALL])(void);
    int fam[P921_N_ALL];   /* 0=ABCD 1=SBCD 2=NBCD */
    unsigned int op, i, j, n[3] = { 0, 0, 0 }, n_mismatch = 0;
    const char *abort_reason = NULL;

    if (s_done) return;
    s_done = 1;
    for (i = 0; i < P921_N_ABCD; i++) {
        ptr[i] = jt[abcd_ops[i]]; fam[i] = 0;
        ptr[P921_N_ABCD + i] = jt[sbcd_ops[i]]; fam[P921_N_ABCD + i] = 1;
    }
    for (i = 0; i < P921_N_NBCD; i++) { ptr[P921_N_ABCD * 2 + i] = jt[nbcd_ops[i]]; fam[P921_N_ABCD * 2 + i] = 2; }

    /* 族内・族間をまとめて総当たり(全20ポインタが互いに異なること) */
    for (i = 0; i < P921_N_ALL && abort_reason == NULL; i++) {
        if (ptr[i] == NULL) abort_reason = "null";
        for (j = i + 1; j < P921_N_ALL && abort_reason == NULL; j++)
            if (ptr[i] == ptr[j]) abort_reason = (fam[i] == fam[j]) ? "dup" : "dup_family";
    }
    if (abort_reason == NULL) {
        for (op = 0; op <= 0xFFFFu; op++)
            for (i = 0; i < P921_N_ALL; i++)
                if (jt[op] == ptr[i]) {
                    int in_range = (fam[i] == 0) ? ((op & 0xF1F0u) == 0xC100u)
                                 : (fam[i] == 1) ? ((op & 0xF1F0u) == 0x8100u)
                                 :                 ((op & 0xFFC0u) == 0x4800u);
                    if (in_range) n[fam[i]]++; else n_mismatch++;
                    break;
                }
        if (n[0] != 128u || n[1] != 128u || n[2] != 50u || n_mismatch != 0u) abort_reason = "count";
    }
    if (abort_reason != NULL) {
        debug_log("[P921-BCDFIX] ABORT reason=%s abcd=%u sbcd=%u nbcd=%u mismatch=%u (expect 128/128/50/0)\n",
                  abort_reason, n[0], n[1], n[2], n_mismatch);
        return;
    }
    n[0] = n[1] = n[2] = 0;
    for (op = 0; op <= 0xFFFFu; op++)
        for (i = 0; i < P921_N_ALL; i++)
            if (jt[op] == ptr[i]) {
                jt[op] = (fam[i] == 0) ? mx_abcd_op : (fam[i] == 1) ? mx_sbcd_op : mx_nbcd_op;
                n[fam[i]]++;
                break;
            }
    debug_log("[P921-BCDFIX] abcd=%u sbcd=%u nbcd=%u mismatch=%u (expect 128/128/50/0)\n",
              n[0], n[1], n[2], n_mismatch);
}

void mx_cpu_musashi_p910_tick(int frame, mx_cpu_musashi_logf logf)
{
    if (logf == NULL || CPU_TYPE != CPU_TYPE_040) return;
    logf("[P910-040EXEC] f=%d movec_040=%u caar_illegal=%u first_caar_ppc=%08X cinv_exec=%u cinv_user=%u "
         "first_cinv_ppc=%08X first_cinv_ir=%04X\n",
         frame, s_p910_movec_040, s_p910_caar_illegal, s_p910_first_caar_ppc, s_p910_cinv_exec, s_p910_cinv_user,
         s_p910_first_cinv_ppc, s_p910_first_cinv_ir);
    /* P916: 060turbo のときだけ。pcr= は ID|Rev|書けるビット の合成値、cbc_w はゲスト $0CBC/$0CBD の生の値
     * (フレーム間のCPU停止中に呼ばれる。RAMを読むだけで副作用は無い) */
    if (s_is_060)
        logf("[P916-060EXEC] f=%d movec_040=%u pcr_rd=%u pcr_wr=%u buscr_rd=%u buscr_wr=%u absent_illegal=%u "
             "absent_hist=802:%u,803:%u,804:%u,805:%u first_absent_ppc=%08X first_absent_cr=%03X first_pcr_ppc=%08X "
             "pcr=%08X buscr=%08X movep_exec=%u cbc_w=%04X\n",
             frame, s_p910_movec_040, s_p916_pcr_rd, s_p916_pcr_wr, s_p916_buscr_rd, s_p916_buscr_wr,
             s_p916_absent_total, s_p916_absent[0], s_p916_absent[1], s_p916_absent[2], s_p916_absent[3],
             s_p916_first_absent_ppc, s_p916_first_absent_cr, s_p916_first_pcr_ppc,
             MX_060_PCR_ID | (MX_060_PCR_REV << 8) | s_060_pcr_w, s_060_buscr, s_p916_movep_exec,
             bus_read16(0x0CBCu));
}

/* --- 初期化・リセット --- */
void mx_cpu_musashi_init(const mx_cpu_bus *bus)
{
    s_bus = *bus;   /* 呼出し側はスタック上の構造体を渡す(m68000_bridge.c の c68k_init)ので値でコピーする */
    s_in_execute = 0;
    m68k_init();    /* 全コールバックをNULLへ戻すので、int_ackの登録は必ずこの後 */
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(musashi_int_ack);
    mx_musashi_fix_cp_dispatch();
    mx_musashi_fix_040_dispatch();   /* P910 */
    mx_musashi_p917_install_bootflag_hook();   /* P917 */
    mx_musashi_fix_bcd_dispatch();   /* P921 */
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

/* P897: FPUの初期化は m68k_pulse_reset() の後(上流 m68k_pulse_reset はFPUに触れない)。
 * P916: 060の PCR/BUSCR の書けるビットはリセットで0(M68060UM 書籍p.8-16 / PDF p.253)
 * P917: reset_seq は [P917-BOOTFLAG] の分母(1=起動直後の初期化、2以降=ハード/ソフトリセット) */
void mx_cpu_musashi_reset(void)
{
    s_p917_reset_seq++;
    m68k_pulse_reset();
    mx68k_m68kfpu_reset();
    s_060_pcr_w = 0;
    s_060_buscr = 0;
}

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

/* s_logf・s_insn(命令フェッチ総数=分母)は P913 のため上方(mx_movep_060 の前)で定義している */
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

/* P904: MOVEC Rn,Rc($4E7B)の観測。"040" プローブ時だけ有効。拡張語の読み出しで生値を控え、
 * 次の命令語フェッチで書込み後のCACRと次PC(例外へ飛んだかどうか)を併せて1行出す */
#define P904_CAP_MOVEC 32u
static int s_p904_movec_on;
static unsigned int s_movec_total, s_movec_lines;
static int s_movec_pending;
static unsigned int s_movec_ppc, s_movec_ext, s_movec_src, s_movec_cacr_before;

/* P906: 040プローブ限定、直近64件の例外{vec, PPC, IR, D0}を保持するリング */
#define P906_RING_SIZE 64u
static struct { unsigned int vec, ppc, ir, d0; } s_exc_ring[P906_RING_SIZE];
static unsigned int s_exc_ring_total;

/* P907: 040プローブ限定、待機ループ入口(P906実測)へ初到達した瞬間に1回だけリングをダンプする */
#define P907_LOOP_ENTRY_PC 0x0000DF0Au
static int s_p907_fired;

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
    /* P904: MOVEC Rn,Rc の拡張語の読み出し(FLAG_Sの検査後、書込みの直前)。REG_IR は実行中の命令語 */
    if (s_p904_movec_on && REG_IR == 0x4E7Bu && address == ((REG_PPC + 2) & 0x00FFFFFFu) && REG_PC == REG_PPC + 4) {
        s_movec_total++;
        s_movec_pending = 1;
        s_movec_ppc = REG_PPC;
        s_movec_ext = v;
        s_movec_src = REG_DA[(v >> 12) & 15];
        s_movec_cacr_before = REG_CACR;
        return;
    }
    if (!(address == (REG_PPC & 0x00FFFFFFu) && REG_PC == REG_PPC + 2)) return;

    if (s_movec_pending) {
        s_movec_pending = 0;
        if (s_movec_lines < P904_CAP_MOVEC) {
            s_movec_lines++;
            s_logf("[P904-MOVEC] n=%u total=%u insn=%u ppc=%08X ext=%04X ctrl=%03X src=%08X cacr_before=%08X cacr_after=%08X next_ppc=%08X\n",
                   s_movec_lines, s_movec_total, s_insn, s_movec_ppc, s_movec_ext, s_movec_ext & 0xFFFu,
                   s_movec_src, s_movec_cacr_before, REG_CACR, address);
        }
    }

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
    /* P907: ループ突入前の直近64件を残すため、フラグを先に立ててから
     * [P906-EXCRING]と同じcount/start算出式でダンプする */
    if (s_p904_movec_on && !s_p907_fired && address == P907_LOOP_ENTRY_PC) {
        static char lbuf[64 * 48 + 1];
        unsigned int total, count, start, i;
        size_t len = 0;
        s_p907_fired = 1;
        total = s_exc_ring_total;
        count = (total < P906_RING_SIZE) ? total : P906_RING_SIZE;
        start = (total < P906_RING_SIZE) ? 0u : (total % P906_RING_SIZE);
        lbuf[0] = '\0';
        for (i = 0; i < count; i++) {
            unsigned int idx = (start + i) % P906_RING_SIZE;
            len += (size_t)snprintf(lbuf + len, sizeof(lbuf) - len, " n%u:vec=%u ppc=%08X ir=%04X d0=%08X",
                                    i, s_exc_ring[idx].vec, s_exc_ring[idx].ppc, s_exc_ring[idx].ir, s_exc_ring[idx].d0);
        }
        s_logf("[P907-LOOPENTRY] total=%u trigger_ppc=%08X insn=%u%s\n", total, address, s_insn, lbuf);
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
    if (s_p904_movec_on) {
        unsigned int idx = s_exc_ring_total % P906_RING_SIZE;
        s_exc_ring[idx].vec = vec;
        s_exc_ring[idx].ppc = REG_PPC;
        s_exc_ring[idx].ir  = REG_IR;
        s_exc_ring[idx].d0  = REG_D[0];
        s_exc_ring_total++;
    }
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
 * model=1(EC030)は P869 の "ec030" と同じ3点(型・24bitマスク・PMMU命令のno-opスタブ)。
 * P910: model=2(MC68040、040turbo)は P909 で起動した "040" と同じ3点(型・24bitマスク・HAS_PMMU=0)。
 * P916: model=3(MC68060相当、060turbo)は model=2 と同じ3点に s_is_060=1 を加える(Musashiに060型は無い)。
 *   MOVEP は P914 と同じくそのまま実行し、MOVEC だけ060の応答にする(movec_filter_040)。
 * model=0 は 68000型へ戻す(m68k_set_cpu_type が CPU_ADDRESS_MASK=0x00FFFFFF・HAS_PMMU=0 を再設定する)。 */
void mx_cpu_musashi_set_model(int model)
{
    if (model == 1) {
        m68k_set_cpu_type(M68K_CPU_TYPE_68EC030);
        CPU_ADDRESS_MASK = 0x00ffffffu;
        HAS_PMMU = 1;
    } else if (model == 2 || model == 3) {
        /* m68k_set_cpu_type が32bitマスク・HAS_PMMU=1 にするので後から上書きする */
        m68k_set_cpu_type(M68K_CPU_TYPE_68040);
        CPU_ADDRESS_MASK = 0x00ffffffu;
        HAS_PMMU = 0;
    } else {
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    }
    s_is_060 = (model == 3);   /* P916 */
    s_p913_is_060 = 0;   /* P913: 本番経路では060プローブを必ず無効にする */
    s_p913_movep_passthrough = 0;   /* P914 */
    fpu_apply_for_model(model != 0);   /* P897。本番経路では直後に reset_hard が set_fpu_config で上書きする */
}

/* P872: 現在の型の分類(ステートのCPU識別子の下位バイト)。
 * 0x00=68000型、0x01=EC030(24bitマスク+PMMU no-opスタブ=本番/P869 "ec030" と同構成)、
 * 0x02=EC030+ハイメモリ、0x03/0x04=040turbo(ハイメモリ無し/有り)、0x05/0x06=060turbo(同、P916)、0xFF=それ以外 */
uint32_t mx_cpu_musashi_get_model_id(void)
{
    if (CPU_TYPE == CPU_TYPE_000) return 0x00u;
    /* P887: 0x02=EC030+PMMU no-opスタブ+32bitマスク+ハイメモリ有効 */
    if (CPU_TYPE == CPU_TYPE_EC030 && CPU_ADDRESS_MASK == 0xffffffffu && HAS_PMMU && s_himem) return 0x02u;
    if (CPU_TYPE == CPU_TYPE_EC030 && CPU_ADDRESS_MASK == 0x00ffffffu && HAS_PMMU) return 0x01u;
    /* P910: 0x04=040+32bitマスク+HAS_PMMU=0+ハイメモリ有効、0x03=040+24bitマスク+HAS_PMMU=0 */
    if (CPU_TYPE == CPU_TYPE_040 && CPU_ADDRESS_MASK == 0xffffffffu && !HAS_PMMU && s_himem && !s_is_060) return 0x04u;
    if (CPU_TYPE == CPU_TYPE_040 && CPU_ADDRESS_MASK == 0x00ffffffu && !HAS_PMMU && !s_himem && !s_is_060) return 0x03u;
    /* P916: 0x06=060turbo(040型+s_is_060)+32bitマスク+ハイメモリ有効、0x05=同+24bitマスク+ハイメモリ無し */
    if (CPU_TYPE == CPU_TYPE_040 && CPU_ADDRESS_MASK == 0xffffffffu && !HAS_PMMU && s_himem && s_is_060) return 0x06u;
    if (CPU_TYPE == CPU_TYPE_040 && CPU_ADDRESS_MASK == 0x00ffffffu && !HAS_PMMU && !s_himem && s_is_060) return 0x05u;
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
    int ec030 = 0, bare = 0, m040 = 0, m060 = 0, m060nt = 0, probe = 0, unrecognized = 0;

    if (env != NULL && env[0] != '\0') {
        if (strcmp(env, "68000-probe") == 0)     { model = "68000-probe"; probe = 1; }
        else if (strcmp(env, "ec030") == 0)      { model = "ec030"; ec030 = 1; probe = 1; }
        else if (strcmp(env, "ec030-bare") == 0) { model = "ec030-bare"; bare = 1; probe = 1; }
        else if (strcmp(env, "040") == 0)        { model = "040"; m040 = 1; probe = 1; }   /* P904 */
        else if (strcmp(env, "060") == 0)        { model = "060"; m060 = 1; probe = 1; }   /* P913 */
        else if (strcmp(env, "060nt") == 0)      { model = "060nt"; m060nt = 1; probe = 1; }   /* P914 */
        else unrecognized = 1;
    }
    s_p913_is_060 = m060 || m060nt;
    s_p913_movep_passthrough = m060nt;
    s_is_060 = m060 || m060nt;   /* P916: get_model_id と EmulatorBridge.c の逆変換(0x0105/0x0106→3)を揃える */

    if (ec030 || bare) {
        m68k_set_cpu_type(M68K_CPU_TYPE_68EC030);
        /* ハイメモリ無しX68030の上位8bit無視ミラー(外部バス24bit相当) */
        CPU_ADDRESS_MASK = 0x00ffffffu;
        /* ec030: PMMU命令をMX作成no-opスタブ(m68kmmu.h)へ通す。PMMU_ENABLED は立てないので変換は起きない。
         * ec030-bare: 上流EC030のまま(PMMU命令は line-1111) */
        HAS_PMMU = ec030 ? 1 : 0;
    } else if (m040 || m060 || m060nt) {
        /* P904: m68k_set_cpu_type が32bitマスク・HAS_PMMU=1 にするので後から上書きする。
         * HAS_PMMU=0: 030形式PMMU命令はno-opスタブでなく line-1111 へ(m68k_in.c の pmmu ハンドラ)。
         * P913: "060" も同じ040型構成(Musashiに060型は無い)。差分はMOVEPのベクタ61化のみ。
         * P914: "060nt" も同構成で、MOVEPはベクタ61化せず素通り実行する */
        m68k_set_cpu_type(M68K_CPU_TYPE_68040);
        CPU_ADDRESS_MASK = 0x00ffffffu;
        HAS_PMMU = 0;
    }
    fpu_apply_for_model(ec030 || bare || m040 || m060 || m060nt);   /* P897。040/060はEC030向け配線の近似(P904) */

    if (probe) {
        s_logf = logf;
        mx68k_musashi_stub_hook = p869_stub_event;
        s_p869_on = (logf != NULL);
        s_p904_movec_on = m040 && s_p869_on;
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
    s_p913_frame = frame;
    vec_other = s_exc_total - s_vec_hist[4] - s_vec_hist[11];
    s_logf("[P869-SUM] f=%d insn=%u enter=%u exit=%u timeout=%u exc_total=%u vec4=%u vec11=%u vec14=%u vec_other=%u "
           "stub_fpu=%u stub_pmmu_noop=%u stub_pmmu_fline=%u cpnop=%u cpnop_fe=%u pc=%08X halted=%d p907_fired=%d "
           "p913_movep_total=%u\n",
           frame, s_insn, s_enter, s_exit, s_timeout, s_exc_total, s_vec_hist[4], s_vec_hist[11], s_vec_hist[14], vec_other,
           s_stub_fpu, s_stub_pmmu_noop, s_stub_pmmu_fline, s_cpnop, s_cpnop_fe, REG_PC, CPU_STOPPED != 0, s_p907_fired,
           s_p913_movep_total);

    /* P905: 040プローブ限定で非ゼロベクタを全件1行出力。sum==exc_total で取りこぼし無しを自己検証できる。
     * バッファは全256ベクタ×" v255=4294967295"(16字)でも収まる大きさ。
     * P913: 060プローブでも同じ内容を [P913-VECHIST] として出す(ゲート・タグのみ別) */
    if (s_p904_movec_on || (s_p913_is_060 && s_p869_on)) {
        static char buf[256 * 16 + 1];
        unsigned int sum = 0;
        size_t len = 0;
        int v;
        buf[0] = '\0';
        for (v = 0; v < 256; v++) {
            if (s_vec_hist[v] == 0) continue;
            sum += s_vec_hist[v];
            len += (size_t)snprintf(buf + len, sizeof(buf) - len, " v%d=%u", v, s_vec_hist[v]);
        }
        s_logf("[%s-VECHIST] f=%d sum=%u exc_total=%u%s\n", s_p904_movec_on ? "P905" : "P913",
               frame, sum, s_exc_total, buf);
    }

    /* P906: リングの中身を古い順に1行出力。未一周(total<64)の間はスロット0から、
     * 一周後は次に上書きされるスロット(=最古)から巡回する。
     * バッファは64件×" n63:vec=255 ppc=FFFFFFFF ir=FFFF d0=FFFFFFFF"(約45字)に余裕を持たせた大きさ */
    if (s_p904_movec_on) {
        static char rbuf[64 * 48 + 1];
        unsigned int total = s_exc_ring_total;
        unsigned int count = (total < P906_RING_SIZE) ? total : P906_RING_SIZE;
        unsigned int start = (total < P906_RING_SIZE) ? 0u : (total % P906_RING_SIZE);
        unsigned int i;
        size_t len = 0;
        rbuf[0] = '\0';
        for (i = 0; i < count; i++) {
            unsigned int idx = (start + i) % P906_RING_SIZE;
            len += (size_t)snprintf(rbuf + len, sizeof(rbuf) - len, " n%u:vec=%u ppc=%08X ir=%04X d0=%08X",
                                    i, s_exc_ring[idx].vec, s_exc_ring[idx].ppc, s_exc_ring[idx].ir, s_exc_ring[idx].d0);
        }
        s_logf("[P906-EXCRING] f=%d total=%u%s\n", frame, total, rbuf);
    }
}
