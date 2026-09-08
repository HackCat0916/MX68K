#include "EmulatorBridge.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <time.h>   /* P694: mx68k_get_rtc_status() の localtime_r()/time() */
/* P633: P490 の CoreMIDI 直参照(packetList の extern 宣言と付け替え)は、
 * CoreMIDI 連携層の Bridge/midi_coremidi.c への移設にともない削除した。
 * このファイルはもう CoreMIDI の型を必要としない。 */
#include "GamePad.h"
/* P557: FD アクセス高速化。★このファイル自身は素の usleep() を呼ぶ必要があるため、
 * include の前に必ず FDD_TIMING_SHIM_NO_MACROS を定義する(定義を怠ると
 * mx68k_fdd_usleep_shim() 内の usleep() が自分自身へマクロ置換され無限自己再帰
 * する)。Bridge/sasi_io_cache.c:40 の SASI_IO_CACHE_NO_MACROS と同じ約束事。 */
#define FDD_TIMING_SHIM_NO_MACROS
#include "fdd_timing_shim.h"
#include "opm_shadow.h"   /* P479: OPM register shadow written by m68000_bridge.c */
#include "mercury_opn_shadow.h"   /* P491: Mercury OPN register shadow written by m68000_bridge.c */

/* P509 (D-48): m68000_bridge.c 定義の診断プローブ。本サイクルの変更を
 * Bridge の 2 ファイルに閉じるため、宣言をヘッダではなくここに置く。
 * 真因確定後のプローブ整理サイクルで撤去する。 */
void p509_fdc_mirror_reset(void);   /* FDC_Init() 直後にコマンドミラーを巻き戻す */
void p509_fdc_hist_dump(void);      /* フレーム末尾から毎フレーム(出力は300フレーム毎) */

#define P29_RTE_STUB_ADDR  0x0FFF00U  /* RTEスタブ配置アドレス（1MB RAM末端・IPLコピー範囲外） */

/* P221b: Config.XVIMode is now DERIVED from the configured clock (see
 * p270_derive_xvimode below), replacing the P146 hard-coded XVIMode=3. The old
 * P221_FORCE_XVIMODE0 measurement toggle is retired — the 10MHz path is now
 * reachable through the normal clock=10 route, and the reachability check it
 * guarded is carried by the P221B_PROBE (EmulatorBridge.h). */
/* P270: the P221b derivation is now a per-clock-value lookup table, not a
 * threshold — the threshold could not correctly express clock-up MOD kits
 * (RedZone 24MHz stays 16MHz-class 0xFE; EXPERT 17MHz stays 10MHz-class 0xFF)
 * whose modded speed is not monotonic with their machine ID class. See the
 * p270_derive_xvimode block below for the full rationale and references. */

/* P51-A — Shrink P16-FIX IPL→MEM byte-swap shadow to skip the vector-table
 * area $0008..$07BF. Set P51A_ENABLE=0 to ablate (restore legacy full-range
 * shadow). Set P51A_VEC23_SAFETY=0 to ablate the vec#2/#3 RTE-stub
 * pre-install (without changing the shrink behavior). The macros live here
 * (near P29_RTE_STUB_ADDR) so the single-edit flip is discoverable.
 * See /tmp/mx68k_P51A_plan.md §2 and §5.2 for rationale. */
#define P51A_ENABLE              1
#define P51A_VEC23_SAFETY        1
#define P51A_VECTOR_SKIP_BEGIN   0x0008
#define P51A_VECTOR_SKIP_END     0x07C0  /* first re-shadowed word pair */

/* P49-B Track C (★) — Compile-time switch for the FDD-INT-PULSE stub.
 *
 * Default OFF (dormant). The stub at mx68k_fdd_insert() ORs bit6 (FDD INT,
 * Spec §3.1) of IOC_IntStat as a one-shot at disk-insert. Track A's tracer
 * results (see Bridge/m68000_bridge.c P49B_TRACE_PC_LO/HI block) will
 * indicate whether IPL is actually polling $E9C001 bit6. If so, this gate
 * is flipped to 1 in a follow-up patch.
 *
 * Requirements Review §6.1 / §6.2 / §6.3 / §4.3 reflected:
 *   §6.1 — DEFAULT OFF; flip ON only after Track A confirms bit6 polling.
 *   §6.2 — bit6 has NO auto-clear path in Core/px68k/x68k/ioc.c
 *          (only bit5 PRT auto-clears on read, per ioc.c:70-86). Therefore
 *          when this gate is enabled, a bit6-CLEAR path must ALSO be added
 *          in Bridge/ (NOT Core/) — otherwise the OR would cause repeated
 *          vec=$61 ACK loops (IRQ storm). This maintenance debt MUST be
 *          paired with the enable.
 *   §6.3 — Spec §5.4 hints IPL may poll bit7 (FDC INT) instead of bit6
 *          (FDD INT). The Decision Gate (Plan §8) adds a row C2 for the
 *          bit7 variant; this stub covers only bit6, so a sibling stub
 *          would be added for bit7 in the same follow-up patch if needed.
 *   §4.3 — `IOC_IntStat |= 0x40u;` is EMULATOR-INTERNAL state manipulation
 *          (writes directly to the static byte exported by ioc.c). It does
 *          NOT go through the CPU bus / IOC_Write(), so it does not
 *          contradict Spec §3.1's "bit6 is read-only on the CPU side"
 *          property — that property concerns CPU-bus writes only.
 *
 * Code Review C-10 (advisory): macro lives at file scope (here) so the
 * future gate-flip is a one-edit operation at a discoverable location.
 * Code Review C-11: enable-side patch will be ≥2 lines (gate + bit6-
 * clear path), not "≤2" as Plan §4.1 originally claimed. */
#define P49B_FDD_INT_PULSE_ENABLE 0

/* P68-FDC-FIX: hard-reset-time FDD SetDelay drain (drive-ready race fix).
 * 1 = enabled (default, bug fix). 0 = revert to pre-P68 behaviour.
 * See mx68k_reset_hard() end-of-function block and /tmp/mx68k_P68_plan.md. */
#define P68_FDC_FIX_ENABLE 1

/* P135 forward declarations — P135_ENABLE probes use these in mx68k_init() before
 * their full definitions at line ~1017+. */
extern int g_mx68k_frame_num;
extern uint32_t g_p169_tvram_wb, g_p169_tvram_ww;   /* P169 */
#if P135_ENABLE
static void p135_host_latch(unsigned char which, int frame,
                            unsigned short before, unsigned short after);
static inline unsigned short p135_mem1ff6(void);
#endif

static FILE* debug_log_file = NULL;
static char DEBUG_LOG_PATH[1024] = "";
static pthread_mutex_t debug_log_mutex = PTHREAD_MUTEX_INITIALIZER;
/* P529: debug_log() の総呼出し回数。[P424-HOTPATH] が毎フレーム
 * mx68k_diag_get_and_reset_log_calls() で読み出し＋ゼロクリアするため、
 * ログ行に出るのは「そのフレーム中の debug_log() 呼出し回数」。
 * debug_log() は CoreAudio リアルタイムスレッドからは呼ばれない設計原則
 * (P464、本ファイル §H5) のため単純な非アトミック加算で足りる。 */
static uint64_t s_debug_log_call_count = 0;
static void ensure_app_support_dir(void);
/* P505 (D-46): SRAM を IPL-ROM 内蔵デフォルトテーブルからシードする。
 * 本体は mx68k_sram_clear() の直前に定義(mx68k_init() と mx68k_sram_clear()
 * の双方から呼ばれるため前方宣言する)。 */
static void sram_seed_defaults(void);

/* P53 — file-static idempotency / one-shot flags.
 * - s_p53_shutdown_done: gates the body of mx68k_shutdown. Reset in
 *   mx68k_init so re-init scenarios work. Single-threaded read/write
 *   (all sites run on the main thread); debug_log mutex protects the
 *   surrounding log writes.
 * - s_p53_atexit_registered: ensures atexit(mx68k_atexit_summary) is
 *   registered exactly once per process. NOT reset on re-init because
 *   libc's atexit list already holds the entry.
 * See /tmp/mx68k_P53_plan.md §3.1 / §3.2 / Edit B1. */
static int s_p53_shutdown_done = 0;
static int s_p53_atexit_registered = 0;

static void debug_log_init(void) {
#ifdef DEBUG
    if (debug_log_file == NULL) {
        ensure_app_support_dir();
        pthread_mutex_lock(&debug_log_mutex);
        if (debug_log_file == NULL) {
            if (DEBUG_LOG_PATH[0] == '\0') {
                const char* home = getenv("HOME");
                if (home) {
                    snprintf(DEBUG_LOG_PATH, sizeof(DEBUG_LOG_PATH),
                             "%s/Library/Application Support/MX68K/debug.log", home);
                }
            }
            debug_log_file = fopen(DEBUG_LOG_PATH, "a");
            if (!debug_log_file) {
                fprintf(stderr, "[MX68K] Failed to open debug log: %s\n", DEBUG_LOG_PATH);
            }
        }
        pthread_mutex_unlock(&debug_log_mutex);
    }
#endif
}

void debug_log(const char* fmt, ...) {
#ifdef DEBUG
    s_debug_log_call_count++;   /* P529 */
    debug_log_init();

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (debug_log_file) {
        pthread_mutex_lock(&debug_log_mutex);
        va_list args2;
        va_start(args2, fmt);
        vfprintf(debug_log_file, fmt, args2);
        va_end(args2);
        fflush(debug_log_file);
        pthread_mutex_unlock(&debug_log_mutex);
    }
#else
    (void)fmt;
#endif
}

/* P529: debug_log() 呼出し回数を読み出して同時にゼロクリアする
 * (get-and-reset 方式)。呼出し側は [P424-HOTPATH] の 1 箇所のみ
 * (m68000_bridge.c、フレーム毎に 1 回) — Swift からは呼ばないため
 * EmulatorBridge.h へは宣言しない (mx68k_diag_mfp_int と同じ
 * Bridge 内部専用関数の慣習)。 */
uint64_t mx68k_diag_get_and_reset_log_calls(void) {
    uint64_t n = s_debug_log_call_count;
    s_debug_log_call_count = 0;
    return n;
}

#if P533_ENABLE
/* P533 [P533-XSNAP]: D-40 診断用の per-frame スナップショット。
 * mx68k_render_begin() で "_b"(フレーム頭)、mx68k_render_end() で "_e"
 * (フレーム末)を採取し、render_end で 1 行に併記して出力する。
 * 宣言・参照とも同一の #if P533_ENABLE ガード配下(P467/P468 ファミリと
 * 同じ規約)。全て既存グローバルの読み取り結果の保存のみ。 */
static uint32_t s_p533_chsize_calls = 0;  /* 当該フレーム中の WinDraw_ChangeSize() 呼出し回数 */
static int32_t  s_p533_tdx_b = 0;         /* TextDotX  (frame begin) */
static int32_t  s_p533_tdx_e = 0;         /* TextDotX  (frame end)   */
static int32_t  s_p533_tdy_b = 0;         /* TextDotY  (frame begin) */
static int32_t  s_p533_tdy_e = 0;         /* TextDotY  (frame end)   */
static uint32_t s_p533_palhash_b = 0;     /* Pal_Regs[1024] の FNV ハッシュ (frame begin) */
static uint32_t s_p533_palhash_e = 0;     /* 同 (frame end) */
static uint8_t  s_p533_sp1_b = 0;         /* SysPort[1] (frame begin) */
static uint8_t  s_p533_sp1_e = 0;         /* SysPort[1] (frame end)   */
static uint8_t  s_p533_cv_b  = 0;         /* Contrast_Value (frame begin) */
static uint8_t  s_p533_cv_e  = 0;         /* Contrast_Value (frame end)   */
#endif

#if P570_ENABLE
/* P570 [P570-GEOM]: D-51 診断用の per-frame 出力ジオメトリ・スナップショット。
 * mx68k_render_begin() で "_b"(フレーム頭)を採取し、mx68k_render_end() で
 * "_e"(フレーム末)を採取して 1 行に併記して出力する。
 * 派生値(TextDotX/TextDotY)の計算元である生の CRTC レジスタを同じ行へ並べ、
 * 「非標準ジオメトリが画面モード切替(R20変化)なのか表示ウィンドウ縮小
 * (R02/R03・R06/R07 のみ変化)なのか」を標本自身から事後判定できるようにする。
 * 宣言・参照とも同一の #if P570_ENABLE ガード配下(P533/P534/P467/P468
 * ファミリと同じ規約)。全て既存グローバルの読み取り結果の保存のみで、
 * エミュレーション状態への書込みは無い。
 * ★配置: memory/feedback_instrumentation_layout_adjacency_corruption の教訓に
 * より、新規 static は既存プローブ static 群(P533 直後)へまとめ、
 * s_compose_fb / s_row_drawn などの大配列の隣接位置を変えない。すべてスカラ
 * static であり、この位置に大配列は無い。 */
static int32_t  s_p570_tdx_b  = 0;        /* TextDotX      (frame begin) */
static int32_t  s_p570_tdy_b  = 0;        /* TextDotY      (frame begin) */
static uint16_t s_p570_hs_b   = 0;        /* CRTC_HSTART   (frame begin) */
static uint16_t s_p570_he_b   = 0;        /* CRTC_HEND     (frame begin) */
static uint16_t s_p570_vs_b   = 0;        /* CRTC_VSTART   (frame begin) */
static uint16_t s_p570_ve_b   = 0;        /* CRTC_VEND     (frame begin) */
/* P571: 上位/下位の取り違えを訂正。Core: crtc.c:342 が CRTC_Regs[0x28] &= 0x07
 * (=3ビット=メモリモード D10-D08)、:346 が CRTC_Regs[0x29] &= 0x1f(=D04-D00)、
 * :348 が VID_MODE = !!(CRTC_Regs[0x29] & 0x10)(=水平周波数 = テクニカルデータ
 * ブック 印刷p.28 の下位バイト D04)としているため、[0x28]=上位・[0x29]=下位が正。 */
static uint8_t  s_p570_r28_b  = 0;        /* CRTC_Regs[0x28] = R20上位 (frame begin) */
static uint8_t  s_p570_r29_b  = 0;        /* CRTC_Regs[0x29] = R20下位 (frame begin) */
static uint8_t  s_p570_vstep_b = 0;       /* CRTC_VStep    (frame begin) */
/* P595 (D-55): geo_mode の分岐条件そのもの(R00/R04 の実測値)をフレーム頭で
 * 採取して同一行へ併記する。派生値(hscale/vscale/offxf/offyf/geo_mode)を
 * 読者が手計算で再現・反証できるようにするための生値。 */
static int32_t  s_p570_r00_b  = 0;        /* R00 = 水平トータル (frame begin) */
static int32_t  s_p570_r04_b  = 0;        /* R04 = 垂直トータル (frame begin) */
#endif

#if P534_ENABLE
/* P534 [P534-GVWCOL]: D-40症状1 の残る1問(MX が GVRAM 列496-511 への
 * 掃除書込みを受け取っているか)を測る累積カウンタ。加算は GVRAM_Write()
 * ラッパ(下記、P467 ヘルパ近傍)、出力は mx68k_render_end() で毎フレーム
 * 1行。宣言・参照とも同一の #if P534_ENABLE ガード配下(P533/P467/P468
 * ファミリと同じ規約)。すべてスカラ static であり、この位置(P533 の
 * スカラ static ブロック直後)には大配列が無いことを Fix Plan §グローバル
 * 配置安全性 Q3 で確認済み。 */
static unsigned long s_p534_calls = 0;          /* GVRAM_Write() 総呼出し数(累積) */
static unsigned long s_p534_wr256 = 0;          /* うち256色分岐で実際に格納された数(累積) */
static unsigned long s_p534_col496_zero = 0;    /* 列496-511 へ値0を書いた数(累積)= 掃除 */
static unsigned long s_p534_col496_nonzero = 0; /* 列496-511 へ非0を書いた数(累積)= 描画 */
static unsigned long s_p534_fcfire = 0;         /* GVRAM_FastClear() 発火数(累積) */
#endif

/* P533: WinDraw_ChangeSize()(Bridge/windraw.c の no-op スタブ)から
 * 呼ばれる計測アクセサ。呼出し側は P533_ENABLE を見ずに無条件で呼ぶため、
 * 関数の定義自体は常に存在させ、中身だけをガードする
 * (P526 の mx68k_diag_set_last_frame_ms と同じ設計 —— 条件コンパイル
 *  ブロックの中に関数ごと置くとリンクエラーになる)。 */
void mx68k_diag_note_chsize_call(void) {
#if P533_ENABLE
    s_p533_chsize_calls++;
#endif
}

#include "../Core/px68k/x68k/x68kmemory.h"
#include "../Core/px68k/x68k/mfp.h"
#include "../Core/px68k/x68k/irqh.h"
#include "../Core/px68k/keyboard.h"
#include "../Core/px68k/x68k/fdd.h"
#include "../Core/px68k/x68k/fdc.h"
#include "../Core/px68k/x68k/gvram.h"
#include "../Core/px68k/x68k/tvram.h"
#include "../Core/px68k/x68k/crtc.h"
#include "../Core/px68k/x68k/palette.h"
#include "../Core/px68k/x68k/bg.h"
#include "../Core/px68k/x68k/ioc.h"
#include "../Core/px68k/x68k/sysport.h"   /* P198: SysPort[] for state save/load */
#include "../Core/px68k/x68k/scc.h"
#include "../Core/px68k/x68k/mouse.h"  /* P195: Mouse_SetData() のプロトタイプ照合用 */
#include "../Core/px68k/x68k/ppi.h"
#include "../Core/px68k/x68k/sasi.h"
#include "../Core/px68k/x68k/scsi.h"
#include "../Core/px68k/x68k/dmac.h"
extern void mx68k_fdd_note_rw(void);   /* P181: defined in status_bridge.c */
extern void mx68k_set_membound(int mb); /* P220b: defined in m68000_bridge.c */
#include "../Core/px68k/x68k/adpcm.h"
#include "../Core/px68k/x68k/mercury.h"
#include "../Core/px68k/x68k/midi.h"   /* P488: MIDI_Init/_Cleanup/_Timer/_DelayOut の宣言元 */
/* P490: MIDI デバイス名一覧 menu_items[8](出力)/[9](入力)へアクセスするため。
 * Core 側ソースは project.pbxproj の COMPILER_FLAGS で本ヘッダを強制 include して
 * いるが、EmulatorBridge.c の PBXBuildFile には settings 句が無く対象外なので
 * 明示 include が必要(Code Review 独立スキャンで確認済み)。 */
#include "px68k_compat.h"
/* P490: MIDI_MODULE は midi.c の非 static グローバルだが midi.h に extern 宣言が
 * 無い([P490-MIDIRST] プローブで実際に設定された音源種別を読むために必要)。 */
extern uint8_t MIDI_MODULE;
#include "../Core/px68k/fmgen/fmg_wrap.h"
#include "../Core/px68k/x68k/m68000.h"
/* P740: Core 同梱の MC68000 逆アセンブラ(Debabelizer)。ビルドには元から
 * 登録されていたが呼び出し元がゼロだった — mx68k_disassemble_line() で配線する。 */
#include "../Core/px68k/x68k/d68k.h"
#include "../Core/px68k/x68k/sram.h"
#include "../Core/px68k/x68k/prop.h"
#include "../Core/c68k/c68k.h"
#include "../Core/px68k/winx68k.h"
#include "../Core/px68k/x68k/windraw.h"
#include "sram_ext_bridge.h"   /* P493: 内蔵 SRAM 64KB 化(上位 48KB を Bridge が提供) */
#include "windrv_bridge.h"     /* P642: Windrv(Mac フォルダのホスト共有) */

// Prototypes from m68000_bridge.c (Core/px68k/m68000/m68000.h)
extern void m68000_init(void);
extern void m68000_reset(void);
extern int32_t m68000_execute(int32_t cycles);
extern void m68000_set_irq_line(int32_t irqline);
extern void m68000_reset_addr_err_count(void);
extern void m68000_reset_pcguard_count(void);  // P14-FIX
extern void m68000_reset_p47bb_counters(void); /* P47-B-β counters reset */
extern void m68000_reset_p47d_counters(void);  /* P47-D session-scope DIAG-F/G/H/I reset */
extern void mx68k_scsi_irq_reset(void);        /* P252: clear internal-SCSI level-1 pending on hard reset */
extern void mx68k_probe_pc_cache_invalidate(void); /* P82-X-Q frame-entry cache invalidate */
extern void p82xq_emit_verdict(void);              /* P82-X-Q probe verdict one-shot */
extern void p82xr_emit_verdict(uint32_t frame);    /* P82-X-R probe verdict one-shot */
extern void p82xr_tick(void);                      /* P82-X-R per-frame CP-R-4/CP-R-5 driver */
extern void p82xt_tick(void);                      /* P82-X-T per-frame CP-T-2/T-3/T-5/T-6 driver */
extern void p82xu_tick(void);                      /* P82-X-U per-frame CP-U-1/U-3/U-5 driver */
extern void mx68k_diag_mfp_int(int32_t irq, const char* src); /* P47-D-DIAG-G wrapper */
extern uint32_t m68000_get_reg(int32_t regnum);   // P28-FIX
extern void m68000_set_reg(int32_t regnum, uint32_t val); // P28-FIX

/* P82-X-G: FDC/IOC register-access trace probe — VERDICT one-shot dump.
 * Defined in m68000_bridge.c (file-scope static rings); called once from the
 * mx68k_run_frame() per-frame route when frame>=95 is first reached. The
 * cross-file declaration is kept internal to Bridge/ (NOT in EmulatorBridge.h,
 * which is the clean Swift<->C public API contract — diagnostic-only internals
 * must not be exposed there). P82XG_ENABLE here must mirror the authoritative
 * definition in m68000_bridge.c; if that probe is disabled, set this to 0. */
#define P82XG_ENABLE 0
#if P82XG_ENABLE
extern void p82xg_emit_verdict(void);
#endif
/* P82-X-H: FDC ISR (vec 0x60) execution-path trace probe — VERDICT one-shot
 * dump. Defined in m68000_bridge.c (file-scope static rings); called once from
 * the mx68k_run_frame() per-frame route when frame>=96 is first reached. The
 * cross-file declaration is kept internal to Bridge/ (NOT in EmulatorBridge.h,
 * which is the clean Swift<->C public API contract — diagnostic-only internals
 * must not be exposed there). P82XH_ENABLE is the single authoritative define
 * in EmulatorBridge.h (this TU includes it); no mirror define is needed. */
#if P82XH_ENABLE
extern void p82xh_emit_verdict(void);
#endif
/* P45-DIAG: irqh.c defines IRQH_IRQ[] but no header exposes it */
extern uint8_t IRQH_IRQ[8];

/* P460 (D-6診断): ADPCM 出力バッファのゼロ→非ゼロ遷移(段差)を実測する
 * 読み取り専用プローブの有効化スイッチ。挙動には一切影響しない。
 * P82XG_ENABLE の precedent に倣い、この定義は本 .c 内に置く
 * (EmulatorBridge.h は Swift<->C のクリーンな公開 API 契約であり、
 * 診断専用の内部定義を置いてはならない)。 */
#define P460_ADPCM_STEP_ENABLE 1

/* P462 (D-6解決): ADPCM デクリック・エンベロープの定数。
 * 診断プローブと異なり本体は無条件で有効(#if ガード無し)だが、定数の置き場所は
 * P460_ADPCM_STEP_ENABLE と同じく本 .c 内とする
 * (EmulatorBridge.h は Swift<->C のクリーンな公開 API 契約であり、
 * Bridge 内部専用の定義を置いてはならない)。 */
#define P462_ENV_UNITY 32768L
#define P462_ATTACK_SAMPLES 64
#define P463_SILENCE_THRESHOLD_SAMPLES 128
#define P463_DECAY_STEP 1

/* P464 (D-6再調査): オーディオ・リングバッファの充填率 / アンダーラン(ゼロ埋め
 * スプライス) / オーバーラン(書込みドロップ) を1本のログ行で同時に可視化する
 * 読み取り専用プローブの有効化スイッチ。挙動には一切影響しない
 * (音声データ tmp_mix の内容は変更しない)。
 * P460_ADPCM_STEP_ENABLE の precedent に倣い、この定義は本 .c 内に置く
 * (EmulatorBridge.h は Swift<->C のクリーンな公開 API 契約であり、
 * 診断専用の内部定義を置いてはならない)。
 * P464_RINGFILL_WINDOW_FRAMES はハードウェア一次資料に基づかない計測ケイデンスの
 * 設計値(64フレーム ≒ 1.0〜1.15秒 @ vhz=55.46/61.46)。値が大小しても判定ロジック
 * 自体は依存せず、ログの粒度/行数だけが変わる。 */
#define P464_RINGFILL_ENABLE 1
#define P464_RINGFILL_WINDOW_FRAMES 64

/* ===========================================================================
 * P48 — FDC ready / DMA0 stall fix block (test#56 stall at PC=0x001fcc)
 * ---------------------------------------------------------------------------
 * Root cause: fdd.SetDelay[0] is drained 3->0 over 3 frames by the every-
 * frame FDD_SetFDInt() call (line ~1049), but the IRQ1 (FDD insert) dispatch
 * is gated by (IOC_IntStat & 2). If IPL2 has not yet enabled bit1 the moment
 * SetDelay hits 0, the IRQ is lost forever; upstream px68k has no re-trigger.
 *
 * Adopted strategy (★ user-approved 案1):
 *   P48-A : Drain SetDelay 3->1 synchronously inside mx68k_fdd_insert() via
 *           TWO successive FDD_SetFDInt() calls; the third 1->0 transition
 *           is then completed naturally at the existing run_frame call site
 *           (line ~1049) on frame=1, which fires IRQH_Int(1,&FDD_Int) via the
 *           regular code path while IOC_IntStat=0x0E (P23-FIX preset) is
 *           still in effect.
 *   P48-C : Hook $E9C001 byte writes; on IOC_IntStat bit1 0->1 rising-edge
 *           with any FDD ready, re-fire IRQH_Int(1, &FDD_Int) as a workaround
 *           for IPL2 toggling FDDI EN after the original IRQ moment.
 *           (Implementation lives in m68000_bridge.c.)
 *   P48-D : One-shot dump of MEM[0x1FC0..0x1FFF] + SSP stack top at frame=50
 *           for forensic visibility if the fix is incomplete.
 *
 * Reset-order dependency:
 *   m68000_reset_p47d_counters() (line ~570) MUST be called AFTER the
 *   IOC_IntStat=0x0E preset (line ~369) inside mx68k_reset_hard() so that
 *   s_p48c_ioc_intstat_prev auto-syncs to the post-preset value (0x0E),
 *   preventing a spurious first-write rising-edge detection.
 *   (See m68000_bridge.c::m68000_reset_p47d_counters() body and C-2 note.)
 * =========================================================================== */
/* P48-A diagnostic counter: number of mx68k_fdd_insert drain events (session
 * scope; not reset on hard reset — it counts per-mount, not per-reset). */
static int s_p48a_drain_count = 0;

// Prototype from sasi_bridge.c
extern void sasi_bridge_install(void);

/* P674: prototypes from scsi_spc_bridge.cpp (extern "C"). Declared locally here
 * rather than in EmulatorBridge.h — that header is read from C and Swift and must
 * stay free of the ported SPC's internals; same pattern as scsi_real_set_disk_path,
 * which scsi_in_bridge.c / scsi_ext_bridge.c declare locally (P510). */
extern int  scsi_real_mo_open(const char* path);
extern int  scsi_real_mo_eject(int force);
extern int  scsi_real_cd_open(const char* path);   /* P676 */
extern int  scsi_real_cd_eject(int force);         /* P676 */

/* P502 (D-9): prototype from sasi_io_cache.c. Declared here (rather than by
 * including Bridge/sasi_io_cache.h) because that header also redefines
 * File_Open/Seek/Read/Write/Close as macros for its sasi.c force-include role;
 * this follows the existing sasi_bridge_install() pattern above. */
extern void sasi_io_cache_invalidate_all(void);

/* P47-C: Missing px68k Init/Timer prototypes (rtc.h/ppi.h are not included via current header chain) */
extern void RTC_Init(void);
extern int  RTC_Timer(int32_t clock);
extern uint8_t RTC_Regs[2][16];   /* Core/px68k/x68k/rtc.c: BANK0/1 各16レジスタ。
                                   * P653: RTC_Regs[1][0] = CLKOUTセレクト(下位3bit)。 */
extern void PPI_Init(void);

// ---- Bridge-side runtime state (px68k Config_t lacks these fields) ----
static volatile int g_pending_hard_reset = 0;
static volatile int g_pending_soft_reset = 0;   /* P186: soft reset をフレーム境界で消費(run_frame とのレース回避) */
static volatile int g_pending_sram_clear = 0;   /* P454: SRAM ゼロクリアをフレーム境界で消費(run_frame とのレース回避) */
/* P502 (D-9): SASI fd キャッシュの無効化をフレーム境界で消費する。イメージの
 * insert/eject は Swift のメインスレッドから来るが、キャッシュ中の fd を
 * 使うのはエミュレーションスレッド(sasi.c の I/O 経路)なので、close() は
 * 必ずエミュレーションスレッド側で走らせる。型は既存の同型フラグ
 * (g_pending_hard_reset / g_pending_sram_clear)と揃えて volatile int。 */
static volatile int g_pending_sasi_cache_invalidate = 0;
/* P198: state save/load scheduled at the run_frame boundary (mirrors the
 * g_pending_hard_reset precedent). The public mx68k_save_state/mx68k_load_state
 * copy the path + set the flag + return 0 (queued); do_save_state/do_load_state
 * run on the emulation thread when the flag is consumed. */
static _Atomic int  g_pending_save = 0;
static _Atomic int  g_pending_load = 0;
/* P481 (D-42): completion notification for the queued save/load. mx68k_save_state/
 * mx68k_load_state only report "queued" (rc=0); the real result used to reach
 * debug_log only, so Swift showed a success toast for an operation that had not
 * run yet — and a rejected (e.g. older-version) state file failed silently.
 * The emulation thread stores the result here and bumps g_state_op_seq; Swift
 * polls the seq and reads rc/kind once it changes. */
static _Atomic int          g_state_op_rc   = 0;   /* rc of the most recent completed save/load */
static _Atomic int          g_state_op_kind = 0;   /* 0=none, 1=save, 2=load */
static _Atomic unsigned int g_state_op_seq  = 0;   /* monotonically increasing, one per completion */
static char         g_pending_state_path[1024] = {0};
static int          do_save_state(const char* path);
static int          do_load_state(const char* path);
static int  g_machine_type    = 0;
/* P268: 実際に配線を確定した機種(scsi_in_bridge_install 実行時に記録)。
 * g_machine_type は設定「適用」で即時書き換わるが、実配線は次の init/hard_reset
 * まで旧機種のまま(P238)。HDD/SCSI insert バックストップはこの確定値で判定し、
 * 未リセットの pending 機種で正当な操作を誤って拒否しないようにする。 */
static int  g_wired_machine_type = 0;
int g_scsi_ext_board_wired = 0;  /* P506: 配線確定した外付けSCSIボード装着状態
                                  * (XM6 scsi.type==1相当)。設定値
                                  * g_scsi_ext_board_installedとは別に、
                                  * mx68k_reset_hard()でのみ確定させる
                                  * (g_wired_machine_typeと同じ方式)。 */
/* P483: Mercury Unit(MK-MU1 / $ECC000)の装着状態。
 * g_mercury_enabled = 設定値(Swift の pushConfig から即時 push される)。
 * g_mercury_installed = 配線確定値(init / ハードリセットでのみ確定)。
 * m68000_bridge.c のバスエラーゲートが extern 参照するため static にしない
 * (g_mx68k_frame_num と同じ様式)。 */
static bool g_mercury_enabled   = false;
int         g_mercury_installed = 0;
/* P686 (D-70): 外付け FDD ユニット(ドライブ 2/3)の装着状態。
 * g_ext_fdd_enabled = 設定値(Swift の pushConfig から即時 push される)。
 * g_ext_fdd_wired   = 配線確定値(init / ハードリセットでのみ確定)。
 * 診断プローブ [P686-FDCDRV](m68000_bridge.c)が extern 参照するため
 * wired 側は static にしない(g_scsi_ext_board_wired と同じ様式)。 */
static bool g_ext_fdd_enabled = false;
int         g_ext_fdd_wired   = 0;
/* P634 (D-43): Mercury の LR クロックを観測経路のみ自走化する機構
 * (実体・状態変数はすべて m68000_bridge.c 側 — 読出フックと同一翻訳単位に
 *  まとめ、状態定義を分散させない。feedback_instrumentation_layout_adjacency_corruption
 *  の教訓により、既存ヘッダへは追加せずここで extern 宣言で受ける。
 *  既存の m68000_bridge.c:305 `extern uint8_t Mcry_LRTiming;` と同じ方針)。 */
extern void p634_lrck_advance(int32_t clock);
extern void p634_lrck_reset(void);
/* P636 (サウンドモニタ): Mercury PCM の活動検出ポーリング。実体・状態変数は
 * P634 と同じ理由で m68000_bridge.c 側にまとめてあり、ここは extern 宣言で受ける。 */
extern void p636_mercury_pcm_poll_samples(void);
/* P640 (D-67): P637 切り分けプローブの要約行をセッション終了時にフラッシュする。
 * 実体・状態変数は P634 / P636 と同じ理由で m68000_bridge.c 側にまとめてあり、
 * ここは extern 宣言で受ける。診断専用で mx68k_shutdown() の 1 箇所からしか
 * 呼ばれないため EmulatorBridge.h へは追加しない —— 隣接する
 * mx68k_p483_dump_mcry_counters がヘッダ宣言なのは、あちらが複数箇所から
 * 呼ばれる公開 API のためで、規約の不統一ではなく公開範囲に応じた使い分け。 */
extern void p637_ch3_dump_summary(const char* tag);
/* P488: MIDI ボード(CZ-6BM1 / YM3802 / $EAE000)の装着状態。Mercury と同型に
 * 「設定値(g_midi_enabled)」と「配線確定値(g_midi_installed)」を分離する。
 * 配線は init / ハードリセットでのみ確定する。
 * ★Mercury Unit と割込みレベル 4 を共有する(midi.c:172,187,202・
 *   midi_darwin.c:59 で 4 がハードコード、IRQH_CallBack[] はレベルあたり
 *   1 コールバックのみ)ため、両方同時装着は不可 — 相互排他ラッチで
 *   Mercury 側を優先し MIDI を強制 OFF にする。 */
static bool g_midi_enabled   = false;
int         g_midi_installed = 0;
/* P490 (Stage 2): MIDI の設定値。いずれも「設定値」であり、リセット送出可否/音源種別/
 * デバイス選択の配線確定は init / ハードリセットのラッチで行う(g_midi_enabled と同型)。
 * 送信遅延だけはフレームループが毎回読むためライブ反映される。 */
static bool g_midi_reset_enabled     = true;   /* 既定 ON */
static int  g_midi_reset_type        = 0;      /* 0=LA,1=GM,2=GS,3=XG(midi.c:82-84 MIDI_ResetType[] と同順) */
/* P490: MIDI_DelayOut() へ渡す遅延(ms)。0 = 現在キュー内の全メッセージを即時送出
 * (midi.c:628-638 の (t - DelayBuf[].time) >= delay が delay=0 なら常に真)。
 * P488 Stage 1 の既定値 0 を維持する(挙動変更なし)。上限 1000ms は
 * DelayBuf[4096] ÷ (31250bps/10bit = 3125B/s) ≒ 1.31 秒からの安全マージン込み。 */
static int  g_midi_delay_ms          = 0;
static int  g_midi_out_device_index  = 0;      /* MIDI_Init() の midOutChg(0,0) 既定と同じ */
static int  g_midi_in_device_index   = 0;      /* MIDI_Init() の midInChg(0) 既定と同じ */
/* P493: 内蔵 SRAM 64KB 化(実機改造相当)の「設定値」。Mercury/MIDI と同型で、
 * 配線(MemRead/WriteTable[0x6A-0x6F] の差し替え)は init / ハードリセットで確定する。 */
static bool g_sram_64k_enabled       = false;
/* P642: Windrv(Mac フォルダのホスト共有)。Mercury/MIDI/SRAM64K と同型で
 * 「設定値(g_windrv_enabled / g_windrv_host_path)」と「配線確定値
 * (g_windrv_installed)」を分離する。配線は init / ハードリセットの
 * windrv_init() でのみ確定する。
 * ★Mercury/MIDI の設定値が static なのに対しこの 2 つが非 static なのは、
 *   実装本体が別翻訳単位(Bridge/windrv_bridge.c)にあり、そこから読む必要が
 *   あるため(宣言は windrv_bridge.h に集約、定義はここ 1 箇所のみ)。 */
bool g_windrv_enabled = false;
char g_windrv_host_path[WINDRV_HOST_PATH_MAX] = {0};
int  g_windrv_installed = 0;
/* P647: Windrv 書込み許可。**共有有効トグルとは独立**の第 2 のトグルで、
 * 既定 false。読み取り共有を有効にしただけでは書込みは一切できない。
 * ★g_windrv_installed と同じく「設定値」と「配線確定値」を分離する。
 *   配線確定値のほうを全ての書込み系コマンドのゲートに使う理由:
 *   設定を動作中に OFF へ切り替えても、既に "r+b" で開かれているホスト
 *   ストリームは残るため、ゲート値だけ即時反映すると「開いているのに
 *   書けない」という中途半端な状態が生まれる。ハードリセット(⌘R)で
 *   windrv_init() が全ハンドルを閉じる、そのタイミングでのみラッチする。 */
bool g_windrv_write_enabled = false;   /* 設定値 */
int  g_windrv_write_wired   = 0;       /* 配線確定値(既定 = 書込み不可) */
/* P490: menu_items[8]/[9] の 1 次元目の添字と、CoreMIDI 連携層がリスト格納に
 * 使う最大数(P633 以降は Bridge/midi_coremidi.c の P633_MIDI_DEV_MAX)。 */
#define MIDI_OUTDEV_ROW  8
#define MIDI_INDEV_ROW   9
#define MIDI_DEV_MAX     8
/* P633: P490 の SIGBUS 回避コード(Core 側 midi_darwin.c の read-only な
 * packetBuf を指す packetList を、Bridge の書込可能バッファへ付け替える)は
 * ここにあったが、CoreMIDI 連携層そのものを Bridge/midi_coremidi.c へ移設し、
 * 新実装が最初から書込可能な自前バッファを所有する設計にしたため不要になり
 * 削除した(midi_darwin.c は Compile Sources から除外済み・ファイルは無改変)。 */
/* P483: Mcry_SetVolume() に渡す音量(0-16)。本家 px68k x11/prop.c:245,686 および
 * px68k-libretro libretro/prop.c:202,641 の既定値 MCR_Volume = 13(両実装で一致)。
 * Mcry_VolumeShift = 1.189207115^(16-13) = 1.6818。
 * 既存 ADPCM_SetVolume(15) との相対ゲインは 1.1892/1.6818 = 0.707(≒ -3.0dB)で、
 * 参照実装の PCM_VOL=15 / MCR_VOL=13 の比と無次元で一致する。
 * ★ADPCM 側の音量を 15 以外へ変更する場合は、この比を保つため
 * MCRY_VOL = ADPCM_VOL - 2 とすること(将来の音量 UI 化時の不変条件)。 */
#define MCRY_VOL 13
/* P483: Mcry_Init は path に slash を連結して M288_Init へ渡す(mercury.c:331)。
 * NULL を渡すのは未定義動作。YMF288 のリズム音源 WAV(2608_*.wav)の探索先として
 * CLAUDE.md 記載の BIOS ディレクトリを渡す。末尾スラッシュは付けない
 * (Mcry_Init 側が付ける)。FM 部は本サイクルの対象外なので WAV が無くても
 * 問題ない(opna.cpp:1922-1925 LoadRhythmSample の戻り値は無視される)。
 * ★NULL は決して返さない。 */
static const char* mx68k_mercury_dir(void) {
    static char dir[1024];
    const char* home = getenv("HOME");
    if (home) snprintf(dir, sizeof(dir), "%s/Library/Application Support/MX68K/bios", home);
    else      dir[0] = '\0';     /* NULL は決して返さない */
    return dir;
}

/* P493: sram.dat / sram_ext.dat 等が置かれる Application Support ディレクトリ。
 * 既存コードが各所で組み立てているのと同じパスを、mx68k_mercury_dir() と同じ
 * 様式(NULL は返さない)で 1 か所に集約する。 */
static const char* mx68k_support_dir(void) {
    static char dir[1024];
    const char* home = getenv("HOME");
    if (home) snprintf(dir, sizeof(dir), "%s/Library/Application Support/MX68K", home);
    else      dir[0] = '\0';
    return dir;
}
/* P447 (C2'): SASI HDD(.hdf)パスの「単一の真実源」シャドウ。
 *
 * Config.HDImage[] は SCSI 機を配線したハードリセットで(P268 の機種排他のため)
 * 空にされるが、SASI 機へ戻したときに復元する値がどこにも残っていなかった。
 * pushConfig 経由だけで更新すると、設定画面の Select…/D&D/Eject(P239 の即時反映
 * UX。pushConfig を通らず mx68k_hdd_insert/_eject を直接呼ぶ)と同期せず、
 * 機種切替と無関係な通常の ⌘R でマウント直後のディスクが消える — これは
 * 本サイクルが直そうとしている「片方向にしか書き戻されないグローバル状態」と
 * 同型の欠陥なので、Config.HDImage[] を書く関数自身がシャドウも更新する設計に
 * している(mx68k_hdd_insert / mx68k_hdd_eject / mx68k_set_hdd_path の3経路)。
 *
 * 不変条件: SASI 機である限り Config.HDImage[unit*2] が非空 ⟺
 * s_sasi_hdd_path[unit] が非空。SCSI 機を配線したときのみ前者だけが空になり
 * 後者は保持される(復元元なので絶対にクリアしない)— 意図的な唯一の例外。
 *
 * 要素サイズは Config.HDImage[] の要素サイズ(prop.h:19 の 4096)に合わせる。
 * mx68k_hdd_insert() が受理する最長パスをシャドウが切り詰めないことを
 * コンパイル時に保証する(下の _Static_assert)。 */
static char s_sasi_hdd_path[MX68K_SASI_UNIT_COUNT][4096];
_Static_assert(sizeof(s_sasi_hdd_path[0]) == sizeof(Config.HDImage[0]),
               "P447: SASI HDD shadow element size must match Config.HDImage[] "
               "so a path accepted by mx68k_hdd_insert() is never truncated");
/* P455: 8 が上限である一次拘束をコンパイル時に固定する。Core sasi.c:426 の
 * 在席判定は Config.HDImage[dev*2+1] まで読むので、最大 device の LUN1 側
 * インデックスが HDImage[] の要素数未満であることを要求する。
 * MX68K_SASI_UNIT_COUNT を 9 以上に増やすとここでビルドが止まる(= OOB を
 * 実行時の SIGBUS ではなくコンパイル時に捕まえる)。 */
_Static_assert((MX68K_SASI_UNIT_COUNT - 1) * 2 + 1
                   < (int)(sizeof(Config.HDImage) / sizeof(Config.HDImage[0])),
               "P455: SASI unit count exceeds Config.HDImage[] slots "
               "(Core sasi.c reads HDImage[dev*2+1] during SELECT)");

/* P456: SASI unit 0..7 の在席ビットマスク(bit u = unit u)。
 * 在席の一次判定は mx68k_hdd_is_inserted()(= Config.HDImage[unit*2][0] != '\0')
 * だけであり、この関数はそれを 8 ユニット分 OR するだけでロジックを持たない。
 *
 * ★2 箇所(mx68k_reset_hard の $ED005A 同期 / mx68k_get_status の
 *   hdd_inserted_mask)から呼ぶ共通実装。同じループを 2 度書くと将来ずれるため
 *   一本化した。mx68k_get_status 側は式が literally 同一になるだけで挙動不変。
 * ★定義位置: 呼び出し箇所の一方 mx68k_reset_hard() がファイル前方にあるため、
 *   定義をここ(reset_hard より前)に置く。mx68k_get_status() 側は後方なので
 *   問題ない。 */
static uint8_t mx68k_sasi_unit_mask(void)
{
    uint8_t mask = 0;
    for (int u = 0; u < MX68K_SASI_UNIT_COUNT; u++) {
        if (mx68k_hdd_is_inserted(u)) mask |= (uint8_t)(1u << u);
    }
    return mask;
}

/* P455 (SF-2): SASI HDD 再適用の結果を、8 ユニット分の分母つきで記録する。
 * 「非空が 0 件」の解釈が 1 通りに収束するよう、常に 8 文字のマップを出す
 * (2 本固定の生値だけだと「本当に 0 台」と「unit 2-7 を見ていないだけ」が
 *  区別できない — CLAUDE.md Self-Falsifiability Gate)。
 * SASI 機では shadow_map == hdimg_map が期待値(P447 不変条件)。
 * 派生値 n_shadow/n_hdimg は、その生値である map と各パス文字列を同一標本内に
 * 併記する。判定(正しい/誤り)は出力せず、読み手に委ねる。 */
static void p455_log_hdd_reapply(const char *wired_label, int wired_type)
{
    char shadow_map[MX68K_SASI_UNIT_COUNT + 1];
    char hdimg_map[MX68K_SASI_UNIT_COUNT + 1];
    unsigned n_shadow = 0, n_hdimg = 0;
    for (int u = 0; u < MX68K_SASI_UNIT_COUNT; u++) {
        int s = (s_sasi_hdd_path[u][0] != '\0');
        int h = (Config.HDImage[u * 2][0] != '\0');
        shadow_map[u] = s ? '1' : '0';
        hdimg_map[u]  = h ? '1' : '0';
        n_shadow += (unsigned)s;
        n_hdimg  += (unsigned)h;
    }
    shadow_map[MX68K_SASI_UNIT_COUNT] = '\0';
    hdimg_map[MX68K_SASI_UNIT_COUNT]  = '\0';
    debug_log("[P447] hdd reapply: wired=%s(%d) shadow_map=%s hdimg_map=%s "
              "n_shadow=%u/%d n_hdimg=%u/%d\n",
              wired_label, wired_type, shadow_map, hdimg_map,
              n_shadow, MX68K_SASI_UNIT_COUNT, n_hdimg, MX68K_SASI_UNIT_COUNT);
    for (int u = 0; u < MX68K_SASI_UNIT_COUNT; u++) {
        if (s_sasi_hdd_path[u][0] == '\0' && Config.HDImage[u * 2][0] == '\0') continue;
        debug_log("[P447]   unit=%d idx=%d shadow='%s' hdimg='%s'\n",
                  u, u * 2, s_sasi_hdd_path[u], Config.HDImage[u * 2]);
    }
}
/* P450: メモリスイッチ自動更新(XM6「メモリスイッチ自動更新」相当)。
 * 既定 true = XM6 の AutoMemSw 既定(mfc/mfc_cfg.cpp:207,265,294)と一致。
 * setter/getter は下方の「---- settings ----」節にあるが、mx68k_reset_hard() から
 * 参照するため実体はここ(reset_hard より前)に置く。 */
static bool g_memsw_auto_update = true;
/* P450: 外付 CZ-6BS1 SCSI ボードが「装着」設定か(Swift の scsiMode=="external")。
 * XM6 の Memory::GetMemType()==SCSIExt に相当する **構成情報**であり、ディスク
 * 在席にも ROM ロード有無にも依存しない。既定 false。
 * ★起動順序の確認(P450 Requirements Review の申し送り): Swift の
 * startEmulation() は pushConfig() を mx68k_init() より前に呼ぶ
 * (EmulatorViewModel.swift: pushConfig(config) → mx68k_init())。
 * mx68k_reset_hard() の入口は init 内・run_frame の pending reset・ステート復元の
 * 3つだけで、いずれも startEmulation の pushConfig より後に走る。したがって
 * 「既定 false のまま最初のハードリセットが走り、外付 SCSI 装着なのに 'V' を
 * 誤って消す」経路は存在しない。 */
static bool g_scsi_ext_board_installed = false;
/* P450: [P450-SCSIRECON] ログが「内蔵SCSI IPL ROM がロード済みか」を同一行へ
 * 出すために参照する。実体の定義は下方(P251 Stage 2c で外部リンケージ化された
 * もの)なので、mx68k_reset_hard() から見えるようにここで前出し宣言する。 */
extern bool s_scsi_in_rom_loaded;
/* P508: 外付 SCSI IPL ROM(SCSIEXROM.DAT → SCSIIPL[])のロード成否。実体の定義は
 * 下方 mx68k_set_scsi_ext_rom_path() の直前にあるので、mx68k_reset_hard() から
 * 見えるようにここで前出しする(上の s_scsi_in_rom_loaded と同じ理由・同じ位置)。
 * 内部リンケージのままにしたいので extern ではなく暫定定義で前出しする。
 * ★このフラグは「直近に mx68k_set_scsi_ext_rom_path() が呼ばれた時点の値」であり
 * 配線確定値ではない(⌘R 単独では再評価されない)。P508 では鮮度是正を意図的に
 * スコープアウトしている — 鮮度が古い場合でも worst case は
 * sasi_bridge_apply_rom_boot_handle() の MX_ROMBOOT_NONE 分岐(安全なバスエラー)に
 * 落ちるだけで、症状①の解消には影響しない。鮮度問題自体は Docs/09 へ別途起票。 */
static bool s_scsi_ext_rom_loaded;
/* P508: 実体は sasi_bridge.c。EmulatorBridge.h には出さない — Swift からは呼ばず、
 * mx68k_reset_hard() 内でのみ使う Bridge 内部の同期処理であるため。 */
void sasi_bridge_apply_rom_boot_handle(int wired_machine_type, bool ext_wired,
                                       bool in_rom_loaded, bool ext_rom_loaded);
static int  g_memory_size_mb  = 1;
static int  g_clock_mhz       = 16;
static bool g_fpu_enabled     = false;
/* P512: 各チップ音量の最終設定値を保持する。mx68k_reset_hard() が
 * ADPCM_SetVolume/OPM_SetVolume を再実行する際、固定値ではなくユーザー設定値を
 * 再適用するために使う(ハードリセットで設定が既定値へ巻き戻る回帰の防止)。
 * 初期値は Bridge 初期化時の固定呼出し(mx68k_init: ADPCM_SetVolume(15) /
 * OPM は未呼出し = fmgen 側 db=0 相当のフルボリューム)と一致させてある。
 * 設定は mx68k_set_adpcm_volume / mx68k_set_opm_volume(このファイル後方)。
 * ★定義位置: mx68k_reset_hard() より前でなければ参照できないため、
 *   セッタ本体ではなくこのモジュール変数群へ置く。 */
static int  g_adpcm_volume    = 15;
static int  g_opm_volume      = 16;
/* P624: ターボモード中の音声再生レート(P555 の g_turbo_mute_audio を置換)。
 * Q16 固定小数の再生速度倍率で 65536 = 1.0 倍。倍速中は 1 秒あたりの生成サンプル数が
 * 実時間の N 倍になるため、P555 では生成側でミュートしていたが、P624 からは
 * 消費側(mx68k_audio_read)が位相アキュムレータ + 線形補間でデシメーションし、
 * テープ早送りと同じピッチシフト再生を行う(WebX68k の resampleSpeed() 相当)。
 * 生成側・チップ状態は一切変えない(生成側は常に素の audio_ring_write() を呼ぶ)。
 * 予約値: MX68K_TURBO_AUDIO_RATE_MUTE(0) = ミュート(汎用 API プリミティブとして
 * 残すが P625 以降 UI からは到達しない)。MX68K_TURBO_AUDIO_RATE_AUTO(1) = P625:
 * ノーウェイト用。実速度がホスト負荷依存で固定倍率に落とせないため、mx68k_audio_read()
 * がリング充填率から自らレートを決定する(詳細は同関数内の P625 コメント)。
 * ★同期規約: 書込み = Swift メインスレッド(mx68k_set_turbo_audio_rate)、
 * 読取り = CoreAudio 実時間スレッド(mx68k_audio_read)というスレッド跨ぎのため
 * _Atomic 必須(P555 の非 atomic フラグより厳密化。読取り元スレッドが変わったのが理由)。 */
static _Atomic(int) g_turbo_audio_ratio_q16 = MX68K_TURBO_AUDIO_RATE_UNITY;
/* P624: デシメーション位相状態のリセット要求世代カウンタ。
 * ★位相状態(pos / 直前 L・R サンプル)そのものは CoreAudio 実時間スレッドだけが
 * 読み書きする(単一書き手則)。倍率変更時のプチノイズ防止のための位相リセットを
 * Swift メインスレッドから直接行うと非 atomic な部分書込みが混ざるため、セッタは
 * 「リセットしてほしい」という意思表示としてこのカウンタを atomic に +1 するだけとし、
 * 実際のゼロ初期化は mx68k_audio_read() 冒頭の世代チェックが行う。 */
static _Atomic(int) g_turbo_audio_reset_gen = 0;
/* P557: FD アクセス高速化。0 = 既定(OFF)= Core/px68k/x68k/fdd.c の usleep 実引数
 * (シーク movetrack*200+150µs / 読込・書込 300µs)をそのまま素の usleep() へ渡す
 * = P557 以前と完全に同一の挙動。1 = ON で XM6 の fast mode 相当の 64µs 固定へ
 * 短縮する。★既定を OFF にしてあるのは、FD 回転タイミングに依存するコピー
 * プロテクトを持つタイトルの挙動を既定では変えないため。
 * 同期規約: 書込み = Swift メインスレッド(mx68k_set_fd_fast_access)、読取り =
 * エミュレーションスレッドのみ(fdd.c の I/O 経路)。g_clock_mhz と同じ
 * 単純グローバル(atomic 不要)の設計を踏襲する。 */
static int  g_fd_fast_access  = 0;
static bool s_bios_loaded     = false;
static bool s_paused          = false;
/* P34-DIAG: スタックダンプカウンター（ファイルスコープ: reset_hardでリセット可能） */
static int s_p34_stack_dump_count = 0;
static int s_p35_oob_dump_count = 0;  /* P35-DIAG: SSP範囲外時のダンプカウンター */

/* P408: CPUモニタ「Execution Granularity」表示用スナップショット(読み取り専用)。
 * 既存 [P385-CHUNK] が毎フレーム算出している値を mx68k_get_status 経由で Swift 側へ
 * 横流しするだけで、エミュレーション挙動・タイミングには一切影響しない。
 * 代入は mx68k_run_frame() 内の #if P385_ENABLE ブロックで、60フレームおきの
 * ログ出力ゲートより手前(=毎フレーム)に行う。P385_ENABLE が 0 の場合は
 * 初期値 0 のまま = 「未計測」。 */
static int32_t g_p408_clock_slice_snapshot   = 0; /* CLOCK_SLICE(現行値、既定200) */
static int32_t g_p408_clkdiv_snapshot        = 0; /* clkdiv 生値 */
static int32_t g_p408_clk_total_snapshot     = 0; /* 1フレームCPU予算 生値(line_budget の分子) */
static int32_t g_p408_vline_total_snapshot   = 0; /* 走査線数 生値(line_budget の分母) */
static int32_t g_p408_chunks_last_frame      = 0; /* 直近フレームのチャンク数(毎フレーム更新) */
static int32_t g_p408_cum_zero_snapshot      = 0; /* 起動来の n==0 累積(毎フレーム更新) */

/* P641(D-65): 直近フレームで実際に使った1フィールドのCPUクロック予算
 * (名目10MHz基準、クロック倍率スケーリング前)。mx68k_get_vsync_hz() が
 * この値から垂直周波数を返すために使う。CRTC_GetFrameClocks() は分数状態を
 * 進めるため毎フレーム1回しか呼べず、Hz 問い合わせ側から直接呼ぶことは
 * できない——MPX68K が X68FrameInfo.refresh_hz をフレーム内で埋めて
 * GameScene 側がそれを読むのと同じ設計。0 = 未計測(最初のフレーム前)。 */
static int32_t g_p641_frame_clocks_10m       = 0;

/* P47-A: Diagnostic counters / last-known values (file-scope so mx68k_reset_hard() can reset them) */
static int      s_p47_panic_logged   = 0;     /* DIAG-2: PC=0xff063? hits dumped */
static int      s_p47_rte_log_cnt    = 0;     /* DIAG-5: P31 RTE stub hit log count */
static uint32_t s_p47_vec46_last     = 0xFFFFFFFFu; /* DIAG-3: last observed vec#0x46 value */
static int      s_p47_vec46_log_cnt  = 0;
static uint32_t s_p47_abort_ptr_last = 0xFFFFFFFFu; /* DIAG-4: last observed $07FC value */
static int      s_p47_abort_log_cnt  = 0;

/* P47-B-α: TimerD vec#0x44 panic placeholder pin & MFP DIAG state (file-scope so
 * mx68k_reset_hard() can reset them across reruns; same convention as P47-A above). */
static int      s_p47b_pin_cnt       = 0;          /* P47-B-α-FIX: pin invocation count */
static int      s_p47b_diag_cnt      = 0;          /* P47-B-α-DIAG-1: log line count (cap 200) */
static int      s_p47b_panic_logged  = 0;          /* P47-B-α-DIAG-2: panic chain entry one-shot */
static uint32_t s_p47b_v44_last      = 0xFFFFFFFFu;/* P47-B-α-DIAG-1: last observed vec#0x44 */
static uint8_t  s_p47b_iprb_last     = 0xFFu;      /* P47-B-α-DIAG-1: last observed IPRB */
static uint8_t  s_p47b_imrb_last     = 0xFFu;      /* P47-B-α-DIAG-1: last observed IMRB */
static uint8_t  s_p47b_isrb_last     = 0xFFu;      /* P47-B-α-DIAG-1: last observed ISRB */

/* P47-A-DIAG-1: 68000 short stack frame (6 bytes: SR + PC) reader.
 * RAM is stored in host-native LE16 by px68k mem_wrap (see Codex inv §D), so
 * uint16_t* casts read the proper 16-bit values directly.
 * Caller must ensure (ssp & 1) == 0 and ssp+5 within RAM bounds.
 * P47-D S-1: Non-static (was static inline) so m68000_bridge.c can call via extern. */
void p47_read_stack_frame(uint32_t ssp, uint16_t *out_sr, uint32_t *out_pc) {
    uint32_t ram_limit = (uint32_t)(12*1024*1024 - 6); /* 0xBFFFF9 */
    if (!MEM || (ssp & 1) || ssp < 0x400 || ssp + 5 >= ram_limit) {
        *out_sr = 0xFFFF;
        *out_pc = 0xFFFFFFFFu;
        return;
    }
    *out_sr = *(uint16_t*)&MEM[ssp];
    uint16_t pc_hi = *(uint16_t*)&MEM[ssp + 2];
    uint16_t pc_lo = *(uint16_t*)&MEM[ssp + 4];
    *out_pc = ((uint32_t)pc_hi << 16) | pc_lo;
}

/* P47-A: Read RAM long word in host-native LE16 order (returns 0xFFFFFFFF on bounds error).
 * P47-D S-1: Non-static (was static inline) so m68000_bridge.c can call via extern. */
uint32_t p47_read_long_le(uint32_t addr) {
    uint32_t ram_limit = (uint32_t)(12*1024*1024 - 4);
    if (!MEM || addr + 3 >= ram_limit) return 0xFFFFFFFFu;
    uint16_t hi = *(uint16_t*)&MEM[addr];
    uint16_t lo = *(uint16_t*)&MEM[addr + 2];
    return ((uint32_t)hi << 16) | lo;
}

/* P684: FDD 2 台 → 4 台(Core fdd.c は元から drive 0-3 対応)。 */
static char g_fdd_path[4][4096];

static uint8_t s_framebuffer[2][1024 * 1024 * 4];   /* P179: double buffer */

/* P535: 永続合成バッファ。参照実装の ScrBuf(MPX68K x11/windraw.c:44,195 /
 * px68k本家 x11/windraw.c:60,308)に対応する。ストライドは disp_w ではなく固定値。
 * 理由: disp_w は毎フレーム TextDotX から再導出されるため、可変ストライドで永続化すると
 * disp_w が変わった瞬間に保持していた画素が別オフセットとして再解釈され、幾何が壊れる。
 * 参照実装が VLINE*FULLSCREEN_WIDTH という固定ストライドを使っているのはこのため。 */
#define MX_COMPOSE_STRIDE 1024
#define MX_COMPOSE_ROWS   1024
_Static_assert(MX_COMPOSE_STRIDE >= 1024, "compose stride must cover disp_w clamp");
_Static_assert(MX_COMPOSE_ROWS   >= 1024, "compose rows must cover disp_h clamp");
static uint8_t s_compose_fb[MX_COMPOSE_ROWS * MX_COMPOSE_STRIDE * 4];

/* P535: このフレームで実際に mx68k_draw_display_line() が描画した画面行(VLINE)の記録。
 * Stage B では「未到達」と「到達したが dirty でないので省略」の区別に拡張する。 */
static uint8_t s_row_drawn[MX_COMPOSE_ROWS];

/* P365: BG+Sprite合成バッファ(最終合成前)。TextDrawWork/GVRAM同様、
 * 同期なしの直接書込み/直接読取り(P343/P348で確立済みの許容パターン)。 */
static uint8_t s_bgsp_buffer[1024 * 1024 * 4];
/* P521: BGSP Compositeモニタ(⌘⌥C)が表示中かどうか。s_bgsp_bufferへの
 * 書込みコスト(毎走査線・毎画素の色変換込みストア)をパネル非表示中は
 * ゼロにする。読み出し側(mx68k_get_bgsp_composite_rgba)は既にP365/
 * EmulatorEngine.swiftのbgspCompositeVisibleでゲート済みだが、書込み側は
 * 無条件だった——本フラグがその欠けていた書込み側ゲートである。
 * 単純なintフラグのSwift(メインスレッド書込み)→描画ループ
 * (CVDisplayLinkスレッド読取り)間の無同期共有は、既存のg_clock_mhz /
 * s_p353_bg0_visible(6298行目)と同じ確立済みパターン。デフォルト0は
 * Swift側bgspCompositeVisibleのデフォルトfalseと一致。 */
static int s_p521_bgsp_composite_visible = 0;

void mx68k_set_bgsp_composite_visible(int visible) {
    s_p521_bgsp_composite_visible = visible ? 1 : 0;
}
static int s_fb_w[2] = {768, 768};                  /* P179: per-buffer published size */
static int s_fb_h[2] = {512, 512};
/* P595 (D-55): 公開フレームと同一スナップショットの表示ジオメトリ。s_fb_w/s_fb_h と
 * 全く同じ [2] 配列パターン(P179)で持ち、同じ front インデックスで読ませることで、
 * エミュレーションスレッドが次フレームを計算中に Metal スレッドが寸法とジオメトリを
 * 取り違えて読む競合を防ぐ。 */
static float s_fb_hscale[2] = {1.0f, 1.0f};
static float s_fb_vscale[2] = {1.0f, 1.0f};
static float s_fb_offx[2]   = {0.0f, 0.0f};
static float s_fb_offy[2]   = {0.0f, 0.0f};
static int   s_fb_geomode[2] = {0, 0};
static _Atomic(int) s_fb_front = 0;                 /* P179: index of latest COMPLETE frame */
#if P303_ENABLE
static _Atomic(int) s_fb_front_frame_num = 0;   /* P303: s_fb_frontと対になるフレーム番号 */
#endif
uint8_t s_ipl_fetch[0x20000];  /* P17-FIX-B: FETCHテーブル用LE16スワップ済みIPLコピー */

// ---- audio ring buffer (lock-free SPSC for CoreAudio real-time safety) ----
#define AUDIO_SAMPLE_RATE    44100
/* P626 (D-62): Core 側音声チップ(ADPCM / OPM / Mercury)の実初期化レート、および
 * 音声生成量計算のレート基準。AUDIO_SAMPLE_RATE(44100)を既定値として持ち、
 * 設定画面のサンプルレート Picker の値が mx68k_set_audio_sample_rate() 経由で
 * 入る。従来は AUDIO_SAMPLE_RATE / 44100 リテラルが直接チップ初期化へ渡されており
 * 設定値は AudioUnit 出力レートにしか反映されていなかったため、22050Hz を選ぶと
 * 生成量が消費量の 2 倍になりリング恒常満杯 = 常時バースト音切れになっていた。
 * 参照実装 3 種(px68k-libretro / MPX68K / px68k 本家)はいずれも設定レートを
 * チップ初期化へ渡している。
 * 「ハードリセットが必要な設定」カテゴリ(機種選択・メモリ容量・FPU・BIOS パス)と
 * 同じ扱いで、g_memory_size_mb / g_mercury_enabled 等と同様に _Atomic は使わない
 * — Swift メインスレッドからのみ書かれ、init / ハードリセット / ステートロードも
 * 同じくメインスレッド由来でのみ起動されるという既存の規約に従う。 */
static int g_audio_sample_rate_hz = AUDIO_SAMPLE_RATE;
/* P211b: max audio samples per emulated frame. audio_frames =
 * round(g_audio_sample_rate_hz/vsync_hz) couples generation to the P211 VSYNC pacing
 * (44100Hz: 55.46Hz->795, 61.46Hz->718).
 * P627: 896 -> 2048 へ拡張。サンプルレート選択肢を 96000Hz まで上方拡張したため、
 * 最大生成量は 96000/55.46 ≒ 1731 フレーム/エミュフレームとなり 896 では 1.9 倍超過し
 * 毎フレームクランプ = 常時アンダーランになる。2048 は 1731 に対して約 18% の余裕。
 * 増加分は下の tmp_opm / tmp_adpcm / tmp_mix 3 本 × 2ch × 2byte = 計 24.6KB の
 * スタック使用(CVDisplayLink スレッドの 512KB に対して問題なし)。 */
#define AUDIO_FRAMES_MAX     2048
/* P624: 4096 -> 16384 へ拡張。ターボ(固定倍率)中は 1 回の CoreAudio コールバックが
 * callback_frames × 倍率 のフレームをリングから消費するため、5x では 4096 フレームでは
 * 余裕が 1.6 倍程度しかなくバースト破棄の原因になっていた。44100Hz で 16384 フレーム
 * = 約 372ms 分。増加分は 12288 フレーム × 2ch × 2byte = 48KB のメモリのみで、
 * ターボ非活性時は mx68k_audio_read() の bypass 経路により挙動は従来と同一。
 * P627: 16384 -> 32768 へ拡張。サンプルレート選択肢の 96000Hz 追加により、16384 では
 * 実時間換算 170ms(現行 44100Hz 時 372ms の 46%)まで目減りしてしまうため。
 * 32768 フレームなら 96000Hz で約 341ms、44100Hz では約 743ms 分。
 * 増加分は 16384 フレーム × 2ch × 2byte = 64KB のメモリのみ。 */
#define AUDIO_RING_FRAMES    32768
#define AUDIO_RING_SAMPLES   (AUDIO_RING_FRAMES * 2)

static int16_t audio_ring[AUDIO_RING_SAMPLES];
static _Atomic(int) audio_ring_write_pos = 0;
static _Atomic(int) audio_ring_read_pos  = 0;

/* P484 (サウンドモニタ): ADPCM 波形プレビュー用スナップショット。
 * 直近フレームの tmp_adpcm[] 末尾 MX68K_ADPCM_WAVEFORM_SAMPLES サンプルを
 * モノラル化(Lch)して時系列順(先頭=古い・末尾=新しい)に保持する。
 * リングではなく単純スナップショット: 1フレームあたりの audio_frames は
 * 44100/vsync_hz ≒ 718〜795 で常に 256 を上回るため、リング+mod 方式にすると
 * 書込みインデックスが毎フレーム 0 に戻り、ローリングウィンドウとして機能しない。
 * 書込みは mx68k_run_frame() 内(CVDisplayLink スレッド)のみ、読出しは
 * mx68k_get_adpcm_status() を同じ呼び出しスタックから呼ぶ Swift 側のみ。 */
static int16_t g_adpcm_waveform[MX68K_ADPCM_WAVEFORM_SAMPLES];

static void audio_ring_init(void) {
    memset(audio_ring, 0, sizeof(audio_ring));
    atomic_store_explicit(&audio_ring_write_pos, 0, memory_order_release);
    atomic_store_explicit(&audio_ring_read_pos, 0, memory_order_release);
}

#if P464_RINGFILL_ENABLE
/* P464 (D-6再調査): リングバッファ診断カウンタ群。
 *
 * ── 生成側(エミュレーションスレッドからのみ更新 → 非atomicで十分) ──
 * s_p464_fill_hist[]  : 「書込み直前」のリング充填率のヒストグラム。
 *                       8バケット × (AUDIO_RING_FRAMES/8) フレーム幅 = 0..AUDIO_RING_FRAMES を網羅し、
 *                       全バケットの合計が window 内の書込み回数(=分母)になるので
 *                       「特定バケットが0件」なのか「そもそも書込みが起きていない」
 *                       のかを標本自身で判別できる(自己反証可能性)。
 *                       ★計上箇所は audio_ring_write() 内部・書込みループ直前の
 *                       この1箇所のみ。呼出し側で write_pos/read_pos を読み直すと
 *                       「書込み後」の充填率と取り違えるため重複実装しない。
 * s_p464_overrun_writes / s_p464_samples_dropped
 *                     : リング満杯で要求サンプルを書き切れなかった回数 / サンプル数。
 * s_p464_abs_sum_*    : |tmp_adpcm| / |tmp_opm| の総和(平均算出用)。分母は
 *                       s_p464_abs_sample_count。
 *
 * ── 消費側(CoreAudio 実時間スレッドから更新 → _Atomic 必須) ──
 * ★このスレッドから debug_log() 等の同期I/Oは一切呼ばない。同期I/O+fflush を
 *   オーディオスレッドで行うとプローブ自身が測定対象のジッターを発生させるため
 *   (Spec Investigation H5指摘)。atomicインクリメントのみ行い、出力は
 *   エミュレーションスレッド側で読み取る。
 * s_p464_callbacks_total は underrun_callbacks の分母を兼ねる
 * (「アンダーラン0件」と「そもそも1回も呼ばれていない」を区別するため)。 */
#define P464_RINGFILL_BUCKETS 8
#define P464_RINGFILL_BUCKET_FRAMES (AUDIO_RING_FRAMES / P464_RINGFILL_BUCKETS)  /* P627: 4096 (P624: 2048 @16384フレーム / それ以前: 512 @4096フレーム) */
static unsigned long long s_p464_fill_hist[P464_RINGFILL_BUCKETS] = {0};
static unsigned long long s_p464_overrun_writes   = 0;
static unsigned long long s_p464_samples_dropped  = 0;
static unsigned long long s_p464_abs_sum_adpcm    = 0;
static unsigned long long s_p464_abs_sum_opm      = 0;
static unsigned long long s_p464_abs_sample_count = 0;
static _Atomic(unsigned long long) s_p464_callbacks_total     = 0;
static _Atomic(unsigned long long) s_p464_underrun_callbacks  = 0;
static _Atomic(unsigned long long) s_p464_samples_zero_filled = 0;
#endif

/* P549: サウンドモニタUI向けの累積(リセットしない)統計。上の P464 側は
 * ウィンドウ毎に atomic_exchange でゼロ化される診断用カウンタであり、UI から
 * 「セッション累積」として参照できない。本カウンタはそれとは完全に独立した
 * 恒常機能であり、★意図的に `#if P464_RINGFILL_ENABLE` の外側に置く ——
 * P464_RINGFILL_ENABLE は D-6 調査由来の診断マクロで将来無効化/削除されうるため、
 * 内側に入れると将来の probe 整理で UI 機能がサイレントに壊れる(Code Review 指摘)。
 * 更新元は CoreAudio 実時間スレッド(mx68k_audio_read)なので _Atomic 必須。 */
static _Atomic(unsigned long long) s_p549_cum_callbacks_total     = 0;
static _Atomic(unsigned long long) s_p549_cum_underrun_callbacks  = 0;
static _Atomic(unsigned long long) s_p549_cum_samples_zero_filled = 0;

/* P464: 戻り値を void -> int(実際に書き込んだサンプル数)へ変更。
 * リング満杯によるドロップ量を呼出し側が直接知るためで、書込みロジック自体は無変更。
 * 本関数は Bridge 内部の static のため Core/ には波及しない。 */
static int audio_ring_write(const int16_t* data, int samples) {
    int write_pos = atomic_load_explicit(&audio_ring_write_pos, memory_order_relaxed);
    int read_pos  = atomic_load_explicit(&audio_ring_read_pos, memory_order_acquire);
#if P464_RINGFILL_ENABLE
    /* 書込みループより前(= 上で読み込んだ write_pos/read_pos のまま)に計上する。 */
    {
        int fill_samples = write_pos - read_pos;
        if (fill_samples < 0) fill_samples += AUDIO_RING_SAMPLES;
        int fill_frames = fill_samples / 2;              /* L/R 2サンプル = 1フレーム */
        int b = fill_frames / P464_RINGFILL_BUCKET_FRAMES;
        if (b < 0) b = 0;
        if (b >= P464_RINGFILL_BUCKETS) b = P464_RINGFILL_BUCKETS - 1;
        s_p464_fill_hist[b]++;
    }
#endif
    int written = 0;
    for (int i = 0; i < samples; i++) {
        int next = (write_pos + 1) % AUDIO_RING_SAMPLES;
        if (next == read_pos) break; // full: drop remaining
        audio_ring[write_pos] = data[i];
        write_pos = next;
        written++;
    }
    atomic_store_explicit(&audio_ring_write_pos, write_pos, memory_order_release);
    return written;
}

static int audio_ring_read(int16_t* data, int samples) {
    int read_pos = atomic_load_explicit(&audio_ring_read_pos, memory_order_relaxed);
    int write_pos = atomic_load_explicit(&audio_ring_write_pos, memory_order_acquire);
    int count = 0;
    for (int i = 0; i < samples; i++) {
        if (read_pos == write_pos) break; // empty
        data[i] = audio_ring[read_pos];
        read_pos = (read_pos + 1) % AUDIO_RING_SAMPLES;
        count++;
    }
    atomic_store_explicit(&audio_ring_read_pos, read_pos, memory_order_release);
    return count;
}

/* P624: リングから L/R 1 フレーム(= 2 サンプル)を取り出す。取り出せたら 1、
 * リングが枯渇していれば 0 を返す。デシメーション経路が★必ずフレーム単位で入力を
 * 扱うためのラッパ(P465-B2 の教訓: サンプル単位で L/R 境界を跨ぐとチャンネルが入替わる)。
 * 端数 1 サンプルしか取れなかった場合はそれを捨てて枯渇扱いとする —— 等倍経路が
 * `got_raw & ~1` で行っている端数処理と同じ意味論。 */
static int audio_ring_read_frame(int16_t out[2]) {
    return audio_ring_read(out, 2) == 2;
}

/* P624: リング内の未再生データを全て破棄する(消費側 = CoreAudio 実時間スレッド専用)。
 * ミュート(ノーウェイト)中に使う。P555 のミュートは「生成側が書き込まない」方式
 * だったためリングは自然に空になっていたが、P624 で生成側の分岐を撤去した結果、
 * ミュート中もリングは満杯のまま滞留する。そのままミュート解除すると約 372ms 分の
 * 古い音声が再生され、以後その遅延が残り続けるため、ミュート中は毎コールバックで
 * リングを空にして P555 当時と同じ「解除時にリングが空」という状態を保つ。
 * SPSC 設計上 read_pos の書き手は本スレッドのみなので、write_pos へ揃えるだけでよい。 */
static void audio_ring_discard_all(void) {
    int write_pos = atomic_load_explicit(&audio_ring_write_pos, memory_order_acquire);
    atomic_store_explicit(&audio_ring_read_pos, write_pos, memory_order_release);
}

/* ================= P698: 録画専用オーディオリング =================
 *
 * 【なぜ第2のリングを新設するのか】
 * 上の audio_ring は SPSC(単一生成者 = エミュレーションスレッドの
 * audio_ring_write() / 単一消費者 = mx68k_audio_read() を呼ぶ CoreAudio 実時間
 * スレッド)専用設計であり、read_pos の書き手が 1 つであることに依存している。
 * 録画側が同じリングを二重に消費するとこの不変条件が壊れるため、録画用には
 * 完全に独立した第2リングを持たせる(P698 Code Investigation §5)。
 *
 * 【生成者と消費者】
 *   生成者: mx68k_rec_audio_write() ← AudioEngine.swift の audioCallback
 *           (CoreAudio 実時間 I/O スレッド)から volume/mute 適用「後」のバッファ。
 *           つまり録画には「実際に耳に聞こえる音」がそのまま入る。
 *   消費者: mx68k_rec_audio_read() ← VideoRecordingService のドレイン経路
 *           (CVDisplayLink スレッド)。
 * この 1:1 対応が保たれる限り audio_ring と同型の release/acquire SPSC で足りる。
 *
 * 【リアルタイムスレッド規約(最重要)】
 * mx68k_rec_audio_write() は CoreAudio 実時間スレッドから毎コールバック呼ばれる。
 * したがってこの関数は:
 *   - ロックを一切取らない
 *   - debug_log() を絶対に呼ばない —— 本ファイル 106-108 行のコメントに明記された
 *     既存の設計原則(P464 / §H5)。debug_log() は pthread_mutex_lock + fflush() の
 *     ブロッキング I/O を含み、まさにリングが溢れる(= ホストが重い)瞬間に
 *     実際の音声再生自体をグリッチさせる。
 *   - 録画していない間は rec_audio_enabled の atomic load 1 回だけで即 return する
 *     (完全ゼロコスト)
 * オーバーラン時は _Atomic カウンタ加算のみを行い、ログ出力は非リアルタイム
 * コンテキスト(audioEncodeQueue)が mx68k_rec_audio_get_and_reset_drop_count() を
 * ポーリングして行う(mx68k_diag_get_and_reset_log_calls() と同型の get-and-reset)。
 *
 * 【容量】44100Hz で 8192 フレーム ≒ 185ms。ドレイン周期(CVDisplayLink =
 * 約 18ms @55.46Hz)の約 10 倍の余裕。メモリは 8192 × 2ch × 2byte = 32KB。 */
#define REC_AUDIO_RING_FRAMES   8192
#define REC_AUDIO_RING_SAMPLES  (REC_AUDIO_RING_FRAMES * 2)

static int16_t rec_audio_ring[REC_AUDIO_RING_SAMPLES];
static _Atomic(int) rec_audio_ring_write_pos = 0;
static _Atomic(int) rec_audio_ring_read_pos  = 0;
/* 録画中のみ true。生成側(実時間スレッド)の唯一の分岐材料。 */
static _Atomic(bool) rec_audio_enabled = false;
/* リング満杯で 1 回分の書込みを丸ごと捨てた回数。実時間スレッドから加算、
 * 非リアルタイム側から get-and-reset で読み出す。 */
static _Atomic(uint32_t) rec_audio_drop_count = 0;
/* P698 残留リスク(リング容量の妥当性)の検証用: 1 回の CoreAudio コールバックが
 * 書き込もうとしたフレーム数の、録画セッション中の最大値(リング満杯で捨てられた
 * 回も込み —— 容量が足りているかを判断するには「要求された量」こそが必要な値。
 * したがってカウントはドロップ判定より前で行う)。AudioEngine の bufferFrames
 * 引数が実際には効いていない(P698 Code Investigation §3)ため、コールバック
 * 1 回あたりの実フレーム数は未実測だった —— REC_AUDIO_RING_FRAMES=8192 が
 * 実測値に対して十分な余裕かを、実時間スレッドでログを出さずに判定するための値。
 * 書き手は生成側スレッド 1 本のみなので load/compare/store で足りる。 */
static _Atomic(int) rec_audio_max_write_frames = 0;

void mx68k_rec_audio_write(const int16_t* buffer, int frames) {
    /* ★録画していない間のコストはこの atomic load 1 回のみ。 */
    if (!atomic_load_explicit(&rec_audio_enabled, memory_order_acquire)) return;
    if (!buffer || frames <= 0) return;

    /* コールバック 1 回あたりの実フレーム数の最大値を記録(ログは出さない)。 */
    if (frames > atomic_load_explicit(&rec_audio_max_write_frames, memory_order_relaxed)) {
        atomic_store_explicit(&rec_audio_max_write_frames, frames, memory_order_relaxed);
    }

    const int samples = frames * 2;   /* L/R インターリーブ */
    int write_pos = atomic_load_explicit(&rec_audio_ring_write_pos, memory_order_relaxed);
    int read_pos  = atomic_load_explicit(&rec_audio_ring_read_pos,  memory_order_acquire);

    /* 満杯と空を区別するため 1 スロットは常に空けておく(= 実効容量は
     * REC_AUDIO_RING_SAMPLES - 1)。 */
    int free_samples = read_pos - write_pos - 1;
    if (free_samples < 0) free_samples += REC_AUDIO_RING_SAMPLES;

    /* ★意味論の相違を明記: 既存 audio_ring_write() は「入るだけ書いて残りを破棄」
     * だが、こちらは frames 分の空きが無ければ**この回の書込み全体をドロップ**する。
     * 部分書込みを許すと L/R フレーム境界そのものは保てても、録画トラックへ
     * 中途半端な長さのバーストが混ざり、後段の PTS 付けと実サンプル数の対応が
     * 崩れるため。ドロップは「その区間が丸ごと欠落」として現れる。 */
    if (free_samples < samples) {
        atomic_fetch_add_explicit(&rec_audio_drop_count, 1, memory_order_relaxed);
        return;
    }

    for (int i = 0; i < samples; i++) {
        rec_audio_ring[write_pos] = buffer[i];
        write_pos = (write_pos + 1) % REC_AUDIO_RING_SAMPLES;
    }
    atomic_store_explicit(&rec_audio_ring_write_pos, write_pos, memory_order_release);
}

int mx68k_rec_audio_read(int16_t* buffer, int max_frames) {
    if (!buffer || max_frames <= 0) return 0;

    int read_pos  = atomic_load_explicit(&rec_audio_ring_read_pos,  memory_order_relaxed);
    int write_pos = atomic_load_explicit(&rec_audio_ring_write_pos, memory_order_acquire);

    int avail_samples = write_pos - read_pos;
    if (avail_samples < 0) avail_samples += REC_AUDIO_RING_SAMPLES;

    /* 生成側が常にフレーム単位(偶数サンプル)で書くため avail_samples は偶数だが、
     * 念のためフレーム単位へ切り捨てて L/R の入替わりを構造的に防ぐ
     * (P465-B2 の教訓: サンプル単位で L/R 境界を跨ぐとチャンネルが入替わる)。 */
    int frames = avail_samples / 2;
    if (frames > max_frames) frames = max_frames;

    const int samples = frames * 2;
    for (int i = 0; i < samples; i++) {
        buffer[i] = rec_audio_ring[read_pos];
        read_pos = (read_pos + 1) % REC_AUDIO_RING_SAMPLES;
    }
    atomic_store_explicit(&rec_audio_ring_read_pos, read_pos, memory_order_release);
    return frames;
}

uint32_t mx68k_rec_audio_get_and_reset_drop_count(void) {
    return atomic_exchange_explicit(&rec_audio_drop_count, 0, memory_order_relaxed);
}

int mx68k_rec_audio_get_max_write_frames(void) {
    return atomic_load_explicit(&rec_audio_max_write_frames, memory_order_relaxed);
}

void mx68k_rec_audio_reset(void) {
    /* ★呼出し規約: rec_audio_enabled が false の状態でのみ呼ぶこと
     * (録画開始準備中 / 停止後)。生成側が動いている最中に索引をゼロへ戻すと
     * SPSC の不変条件が壊れる。VideoRecordingService は start() の
     * set_enabled(true) の**直前**と stop() の set_enabled(false) の**後**でのみ呼ぶ。 */
    atomic_store_explicit(&rec_audio_ring_read_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&rec_audio_ring_write_pos, 0, memory_order_release);
    atomic_store_explicit(&rec_audio_drop_count, 0, memory_order_relaxed);
    atomic_store_explicit(&rec_audio_max_write_frames, 0, memory_order_relaxed);
}

void mx68k_rec_audio_set_enabled(bool enabled) {
    atomic_store_explicit(&rec_audio_enabled, enabled, memory_order_release);
}

// ---- Helper: ensure app-support directory exists ----
/* P703: 中間ディレクトリも含めて再帰的に作成する。
 *
 * 従来は組み立てた完全パスに対して mkdir() を 1 回呼ぶだけだった。macOS では
 * ~/Library/Application Support が OS により常に事前作成されているため、これで
 * 問題が表面化しなかったが、iOS のサンドボックスコンテナには
 * Library/Application Support 自体が存在せず(T-0d 実測: Library 直下は
 * Caches / Preferences / SplashBoard のみ)、非再帰 mkdir が ENOENT で失敗して
 * debug.log が一切作られなかった。
 *
 * ★macOS での挙動は不変: 各階層は既に存在するので、途中段の mkdir() はすべて
 *   EEXIST で失敗して素通りし、最終段も従来と同じ結果になる(戻り値は従来から
 *   一貫して無視しており、EEXIST を「エラー扱いしない」規約もそのまま)。
 * ★シグネチャ・呼び出し元は一切変更していない。 */
static void ensure_app_support_dir(void) {
    const char* home = getenv("HOME");
    if (!home) return;
    char dir[1024];
    int n = snprintf(dir, sizeof(dir), "%s/Library/Application Support/MX68K", home);
    if (n < 0 || (size_t)n >= sizeof(dir)) return;   /* 切り詰めが起きたら何もしない */

    /* home 自体は存在する前提なので、その直後の区切りから走査を始める。
     * 各 '/' で一旦終端して中間ディレクトリを作る(既存なら EEXIST で no-op)。 */
    size_t home_len = strlen(home);
    for (char* p = dir + home_len + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        (void)mkdir(dir, 0755);
        *p = '/';
    }
    (void)mkdir(dir, 0755);
}

/* P221b: derive Config.XVIMode (the SysPort $E8E00B CPU/clock nibble) from the
 * configured clock. SASI/SCSI machine type is orthogonal (SUPER = SCSI + 10MHz
 * reads 0xFF, proving XVIMode does not encode the storage bus; code inv §1).
 * 10MHz->0 (0xFF), 16MHz->1 (0xFE), else->3 (0xDC/25MHz). This replaces the
 * P146 hard-coded XVIMode=3, which made si misreport 030/25MHz at every clock
 * (D-15). Config.XVIMode is read in the Core at exactly one place (sysport.c:86)
 * and feeds no clock/cycle timing — it only changes the $E8E00B byte. */
/* P270: the P221b threshold approach above could not correctly express the
 * clock-generation class of clock-up MOD kits. XVIMode is NOT "the raw clock
 * value" nor "the storage bus" — it is the clock-generation class of the real
 * machine each option is historically tied to (10MHz-class = 0xFF machines /
 * 16MHz-class = 0xFE machines). A clock-up MOD swaps only the crystal
 * oscillator, so it changes the *measured* speed but not the machine's ID
 * class. A real-machine XM6 reference (si output): XVI+RedZone(24MHz)
 * reports "clock switch: 16MHz mode" (unchanged from stock XVI) while only
 * "micro processing unit: 68000 (24.0MHz)" reflects the modded speed — i.e. the
 * modded clock does not change the machine ID class. EXPERT mod (17.4MHz,
 * rounded to 17) exceeds 16 numerically but its real machine generation is
 * 10MHz-class, so a simple threshold misclassifies it as 030/0xDC. Hence a
 * per-value table, not a threshold. Upstream Core/px68k/x68k/sysport.c:88
 * independently corroborates this with its existing "case 1: // XVI or RedZone"
 * comment — RedZone was always meant to map to the same XVIMode value as stock
 * XVI. The P220/P221 code-inv fact (SASI/SCSI storage type is orthogonal to
 * XVIMode) is preserved. */
static int p270_derive_xvimode(int clock_mhz) {
    switch (clock_mhz) {
        case 16:   /* 定格 SUPER/XVI/Compact */
        case 24:   /* RedZone(XVI改) */
            return 1;   /* 0xFE, 16MHz級 */
        case 25:   /* X68030定格(Phase5専用。現行SASI/SCSI 2値からは到達しない) */
            return 3;   /* 0xDC */
        default:   /* 10(定格)/12(ACE改)/15(PRO改)/17(EXPERT改,17.4丸め)/20(Lucky!)
                    * 他すべて10MHz級 */
            return 0;   /* 0xFF */
    }
}

/* P472: physical guest RAM size actually allocated for MEM. The memory-size
 * setting (g_memory_size_mb) never reaches the core memory map — it is a
 * display-only value — so MEM is always this fixed size and every consumer
 * (allocation, state save, state load) must derive its size from here. */
#define MX68K_RAM_BYTES (12 * 1024 * 1024)

/* P565 変更3: MEM 確保末尾に置くホスト側ガード領域のサイズ。
 * ゲストアドレス空間には一切影響しない(MEM の C68k_Set_Fetch 範囲
 * $000000-$BFFFFF は不変)。ゲスト PC が分岐せず直線前進で 12MB 境界を
 * 越えたとき、ホストプロセスが確保領域外を読んで SIGSEGV になる前に
 * ILLEGAL 命令(0x4AFC)へ着地させ、c68k のベクタ4例外へ降格させるための
 * 安全マージンである(D-50 / P564)。P564 実測の直線前進速度は
 * 約 1.6KB/chunk なので 64KB は約 40 倍の裕度。
 * MX68K_RAM_BYTES 自体は不変のため、セーブステートの RAM ブロックサイズ
 * (do_save_state / do_load_state)には影響しない。 */
#define P565_MEM_GUARD_BYTES 65536

/* P565: バッファを 68000 の ILLEGAL オペコード 0x4AFC で埋める
 * (実装とエンディアン根拠は Bridge/m68000_bridge.c を参照)。 */
extern void mx68k_p565_fill_illegal(void *dst, size_t bytes);

// ---- init / shutdown ----
int mx68k_init(void) {
    debug_log_init();
    debug_log("[MX68K] mx68k_init() called\n");

    /* P633: ここにあった P490 の packetList 付け替え(欠陥 A 対策)は、CoreMIDI
     * 連携層を Bridge/midi_coremidi.c へ移設し、新実装が自前の書込可能バッファを
     * 静的に所有する設計にしたため不要になり削除した。 */

    /* P221b: derive XVIMode from the configured clock (init default 16 -> 1);
     * reset_hard re-derives from the actual clock. Belt-and-suspenders so the
     * SysPort byte is sane even before the first hard reset. See
     * p270_derive_xvimode / mx68k_reset_hard for the full rationale. */
    Config.XVIMode = p270_derive_xvimode(g_clock_mhz);

#if P53_ENABLE
    /* P53 — reset idempotency flag on each init so re-init scenarios work.
     * Atexit registration is one-shot per process (handled below near the
     * end of mx68k_init). See /tmp/mx68k_P53_plan.md §3.1 / Edit B4(i). */
    s_p53_shutdown_done = 0;
#endif

    memset(s_compose_fb, 0, sizeof(s_compose_fb));   /* P535: 永続合成バッファの初期化 */

    // Allocate core memory buffers if not already present.
    // MEM is up to 12 MB; IPL 256 KB; FONT 768 KB.
    if (!MEM) {
        /* P22-FIX: +4 bytes so FETCH_WORD at MEM boundary (0xBFFFFF) does not
         * read MEM[0xC00000] past the 12 MB allocation, causing SIGSEGV ~frame 49. */
        /* P472: MX68K_RAM_BYTES is the single source of truth for the guest RAM
         * size — MEM is always physically 12 MB regardless of the cosmetic
         * g_memory_size_mb setting, so state save/load must size their RAM block
         * from this constant, not from that setting (see do_save_state /
         * do_load_state). The +4 guard bytes stay out of the state block. */
        /* P565 変更3: 末尾に P565_MEM_GUARD_BYTES(64KB)のホスト側ガードを
         * 追加確保する。ゲストから見えるメモリマップは不変(Fetch 範囲も
         * $000000-$BFFFFF のまま)で、確保サイズだけが増える。 */
        MEM = (uint8_t*)malloc(MX68K_RAM_BYTES + 4 + P565_MEM_GUARD_BYTES);
        if (!MEM) return -1;
        /* 従来どおり RAM 本体 + P22-FIX の +4 バイトはゼロ埋め(byte-equivalent)。 */
        memset(MEM, 0, MX68K_RAM_BYTES + 4);
        /* P565: 追加分のみ ILLEGAL(0x4AFC)で埋める。ハードリセットでは
         * MEM 全体の memset は行われない(ベクタ領域の部分クリアのみ)ため、
         * このガードはプロセス寿命の間そのまま残る。 */
        mx68k_p565_fill_illegal(MEM + MX68K_RAM_BYTES + 4, P565_MEM_GUARD_BYTES);
#if P135_ENABLE
        /* P135 Part1-A: init clear 実行を latch。before は確保直後 (未定義) ゆえ
         *   0xFFFF sentinel・after は memset 後 (=0x0000) を計測。frame ラベルは
         *   m68000_bridge.c の Part2 と同一カウンタ。 */
        p135_host_latch(0u, g_mx68k_frame_num, 0xFFFFu, p135_mem1ff6());
#endif
    }
    if (!IPL) {
        IPL = (uint8_t*)malloc(0x40000);  // P26-FIX: rm_ipl の addr & 0x3fffe OOB防止（最大オフセット0x3FFFE必要）
        if (!IPL) return -1;
        memset(IPL, 0xFF, 0x40000);       // 未使用ROM領域デフォルト0xFF
    }
    if (!FONT) {
        FONT = (uint8_t*)malloc(0xC0000);
        if (!FONT) return -1;
        memset(FONT, 0, 0xC0000);
    }

    // Set pixel-format masks before Pal_Init so 32-bit palettes are generated
    // with the px68k native layout (R@bits24-31, G@16-23, B@8-15).
    // We swizzle to RGBA8888 in mx68k_get_framebuffer().
    WinDraw_Pal32R = 0xFF000000;
    WinDraw_Pal32G = 0x00FF0000;
    WinDraw_Pal32B = 0x0000FF00;

    Memory_Init();
    MFP_Init();
    /* P143b: P81-A timer-stop REMOVED. MFP_Init sets warm-start TCDCR=0x77 (Timer C
     * running), matching the fully-working reference MPX68K. Zeroing TACR/TBCR/TCDCR
     * here stopped MFP Timer C, so the IPLROM's level-6 timer wait never fired -> the
     * icount-299949 interrupt-level divergence and downstream boot stall. */
    Keyboard_Init();
    Keymap_Init();
    FDD_Init();
    FDC_Init();
    /* P686 (D-70): 設定値を配線確定値へラッチし、Core へ接続台数を伝える。
     * fdc_drives は FDC_Init() の memset 対象外だが、将来 FDC_Init() の
     * 呼び出し地点が増えたときに巻き戻りの窓を作らないよう、毎回明示的に
     * 呼び直す(意図を呼び出し側からも読めるようにする)。 */
    g_ext_fdd_wired = g_ext_fdd_enabled ? 1 : 0;
    FDC_SetDriveCount(g_ext_fdd_wired ? 4 : 2);
    debug_log("[P686-EXTFDD] init/reset: enabled=%d -> wired=%d drives=%d\n",
              (int)g_ext_fdd_enabled, g_ext_fdd_wired, g_ext_fdd_wired ? 4 : 2);
    p509_fdc_mirror_reset();   /* P509: Bridge 側コマンドミラーを fdc.c と同時に巻き戻す */
    TVRAM_Init();       /* P171: init TextDrawPattern (text bit-expand table). Missing in
                         * the port -> TextDrawWork stayed 0 -> blank text layer. MPX calls
                         * this in its reset sequence (winx68k.cpp:293). */
    GVRAM_Init();
    CRTC_Init();        /* P641: 内部で CrtcFieldClock_Init() = 分数状態のゼロクリア */
    g_p641_frame_clocks_10m = 0;  /* P641: 次フレームまでは旧式2値へ退避させる */
    Pal_Init();
    BG_Init();
    IOC_Init();
    SCC_Init();
    SASI_Init();
    sasi_bridge_install();
    SCSI_Init();
    /* P269/P510: init 時点では配線確定値がまだ cold(g_scsi_ext_board_wired==0)
     * なのでゲートは必ず不成立 = 従来どおり Core 観測フック。実配線は最初の
     * mx68k_reset_hard() で確定する。 */
    scsi_ext_bridge_install(g_scsi_ext_board_wired,
                            s_scsi_ext_rom_loaded ? 1 : 0,
                            g_wired_machine_type);
    DMA_Init();
    ADPCM_Init(g_audio_sample_rate_hz);   /* P626: 設定サンプルレート(既定 44100) */
    ADPCM_SetVolume(15);   /* P210: port omitted this call. ADPCM_VolumeShift stays at its
                            * init value 65536 (~4096x the normal 13-16) -> saturation clip
                            * (ADPCM far louder than FM, distortion). Reference MPX68K
                            * winx68k.cpp:698 / px68k-libretro winx68k.cpp:653 call
                            * ADPCM_SetVolume(Config.PCM_VOL), default PCM_VOL=15. Core unchanged. */
    OPM_Init(4000000, g_audio_sample_rate_hz);   /* P626: 設定サンプルレート(既定 44100) */
    p479_opm_shadow_reset();   /* P479: keep the Bridge-side OPM shadow in sync with the chip */
    /* P483: Mercury Unit(MK-MU1 / $ECC000)PCM 部の配線。
     * 設定値 g_mercury_enabled を配線確定値 g_mercury_installed へラッチする
     * (Swift の pushConfig は mx68k_init() の前に呼ばれるので、この時点で
     *  g_mercury_enabled は既に正しい — EmulatorViewModel.swift:135-138)。
     * 変更 4: Mcry_Cleanup() を Mcry_Init() の直前に置く。M288_Cleanup() は
     *   delete ymf288a/b + NULL 代入(fmg_wrap.cpp:261-266)で、NULL への delete は
     *   無害なので初回 init でも安全。これが無いとリセットごとに YMF288 が 2 個リークする。
     * 変更 5: 従来の Mcry_Init(44100, NULL) は snprintf("%s%c", path, slash)
     *   (mercury.c:331)へ NULL を渡す未定義動作だった。
     * 変更 6: Mcry_SetVolume 未呼出だと Mcry_VolumeShift が初期値 65536 のままで
     *   OutData/65536 ≒ 0 = PCM 部が実質ミュートになる(P210 の ADPCM と同型)。 */
    {
        int p483_prev = g_mercury_installed;
        g_mercury_installed = g_mercury_enabled ? 1 : 0;
        Mcry_Cleanup();
        Mcry_Init(g_audio_sample_rate_hz, mx68k_mercury_dir());   /* P626: 設定サンプルレート */
        Mcry_SetVolume(MCRY_VOL);
        p491_mercury_opn_shadow_reset();   /* P491: keep the Bridge-side OPN shadow in sync with the chip */
        p634_lrck_reset();                 /* P634 (D-43): Mcry_Init が Mcry_LRTiming=0 に
                                            * するのと同じ箇所で自走 LR 位相も 0 クリアし、
                                            * 状態の非同期を作らない。 */
        debug_log("[P483-MERCURY] init: enabled=%d installed_prev=%d installed=%d "
                  "vol=%d rate=%d dir=%s\n", (int)g_mercury_enabled, p483_prev,
                  g_mercury_installed, MCRY_VOL, g_audio_sample_rate_hz, mx68k_mercury_dir());
    }
    /* P488: MIDI 相互排他ラッチ。Mercury 側ラッチの直後に置く
     * (g_mercury_installed が確定した後に読む必要があるため)。
     * 呼出順序 Cleanup → Config.MIDI_SW 更新 → Init は入れ替え不可:
     *   MIDI_Cleanup() は「更新前の」Config.MIDI_SW を見てリセット送出要否を決め
     *   (midi.c:363)、MIDI_Init() は「更新後の」Config.MIDI_SW を見てデバイスを
     *   開くか inactive にするかを決める(midi.c:319,334)。
     * MIDI_Cleanup() の全処理は if(hOut)/if(hIn) の内側(midi.c:356-372)で、
     * hOut/hIn は BSS の 0 初期化グローバルなので未初期化状態での呼出も no-op。 */
    {
        int p488_prev = g_midi_installed;
        int p488_conflict = (g_midi_enabled && g_mercury_installed);
        g_midi_installed = (g_midi_enabled && !g_mercury_installed) ? 1 : 0;
        if (p488_conflict) {
            debug_log("[P488-MIDI] init: enabled=1 but mercury_installed=1 -> "
                      "forced OFF (shared IRQ4, mutual exclusion)\n");
        }
        MIDI_Cleanup();                          /* 常に呼ぶ(安全性確認済み)。旧 Config.MIDI_SW を見る */
        /* P490: Config.MIDI_Reset も Config.MIDI_SW と全く同じ順序制約の対象。
         * MIDI_Cleanup() のリセット送出ガードは midi.c:363 で
         * `Config.MIDI_SW && Config.MIDI_Reset` の両方を読むため、Cleanup は
         * 「更新前の(=直前まで実際に使われていた)」設定でリセット送出可否を決める。
         * これは意図した設計 — 撤収時のリセット送出は「今まさに閉じようとしている
         * 旧セッションの設定」に従うのが正しく、これから適用する新設定に従うのは
         * 誤り。よって Config.MIDI_Reset / Config.MIDI_Type の更新は
         * MIDI_Cleanup() より後・MIDI_Init() より前に置く。 */
        Config.MIDI_SW    = g_midi_installed ? 1 : 0;
        Config.MIDI_Reset = g_midi_reset_enabled ? 1 : 0;
        /* MIDI_Init() 冒頭の MIDI_SetModule()(midi.c:303→212-218)が
         * Config.MIDI_Type を読むため、Init より前に設定する。 */
        Config.MIDI_Type  = g_midi_reset_type;
        MIDI_Init();                             /* 常に呼ぶ。新 Config.MIDI_SW を見る */
        debug_log("[P488-MIDI] init: enabled=%d installed_prev=%d installed=%d "
                  "mercury_installed=%d\n", (int)g_midi_enabled, p488_prev,
                  g_midi_installed, g_mercury_installed);
        debug_log("[P490-MIDIRST] init: MIDI_SW=%d MIDI_Reset=%d MODULE=%d -> reset_called=%d\n",
                  Config.MIDI_SW, Config.MIDI_Reset, (int)MIDI_MODULE,
                  (Config.MIDI_SW && Config.MIDI_Reset) ? 1 : 0);
        /* P490: MIDI_Init() は(直前の MIDI_Cleanup() で hOut/hIn が 0 に戻るため)
         * 毎回 midOutChg(0,0)/midInChg(0) で index 0 のデバイスを開く。
         * 保存済みのデバイス選択をここで再適用する(index 0 なら no-op)。 */
        if (g_midi_installed) {
            if (g_midi_out_device_index > 0) midOutChg((uint32_t)g_midi_out_device_index, 0);
            if (g_midi_in_device_index  > 0) midInChg((uint32_t)g_midi_in_device_index);
        }
    }
    /* P493: 内蔵 SRAM 64KB 化の配線確定(設定値 g_sram_64k_enabled → テーブル差替)。
     * 上位 48KB($ED4000-$EDFFFF, index 0x6A-0x6F)のみが対象で、低位 16KB には
     * 触れない。sram_ext.dat の読み込みはプロセス内で一度だけ。 */
    sram_ext_install_table(g_sram_64k_enabled);
    sram_ext_load(mx68k_support_dir());
    /* P642: Windrv の配線確定(設定値 → g_windrv_installed)。マウントルートの
     * realpath() 正規化に失敗した場合も未装着へ倒れるため、ここを通れば
     * 「装着 = 検証済みルートが必ず存在する」が保証される。 */
    windrv_init();
    IRQH_Init();
    m68000_init();

    // Load SRAM if present
    SRAM_Init();
#if P142D_SRAM_INIT
    /* P142d(原型): align SRAM init with the fully-working reference MPX68K.
       MX68K Core SRAM_Init leaves SRAM=0xFF (its File_OpenCurDir load fails).
       MPX68K zeroes SRAM and presets a few fields, leaving $ED0000 = 0x00
       (invalid signature) so the IPLROM self-heals SRAM from ROM and boots.
       P505 (D-46): その「署名を無効のまま残して IPL-ROM に自己修復させる」
       設計が、Hard Reset 直後に Bridge が書いた HD_MAX 等を自己修復が 0 で
       上書きしてしまう競合を生んでいた。シードを sram_seed_defaults() に
       集約し、署名を含む 91 バイトを IPL-ROM 内蔵デフォルトテーブルから
       直接複写することで自己修復自体が発火しないようにした(詳細は
       sram_seed_defaults() のコメントと .mx68k_cycles/P504_verify_inv.md)。
       P202: after this seed, sram.dat is loaded with a signature check (below):
       a valid saved SRAM overrides the seed to restore the user's switch
       settings; an invalid signature / missing file keeps the seed. The former
       pre-P200 stall came from correctly matching the signature and then
       adopting settings (SASI/SCSI boot) the emulator could not yet honor; P200
       added SASI HDD support, so a valid saved SRAM now boots correctly. */
    sram_seed_defaults();

    /* P202: MPX68K 同型の SRAM 永続化ロード。シード後に保存 SRAM があり署名が
       有効なら生 16KB で上書き = switch 設定(HDD台数/ブートデバイス/メモリ)を復元。
       署名無効/サイズ不正/ファイル無しならシードのまま(安全側フォールバック)。
       署名 = SRAM 起動アドレス guest $ED0010-13 == 0x00ED0100(実 sram.dat 実測値。
       SRAM[] は adr^1 のバイトスワップ表現)。 */
    {
        const char* sram_home = getenv("HOME");
        if (sram_home) {
            char sram_path[1024];
            snprintf(sram_path, sizeof(sram_path),
                     "%s/Library/Application Support/MX68K/sram.dat", sram_home);
            FILE* sram_fp = fopen(sram_path, "rb");
            if (sram_fp) {
                uint8_t sram_tmp[0x4000];
                size_t sram_n = fread(sram_tmp, 1, 0x4000, sram_fp);
                fclose(sram_fp);
                if (sram_n == 0x4000 &&
                    sram_tmp[0x10^1]==0x00 && sram_tmp[0x11^1]==0xED &&
                    sram_tmp[0x12^1]==0x01 && sram_tmp[0x13^1]==0x00) {
                    memcpy(SRAM, sram_tmp, 0x4000);
                    debug_log("[P202-SRAM] loaded sram.dat (valid signature 0x00ED0100)\n");
                } else {
                    debug_log("[P202-SRAM] sram.dat rejected (size=%zu or bad sig) -> seed kept\n", sram_n);
                }
            }
        }
    }
#else
    const char* home = getenv("HOME");
    if (home) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/Library/Application Support/MX68K/sram.dat", home);
        FILE* fp = fopen(path, "rb");
        if (fp) {
            (void)fread(SRAM, 1, 0x4000, fp);
            fclose(fp);
        }
    }
#endif

#if P65_SRAM_FIX_ENABLE
    {
        if (SRAM[0x1e] == 0 && SRAM[0x1f] == 0 &&
            SRAM[0x20] == 0 && SRAM[0x21] == 0) {
            SRAM[0x1e] = SRAM[0x1f] = SRAM[0x20] = SRAM[0x21] = 0xFF;
            debug_log("[P65-SRAM-FIX] seeded SRAM[0x1e..0x21]=0xFFFFFFFF\n");
        }
    }
#endif /* P65_SRAM_FIX_ENABLE */

#if P220_PROBE
    {
        /* P220 (a): value after the seed + sram.dat load, before our reset_hard
         * write — the pre-write baseline. */
        uint32_t v = ((uint32_t)SRAM[0x08^1] << 24) |
                     ((uint32_t)SRAM[0x09^1] << 16) |
                     ((uint32_t)SRAM[0x0A^1] << 8)  |
                     ((uint32_t)SRAM[0x0B^1]);
        debug_log("[P220-MEMSIZE] a init post-sramdat mb=%d $ED0008=0x%08x\n",
                  g_memory_size_mb, v);
    }
#endif
    audio_ring_init();
    mx68k_reset_hard();

#if P53_ENABLE && P53_ATEXIT_ENABLE
    /* P53 — register atexit summary helper exactly once per process.
     * mx68k_atexit_summary emits [P52-SUMMARY] etc. without freeing memory,
     * so it is safe regardless of whether mx68k_shutdown has already run.
     * See /tmp/mx68k_P53_plan.md §3.2 / Edit B4(ii). */
    if (!s_p53_atexit_registered) {
        if (atexit(mx68k_atexit_summary) == 0) {
            s_p53_atexit_registered = 1;
            debug_log("[P53-INIT] atexit registered=1\n");
        } else {
            debug_log("[P53-INIT] atexit registered=0 (registration failed)\n");
        }
    }
#endif

    debug_log("[MX68K] mx68k_init() returning 0\n");
    return 0;
}

/* P53 — atexit-safe summary helper. Emits SUMMARY tags only; does NOT free
 * memory or close files. Belt-and-braces safety net for any future codepath
 * that reaches libc exit(3) without going through mx68k_shutdown.
 * See /tmp/mx68k_P53_plan.md §3.2 / Edit B2. */
void mx68k_atexit_summary(void) {
#if P53_ENABLE && P53_ATEXIT_ENABLE
    /* debug_log auto-reopens the file via debug_log_init if it was closed. */
    debug_log("[P53-ATEXIT-FIRE] enter\n");
  #if P51B_ENABLE
    m68000_p51b_dump_summary();
  #endif
  #if P52_ENABLE
    m68000_p52_dump_summary();
  #endif
  #if P57A_ENABLE
    m68000_p57a_dump_summary();
  #endif
    if (debug_log_file) {
        fflush(debug_log_file);
    }
    debug_log("[P53-ATEXIT-FIRE] exit\n");
    if (debug_log_file) {
        fflush(debug_log_file);
    }
#endif
}

int mx68k_p53_appdelegate_enabled(void) {
#if P53_ENABLE && P53_APPDELEGATE_ENABLE
    return 1;
#else
    return 0;
#endif
}

int mx68k_p53_sigsrc_enabled(void) {
#if P53_ENABLE && P53_SIGSRC_ENABLE
    return 1;
#else
    return 0;
#endif
}

/* P53 — Swift→debug.log marker bridge (Code Review C-2).
 * NSLog only writes to Apple Unified Logging, never to debug.log, so any
 * Swift-side marker that needs to appear in debug.log MUST route through
 * these helpers. All three internally call debug_log, which holds
 * debug_log_mutex and is safe from any thread. */
void mx68k_log_delegate_fire(void) {
    debug_log("[P53-DELEGATE-FIRE] applicationWillTerminate entered\n");
}

void mx68k_log_sig_catch(const char* signame) {
    if (!signame) signame = "?";
    debug_log("[P53-SIG-CATCH] sig=%s rerouting via NSApp.terminate\n", signame);
}

void mx68k_log_marker(const char* msg) {
    if (!msg) return;
    debug_log("%s\n", msg);
}

void mx68k_shutdown(void) {
#if P53_ENABLE
    /* P53 idempotency guard — absorbs duplicate calls from .onDisappear,
     * applicationWillTerminate, atexit (atexit calls mx68k_atexit_summary,
     * not this, but defense-in-depth). File-static flag, reset in
     * mx68k_init for forward compat with re-init.
     * See /tmp/mx68k_P53_plan.md §3.1 / Edit B3. */
    if (s_p53_shutdown_done) {
        debug_log("[P53-SHUTDOWN-IDEMPOTENT] skip (already done)\n");
        return;
    }
    s_p53_shutdown_done = 1;
#endif

#if P51B_ENABLE
    /* P51-B Edit E (I-3 adopted, Plan §5.5 / §9.6): emit session-end summary
     * before freeing buffers and closing the log file. */
    m68000_p51b_dump_summary();
#endif
#if P52_ENABLE
    /* P52 summary at session end. /tmp/mx68k_P52_plan.md §7.7. */
    m68000_p52_dump_summary();
#endif
#if P57A_ENABLE
    /* P57-A summary at session end (Plan §7 Edit F-2, P52 非依存; Code Major-4). */
    m68000_p57a_dump_summary();
#endif
    /* P483: Mercury 窓アクセスの分母(モニタ B のサマリ)をセッション終了時にも吐く。
     * 「pass が 0」だったときに blocked が非 0 か 0 かで解釈が一意に決まる。 */
    mx68k_p483_dump_mcry_counters("shutdown");
    /* P640: P637 (D-67 切り分けプローブ) の要約行をセッション終了時に必ず 1 行出す。
     * P637 の periodic トリガは「対象アドレスに一致したアクセス」の末尾からしか
     * 呼ばれないため、これが無いと stat_rd_b/w・err1・ch3_ccr_wr(総数)が
     * 「0 件」なのか「発火条件を満たさなかった」のか区別できない。
     * 宣言は P634 / P636 と同じくファイル先頭の独立 extern (:474 付近)。
     * g_mercury_installed ゲート: P637 のカウンタ自体がこのゲート内でのみ
     * 更新されるため、未装着時は無意味な行を出さない (Mercury 未装着時の
     * コストゼロを維持する P637 の既存方針を踏襲)。 */
    if (g_mercury_installed) {
        p637_ch3_dump_summary("shutdown");
    }
    ensure_app_support_dir();
    /* P202: save SRAM on shutdown. Paired with the signature-checked load in
       mx68k_init (P202), this persists the user's switch settings (HDD count,
       boot device, memory) across app restarts. */
    const char* home = getenv("HOME");
    if (home) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/Library/Application Support/MX68K/sram.dat", home);
        FILE* fp = fopen(path, "wb");
        if (fp) {
            fwrite(SRAM, 1, 0x4000, fp);
            fclose(fp);
        }
    }
    /* P493: 上位 48KB は別ファイル sram_ext.dat へ。16KB 設定時は何もしない。 */
    sram_ext_save(mx68k_support_dir());
    scsi_real_install_teardown();   // P275: 次回電源ONで内蔵SCSIの新ディスクパスを反映できるようにする
    /* P447 (D): 配線確定機種を cold(プロセス起動直後)と同じ 0 へ戻す。
     * これが無いと、直前セッションが SCSI 機だった場合に電源OFF-ON で
     * g_wired_machine_type==4 が残り、startEmulation の外付 CZ-6BS1 復元ループ
     * (mx68k_init より前に走る)が mx68k_scsi_insert() の
     * `if (g_wired_machine_type == 4) return -2;` で無言に拒否され、外付 SCSI
     * ディスクがプロセス再起動まで消える —(A)と同じ「リセット/電源で戻らない
     * グローバル状態」クラスの欠陥。cold 状態での排他は Swift 側 config 値ゲートが
     * 担う旨は mx68k_scsi_insert() 自身のコメントが明記しており、この 1 行は
     * 設計者が前提としていた不変条件を復元するだけで新しい前提を導入しない。 */
    g_wired_machine_type = 0;
    g_scsi_ext_board_wired = 0;   /* P506: 同上 — 新設した配線確定値も cold 状態へ戻す
                                   * (「リセット/電源で戻らないグローバル状態」という
                                   *  D-30/D-31 と同じ欠陥クラスを新変数だけ破らないため)。 */
    free(MEM); MEM = NULL;
    free(IPL); IPL = NULL;
    free(FONT); FONT = NULL;

    if (debug_log_file) {
        fclose(debug_log_file);
        debug_log_file = NULL;
    }
}

/* P195: マウス移動量/ボタンの累積器。定義は mx68k_mouse_move 付近(下方)だが、
 * mx68k_reset_hard() がリセット時にクリアするため、ここで宣言を前出しする。
 * mx68k_mouse_move はメインスレッド、Mouse_SetData はエミュスレッドから
 * 呼ばれるため atomic。 */
static _Atomic int s_mouse_dx  = 0;
static _Atomic int s_mouse_dy  = 0;
static _Atomic int s_mouse_btn = 0;      /* upstream の MouseStat 相当(bit0=左 / bit1=右) */

/* P340 fix1: 通しフレームカウンタ。実体の定義は下方(P294 render static 群)だが、
 * mx68k_reset_hard() がリセット時に0へ戻すため、ここで前方宣言を前出しする
 * (C の file-scope static は仮定義を複数置いても同一オブジェクトを指す)。 */
static int s_render_fb_call_count;

/* P504 (D-46 検証プローブ): 起動後 N フレーム時点で 2 標本目を採るためのカウンタ。
 * -1 = 無効(未アーム、または既に採取済み)。mx68k_reset_hard() 末尾でアームし、
 * mx68k_run_frame() が毎フレーム減算する。挙動変更なし(読み取り専用の診断)。 */
static int s_p504_boot_sample_countdown = -1;

/* P504 (D-46 検証プローブ): SRAM の起動関連フィールドの生バイトを 1 行出力する。
 * SRAM[] は adr^1 のバイトスワップ表現(既存の全 SRAM アクセスと同じ規約)。
 * 派生値・計算値は一切出さず、生バイトのみを 16 進でそのまま記録する。 */
static void p504_dump_sram_boot_fields(const char* tag) {
    debug_log("[P504-SRAMBOOT] tag=%s frame=%d "
              "sig=%02x%02x%02x%02x%02x%02x%02x%02x "
              "romvec=%02x%02x%02x%02x hdmax=%02x ed0018=%02x\n",
              tag, g_mx68k_frame_num,
              SRAM[0x00^1], SRAM[0x01^1], SRAM[0x02^1], SRAM[0x03^1],
              SRAM[0x04^1], SRAM[0x05^1], SRAM[0x06^1], SRAM[0x07^1],
              SRAM[0x0C^1], SRAM[0x0D^1], SRAM[0x0E^1], SRAM[0x0F^1],
              SRAM[0x5A^1], SRAM[0x18^1]);
}

// ---- reset ----
void mx68k_reset_hard(void) {
    debug_log("[MX68K] mx68k_reset_hard() START\n");

    /* P146/P221b: Config.XVIMode is never initialized in the MX Bridge
     * (Config={0} in winx68k_compat.c), so it must be set explicitly. The IPLROM
     * at guest FF009C does btst #0,$E8E00B; SysPort_Read returns 0xFF (bit0=1)
     * only for XVIMode==0, else 0xDC/0xFE (bit0=0). P146 pinned this to 3 to
     * match MPX68K and clear the frame~90 panic, but 3 made si misreport
     * 030/25MHz at every clock (D-15). P221b derives it from the configured
     * clock instead (10->0/0xFF, 16->1/0xFE, else 3/0xDC). In the MX Core,
     * Config.XVIMode is read ONLY at sysport.c:86 — it feeds no clock/cycle
     * timing, so this changes only the SYSPORT $E8E00B byte. The IPL clock
     * self-detect branch is byte-identical for 0xDC and 0xFE (both bit0=0). */
    /* P270: the P221b threshold was replaced by a per-clock-value lookup
     * (p270_derive_xvimode) because clock-up MOD kits break the assumption that
     * a higher raw clock means a higher machine ID class: RedZone(24MHz) is an
     * XVI mod that must stay 16MHz-class (0xFE), and EXPERT(17MHz) is a
     * 10MHz-class mod that must stay 0xFF. See that function's comment for the
     * XM6 si reference and the upstream sysport.c:88 "XVI or RedZone" note. */
    Config.XVIMode = p270_derive_xvimode(g_clock_mhz);

#if P221B_PROBE
    debug_log("[P221B-MACHINE] machine=%d clock=%d -> XVIMode=%d $E8E00B=0x%02x\n",
              g_machine_type, g_clock_mhz, Config.XVIMode,
              (unsigned)SysPort_Read(0xe8e00b));   /* guest-visible byte, read back */
#endif

    debug_log("[MX68K] Step 0: IPL=%p MEM=%p\n", (void*)IPL, (void*)MEM);
    debug_log("[MX68K] Step 1: P16-FIX byte-swap IPLROM -> MEM before C68k_Reset()\n");
    /* P16-FIX: copy IPLROM to MEM with 16-bit byte-swap so that rm16_main
     * (LE16 reads via *(uint16_t*)&MEM[addr]) yields the correct BE values
     * when C68k_Reset() reads the reset vectors from MEM[0..7].
     * The FETCH path (0xFC0000-0xFFFFFF) accesses IPL[] directly via FETCH_WORD
     * (big-endian byte read), so IPL[] itself must remain in BE order -- we only
     * swap the MEM copy.  wm_main/rm_main XOR-1 byte access and wm16_main/rm16_main
     * 16-bit access are both consistent with LE16 storage in MEM, so this swap is
     * safe for all subsequent RAM access in the 0x000000-0x01FFFF range. */
#if P51A_ENABLE
    if (IPL && MEM) {
        /* P51-A: Reset vector ($0..$7) MUST be byte-swapped for C68k_Reset().
         * Verified: Core/c68k/c68k.c:95-96 reads MEM[0]/MEM[4] via
         * C68k_Read_Long inside C68k_Reset(). Keep this explicit 4-pair swap. */
        MEM[0] = IPL[1]; MEM[1] = IPL[0];
        MEM[2] = IPL[3]; MEM[3] = IPL[2];
        MEM[4] = IPL[5]; MEM[5] = IPL[4];
        MEM[6] = IPL[7]; MEM[7] = IPL[6];
        /* P51-A: Clean the skipped vector-table area to zero. Required on the
         * manual hard-reset path (line 669) so stale BIOS writes from the
         * previous session don't leak through; harmless on cold boot. */
#if P135_ENABLE
        /* P135 Part1-B-memset: vector-skip 0 clear 実行を latch。範囲は [0x0008,0x07C0) で
         *   0x1FF6 を含まないが、reset_hard がこの frame に走ったことの記録として捕捉。 */
        {
            unsigned short b135 = p135_mem1ff6();
            memset(MEM + P51A_VECTOR_SKIP_BEGIN, 0,
                   P51A_VECTOR_SKIP_END - P51A_VECTOR_SKIP_BEGIN);
            p135_host_latch(1u, g_mx68k_frame_num, b135, p135_mem1ff6());
        }
#else
        memset(MEM + P51A_VECTOR_SKIP_BEGIN, 0,
               P51A_VECTOR_SKIP_END - P51A_VECTOR_SKIP_BEGIN);
#endif
        /* P51-A: skip $0008..$07BF (68k exception vectors + X68k IRQ area);
         * BIOS installs real handlers there. Aligns with MPX68K/upstream
         * px68k/px68k-libretro reset behavior (no IPL→MEM shadow). */
#if P135_ENABLE
        /* P135 Part1-B-shadow: IPL→MEM byte-swap loop 実行を latch。範囲は
         *   [P51A_VECTOR_SKIP_END,0x20000) で 0x1FF6 を含む (IPLROM の非0値を書く)。 */
        {
            unsigned short b135 = p135_mem1ff6();
            for (int i = P51A_VECTOR_SKIP_END; i < 0x20000; i += 2) {
                MEM[i]   = IPL[i + 1];
                MEM[i+1] = IPL[i];
            }
            p135_host_latch(2u, g_mx68k_frame_num, b135, p135_mem1ff6());
        }
#else
        for (int i = P51A_VECTOR_SKIP_END; i < 0x20000; i += 2) {
            MEM[i]   = IPL[i + 1];
            MEM[i+1] = IPL[i];
        }
#endif
        debug_log("[P51-A] vector area $0008..$07BF excluded from IPL shadow "
                  "(skip=%u bytes; manual-reset zeroed)\n",
                  (unsigned)(P51A_VECTOR_SKIP_END - P51A_VECTOR_SKIP_BEGIN));
    }
#else
    /* Original P16-FIX, kept for ablation testing (P51A_ENABLE=0). */
    if (IPL && MEM) {
        for (int i = 0; i < 0x20000; i += 2) {
            MEM[i]   = IPL[i + 1];
            MEM[i+1] = IPL[i];
        }
    }
#endif
    debug_log("[MX68K] Step 2: IRQH_Init() + m68000_reset()\n");
    IRQH_Init();
    m68000_reset();
    debug_log("[MX68K] Step 3: Memory_Init()\n");
    Memory_Init();
    debug_log("[MX68K] Step 4: MFP_Init()\n");
    MFP_Init();
    /* P143b: P81-A timer-stop REMOVED. MFP_Init sets warm-start TCDCR=0x77 (Timer C
     * running), matching the fully-working reference MPX68K. Zeroing TACR/TBCR/TCDCR
     * here stopped MFP Timer C, so the IPLROM's level-6 timer wait never fired -> the
     * icount-299949 interrupt-level divergence and downstream boot stall. */
    /* P27-FIX: VSYNC割り込みを事前有効化（IERB bit6）
     * MFP_Int(9)はirq=9→IERB bit6を使用（IERAのbit6はTimer A、IERBのbit6がGPIP7/VSYNC）。
     * IPL frame=0〜1でF-line例外ループし MFP初期化ルーチンに到達できないため、
     * ここでプリセットすることで VSYNC受理→SR IPL降下（6→0）→FDD IRQ1受理可能にする。
     * BIOSが後で上書きするため副作用なし。 */
    MFP[MFP_IERB] |= 0x40;  // GPIP7/VSYNC → IERB bit6
    MFP[MFP_IMRB] |= 0x40;  // VSYNCマスク解除
    debug_log("[MX68K] P27-FIX: MFP IERB/IMRB bit6 preset (VSYNC=0x%02X IMRB=0x%02X)\n",
              MFP[MFP_IERB], MFP[MFP_IMRB]);
    debug_log("[MX68K] Step 5: Keyboard_Init()\n");
    Keyboard_Init();
    debug_log("[MX68K] Step 6: Keymap_Init()\n");
    Keymap_Init();
    debug_log("[MX68K] Step 7: FDD_Reset() + FDC_Init()\n");
    FDD_Reset();
    FDC_Init();
    /* P686 (D-70): 設定値を配線確定値へラッチし、Core へ接続台数を伝える。
     * fdc_drives は FDC_Init() の memset 対象外だが、将来 FDC_Init() の
     * 呼び出し地点が増えたときに巻き戻りの窓を作らないよう、毎回明示的に
     * 呼び直す(意図を呼び出し側からも読めるようにする)。 */
    g_ext_fdd_wired = g_ext_fdd_enabled ? 1 : 0;
    FDC_SetDriveCount(g_ext_fdd_wired ? 4 : 2);
    debug_log("[P686-EXTFDD] init/reset: enabled=%d -> wired=%d drives=%d\n",
              (int)g_ext_fdd_enabled, g_ext_fdd_wired, g_ext_fdd_wired ? 4 : 2);
    p509_fdc_mirror_reset();   /* P509: Bridge 側コマンドミラーを fdc.c と同時に巻き戻す */
    /* P509 (D-48): P46-FIX-B の FDC_SetForceReady(1) をここから削除した。
     * P46-FIX-B が当初意図していたポート $E94005(ドライブレディレジスタ)には
     * fdc.ready は一切効いておらず(fdc.c:465-469 は FDD_IsReady() のみを参照する)、
     * その意味で目的を果たしていなかった。一方で fdc.c 内の他 7 箇所
     * (SenseDeviceStatus 等の NOT READY 抑止)には現に効いており、これが
     * D-48(起動可能デバイス皆無時のエラーメッセージ点滅)の直接の原因だった。
     * P46-FIX-B 導入時(コミット cb9a518、複数変更の一括投入で個別の導入理由の
     * 記録なし)の意図はコード読解からの推定にとどまる。
     * 実機の強制 READY は OPM レジスタ $1B bit6(CT2)経由の一時的なトリックで
     * あり、リセット時は OFF が正しい(XM6/XEiJ/px68k 本家/px68k-libretro の
     * 4 参照実装すべて一致)。 */
    debug_log("[MX68K] Step 7.5: TVRAM_Init()\n");
    TVRAM_Init();       /* P171: see mx68k_init — text pattern table init, was omitted. */
    debug_log("[MX68K] Step 8: GVRAM_Init()\n");
    GVRAM_Init();
    debug_log("[MX68K] Step 9: CRTC_Init()\n");
    CRTC_Init();
    debug_log("[MX68K] Step 10: Pal_Init()\n");
    Pal_Init();
    debug_log("[MX68K] Step 11: BG_Init()\n");
    BG_Init();
    debug_log("[MX68K] Step 12: IOC_Init()\n");
    IOC_Init();
    /* P144: P23/P24-FIX IOC preset REMOVED. IOC_Init() leaves IOC_IntStat=0
     * (interrupt-enable mask off) exactly like the fully-working reference
     * MPX68K; the guest IPLROM enables the IOC via IOC_Write($E9C001/$E9C003)
     * at the correct moment. The old preset predated the Timer-C fix (P143b):
     * when boot stalled before the IPLROM reached its own IOC-init, MX force-
     * enabled the mask so an FDD-insert would be visible. Post-Timer-C the
     * preset instead makes FDD_SetFDInt raise a spurious level-1 (IPL1)
     * interrupt (the fdd.c guard "IOC_IntStat & 2" becomes true with no real
     * device source pending), taken right after the MFP level-6 handler's RTE
     * (differential-trace icount 299989) -> guest jumps to uninitialized RAM
     * 0x0FFF20. Removing the preset matches MPX68K and eliminates that spurious
     * interrupt. See .mx68k_cycles/P144_{code,spec,plan}_inv.md. */

    /* P47-C-1/2: RTC_Init / PPI_Init are missing from this bridge but are
     * called by MPX68K (winx68k.cpp:291-292). Without RTC_Init the RTC regs
     * remain zero, causing IPL_ROM RTC reads to return undefined values
     * (potentially boot-magic mismatch -> panic terminus 0xff063c). PPI (8255)
     * drives joystick/printer ports that IPL probes during boot. */
    RTC_Init();
    debug_log("[MX68K] P47-C-1: RTC_Init() called\n");
    PPI_Init();
    debug_log("[MX68K] P47-C-2: PPI_Init() called\n");
    // P588 (D-53): PPI_Init() は ppi.PortC を 0x0b(bit4/5=low)へ戻すが、
    // GamePad_Write を呼ばないため Bridge 側の strobe キャッシュ(pad_strobe)が
    // 追随しない。直前に pad_strobe が high(0xff)だった場合、ハードリセット後も
    // GamePad_Read が pad_btn1(H側バンク)を返し続け、標準プロファイルでは
    // pad_btn1 が一切更新されないため方向・ボタン入力が全滅する
    // (`.mx68k_cycles/P587_gamepad_bug_inv.md` / `P588_plan.md` 参照)。
    // PPI_Init() が確立する low 状態へ明示的に再同期する(XM6 PPI::Reset() の
    // ppi.portc=0 と等価)。
    GamePad_Write(0, 0x00);
    GamePad_Write(1, 0x00);

    debug_log("[MX68K] Step 13: SCC_Init()\n");
    SCC_Init();
    /* P195: SCC_Init() は MouseX/MouseY/MouseSt をゼロにする。Bridge 側の累積器も
     * 併せてクリアしないと、リセット直後に古い移動量が配送されてしまう。 */
    atomic_store(&s_mouse_dx, 0);
    atomic_store(&s_mouse_dy, 0);
    atomic_store(&s_mouse_btn, 0);
    /* P340 fix1: 通しフレームカウンタをリセットで0へ戻す。診断プローブの
     * `fb_call_count > N`型ゲート(P305等)がリセット後に猶予期間なしで即発火し
     * 同期ディスクI/O暴走→フリーズを招くのを防ぐ防御的第二層。 */
    s_render_fb_call_count = 0;
    /* P535: 永続合成バッファはフレームをまたいで内容を保持するため、リセットでは
     * 明示的に消す(旧設計の毎フレーム memset が担っていた消去の代替)。
     * mx68k_reset_soft() は本関数へ委譲するので、ここ 1 箇所で両経路をカバーする。 */
    memset(s_compose_fb, 0, sizeof(s_compose_fb));
    debug_log("[MX68K] Step 14: SASI_Init()\n");
    SASI_Init();
    /* P502 (D-9): SASI fd キャッシュをハードリセットで確実に捨てる。この直後の
     * P447/P455 再適用ブロック(1400 行台)が Config.HDImage[] を書き換えるため、
     * 旧イメージの fd がここで残っていてはならない。mx68k_reset_hard() は
     * g_pending_hard_reset 経由(consume_pending_ops:2327 — P503 で
     * mx68k_run_frame() から抽出、mx68k_pump_pending() からも呼ばれる)か、
     * init / ステートロード時の
     * 機種不一致という単一スレッド区間からしか呼ばれない(Swift 側は
     * mx68k_schedule_hard_reset() のみ使用)ので、ここでの直接 close() は
     * エミュレーションスレッド以外と競合しない。 */
    sasi_io_cache_invalidate_all();
    sasi_bridge_install();
    /* P450 (D-30): 内蔵SCSI(SPC)をハードリセットのたびに破棄→再構築する。
     * scsi_real_install_construct() は `if (s_scsi_instance) return;` の一度きり
     * ガードを持つため、これが無いと SetDiskPath() の再投入も SCSI::Reset() も
     * 走らず、設定画面でのディスク差し替え/取り外しが ⌘R で反映されない。
     * ★挿入位置はここでなければならない — construct は直下の
     * scsi_in_bridge_install() の内部(scsi_in_bridge.c)から呼ばれるため、
     * teardown はその直前でなければ「破棄したまま再構築されない」窓が残る。
     * ★P275 コメント(scsi_spc_bridge.cpp の teardown 直上)の「リセットでは
     * 機器構成を再認識しない」という判断を意図的に反転する。
     * ★★これは実機/XM6 の実際の挙動からの *意図的な逸脱* である —
     * ユーザーの XM6 実機確認(2026-07-28)により、XM6 もディスク構成の変更は
     * 電源 OFF/ON でのみ反映し、リセットでは反映しないことが判明している。
     * それでもこの逸脱を選ぶ理由は 2 つ: (1) Docs/05 §5.2「3. ハードリセットで
     * 反映」という自プロジェクトの受入基準、(2) P447 で SASI 側だけが既に
     * ⌘R 反映になっており、SASI と SCSI の非対称を解消する必要があること。
     * 逸脱を承知のうえでの設計判断であり、「XM6 準拠」ではない。
     * 機種 SASI の場合: 直下の scsi_in_bridge_install が即 return するため
     * teardown だけが走り SPC が解放される = P268 の機種排他が強化される。
     * 安全性: Cleanup() が dcache->Save() を経て閉じるため書き戻し喪失はなく、
     * teardown は NULL ガードで冪等。 */
    {
        bool p450_had = scsi_real_instance_exists();
        debug_log("[P450-SCSIRECON] teardown+reconstruct: had_instance=%d machine=%d rom_loaded=%d\n",
                  p450_had ? 1 : 0, g_machine_type, s_scsi_in_rom_loaded ? 1 : 0);
    }
    scsi_real_install_teardown();
    /* P510 (D-32): 移植済み XM6 SPC 実装へ「どの SCSI 構成として振る舞うか」を
     * 伝える唯一の呼び出し箇所。★この位置(直下の scsi_in_bridge_install() より
     * 前)でなければならない — scsi_in_bridge_install() はゲート成立時にその場で
     * scsi_real_install_construct() → SCSI::Reset() を走らせ、Reset() が
     * Memory::GetMemType() を読むため、後から設定したのでは内蔵 SCSI 機で
     * 前回リセットの残留値が使われてしまう。
     * ★毎リセット無条件に 3 モードのいずれかを明示設定する(SASI 機で呼ばれず
     * 値が残る = P447/D-31 型の片道書換え欠陥の回避)。
     * ★材料は 3 つとも設定由来の生値で、この時点(配線確定前)でも参照可能:
     *   g_machine_type(4=内蔵SCSI機)・g_scsi_ext_board_installed(外付装着設定)・
     *   s_scsi_ext_rom_loaded(SCSIEXROM.DAT ロード成否)。下行以降の
     *   g_wired_machine_type / g_scsi_ext_board_wired はまだ前回値なので使えない。 */
    {
        int p510_mode;
        if (g_machine_type == 4) {
            p510_mode = P510_SCSI_MODE_INT;
        } else if (g_scsi_ext_board_installed && s_scsi_ext_rom_loaded) {
            p510_mode = P510_SCSI_MODE_EXT;
        } else {
            p510_mode = P510_SCSI_MODE_SASI;
        }
        debug_log("[P510-MODE] scsi mode=%d (machine=%d ext_installed=%d ext_rom_loaded=%d)\n",
                  p510_mode, g_machine_type,
                  g_scsi_ext_board_installed ? 1 : 0, s_scsi_ext_rom_loaded ? 1 : 0);
        scsi_real_set_scsi_mode(p510_mode);
    }
    scsi_in_bridge_install(g_machine_type); /* P247 Stage1: SCSI機種のみindex0x4bを差替(パススルー) */
    /* P447 Phase 1 診断プローブ [P447-SASIWIRE]: 配線確定直後・g_wired_machine_type
     * 上書き前に無条件で1行出力する。この位置でなければならない理由は2つ —
     * (1) sasi_bridge_install/scsi_in_bridge_install の対が走り終えた後でないと
     *     MemRead/WriteTable[0x4b] の「配線確定値」を観測できない、
     * (2) 下行の代入より前でないと wired_prev(直前セッションの機種)が失われる。 */
    scsi_in_bridge_log_slots(g_machine_type, g_wired_machine_type);
    g_wired_machine_type = g_machine_type;  /* P268: 配線確定機種を記録(insert バックストップ用) */
    g_scsi_ext_board_wired = (g_scsi_ext_board_installed && g_wired_machine_type != 4) ? 1 : 0;
    /* P506: 直前行の代入によりこの時点で g_wired_machine_type == g_machine_type は
     * 同値だが、「配線確定値でゲートする」という本サイクルの主張と字面を一致させ、
     * リポジトリ内の既存判定(:1425/1461/4456/4506/4571 相当)と表記を揃えるため
     * g_wired_machine_type を使う。機種排他項(!= 4)は XM6 の scsi.type が
     * 内蔵SCSI機なら必ず外付け非扱いになる排他的な意味論と一致させるためのもの。 */
    /* P447 (C2'): 配線確定機種に合わせて Config.HDImage[] を再適用する。
     * ⌘R / 電源OFF-ON / ステート復元の3つのリセット入口はいずれもここへ集約
     * されるので、この1か所だけで全経路のカバレッジが取れる。
     * mx68k_hdd_insert()/mx68k_hdd_eject() は呼ばず Config.HDImage[] へ直接代入
     * する — シャドウへの再帰書込みと、無用な debug_log 重複を避けるため。 */
    if (g_wired_machine_type == 4) {
        /* SCSI 機: 内蔵 SASI は配線されない(P268 の機種排他)。パスを外して
         * 排他を配線と同時に成立させる。★シャドウは絶対にクリアしない —
         * SASI 機へ戻したときの復元元がそこにあることが本修正の目的そのもの。
         * P455: 2 ユニット直書き → 全 MX68K_SASI_UNIT_COUNT ユニットのループ。 */
        for (int unit = 0; unit < MX68K_SASI_UNIT_COUNT; unit++) {
            Config.HDImage[unit * 2][0] = '\0';
        }
        p455_log_hdd_reapply("SCSI", g_wired_machine_type);
    } else {
        for (int unit = 0; unit < MX68K_SASI_UNIT_COUNT; unit++) {
            int idx = unit * 2;
            if (s_sasi_hdd_path[unit][0] != '\0') {
                strlcpy(Config.HDImage[idx], s_sasi_hdd_path[unit],
                        sizeof(Config.HDImage[idx]));
            } else {
                Config.HDImage[idx][0] = '\0';
            }
        }
        p455_log_hdd_reapply("SASI", g_wired_machine_type);
    }
    /* P450: XM6 SASI::Reset() 相当のメモリスイッチクリア。
     * ★配線(scsi_in_bridge_install)の後でなければならない — 内蔵 SCSI が
     * 実際に配線されたか(= SCSI::Reset() が 'V' を書いたか)が確定するのは
     * その後。機種 SCSI で構築が成立した場合はここは何もしない(scsi_present)。
     *
     * ★scsi_present は「SCSI I/F が装備されているか」= 機種/ボード構成のみで
     * 決まる述語。XM6 SASI::Reset() の sasi.scsi_type が Memory::GetMemType() だけ
     * から決まり、ディスク在席にも ROM ロード有無にも依存しないのと同じ意味論
     * (XM6 vm/sasi.cpp:133-160)。
     * ★これを「機種 SASI ならクリア」と単純化してはならない — SASI 機 + 外付
     * CZ-6BS1 の構成で $ED006F='V' を消してしまい外付 SCSI 起動が壊れる。
     * この OR の第2項は load-bearing であり、将来のリファクタで機種判定のみへ
     * 単純化することを禁ずる(Docs/09 D-30 の解決記述にも同旨を明記)。 */
    {
        bool p450_scsi_present =
              (g_wired_machine_type == 4)          /* 内蔵 SCSI 機 (XM6 scsi_type=2) */
           || g_scsi_ext_board_installed;          /* 外付 CZ-6BS1 装着 (XM6 scsi_type=1) */
        sasi_bridge_apply_memsw(g_wired_machine_type,
                                mx68k_get_memsw_auto_update(),
                                p450_scsi_present);
    }
    /* P456 (D-33): SRAM $ED005A(SASI ドライブ index の排他的上限値)の自動同期。
     * XM6 SASI::Reset() の SetMemSw(0x5a, ...) に相当。
     *
     * ★上の P450 ブロックとは完全に独立した別ブロック・別関数である。
     *   p450_scsi_present は**流用しない** — $ED005A の条件は XM6 の
     *   scsi_type < 2(= SASI I/F 搭載か)であり、$ED006F/70/71 の
     *   scsi_type == 0 とは別の述語。外付 CZ-6BS1 装着の SASI 機
     *   (scsi_type==1)では $ED005A を書かねばならず、p450_scsi_present を
     *   使うと $ED005A=0 になって全 SASI が沈黙する。
     *   (Docs/09 D-30 の「将来 Pxx のレビュー必須条件」= p450_scsi_present の
     *    OR 合成と memsw 適用ロジックへの不干渉 — を構造的に守るため、
     *    上のブロックには一切触れていない。)
     * ★在席依存(unit_mask)はこの「値算出」に閉じており p450_scsi_present には
     *   波及しない。$ED005A は XM6 でも sasi_drives という**量**なので在席に
     *   依存するのが正しい一方、$ED006F/70/71 は「I/F が装備されているか」という
     *   **構成**の述語で在席に依存してはならない。将来のリファクタで
     *   p450_scsi_present に unit_mask を混ぜることは D-30 違反であり禁止。
     *
     * ★この位置でなければならない理由:
     *   (i)  Config.HDImage[] 再投入(上の P447/P455 ブロック)の後なので在席が確定、
     *   (ii) scsi_in_bridge_install() の後なので配線機種が確定、
     *   (iii) CPU 実行開始前なので、IPL-ROM が f:0x10D34 で行う
     *         `MOVE.B $ED005A,$0CB4`(ハードリセット直後の唯一の読み取り)に間に合う。 */
    {
        bool p456_sasi_if_present = (g_wired_machine_type != 4);  /* XM6 scsi_type < 2 */
        sasi_bridge_apply_sasi_count(g_wired_machine_type,
                                     mx68k_get_memsw_auto_update(),
                                     p456_sasi_if_present,
                                     mx68k_sasi_unit_mask());
    }
    /* P508 (D-47 症状①): SRAM $ED000C-0F(ROM 起動ハンドル)を配線構成へ同期。
     *
     * ★この位置でなければならない理由は P456 ブロックと同じ 3 点:
     *   (i)   Config.HDImage[] 再投入の後なので在席が確定、
     *   (ii)  scsi_in_bridge_install() / 上の g_scsi_ext_board_wired 代入の後なので
     *         配線確定機種と外付ボードの配線確定装着状態が確定、
     *   (iii) CPU 実行開始前なので、IPL-ROM の ROM 起動プローブに間に合う。
     *
     * ★P505 sram_seed_defaults()(mx68k_init()/mx68k_sram_clear() から呼ばれ、
     *   $ED000C を含む 91 バイトを IPL 既定値でシードする)との関係: 本呼び出しは
     *   reset_hard のタイミングなので必ずシードより後に走る。「P505 がシードした
     *   IPL 既定値($00FC0000)を、実際の配線構成に応じて上書きする」という関係で
     *   あり、役割が異なる(シード=初期値設定 / 本処理=配線確定後の実値設定)ため
     *   競合しない。
     *
     * ★判定は呼び出し先が持つ(P450/P456 と違い、材料が 4 つとも既存の配線確定値
     *   またはロードフラグそのままなので、ここでの合成は行わない)。 */
    sasi_bridge_apply_rom_boot_handle(g_wired_machine_type,
                                      g_scsi_ext_board_wired != 0,
                                      s_scsi_in_rom_loaded,
                                      s_scsi_ext_rom_loaded);
    debug_log("[MX68K] Step 15: SCSI_Init()\n");
    SCSI_Init();
    /* P269/P510: 外付けSCSI($EA0000, index0x50)。ゲート成立時は移植済み SPC への
     * サブデコード分配、不成立時は従来どおり Core 観測フック。
     * ★モード(scsi_real_set_scsi_mode)はここでは呼ばない — 上の
     * scsi_in_bridge_install() より前で 1 回だけ設定済みであり、唯一の呼び出し箇所
     * という前提を崩さないため。 */
    scsi_ext_bridge_install(g_scsi_ext_board_wired,
                            s_scsi_ext_rom_loaded ? 1 : 0,
                            g_wired_machine_type);
    debug_log("[MX68K] Step 16: DMA_Init()\n");
    DMA_Init();
    debug_log("[MX68K] Step 17: ADPCM_Init(%d)\n", g_audio_sample_rate_hz);
    ADPCM_Init(g_audio_sample_rate_hz);   /* P626: 設定サンプルレート(既定 44100) */
    ADPCM_SetVolume((uint8_t)g_adpcm_volume);   /* P210: reset-path self-containment (ADPCM_Init does not
                            * touch ADPCM_VolumeShift; harmless re-apply).
                            * P512: 固定値 15 ではなくユーザー設定値を再適用する
                            * — 固定値のままだとハードリセット(⌘R・機種/メモリ変更)の
                            * たびに ADPCM 音量スライダの設定が黙って既定値へ戻る。 */
    debug_log("[MX68K] Step 18: OPM_SetRate(4000000, %d) + OPM_Reset()\n", g_audio_sample_rate_hz);
    /* P626 (D-62): ハードリセット経路では従来 OPM のレートが一切更新されず、OPM は
     * プロセス起動時の mx68k_init() でのみ初期化されていた(既存の非対称性)。
     * これが無いと、サンプルレート設定変更後のハードリセットで ADPCM / Mercury だけが
     * 新レートへ切り替わり OPM だけ旧レートに残る = チップ間のピッチ不整合になる。
     * ★ここで OPM_Init() を使ってはならない: (1) fmg_wrap.cpp:95 が既存 opm を
     *   delete せず new MyOPM() するためリセットのたびに確定的にメモリリークする
     *   (Mcry_Init が直前の Mcry_Cleanup() で回避している同種の問題への対策が
     *    OPM_Init には無い)、(2) opm.cpp:37-47 の OPM::Init() が必ず SetVolume(0) を
     *   呼ぶためリセットのたびに OPM が無音化する(P512 が OPM_Reset() +
     *   明示的 OPM_SetVolume() の組合せへ置き換えて一度回避した問題そのもの)。
     * OPM_SetRate() は opm.cpp:52-61 のとおり clock / pcmrate / rate 代入と
     * RebuildTimeTable() のみで、new / SetVolume / Reset への波及が無い。
     * 呼出し順序は OPM::Init() 自身の内部順序(SetRate → Reset → SetVolume)に倣い、
     * 既存の OPM_Reset() / OPM_SetVolume() より前に置く。 */
    OPM_SetRate(4000000, g_audio_sample_rate_hz);
    OPM_Reset();
    OPM_SetVolume((uint8_t)g_opm_volume);   /* P512: ADPCM 側と対称な自己完結性。現状は
                            * OPM::Reset() が fmvolume に触れない(SetVolume(0) は OPM::Init()
                            * 側のみ)ため実質 no-op だが、その Init/Reset 実装分離という
                            * 偶然への依存を無くす。 */
    p479_opm_shadow_reset();   /* P479: keep the Bridge-side OPM shadow in sync with the chip */
    debug_log("[MX68K] Step 19: Mcry_Init()\n");
    /* P483: init 経路と同一(装着ラッチ + Cleanup/Init(path)/SetVolume)。
     * 装着設定はここで初めて配線に反映される(設定「適用」だけでは変わらない)。
     * モニタ B のサマリも、カウンタをリセットせずここで一度吐く。 */
    mx68k_p483_dump_mcry_counters("reset");
    {
        int p483_prev = g_mercury_installed;
        g_mercury_installed = g_mercury_enabled ? 1 : 0;
        Mcry_Cleanup();
        Mcry_Init(g_audio_sample_rate_hz, mx68k_mercury_dir());   /* P626: 設定サンプルレート */
        Mcry_SetVolume(MCRY_VOL);
        p491_mercury_opn_shadow_reset();   /* P491: keep the Bridge-side OPN shadow in sync with the chip */
        p634_lrck_reset();                 /* P634 (D-43): init 経路と同一 — Mcry_Init が
                                            * Mcry_LRTiming=0 にするのと同じ箇所で
                                            * 自走 LR 位相も 0 クリアする。 */
        debug_log("[P483-MERCURY] reset: enabled=%d installed_prev=%d installed=%d "
                  "vol=%d rate=%d dir=%s\n", (int)g_mercury_enabled, p483_prev,
                  g_mercury_installed, MCRY_VOL, g_audio_sample_rate_hz, mx68k_mercury_dir());
    }
    /* P488: init 経路と同一の MIDI 相互排他ラッチ(Mercury 側ラッチの直後)。
     * 装着設定はここで初めて配線に反映される(設定「適用」だけでは変わらない)。 */
    {
        int p488_prev = g_midi_installed;
        int p488_conflict = (g_midi_enabled && g_mercury_installed);
        g_midi_installed = (g_midi_enabled && !g_mercury_installed) ? 1 : 0;
        if (p488_conflict) {
            debug_log("[P488-MIDI] reset: enabled=1 but mercury_installed=1 -> "
                      "forced OFF (shared IRQ4, mutual exclusion)\n");
        }
        MIDI_Cleanup();                          /* 常に呼ぶ。旧 Config.MIDI_SW を見る */
        /* P490: init 経路と同一。Config.MIDI_Reset も MIDI_Cleanup() 側(midi.c:363)で
         * 「更新前の値」が読まれる — 撤収時のリセット送出は旧セッションの設定に従うのが
         * 正しいという意図した設計(init 経路の注記と対)。 */
        Config.MIDI_SW    = g_midi_installed ? 1 : 0;
        Config.MIDI_Reset = g_midi_reset_enabled ? 1 : 0;
        Config.MIDI_Type  = g_midi_reset_type;   /* MIDI_Init() 内の MIDI_SetModule() が読む */
        MIDI_Init();                             /* 常に呼ぶ。新 Config.MIDI_SW を見る */
        debug_log("[P488-MIDI] reset: enabled=%d installed_prev=%d installed=%d "
                  "mercury_installed=%d\n", (int)g_midi_enabled, p488_prev,
                  g_midi_installed, g_mercury_installed);
        debug_log("[P490-MIDIRST] reset: MIDI_SW=%d MIDI_Reset=%d MODULE=%d -> reset_called=%d\n",
                  Config.MIDI_SW, Config.MIDI_Reset, (int)MIDI_MODULE,
                  (Config.MIDI_SW && Config.MIDI_Reset) ? 1 : 0);
        /* P490: init 経路と同一 — MIDI_Init() が index 0 に戻すデバイス選択を再適用。 */
        if (g_midi_installed) {
            if (g_midi_out_device_index > 0) midOutChg((uint32_t)g_midi_out_device_index, 0);
            if (g_midi_in_device_index  > 0) midInChg((uint32_t)g_midi_in_device_index);
        }
    }
    /* P493: init 経路と同一の SRAM 64KB 配線確定(設定「適用」だけでは変わらず、
     * ここで初めて反映される)。sram_ext_load() は 2 回目以降 no-op なので、
     * ハードリセットでゲストが書いた上位 48KB の内容が巻き戻ることはない
     * (実機のバッテリバックアップと同じ挙動)。 */
    sram_ext_install_table(g_sram_64k_enabled);
    sram_ext_load(mx68k_support_dir());
    /* P642: init 経路と同一の Windrv 配線確定。設定「適用」だけでは変わらず、
     * ここで初めて反映される(Mercury/MIDI/SRAM64K と同じ規約)。開いていた
     * ホストファイル/検索コンテキストは windrv_init() 内で全て解放される。 */
    windrv_init();
    /* P494-②: 命令フェッチ用 Fetch[] の配線もここで確定させる。SRAM_Init()
     * (sram.dat 読込)・sram_ext_load() の完了後という順序をこの 1 箇所で
     * 自動的に満たすため、mx68k_init() 側には別途追加しない
     * (mx68k_init() は末尾で必ず mx68k_reset_hard() を呼ぶ)。 */
    sram_ext_install_fetch(g_sram_64k_enabled);

    /* P20-FIX: FALLBACK_VEC block removed.
     *
     * Root cause analysis showed that memset(MEM+8, 0, 0x3F8) + FALLBACK_VEC was
     * overwriting BIOS startup code at MEM[0x0008..0x03FF].  The X68000 BIOS ROM is
     * overlaid at address 0x000000 during cold boot, so P16-FIX correctly copies the
     * byte-swapped ROM into MEM[0x0000..0x1FFFF].  The region 0x0008..0x03FF holds
     * actual M68K instruction bytes that the CPU fetches and executes as part of the
     * BIOS initialisation sequence.  Replacing them with FALLBACK_VEC stubs caused the
     * CPU to spin through ORI.B #0,D0 loops instead of running real BIOS code, and
     * eventually caused SR=0x2700 to be pushed onto a stack that had grown into the
     * vector table area (0x00BC), corrupting the TRAP #15 vector and producing the
     * 0x27004C98 bad-PC values reported by P14-PCGUARD.
     *
     * The BIOS itself installs all exception vectors during its boot sequence; no
     * bridge-level patching of the vector table is needed or correct here. */

    /* P38-FIX-A: Timer D (vec#0x44, addr=0x110-0x113) / Timer C (vec#0x45, addr=0x114-0x117) に
     * RTEスタブを設置。MFP initregsにより両タイマーは初期状態で有効かつマスクなし
     * (IERB=0x3e, IMRB=0x3e, TCDCR=0x77)。IPLROM文字列残留値（"ram ", "term"等）が
     * 残った状態で発火すると不正ジャンプが発生し、-6バイト×N/フレームのスタックリークを
     * 引き起こす（テスト#39で確認: Timer Dは1フレームに約16回発火）。
     * P32-FIXと同一パターン（P29_RTE_STUB_ADDR = 0x000FFF00）を使用。
     * BIOSが正しいハンドラアドレスを設定するまでの間のみ有効。 */
    if (MEM) {
        /* RTEスタブ (0x4E73) はP32-FIXで設置済み（MEM[P29_RTE_STUB_ADDR]）。ここでは追記のみ。 */
        /* Timer D vec#0x44 (addr=0x110-0x113) にRTEスタブポインタを書き込む (LE16×2) */
        *(uint16_t*)&MEM[0x110] = (uint16_t)((P29_RTE_STUB_ADDR >> 16) & 0xFFFFU); /* 0x000F */
        *(uint16_t*)&MEM[0x112] = (uint16_t)( P29_RTE_STUB_ADDR        & 0xFFFFU); /* 0xFF00 */
        /* Timer C vec#0x45 (addr=0x114-0x117) にRTEスタブポインタを書き込む (LE16×2) */
        *(uint16_t*)&MEM[0x114] = (uint16_t)((P29_RTE_STUB_ADDR >> 16) & 0xFFFFU); /* 0x000F */
        *(uint16_t*)&MEM[0x116] = (uint16_t)( P29_RTE_STUB_ADDR        & 0xFFFFU); /* 0xFF00 */
    }
    debug_log("[MX68K] [P38-FIX-STUB] Timer D vec#0x44 (0x110) -> RTE stub at 0x%06x\n",
              P29_RTE_STUB_ADDR);
    debug_log("[MX68K] [P38-FIX-STUB] Timer C vec#0x45 (0x114) -> RTE stub at 0x%06x\n",
              P29_RTE_STUB_ADDR);
    /* P38-DIAG: Timer C/D RTEスタブ書き込み確認 */
    if (MEM) {
        uint32_t td_vec = (((uint32_t)*(uint16_t*)&MEM[0x110]) << 16) |
                           ((uint32_t)*(uint16_t*)&MEM[0x112]);
        uint32_t tc_vec = (((uint32_t)*(uint16_t*)&MEM[0x114]) << 16) |
                           ((uint32_t)*(uint16_t*)&MEM[0x116]);
        debug_log("[P38-DIAG-STUB] vec#0x44(TimerD)=0x%08x vec#0x45(TimerC)=0x%08x "
                  "(both expect 0x%06x)\n", td_vec, tc_vec, P29_RTE_STUB_ADDR);
        if (td_vec != P29_RTE_STUB_ADDR || tc_vec != P29_RTE_STUB_ADDR) {
            debug_log("[P38-DIAG-STUB] WARNING: Timer C/D stub mismatch!\n");
        }
    }

    /* P32-FIX: VSYNC例外ベクタ#0x46 (addr=0x118) にRTEスタブを設置（全Init()完了後）
     * P29-FIXを全Init()完了後の最後に移動する。
     * 旧位置（MFP_Init直後）ではKeyboard_Init〜Mcry_InitのいずれかのInit()が
     * MEM[0x118-0x11B]を上書きする可能性があった。
     * 全Init()完了後に設置することで、Init()による上書きを完全に防ぐ。
     * BIOSが正しいVSYNCハンドラを設定するまでの間のみ有効。 */
    if (MEM) {
        /* RTEスタブ (0x4E73) をMEM[0x0FFF00]に配置 (LE16格納) */
        *(uint16_t*)&MEM[P29_RTE_STUB_ADDR] = 0x4E73U;
        /* ベクタテーブル 0x118〜0x11B に P29_RTE_STUB_ADDR (0x000FFF00) を書き込む (LE16×2) */
        *(uint16_t*)&MEM[0x118] = (uint16_t)((P29_RTE_STUB_ADDR >> 16) & 0xFFFFU); /* 0x000F */
        *(uint16_t*)&MEM[0x11A] = (uint16_t)( P29_RTE_STUB_ADDR        & 0xFFFFU); /* 0xFF00 */
    }
    debug_log("[MX68K] P32-FIX (P29-FIX moved): VSYNC vector#0x46 -> RTE stub at 0x%06x\n",
              P29_RTE_STUB_ADDR);
#if P51A_ENABLE && P51A_VEC23_SAFETY
    /* P51-A safety: pre-install RTE-stub pointer for bus error (vec#2) and
     * address error (vec#3). After the P51-A shrink, MEM[$8..$F] starts at
     * zero (instead of garbage IPL bytes). If either exception fires before
     * BIOS installs them, the CPU would otherwise jump to PC=0. Pointing
     * them at the P29 RTE stub ($0FFF00 = RTE) lets execution return safely
     * the few times this could happen in early boot. BIOS overwrites these
     * later -- no effect on the real boot path.
     * See /tmp/mx68k_P51A_plan.md §5.2. */
    if (MEM) {
        /* vec#2 (Bus Error) at $0008..$000B -> RTE stub (LE16 x2) */
        *(uint16_t*)&MEM[0x008] = (uint16_t)((P29_RTE_STUB_ADDR >> 16) & 0xFFFFU);
        *(uint16_t*)&MEM[0x00A] = (uint16_t)( P29_RTE_STUB_ADDR        & 0xFFFFU);
        /* vec#3 (Address Error) at $000C..$000F -> RTE stub (LE16 x2) */
        *(uint16_t*)&MEM[0x00C] = (uint16_t)((P29_RTE_STUB_ADDR >> 16) & 0xFFFFU);
        *(uint16_t*)&MEM[0x00E] = (uint16_t)( P29_RTE_STUB_ADDR        & 0xFFFFU);
        debug_log("[P51-A-VEC23] vec#2/#3 pre-installed at RTE stub 0x%06x\n",
                  (unsigned)P29_RTE_STUB_ADDR);
    }
#endif
    /* P34-DIAG: RTEスタブ書き込み直後の内容確認 */
    if (MEM) {
        uint16_t stub_word = *(uint16_t*)&MEM[P29_RTE_STUB_ADDR];
        uint16_t vec_hi    = *(uint16_t*)&MEM[0x118];
        uint16_t vec_lo    = *(uint16_t*)&MEM[0x11A];
        uint32_t vec_full  = ((uint32_t)vec_hi << 16) | vec_lo;
        debug_log("[P34-DIAG-STUB] after P32-FIX write: MEM[0x%06x]=0x%04x (expect 0x4E73) "
                  "vec#0x46=0x%08x (expect 0x000FFF00)\n",
                  P29_RTE_STUB_ADDR, stub_word, vec_full);
        if (stub_word != 0x4E73U) {
            debug_log("[P34-DIAG-STUB] WARNING: RTE stub word mismatch! Got 0x%04x\n", stub_word);
        }
    }

    /* P36-FIX: IPLROMリセットベクタ領域（オフセット0x10000）からSSP/PCを読む。
     * IPLROM.DATの先頭0x10000バイトはBIOSコード（0x2F0841F8=MOVEM命令）であり
     * リセットベクタではない。MC68000リセットベクタはオフセット0x10000にある。
     * 実測値: IPL[0x10000..7]=00 00 20 00 00 FF 00 10 → SSP=0x2000, PC=0xFF0010 */
    if (IPL) {
        uint32_t ssp = ((uint32_t)IPL[0x10000] << 24) | ((uint32_t)IPL[0x10001] << 16) |
                       ((uint32_t)IPL[0x10002] <<  8) |  (uint32_t)IPL[0x10003];
        uint32_t ipl_pc = ((uint32_t)IPL[0x10004] << 24) | ((uint32_t)IPL[0x10005] << 16) |
                          ((uint32_t)IPL[0x10006] <<  8) |  (uint32_t)IPL[0x10007];
        debug_log("[MX68K] P36-FIX: IPL[0x10000..7] SSP=0x%08x PC=0x%08x\n", ssp, ipl_pc);
        if (ssp == 0 || ssp > 0x00C00000U) {
            debug_log("[MX68K] P36-FIX: SSP=0x%08x out of RAM range [0..0xBFFFFF], skipping\n", ssp);
        } else {
            if (ssp & 1) { ssp &= ~1u; }
            m68000_set_reg(M68K_MSP, ssp);
            debug_log("[MX68K] P36-FIX: SSP set to 0x%08x\n", ssp);
        }
        if (ipl_pc < 0x00FE0000U || ipl_pc > 0x00FFFFFFU) {
            debug_log("[MX68K] P36-FIX: PC=0x%08x not in IPLROM range [0xFE0000..0xFFFFFF], skipping\n", ipl_pc);
        } else {
            m68000_set_reg(M68K_PC, ipl_pc);
            debug_log("[MX68K] P36-FIX: PC set to 0x%08x\n", ipl_pc);
        }
    }
    /* P36-FIX: 設定後の確認ログ */
    {
        uint32_t cur_ssp = m68000_get_reg(M68K_MSP);
        uint32_t cur_pc  = m68000_get_reg(M68K_PC);
        /* MEM[0x118-0x11B]（VSYNC vec）の確認 */
        uint32_t vsync_vec = (((uint32_t)*(uint16_t*)&MEM[0x118]) << 16) |
                              ((uint32_t)*(uint16_t*)&MEM[0x11A]);
        debug_log("[MX68K] P36-FIX: after fix SSP=0x%08x PC=0x%08x VSYNC_vec=0x%08x\n",
                  cur_ssp, cur_pc, vsync_vec);
    }

    /* P37-DIAG: MFP割り込みベクタ範囲ダンプ（reset_hard完了直前） */
    if (MEM) {
        debug_log("[P37-DIAG-VEC] IERA=0x%02x IERB=0x%02x IMRA=0x%02x IMRB=0x%02x\n",
                  MFP[MFP_IERA], MFP[MFP_IERB], MFP[MFP_IMRA], MFP[MFP_IMRB]);
        debug_log("[P37-DIAG-VEC] TCDCR=0x%02x TDDR=0x%02x TCDR=0x%02x\n",
                  MFP[MFP_TCDCR], MFP[MFP_TDDR], MFP[MFP_TCDR]);
        /* ベクタ#0x40〜#0x4F（MFP割り込みベクタ範囲）をダンプ */
        for (int _vi = 0x40; _vi <= 0x4F; _vi++) {
            uint32_t _vaddr = _vi * 4;
            uint32_t _vvec  = (((uint32_t)*(uint16_t*)&MEM[_vaddr  ]) << 16) |
                               ((uint32_t)*(uint16_t*)&MEM[_vaddr+2]);
            debug_log("[P37-DIAG-VEC] vec#0x%02x (MEM[0x%03x])=0x%08x\n",
                      _vi, _vaddr, _vvec);
        }
    }

    /* P35-DIAG: アドレスエラーベクタ(vec#2/#3, addr=0x008-0x00F)の内容確認 */
    if (MEM) {
        uint16_t buserr_hi = *(uint16_t*)&MEM[0x008];
        uint16_t buserr_lo = *(uint16_t*)&MEM[0x00A];
        uint32_t buserr_vec = ((uint32_t)buserr_hi << 16) | buserr_lo;
        uint16_t aderr_hi  = *(uint16_t*)&MEM[0x00C];
        uint16_t aderr_lo  = *(uint16_t*)&MEM[0x00E];
        uint32_t aderr_vec = ((uint32_t)aderr_hi << 16) | aderr_lo;
        debug_log("[P35-DIAG-VECTBL] at reset_hard end: "
                  "bus_err_vec(#2)=0x%08x addr_err_vec(#3)=0x%08x\n",
                  buserr_vec, aderr_vec);
    }
    /* P34-DIAG / P35-DIAG: スタックダンプカウンターをリセット（再起動後も正常動作のため） */
    s_p34_stack_dump_count = 0;
    s_p35_oob_dump_count = 0;

    /* P47-A: Reset diagnostic counters and last-known sentinel values on hard reset */
    s_p47_panic_logged   = 0;
    s_p47_rte_log_cnt    = 0;
    s_p47_vec46_last     = 0xFFFFFFFFu;
    s_p47_vec46_log_cnt  = 0;
    s_p47_abort_ptr_last = 0xFFFFFFFFu;
    s_p47_abort_log_cnt  = 0;

    /* P47-B-α: Reset diagnostic counters and last-known sentinel values on hard reset
     * (same convention as P47-A above; ensures correct behaviour across hot resets) */
    s_p47b_pin_cnt       = 0;
    s_p47b_diag_cnt      = 0;
    s_p47b_panic_logged  = 0;
    s_p47b_v44_last      = 0xFFFFFFFFu;
    s_p47b_iprb_last     = 0xFFu;
    s_p47b_imrb_last     = 0xFFu;
    s_p47b_isrb_last     = 0xFFu;

    /* P47-B-β: Reset write-intercept counters (defined in m68000_bridge.c) */
    m68000_reset_p47bb_counters();

    /* P47-D: Reset DIAG-F/G/H/I session-scope counters & ring/histogram.
     *
     * P48-C reset-order dependency (C-2): this call also re-initialises
     * s_p48c_ioc_intstat_prev from the CURRENT IOC_IntStat value. It must
     * therefore run AFTER the P23-FIX IOC_IntStat=0x0E preset above (line
     * ~369). Both conditions hold here (we are well past line 369). */
    m68000_reset_p47d_counters();

    /* P252 Stage 2c-2: clear the internal-SCSI level-1 pending state so a hard
     * reset starts with a clean multiplexer (symmetric with the counters above). */
    mx68k_scsi_irq_reset();

#if P73A_ENABLE
    /* P73-A: IOC vec スロット (0x180/0x184/0x188/0x18C) を P49-A handler
     * (0x000FFF20) に事前初期化する。
     * 根本原因 (P72-A 確定): 第 1 ブートサイクルの IPLROM が
     * trace_Memory_WriteW を経由せず 0x184 に 0x00FFFF20 を書き込む。
     * 最初の FDD IRQ ACK (frame=22) がその不正値にディスパッチされ
     * TRAP#14 を誘発する。m68000_reset_p47d_counters() 完了後に実行するため
     * P49-A handler は既に 0x000FFF20 に設置済み。
     * MEM[] は host-LE16 配置 (P32-FIX / P38-FIX と同パターン)。 */
    if (MEM) {
        /* vec#0x60 (0x180-0x183) */
        *(uint16_t*)&MEM[0x180] = (uint16_t)((0x000FFF20u >> 16) & 0xFFFFU); /* 0x000F */
        *(uint16_t*)&MEM[0x182] = (uint16_t)( 0x000FFF20u        & 0xFFFFU); /* 0xFF20 */
        /* vec#0x61 (0x184-0x187) — FDD IRQ1 primary target */
        *(uint16_t*)&MEM[0x184] = (uint16_t)((0x000FFF20u >> 16) & 0xFFFFU); /* 0x000F */
        *(uint16_t*)&MEM[0x186] = (uint16_t)( 0x000FFF20u        & 0xFFFFU); /* 0xFF20 */
        /* vec#0x62 (0x188-0x18B) */
        *(uint16_t*)&MEM[0x188] = (uint16_t)((0x000FFF20u >> 16) & 0xFFFFU); /* 0x000F */
        *(uint16_t*)&MEM[0x18A] = (uint16_t)( 0x000FFF20u        & 0xFFFFU); /* 0xFF20 */
        /* vec#0x63 (0x18C-0x18F) */
        *(uint16_t*)&MEM[0x18C] = (uint16_t)((0x000FFF20u >> 16) & 0xFFFFU); /* 0x000F */
        *(uint16_t*)&MEM[0x18E] = (uint16_t)( 0x000FFF20u        & 0xFFFFU); /* 0xFF20 */
        debug_log("[P73-A] IOC vec 0x180/184/188/18C preset -> 0x000FFF20 "
                  "(P49-A handler; guards first FDD IRQ before BIOS vec-table write)\n");
        debug_log("[P73-A-VERIFY] vec#0x61(0x184)=0x%08x vec#0x60(0x180)=0x%08x "
                  "vec#0x62(0x188)=0x%08x vec#0x63(0x18C)=0x%08x "
                  "(all expect 0x000FFF20)\n",
                  (unsigned)p47_read_long_le(0x184),
                  (unsigned)p47_read_long_le(0x180),
                  (unsigned)p47_read_long_le(0x188),
                  (unsigned)p47_read_long_le(0x18C));
    }
#endif /* P73A_ENABLE */

    /* P47-A-DIAG-7: Vector #2 (Bus Error) / #3 (Address Error) dump after init */
    if (MEM) {
        uint32_t v2 = p47_read_long_le(0x08);
        uint32_t v3 = p47_read_long_le(0x0C);
        debug_log("[P47-A-DIAG-7] vec#2 (Bus Error) = 0x%08x, vec#3 (Address Error) = 0x%08x\n",
                  v2, v3);
    }

    /* P68-FDC-FIX: Drain fdd.SetDelay[] to 0 for all mounted drives at the
     * end of every hard reset, so FDD_IsReady() returns TRUE before the
     * post-reset IPLROM executes its drive-ready check at PC≈0xFF0628.
     *
     * Root cause (P68 investigation): mx68k_fdd_insert() leaves
     * fdd.SetDelay[drv] != 0 (upstream 3-frame insert-settle counter).
     * FDD_Reset() does not touch SetDelay[], and the pending-reset frame
     * returns before reaching the per-frame FDD_SetFDInt() drain. The CPU
     * also runs one frame BEFORE that per-frame call, so the IPLROM reads
     * 0xE94005 while FDD_IsReady(0)==FALSE -> FDC_RDY=0 -> panic.
     *
     * FDD_SetFDInt() (no args) decrements every drive's SetDelay by one and
     * is a no-op on drives already at 0; three calls guarantee SetDelay
     * 3->0 for both drive 0 and drive 1. The 1->0 transition still
     * dispatches IRQH_Int(1,&FDD_Int) (IOC_IntStat bit1 is set by the
     * P23-FIX preset earlier in this function), so the FDD-insert IRQ1 is
     * preserved -- it now fires during reset rather than at end of frame
     * N+1. The existing P48-A drain in mx68k_fdd_insert() is left
     * unchanged. */
#if P68_FDC_FIX_ENABLE
    /* P68-g2: Pre-select drive 0 in fdc.ctrl so the drive-ready port 0xE94005
     * returns bit7=1 before the IPLROM writes its own drive-select command.
     * FDC_Init() zeros fdc.ctrl; 0xE94005 read = (fdc.ctrl&1)&&FDD_IsReady(0).
     * FDC_Write(addr, data) switches on addr&0x07; 0xE94005&0x07=5 -> ctrl. */
    FDC_Write(0xE94005u, 0x01u);  /* fdc.ctrl = 0x01 (drive 0 selected) */
    FDD_SetFDInt();   /* SetDelay 3 -> 2 (no-op on drives already at 0) */
    FDD_SetFDInt();   /* SetDelay 2 -> 1 */
    FDD_SetFDInt();   /* SetDelay 1 -> 0  + IRQH_Int(1,&FDD_Int) on settle */
    debug_log("[P68-FDC-FIX] reset-time drain+preselect: "
              "FDD_IsReady(0)=%d FDD_IsReady(1)=%d IOC_IntStat=0x%02x\n",
              FDD_IsReady(0), FDD_IsReady(1), (unsigned)IOC_IntStat);
#endif

    /* P220: write the configured RAM size to guest SRAM $ED0008 (raw byte-count
     * longword, big-endian; SRAM[] is adr^1 byte-swapped, same convention as the
     * seed at :472). guest $ED0008-B = 0x00 (mb<<4) 0x00 0x00 = (mb<<4)<<16
     * = mb<<20 = mb*0x100000. 12MB->0x00C00000, 8MB->0x00800000, 2MB->0x00200000.
     * Matches Core sram.c:47 SRAM_SetRamSize (Memory_WriteB(0xed0009, size&0xf0)
     * with size=mb<<4). reset_hard is the only correct insertion point: init-time
     * seed/sram.dat load is not re-applied after a settings-change hard reset, and
     * this is the last place the process touches $ED0008 before the guest runs.
     * Physical MEM allocation stays 12MB — this is declarative size reporting only. */
    {
        int p220_mb = (g_memory_size_mb > 0 && g_memory_size_mb <= 12)
                        ? g_memory_size_mb : 12;
        SRAM[0x08^1] = 0x00;
        SRAM[0x09^1] = (uint8_t)(p220_mb << 4);
        SRAM[0x0A^1] = 0x00;
        SRAM[0x0B^1] = 0x00;
#if P220_PROBE
        {
            uint32_t v = ((uint32_t)SRAM[0x08^1] << 24) |
                         ((uint32_t)SRAM[0x09^1] << 16) |
                         ((uint32_t)SRAM[0x0A^1] << 8)  |
                         ((uint32_t)SRAM[0x0B^1]);
            debug_log("[P220-MEMSIZE] b post-write mb=%d $ED0008=0x%08x\n",
                      p220_mb, v);
        }
#endif
        mx68k_set_membound(p220_mb);   /* P220b: burn the same clamped value into the RAM boundary */
    }

    /* P504 (D-46 検証プローブ): 既存の全 SRAM 書込みが完了した直後、かつ CPU 実行
     * 開始前の状態を 1 標本目として記録する。続けて起動後 5 フレーム時点の
     * 2 標本目をアームする(IPL-ROM 自己修復の発火有無を生バイトで判別するため)。 */
    p504_dump_sram_boot_fields("post_reset");
    s_p504_boot_sample_countdown = 5;   /* 起動後5フレーム時点で2標本目を採る */

#if P602_ENABLE
    /* P602 (D-57): IOCS ワークエリア $0CBC-$0CBF の初期値スナップショット。
     * ★この位置でなければならない —— 全 HW init が終わり、かつ CPU が 1 命令も
     * 実行していない時点の値でなければ「ROM IOCS 1.30 未満では初期化されない =
     * リセット直後の残留 RAM 値がそのまま読まれる」(経路B仮説)を検証できない。
     * 同時に P602 の全カウンタを 0 に戻し、この run を run=N として区切る。
     * 読み出しのみ —— guest メモリには一切書かない。 */
    p602_workarea_snapshot("post_reset");
#endif

    debug_log("[MX68K] mx68k_reset_hard() END\n");
}

void mx68k_reset_soft(void) {
    /* P186b: Soft reset を hard reset と同じ完全再起動経路に通す。真の reset PC 設定は
     * P36-FIX(880-903)の m68000_set_reg(M68K_PC, 0xFF0010) で、それ + IPL 全体シャドウ +
     * HW 再init + MX 固有ブートタイミング stub 群が揃って初めて A> に到達する。IPLROM.DAT
     * 先頭 0x10000 は BIOS コードでリセットベクタでない(真のベクタは IPL オフセット 0x10000)ため
     * MEM[0..7] の byte-swap だけでは reset PC を正しく設定できず、下流で odd-address fault
     * (PC=$00000001)になっていた。個別移植は stub 欠落で再 stall のリスクが高いため、A> 到達
     * 実証済みの hard reset 本体をそのまま呼ぶ。X68000 実機の RESET スイッチも IPL からの
     * full reboot ゆえ挙動として妥当。現 MX は soft/hard で保存すべき差分状態を持たないため
     * 実害なし(将来 soft の RAM 保持を厳密化する場合はここで再分岐する)。 */
    mx68k_reset_hard();
}

void mx68k_nmi(void) {
    IRQH_Int(7, NULL);
}

void mx68k_pause(bool pause) {
    s_paused = pause;
}

// ---- frame ----
/* P82-G: current frame index, published each frame from mx68k_run_frame()
 * so the m68000_bridge.c chunk-loop sampler (Part B) can frame-gate.
 * Diagnostic-only; written here, read only by the P82-G probe. */
int g_mx68k_frame_num = 0;

#if P135_ENABLE
/* =====================================================================
 * P135 Part 1: host-path runtime latch (cross-TU)。
 * init clear / IPL→MEM shadow が boot 後 (frame>=80・特に 88-89) に走るかを
 * 動的確認する。非evict accumulator (boot-lifetime・p135_reset では触れない)。
 * m68000_bridge.c の p135_dump が extern 参照する。READ-ONLY: MEM[0x1FF6] を
 * 読むだけで guest メモリは改変しない。p135_hostlat_t は EmulatorBridge.h 定義。
 * ===================================================================== */
p135_hostlat_t g_p135_hostlat[P135_HOSTLAT];
uint32_t       g_p135_hostlat_count = 0;

/* host-path 実行を latch。which: 0=A-init-clear, 1=B-memset-vecskip, 2=B-shadow-loop。
 *   before/after は呼び出し側が MEM[0x1FF6] LE16 を計測して渡す。
 *   方針: 先頭スロットは常に保持 + frame>=80 の実行も確実に保持 (容量内)。 */
static void p135_host_latch(unsigned char which, int frame,
                            unsigned short before, unsigned short after) {
    if (g_p135_hostlat_count < (uint32_t)P135_HOSTLAT) {
        p135_hostlat_t *h = &g_p135_hostlat[g_p135_hostlat_count];
        h->set        = 1;
        h->which_path = which;
        h->frame      = frame;
        h->mem_before = before;
        h->mem_after  = after;
    }
    g_p135_hostlat_count++;   /* 常に加算 (overflow 検知用) */
}

/* MEM[0x1FF6] の LE16 真値を READ-ONLY で取得 (MEM 未確保なら 0xFFFF)。 */
static inline unsigned short p135_mem1ff6(void) {
    if (!MEM) return 0xFFFFu;
    return *(unsigned short*)&MEM[0x1FF6u];
}
#endif /* P135_ENABLE */

/* =====================================================================
 * P82-X-V CP-V-2 + CP-V-5 per-frame driver
 * (Plan: /tmp/mx68k_P82-X-V_plan.md §3.2 / §3.5).
 *
 * Codex Q2 verdict adopted: HD63450 DMAC delivers vectored IRQs via
 * programmed NIV/EIV (per-channel), NOT the level-3 autovector slot at
 * byte $6C. CP-V-2 therefore observes:
 *   1. DMA[0..3].NIV / .EIV (8 values, register snapshot).
 *   2. Vector table entries at (4 * NIV) and (4 * EIV) per channel
 *      (8 long-word reads through p47_read_long_le).
 *   3. The reset SSP slot at $00000000 — px68k boots with NIV=$00 by
 *      default, so a stray IRQ would jump to mem_read_long($00000000)
 *      (= reset SSP, garbage PC fetch → bus/addr error → $ff062a panic).
 *   4. Legacy autovector level-3 slot at $0000006C — recorded for
 *      completeness so the original H5-C variant remains observable.
 *
 * CP-V-5 emits a 1-line per-frame snapshot of IRQH[1..7] + IRQLine +
 * IPL mask + MFP IPRA/IPRB + DMA0 CSR/CCR during window [80, 95].
 * All reads are side-effect-free.
 * ===================================================================== */
#if P82XV_ENABLE
static uint8_t s_p82xv_v2_armed_postinit = 0;
static uint8_t s_p82xv_v2_armed_panic    = 0;

/* hard-reset hook — called from m68000_reset_p47d_counters via extern decl */
void p82xv_reset_tick_state(void) {
    s_p82xv_v2_armed_postinit = 0;
    s_p82xv_v2_armed_panic    = 0;
}

/* CP-V-2 emit body — read DMAC NIV/EIV (register state) + the vector
 * table entries those NIV/EIV indexes into + reset SSP + autovec lvl3.
 * stage_tag is the literal "A" or "B" string suffix for the emit line. */
static void p82xv_v2_emit(const char *stage_tag, int32_t frame) {
    uint8_t  niv[4], eiv[4];
    uint32_t vec_at_niv[4], vec_at_eiv[4];
    for (int ch = 0; ch < 4; ch++) {
        niv[ch] = DMA[ch].NIV;
        eiv[ch] = DMA[ch].EIV;
        /* p47_read_long_le is BPC-safe (returns 0xFFFFFFFF on OOB). The
         * vector table address is (vector_number * 4). */
        vec_at_niv[ch] = p47_read_long_le((uint32_t)niv[ch] * 4u);
        vec_at_eiv[ch] = p47_read_long_le((uint32_t)eiv[ch] * 4u);
    }
    uint32_t reset_ssp     = p47_read_long_le(0x00000000u);
    uint32_t autovec_lvl3  = p47_read_long_le(MX68K_VEC_6C_ADDR);
    uint32_t vec_buserr    = p47_read_long_le(MX68K_VEC_BUSERR_ADDR);
    uint32_t vec_addrerr   = p47_read_long_le(MX68K_VEC_ADDRERR_ADDR);
    uint32_t vec_spurious  = p47_read_long_le(MX68K_VEC_SPURIOUS_ADDR);

    debug_log("[P82XV-PROBE-V2] stage=%s frame=%d "
              "ch0_niv=$%02x ch0_eiv=$%02x ch1_niv=$%02x ch1_eiv=$%02x "
              "ch2_niv=$%02x ch2_eiv=$%02x ch3_niv=$%02x ch3_eiv=$%02x "
              "vec_at_ch0niv($%03x)=$%08x vec_at_ch0eiv($%03x)=$%08x "
              "vec_at_ch1niv($%03x)=$%08x vec_at_ch1eiv($%03x)=$%08x "
              "vec_at_ch2niv($%03x)=$%08x vec_at_ch2eiv($%03x)=$%08x "
              "vec_at_ch3niv($%03x)=$%08x vec_at_ch3eiv($%03x)=$%08x "
              "reset_ssp=$%08x autovec_lvl3($6C)=$%08x "
              "vec_buserr($08)=$%08x vec_addrerr($0C)=$%08x "
              "vec_spurious($60)=$%08x\n",
              stage_tag, (int)frame,
              (unsigned)niv[0], (unsigned)eiv[0],
              (unsigned)niv[1], (unsigned)eiv[1],
              (unsigned)niv[2], (unsigned)eiv[2],
              (unsigned)niv[3], (unsigned)eiv[3],
              (unsigned)(niv[0] * 4u), (unsigned)vec_at_niv[0],
              (unsigned)(eiv[0] * 4u), (unsigned)vec_at_eiv[0],
              (unsigned)(niv[1] * 4u), (unsigned)vec_at_niv[1],
              (unsigned)(eiv[1] * 4u), (unsigned)vec_at_eiv[1],
              (unsigned)(niv[2] * 4u), (unsigned)vec_at_niv[2],
              (unsigned)(eiv[2] * 4u), (unsigned)vec_at_eiv[2],
              (unsigned)(niv[3] * 4u), (unsigned)vec_at_niv[3],
              (unsigned)(eiv[3] * 4u), (unsigned)vec_at_eiv[3],
              (unsigned)reset_ssp, (unsigned)autovec_lvl3,
              (unsigned)vec_buserr, (unsigned)vec_addrerr,
              (unsigned)vec_spurious);
}

void p82xv_tick(void) {
    int32_t f = (int32_t)g_mx68k_frame_num;

    /* ---- CP-V-2 stage A (post-init) and stage B (panic-near) ---- */
    if (!s_p82xv_v2_armed_postinit && f == (int32_t)P82XV_V2_FRAME_POSTINIT) {
        s_p82xv_v2_armed_postinit = 1;
        p82xv_v2_emit("A", f);
    }
    if (!s_p82xv_v2_armed_panic && f == (int32_t)P82XV_V2_FRAME_PANIC) {
        s_p82xv_v2_armed_panic = 1;
        p82xv_v2_emit("B", f);
    }

    /* ---- CP-V-5 per-frame sampler — window [80, 95] inclusive (cap 16) ---- */
    if (f >= P82XV_WIN_LO && f <= P82XV_WIN_HI) {
        uint16_t sr  = (uint16_t)m68000_get_reg(M68K_SR);
        uint8_t  ipl = (uint8_t)((sr >> 8) & 0x7u);
        uint8_t  ipra = (uint8_t)MFP[MFP_IPRA];
        uint8_t  iprb = (uint8_t)MFP[MFP_IPRB];
        debug_log("[P82XV-PROBE-V5] f=%d "
                  "irqh[1..7]=$%02x/$%02x/$%02x/$%02x/$%02x/$%02x/$%02x "
                  "irqline=$%02x ipl=%u mfp_ipra=$%02x iprb=$%02x "
                  "iocstat=$%02x dma0_csr=$%02x ccr=$%02x\n",
                  (int)f,
                  (unsigned)IRQH_IRQ[1], (unsigned)IRQH_IRQ[2],
                  (unsigned)IRQH_IRQ[3], (unsigned)IRQH_IRQ[4],
                  (unsigned)IRQH_IRQ[5], (unsigned)IRQH_IRQ[6],
                  (unsigned)IRQH_IRQ[7],
                  (unsigned)(C68K.IRQLine & 0xFFu),
                  (unsigned)ipl, (unsigned)ipra, (unsigned)iprb,
                  (unsigned)IOC_IntStat,
                  (unsigned)DMA[0].CSR, (unsigned)DMA[0].CCR);
    }
}
#else  /* P82XV_ENABLE == 0 — no-op stubs (orphan-free) */
void p82xv_reset_tick_state(void) { }
void p82xv_tick(void)             { }
#endif /* P82XV_ENABLE */

/* P294: per-scanline 描画化 — 旧 mx68k_render_frame() をフレーム境界の begin/end と
 * 表示行1本の draw_line に3分割。begin は exec ループ直前、draw_line は LINE-END の
 * p214_r1_sample_line 直後(CRTC_VStep 分岐)、end は exec ループ終了直後で呼ぶ。
 * 定義は mx68k_get_framebuffer 付近。全て emulation スレッド内で完結。 */
static void mx68k_render_begin(void);
static void mx68k_draw_display_line(void);
static void mx68k_render_end(void);

/* P294: 旧 mx68k_render_frame() のフレーム単位ローカルを file-scope static へ格上げ。
 * begin が設定、draw_line が加算・参照、end が消費する。 */
static uint8_t *s_render_fb;
static int s_render_back;
static int s_render_disp_w, s_render_disp_h, s_render_orig_textdotx;
/* P595 (D-55): 公開フレームと対になる表示ジオメトリ(単位は「標準表示窓=1.0」)。
 * mx68k_render_begin() のローカル計算結果をここへ複写し、mx68k_render_end() の
 * publish で s_fb_*[back] へ移す(P571 の s_render_out_* を置換)。
 * 標準ラスタでは hscale=vscale=1.0・offx=offy=0.0(P212 と完全同一)。 */
static float s_render_hscale = 1.0f, s_render_vscale = 1.0f;
static float s_render_offx = 0.0f, s_render_offy = 0.0f;
static int   s_render_geomode = 0;
static int s_render_log_this;
static int s_render_nonzero_pixels;
static int s_render_fb_call_count;
#if P217_PROBE
static int s_p217_n_bgzero, s_p217_y_min, s_p217_y_max;
#endif
#if P543_ENABLE
/* P543 [P543-DISPWIN]: D-36着手条件検証用。フレーム全表示期間にわたって
 * 「MX68K は描画するが上流3条件ゲートなら描画しない」走査線を数えるだけの
 * 読み取り専用アキュムレータ。begin がリセット、draw_display_line が加算、
 * end が消費する(P294 の既存 s_render_* 群と同じ寿命)。 */
static int s_p543_lines_drawn;
static int s_p543_lines_mx_draws;
static int s_p543_lines_would_block;
static int s_p543_first_block_vline;
static int s_p543_last_block_vline;
#endif

/* P175: Core Keyboard_Int() is a stub; deliver a queued scancode to the MFP here
 * (upstream x11/keyboard.c:637). Raises the MFP keyboard RX interrupt MFP_Int(3). */
static void mx68k_keyboard_int(void) {
    if (KeyBufRP != KeyBufWP) {
        if (!KeyIntFlag) {
            LastKey = KeyBuf[KeyBufRP];
            KeyBufRP = (uint8_t)((KeyBufRP + 1) & (KeyBufSize - 1));
            KeyIntFlag = 1;
            MFP_Int(3);
        }
    } else if (!KeyIntFlag) {
        LastKey = 0;
    }
}

/* ===================== P214 診断 probe（測定のみ・既定は休止 = P214_ENABLE 0） =====================
 * 描画欠陥の class 判別のための read-only 計測。描画結果を 1 画素も変えず、エミュレーション
 * 状態にも一切触れない（自身の static カウンタのみ更新）。
 *
 *  R1 = 表示行ごとに「映像状態」(CRTC スクロール/VCReg/BG スクロール/ラスタコピー/パレット)を
 *       ハッシュし、前行と異なる行数を数える。書き手を問わない（DMA 由来でも見える）ため
 *       class 1(ラスタ分割)の主測定。
 *  R2 = 書き込み分類（m68000_bridge.c 側・帰属の裏取り）。
 *  S  = 静的署名（class 2/3/4 用）。
 * 出力は mx68k_render_frame() 冒頭で 60 フレームに 1 行のみ。
 * P214_ENABLE / p214_r2_format_and_reset の宣言は EmulatorBridge.h。
 */
#if P214_ENABLE
#define P214_FNV_BASIS 2166136261u

static unsigned s_p214_chg_vc    = 0;   /* 前行と映像状態ハッシュが異なった表示行数 */
static unsigned s_p214_chg_pal   = 0;   /* 同 パレット */
static unsigned s_p214_chg_spr   = 0;   /* P277: 前行とSprite_Regsハッシュが異なった表示行数 */
#if P295_ENABLE
static unsigned s_p295_raster_irq_fires = 0;   /* P295: ラスタ(CRTC IntLine)割込み発火回数/フレーム */
#endif
static uint32_t s_p214_prev_vc   = 0;
static uint32_t s_p214_prev_pal  = 0;
static uint32_t s_p214_prev_spr  = 0;   /* P277 */
static int      s_p214_have_prev = 0;   /* フレーム内の最初の表示行では比較しない */

#if P290_ENABLE
static uint32_t s_p290_prev_frame_spr = 0;   /* 前フレーム末のSprite_Regsハッシュ */
static int      s_p290_have_frame     = 0;   /* 初回フレームでは比較しない */
#endif

static inline uint32_t p214_fnv1a(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* 行末（vl++ の直前）で呼ぶ。表示行のみ対象。read-only。 */
static void p214_r1_sample_line(int vl) {
    if (vl < (int)CRTC_VSTART || vl >= (int)CRTC_VEND) return;

    uint32_t h_vc = P214_FNV_BASIS;
    h_vc = p214_fnv1a(h_vc, &CRTC_Regs[0x14], 0x14);  /* 0x14-0x27: Text/Grph スクロール 20B */
    h_vc = p214_fnv1a(h_vc, VCReg0, 2);
    h_vc = p214_fnv1a(h_vc, VCReg1, 2);
    h_vc = p214_fnv1a(h_vc, VCReg2, 2);
    h_vc = p214_fnv1a(h_vc, &BG_Regs[0x00], 8);       /* BG0/BG1 ScrollX/Y（ラスタ同期スクロール） */
    h_vc = p214_fnv1a(h_vc, &BG_Regs[8], 2);          /* P279: BG_Regs[8],[9] — BG0/BG1 ON/OFF・キャラサイズ */
    h_vc = p214_fnv1a(h_vc, &BG_Regs[0x11], 1);       /* P279: BG_Regs[0x11] — 表示モード関連 */
    h_vc = p214_fnv1a(h_vc, &CRTC_Regs[0x28], 8);     /* ラスタコピー Src/Dst 等 */
    h_vc = p214_fnv1a(h_vc, &CRTC_Mode, 1);           /* CRTC_Regs[] 配列外の独立 global */

    uint32_t h_pal = p214_fnv1a(P214_FNV_BASIS, Pal_Regs, sizeof(Pal_Regs));

    extern uint8_t Sprite_Regs[0x800];   /* P277: スプライト座標/属性(128枚×16B)。state save/load用externと同一配列 */
    uint32_t h_spr = p214_fnv1a(P214_FNV_BASIS, Sprite_Regs, sizeof(Sprite_Regs));

    if (s_p214_have_prev) {
        if (h_vc  != s_p214_prev_vc)  s_p214_chg_vc++;
        if (h_pal != s_p214_prev_pal) s_p214_chg_pal++;
        if (h_spr != s_p214_prev_spr) s_p214_chg_spr++;
    }
    s_p214_prev_vc   = h_vc;
    s_p214_prev_pal  = h_pal;
    s_p214_prev_spr  = h_spr;
    s_p214_have_prev = 1;
}

static void p214_r1_frame_reset(void) {
    s_p214_chg_vc    = 0;
    s_p214_chg_pal   = 0;
    s_p214_chg_spr   = 0;
    s_p214_have_prev = 0;
}

/* P278: D-20実測用。スプライトテーブル(128枚)全件を座標/パターン/優先度
 * 付きでダンプする。read-only(Sprite_Regsを読むのみ、一切書き込まない)。
 * 症状発生時のdebug.logから該当スプライトの状態を直接確認するための
 * 一時的な計測。 */
static void p278_dump_sprites(int frame_num) {
    typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) P278SprEnt;
    extern uint8_t Sprite_Regs[0x800];
    const P278SprEnt *sct = (const P278SprEnt *)Sprite_Regs;
    for (int n = 0; n < 128; n++) {
        uint16_t posx = sct[n].posx & 0x3ff;
        uint16_t posy = sct[n].posy & 0x3ff;
        if (posx == 0 && posy == 0) continue;   /* 未使用スロットの大半を除外(簡易フィルタ) */
        debug_log("[P278-SPR] f=%d n=%d x=%u y=%u ctrl=%04x pri=%u\n",
                  frame_num, n, posx, posy, sct[n].ctrl, sct[n].ply & 3);
    }
}
#endif /* P214_ENABLE */

#if P291_ENABLE
/* P291: D-4実測用。毎フレーム、表示走査線ごとにY方向で重なるスプライト数を
 * 計測し、フレーム内最大値と複数閾値(16/24/32)超過走査線数を記録する。
 * 可視判定式は Core/px68k/x68k/bg.c:316-323 (Sprite_DrawLineMcr) を read-only で
 * 再現(Coreは無変更・参照のみ)。ステートレス(自身の状態変数を持たない)。
 * 走査線ループは VLINEBG と同じ 0-based 座標系(0..disp_h-1)。 */
static void p291_sprline_scan(int frame_num, int disp_h) {
    typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) P291SprEnt;
    extern uint8_t Sprite_Regs[0x800];
    const P291SprEnt *sct = (const P291SprEnt *)Sprite_Regs;
    int max_count = 0;
    int lines_gt16 = 0, lines_gt24 = 0, lines_gt32 = 0;
    for (int Y = 0; Y < disp_h; Y++) {   /* VLINEBGと同じ0-based座標系(Code Review round1指摘で修正) */
        int count = 0;
        for (int n = 0; n < 128; n++) {
            uint16_t px = sct[n].posx & 0x3ff, py = sct[n].posy & 0x3ff;
            if (px == 0 && py == 0) continue;   /* P278/P290と同じ簡易フィルタ */
            /* Core/px68k/x68k/bg.c:316-323 (Sprite_DrawLineMcr) の可視判定を
             * read-onlyで再現(Coreは無変更・参照のみ)。 */
            int32_t row = 16 - ((int32_t)py - Y + BG_VLINE);
            if (row >= 0 && row <= 15) count++;
        }
        if (count > max_count) max_count = count;
        if (count > 16) lines_gt16++;
        if (count > 24) lines_gt24++;
        if (count > 32) lines_gt32++;
    }
    debug_log("[P291-SPRLINE] f=%d max_line_count=%d lines_gt16=%d lines_gt24=%d lines_gt32=%d\n",
              frame_num, max_count, lines_gt16, lines_gt24, lines_gt32);
}
#endif /* P291_ENABLE */

#if P218_PROBE
/* P218: 黒画面の問い（plan §5）を実測に預ける一度きりのスキャン。
 * 「GVRAM 非ゼロ + 画面が黒」には 2 通りの読みがあり、非ゼロ word 数だけでは
 * 区別できない: (a) 表示されるべき絵が黒 = 本物の未解明の矛盾 /
 * (b) グラフィック面 OFF・パレット全黒・コントラスト 0 = 矛盾ではない。
 * → 切り分けに要る状態を同時刻に採る。判定表は plan §5.3。
 * GO/NO-GO の材料ではない（唯一の権威は plan §4.4）。read-only。
 * VCReg2[1] は raw のまま出す: 描画側の実ゲートは EmulatorBridge.c の
 * `if (VCReg2[1] & 0x0F)` なので、判定時にそのマスクを当てられるようにする。 */
void p218_gvram_census_once(const char *tag) {
    static int s_done_g = 0, s_done_s = 0;
    int *done = (tag[0] == 'G') ? &s_done_g : &s_done_s;
    if (*done) return;
    *done = 1;

    const uint16_t *gv = (const uint16_t *)GVRAM;
    unsigned total = (unsigned)(sizeof(GVRAM) / sizeof(uint16_t));
    unsigned nonzero = 0;
    for (unsigned i = 0; i < total; i++)
        if (gv[i]) nonzero++;

    /* 非黒の判定は Pal32_FullMask（= WinDraw_Pal32R|G|B, palette.c:61）で行う。
     * GrphPal32 の最下位 bit は Abit32（透明 bit, palette.c:59,195）で色ではないため、
     * 素の != 0 では「黒だが I bit 付き」を非黒と誤計上する。 */
    unsigned grphpal_nonblack = 0;
    for (unsigned i = 0; i < 256; i++)
        if (GrphPal32[i] & Pal32_FullMask) grphpal_nonblack++;

    debug_log("[P218-CENSUS-%s] gvram_nonzero_words=%u/%u "
              "VCReg0=%02x%02x VCReg1=%02x%02x VCReg2=%02x%02x "
              "grphpal_nonblack=%u/256 contrast=%u CRTC_R20=%02x\n",
              tag, nonzero, total,
              (unsigned)VCReg0[0], (unsigned)VCReg0[1],
              (unsigned)VCReg1[0], (unsigned)VCReg1[1],
              (unsigned)VCReg2[0], (unsigned)VCReg2[1],
              grphpal_nonblack, (unsigned)Contrast_Value,
              (unsigned)CRTC_Regs[0x29]);
}
#endif /* P218_PROBE */

#ifndef P467_ENABLE
#define P467_ENABLE 1   /* ★P533: D-40調査(H2=GVRAM_FastClearの非ラップ書込み)の実測のため
                         * このサイクル限定で一時的に 1 へ戻した。hands-on計測完了後、
                         * 次サイクルの判定を待って P513 同様 0 へ戻すか判断する。
                         * ★[P468-GVWR](分母)と必ず同時に有効化すること —— P467 単独では
                         *   0行の解釈が一意にならない(自己反証可能性を欠く)。
                         * --- 以下、P513 時点の経緯 ---
                         * D-35症状2(でたな!!ツインビー遷移時のグラフィック残像)調査用、一時的に有効化。
                         * ★P513で休止(D-35症状2調査は P471(2026-07-31)で既に終了・凍結済み。
                         *   D-39=でたな!!ツインビーのフレーム時間超過の残置プローブ課税仮説の検証のため、
                         *   高速クリアのたびに走る GVRAM 全走査×2 をコンパイル対象から除外する)。
                         *   再度必要になった場合は 1 へ戻すだけで復活できる。 */
#endif
#if P467_ENABLE
/* P467: GVRAM 全域の非ゼロ word 数を数える読み取り専用ヘルパー。
 * ループは [P218-CENSUS](:1930-1934)と同一 —— 重複実装で数え方がズレる事故を
 * 避けるため、走査範囲(sizeof(GVRAM)/sizeof(uint16_t) = 0x40000 word)も
 * 同じ式をそのまま使う。GVRAM への書込みは一切行わない。
 * 分母(総 word 数)は呼出し側がログに併記できるよう out_total で返す。 */
static unsigned p467_gvram_nonzero_words(unsigned *out_total) {
    const uint16_t *gv = (const uint16_t *)GVRAM;
    unsigned total = (unsigned)(sizeof(GVRAM) / sizeof(uint16_t));
    unsigned nonzero = 0;
    for (unsigned i = 0; i < total; i++)
        if (gv[i]) nonzero++;
    if (out_total) *out_total = total;
    return nonzero;
}
#endif /* P467_ENABLE */

#if P534_ENABLE
/* P534 [P534-GVWCOL] 計数ラッパ。gvram.c は Bridge/p534_gvwcol_hook.h の
 * force-include で GVRAM_Write を p534_gvram_write_core へ改名済みなので、
 * 本関数(GVRAM_Write)が全呼出し元(mem_wrap.c:275/295-296)から見た実体に
 * なる。CPU 経由・DMA 経由の区別なく GVRAM_Write を通る書込みをすべて
 * 捕捉する(Bridge の trace_Memory_WriteB/W では DMA 経由を取りこぼす)。
 *
 * 判定は Core/px68k/x68k/gvram.c:146-211 の逐語転記(簡約しない)。
 * 列算出式 col = (adr & 0x3ff) >> 1 は MPX68K 計装パッチと同一地点・同一式。
 * 読み取りのみ —— GVRAM・レジスタ・描画バッファへの書込みは一切なく、
 * 受け取った (adr, data) を無変更で本体へ渡す(早期 return なし)。
 *
 * ★このファイル(EmulatorBridge.c)には force-include が掛からないため、
 *   GVRAM_Write マクロは可視でない = 無限再帰にはならない。
 * ★プロトタイプをここに置く理由: Fix Plan 変更4a はカウンタ宣言と同じ
 *   167-183行付近を挙げていたが、その位置では px68k 系ヘッダ未 include で
 *   FASTCALL が未定義(EmulatorBridge.h/GamePad.h とも <stdint.h> 系のみ)。
 *   宣言の移動は記憶域を伴わないため §グローバル配置安全性 の前提
 *   (gvram.c/EmulatorBridge.c の配置不変)には影響しない。 */
void FASTCALL p534_gvram_write_core(uint32_t adr, uint8_t data);  /* gvram.c の改名後の実体 */

void FASTCALL GVRAM_Write(uint32_t adr, uint8_t data) {
    s_p534_calls++;
    {
        uint32_t a = adr;
#ifndef __BIG_ENDIAN__
        a ^= 1;                       /* gvram.c:153-155 */
#endif
        a &= 0x3fffff;                /* gvram.c:157 */
        if (!(CRTC_Regs[0x28] & 8)) { /* gvram.c:159 (65536配置なら対象外) */
            uint32_t m = (uint32_t)(CRTC_Regs[0x28] & 3);
            if ((m == 1u || m == 2u)              /* gvram.c:197-198 */
                && a < 0x100000u                  /* gvram.c:199 */
                && !(a & 1u)) {                   /* gvram.c:201 */
                uint32_t idx = a;
                if (idx & 0x80000u) idx += 1u;    /* gvram.c:208 */
                idx &= 0x7ffffu;                  /* gvram.c:209 */
                {
                    uint32_t col = (idx & 0x3ffu) >> 1;   /* MPX [R1-GVWCOL] と同一式 */
                    s_p534_wr256++;
                    if (col >= 496u && col <= 511u) {
                        if (data == 0) s_p534_col496_zero++;
                        else           s_p534_col496_nonzero++;
                    }
                }
            }
        }
    }
    p534_gvram_write_core(adr, data);
}
#endif /* P534_ENABLE */

#ifndef P468_ENABLE
#define P468_ENABLE 1   /* ★P533: D-40調査(H2)の実測のため、このサイクル限定で一時的に 1 へ戻した。
                         *   本プローブは [P467-FASTCLR] が 0 行だった場合に「条件が偽だった」のか
                         *   「そもそもブロックに到達していない」のかを区別するための分母であり、
                         *   P467_ENABLE と必ず同時に有効化する。m68000_bridge.c 側の定義も
                         *   同じ値(1)へ揃えてある。
                         * --- 以下、P513 時点の経緯 ---
                         * D-35症状2(でたな!!ツインビー遷移時のグラフィック残像)の3仮説
                         * (H-A: page1書込みの取りこぼし / H-B: page1側スクロールレジスタが
                         *  一度も観測されていない / H-C: 高速クリア要求自体が届いていない)を
                         * 同時に切り分けるための計装、一時的に有効化。
                         * ★このマクロは Bridge/m68000_bridge.c 側でも同じ #ifndef 形で
                         *   独立に定義している(P467_ENABLE と同じ in-file 定義規約)。
                         * ★P513で休止(D-35症状2調査は P471(2026-07-31)で既に終了・凍結済み。
                         *   D-39=でたな!!ツインビーのフレーム時間超過の残置プローブ課税仮説の検証)。
                         *   m68000_bridge.c 側の定義も同じ値へ揃えること。 */
#endif

#if P367_ENABLE
/* DMA_Exec(ch) の前後スナップショットから、この転送の書込み先が
 * $EB0000-$EB0811(BG/スプライトレジスタ全域)と重なるかを判定してログする。
 * dmac.c:227-234 の転送先選択(OCR&0x80 ? MAR : DAR)を読み取り専用で模倣する
 * のみで、DMA[] への書込みは一切行わない。300フレームに1回のゲート。 */
static void p367_dma_write_check(int ch, uint32_t dst_before, uint16_t mtc_before,
                                 uint32_t dst_after, uint16_t mtc_after, int vl) {
    if (mtc_before == mtc_after) return;   /* 転送なし(既存、共通で先に判定) */

#if P436_ENABLE
    {
        uint32_t lo436 = (dst_before < dst_after) ? dst_before : dst_after;
        uint32_t hi436 = (dst_before < dst_after) ? dst_after  : dst_before;
        static uint32_t s_p436_dma_count = 0;
        if (!(hi436 < 0xEB0000u || lo436 > 0xEB0811u) && s_p436_dma_count < 200u) {
            debug_log("[P436-REGWRITE] src=DMA ch=%d f=%d vl=%d dst=0x%06x-0x%06x "
                      "bytes=%d (count=%u)\n",
                      ch, g_mx68k_frame_num, vl, lo436, hi436,
                      (int)(mtc_before - mtc_after), ++s_p436_dma_count);
        }
    }
#endif

    if ((g_mx68k_frame_num % 300) != 0) return;   /* 既存P367の300フレームゲート、維持 */
    uint32_t lo = (dst_before < dst_after) ? dst_before : dst_after;
    uint32_t hi = (dst_before < dst_after) ? dst_after  : dst_before;
    if (hi < 0xEB0000u || lo > 0xEB0811u) return;   /* 対象範囲と重ならない */
    debug_log("[P367-REGW] src=DMA ch=%d f=%d vl=%d dst=0x%06x-0x%06x bytes=%d\n",
              ch, g_mx68k_frame_num, vl, lo, hi, (int)(mtc_before - mtc_after));
}
#endif

/* P503: フレーム境界で消費される pending 操作群を mx68k_run_frame() から抽出。
 * 戻り値 1 = 「このフレームを丸ごと使うアクションを1件消費した」(呼出元は
 * CPU 実行へ進まず即 return する)、0 = 何も消費しなかった、または
 * fall-through 扱いの軽量処理(SASI fd キャッシュ無効化)のみを行った。
 * 一時停止中(設定シート表示等)は mx68k_pump_pending() 経由でこの関数だけが
 * 呼ばれるため、pending 操作がシートを閉じるまで滞留しない。 */
static int consume_pending_ops(void) {
    /* P503 (c2): SRAM ゼロクリアを hard/soft reset より**先**にチェックする。
     * 旧順序(hard_reset が先)では「Clear SRAM → ⌘R」を連続操作したとき、
     * hard reset が先に消費され(mx68k_reset_hard() は SRAM を再読込/再クリア
     * しない設計なので古い SRAM のまま走る)、クリアは次のフレーム境界で
     * 適用されていた。順序を入れ替えることで「クリア後の SRAM でリセット」
     * という利用者の意図通りの順になる(reset は最大1フレーム後へ持ち越し)。 */
    /* P454: SRAM ゼロクリアをフレーム境界で消費する(Swift メインスレッドから
     * 直接 memset すると run_frame 中の SRAM 参照とレースするため)。 */
    if (g_pending_sram_clear) {
        g_pending_sram_clear = 0;
        mx68k_sram_clear();
        /* 実機のバックアップ電池除去に相当する操作のため、次回の完全な電源OFF-ONを
         * 待たずとも効果が及ぶよう、この時点の(ゼロ化された)SRAM を即座にディスクへも
         * 反映する。独自に fopen/fwrite せず既存の mx68k_sram_save()
         * (ensure_app_support_dir() 呼出し込み)をそのまま再利用する。 */
        mx68k_sram_save();
        return 1;   /* 他の pending チェックと同様「1フレーム1アクション」に揃える */
    }
    // フレーム先頭でペンディングリセットを処理（スレッドセーフ）
    if (g_pending_hard_reset) {
        g_pending_hard_reset = 0;
        g_pending_soft_reset = 0;   /* P186: hard 優先時に冗長 soft 再ブートを防ぐ */
        mx68k_reset_hard();
        return 1;
    }
    if (g_pending_soft_reset) {
        g_pending_soft_reset = 0;
        mx68k_reset_soft();
        return 1;
    }
    /* P502 (D-9): SASI fd キャッシュの無効化をフレーム境界で消費する。
     * close() のみの軽量処理なので、上の pending 群と違って return せず
     * そのまま通常の CPU 実行へ fall-through する(1フレームを潰さない)。 */
    if (g_pending_sasi_cache_invalidate) {
        g_pending_sasi_cache_invalidate = 0;
        sasi_io_cache_invalidate_all();
    }
    /* P198: consume a queued state save/load at the frame boundary (emulation
     * thread), so serialization sees a stable, non-mid-instruction machine. */
    /* P481 (D-42): order is "run → store rc/kind → bump seq → clear the pending
     * flag last". The previous order cleared the flag first, which would let a
     * reader that watches the flag observe "done" before the result was stored.
     * Swift polls g_state_op_seq (not the flag), so this ordering is for
     * consistency rather than a live defect. */
    if (atomic_load(&g_pending_save)) {
        int rc = do_save_state(g_pending_state_path);
        debug_log("[P198] do_save_state('%s') rc=%d\n", g_pending_state_path, rc);
        atomic_store(&g_state_op_rc, rc);
        atomic_store(&g_state_op_kind, 1);
        atomic_fetch_add(&g_state_op_seq, 1);
        atomic_store(&g_pending_save, 0);
        return 1;
    }
    if (atomic_load(&g_pending_load)) {
        int rc = do_load_state(g_pending_state_path);
        debug_log("[P198] do_load_state('%s') rc=%d\n", g_pending_state_path, rc);
        atomic_store(&g_state_op_rc, rc);
        atomic_store(&g_state_op_kind, 2);
        atomic_fetch_add(&g_state_op_seq, 1);
        atomic_store(&g_pending_load, 0);
        return 1;
    }
    return 0;
}

/* P503 (c1): 一時停止中(設定シート表示等)でも pending 操作(SRAM clear /
 * hard・soft reset / SASI キャッシュ無効化 / state save・load)を消費する
 * ための公開 API。CVDisplayLink コールバックから、CPU 実行を伴う
 * mx68k_run_frame() の代わりに呼ぶ。非一時停止時の経路は無変更
 * (mx68k_run_frame() が従来通り同じ順序で消費する)。 */
void mx68k_pump_pending(void) {
    (void)consume_pending_ops();
}

void mx68k_run_frame(void) {
    if (consume_pending_ops()) return;

    /* P443 (D-7): ゲスト側イジェクト後も g_fdd_path[] に古いパスが残る副次不具合の
     * 解消。Core が FDD_EjectFD() で StatBar_ParamFDD(drive, 0, ...) を呼ぶため
     * mx68k_fdd_media_present() は既に 0 を返している。挿入側 mx68k_fdd_insert() は
     * FDD_SetFD()(= s_fdd_present[] 更新)の後に g_fdd_path[] を書くよう並べ替え済み
     * なので、挿入直後にここで誤ってクリアする競合窓は無い。 */
    if (!mx68k_fdd_media_present(0) && g_fdd_path[0][0]) g_fdd_path[0][0] = '\0';
    if (!mx68k_fdd_media_present(1) && g_fdd_path[1][0]) g_fdd_path[1][0] = '\0';
    /* P684: ドライブ 2/3 も同じ自己修復を行う。 */
    if (!mx68k_fdd_media_present(2) && g_fdd_path[2][0]) g_fdd_path[2][0] = '\0';
    if (!mx68k_fdd_media_present(3) && g_fdd_path[3][0]) g_fdd_path[3][0] = '\0';

    static int frame_num = 0;
    static int first = 1;
    /* P82-G: publish the current frame index for the m68000_bridge.c
     * chunk-loop sampler (Part B), which has no other access to it. */
    g_mx68k_frame_num = frame_num;

#if P220_PROBE
    /* P220 (c): after the guest is running, has the IPL overwritten our value?
     * Rate-limited to frames 0 / 5 / 60 only. */
    if (frame_num == 0 || frame_num == 5 || frame_num == 60) {
        uint32_t v = ((uint32_t)SRAM[0x08^1] << 24) |
                     ((uint32_t)SRAM[0x09^1] << 16) |
                     ((uint32_t)SRAM[0x0A^1] << 8)  |
                     ((uint32_t)SRAM[0x0B^1]);
        debug_log("[P220-MEMSIZE] c frame=%d mb=%d $ED0008=0x%08x\n",
                  frame_num, g_memory_size_mb, v);
    }
#endif

    /* P504 (D-46 検証プローブ): reset 直後にアームされたカウンタを毎フレーム減算し、
     * 5 フレーム経過時点で 2 標本目を採る。g_mx68k_frame_num 更新後に置くことで
     * frame= フィールドが当該フレームの値になり、post_reset 行との差分が意図通りに
     * なる。consume_pending_ops() の reset 分岐が return するため、reset 実行フレーム
     * そのものではここに到達せず、カウントダウンは次フレーム以降で正しく進む。 */
    if (s_p504_boot_sample_countdown > 0) {
        s_p504_boot_sample_countdown--;
        if (s_p504_boot_sample_countdown == 0) {
            p504_dump_sram_boot_fields("boot+5frames");
            s_p504_boot_sample_countdown = -1;   /* 再送しない */
        }
    }

    /* P82-X-Q (P82XQ-PROBE-Q1) frame-entry invalidate (Plan §3.3 trigger #1):
     * drop any cached chunk-boundary state from the prior frame so the next
     * probe sample re-resolves the canonical guest PC via the Tier-2
     * BasePC-hint walk. Bounded by the 60Hz call rate so cost is trivial. */
    mx68k_probe_pc_cache_invalidate();
    if (first) {
        debug_log("[MX68K] mx68k_run_frame() FIRST CALL\n");
#if P117_EXEC_CYCLE_FEED
        /* P117-AB experiment marker: feeding EXECUTED cycles (ex) to MFP_Timer/RTC_Timer.
         * Absent in control (flag=0) so control debug.log stays byte-identical. */
        debug_log("[P117-AB] EXEC_CYCLE_FEED=1 (experiment: MFP_Timer/RTC fed ex, not sc)\n");
#endif
        /* P34-DIAG: フレーム開始時点でのRTEスタブとVSYNCベクタのランタイム確認 */
        if (MEM) {
            uint16_t stub_word = *(uint16_t*)&MEM[P29_RTE_STUB_ADDR];
            uint16_t vec_hi    = *(uint16_t*)&MEM[0x118];
            uint16_t vec_lo    = *(uint16_t*)&MEM[0x11A];
            uint32_t vec_full  = ((uint32_t)vec_hi << 16) | vec_lo;
            debug_log("[P34-DIAG-STUB] frame=0 runtime: MEM[0x%06x]=0x%04x (expect 0x4E73) "
                      "vec#0x46=0x%08x (expect 0x000FFF00)\n",
                      P29_RTE_STUB_ADDR, stub_word, vec_full);
            /* P35-DIAG: frame=0開始時点でのアドレスエラーベクタ確認 */
            uint16_t buserr_hi2 = *(uint16_t*)&MEM[0x008];
            uint16_t buserr_lo2 = *(uint16_t*)&MEM[0x00A];
            uint32_t buserr_vec2 = ((uint32_t)buserr_hi2 << 16) | buserr_lo2;
            uint16_t aderr_hi2  = *(uint16_t*)&MEM[0x00C];
            uint16_t aderr_lo2  = *(uint16_t*)&MEM[0x00E];
            uint32_t aderr_vec2 = ((uint32_t)aderr_hi2 << 16) | aderr_lo2;
            debug_log("[P35-DIAG-VECTBL] frame=0 start: "
                      "bus_err_vec(#2)=0x%08x addr_err_vec(#3)=0x%08x\n",
                      buserr_vec2, aderr_vec2);
        }
        first = 0;
    }
#if P56_ENABLE
    /* P56 snapshot trigger: at frame == P56_SNAPSHOT_TRIGGER_FRAME (default 60),
     * delegate to m68000_p56_take_snapshot() (m68000_bridge.c 側 file-scope
     * static にアクセスするため). /tmp/mx68k_P56_plan.md §3.4 / Edit F. */
    m68000_p56_take_snapshot(frame_num);
#endif /* P56_ENABLE */
    // P10/P14: reset per-frame counters so the first N events per frame are logged
    m68000_reset_addr_err_count();
    m68000_reset_pcguard_count();

    /* P30-DIAG: MEM[0x118-0x11B]（VSYNC例外ベクタ#0x46）変化監視 */
    if (MEM) {
        static uint32_t s_last_vsync_vec = 0xFFFFFFFF;
        uint32_t cur = (((uint32_t)*(uint16_t*)&MEM[0x118]) << 16) |
                        ((uint32_t)*(uint16_t*)&MEM[0x11A]);
        if (cur != s_last_vsync_vec) {
            debug_log("[P30-DIAG] frame=%d MEM[0x118-11B] changed: 0x%08x -> 0x%08x\n",
                      frame_num, s_last_vsync_vec, cur);
            s_last_vsync_vec = cur;
        }
    }

    /* P47-B-α-DIAG-1: vec#0x44 + MFP IPRB/IMRB/ISRB/TCDR/TDDR 監視
     * 真因観測: IPL boot 中に vec#0x44 が panic placeholder 0x44FF05E4 で
     * 上書きされ、frame=24 で TimerD underflow → panic chain で SR.IPL=6 固着。
     * 変化検出 + frame≤30 + 10frame毎 + 上限200行で網羅的に観測する。 */
    if (MEM) {
        uint32_t v44_now = p47_read_long_le(0x110);
        uint8_t iprb = MFP[MFP_IPRB];
        uint8_t imrb = MFP[MFP_IMRB];
        uint8_t isrb = MFP[MFP_ISRB];
        uint8_t tcdr = MFP[MFP_TCDR];
        uint8_t tddr = MFP[MFP_TDDR];

        int changed = (v44_now != s_p47b_v44_last) ||
                      (iprb   != s_p47b_iprb_last) ||
                      (imrb   != s_p47b_imrb_last) ||
                      (isrb   != s_p47b_isrb_last);
        int periodic = (frame_num <= 30) || ((frame_num % 10) == 0);

        if ((changed || periodic) && s_p47b_diag_cnt < 200) {
            debug_log("[P47-B-α-DIAG-1] frame=%d vec#0x44=0x%08x IPRB=0x%02x IMRB=0x%02x ISRB=0x%02x TCDR=0x%02x TDDR=0x%02x\n",
                      frame_num, v44_now, iprb, imrb, isrb, tcdr, tddr);
            s_p47b_diag_cnt++;
            s_p47b_v44_last  = v44_now;
            s_p47b_iprb_last = iprb;
            s_p47b_imrb_last = imrb;
            s_p47b_isrb_last = isrb;
        }
    }

    /* P47-B-α-FIX: TimerD vec#0x44 (RAM 0x110) panic placeholder pin
     * 真因: IPL boot中に vec#0x44 が 0x44FF05E4 (panic placeholder) で上書きされ
     *       frame=24 で TimerD underflow → panic chain (0xff05e4..0xff063c)
     *       → 0xff063c 自己無限ループで SR.IPL=6 永久固着
     * 対策: 0x44FF05E4 を検出したら P29_RTE_STUB_ADDR (0x000FFF00) に書き戻す。
     *       条件付き上書きなので、Human68k 起動後に正規ハンドラが登録されたら
     *       pin は自然に外れる（0x44FF05E4 以外なら何もしない）。
     *       p47_read_long_le() は内部で bounds check 済み（追加不要）。 */
#if !P206_VACANT_SENTINEL_RESTORE
    {
        uint32_t v44 = p47_read_long_le(0x110);
        if (v44 == 0x44FF05E4U) {
            /* RTE スタブ (0x4E73) は P32-FIX で MEM[P29_RTE_STUB_ADDR] に設置済み */
            *(uint16_t*)&MEM[0x110] = (uint16_t)((P29_RTE_STUB_ADDR >> 16) & 0xFFFFU);
            *(uint16_t*)&MEM[0x112] = (uint16_t)( P29_RTE_STUB_ADDR        & 0xFFFFU);

            if (s_p47b_pin_cnt < 30 ||
                ((s_p47b_pin_cnt % 100) == 0 && s_p47b_pin_cnt < 1000)) {
                debug_log("[P47-B-α-PIN] vec#0x44 (RAM 0x110) pinned: 0x%08x -> 0x%08x at frame=%d (count=%d)\n",
                          v44, P29_RTE_STUB_ADDR, frame_num, s_p47b_pin_cnt);
            }
            s_p47b_pin_cnt++;
        }
    }
#endif

    /* P47-B-α-DIAG-2: panic chain 突入検知 (1回限り)
     * PC が panic dispatch (0xff05e4) ～ panic loop (0xff063c) に入った
     * 最初のフレームで PC/SR/SSP を記録し、pin の効果検証に使う。
     * pin が正しく動作すれば このログは出ない。 */
    if (!s_p47b_panic_logged) {
        uint32_t pc = m68000_get_reg(M68K_PC);
        if (pc >= 0xff05e4U && pc <= 0xff063cU) {
            uint32_t sr  = m68000_get_reg(M68K_SR);
            uint32_t ssp = m68000_get_reg(M68K_SP);
            debug_log("[P47-B-α-DIAG-2] PANIC CHAIN ENTRY frame=%d PC=0x%08x SR=0x%04x SSP=0x%08x\n",
                      frame_num, pc, sr, ssp);
            s_p47b_panic_logged = 1;
        }
    }

    /* P47-A-DIAG-3: vec #0x46 (RAM 0x118) overwrite watch — capped log count separate
     * from P30-DIAG so we can see initial value vs IPLROM MFP-init overwrite. */
    if (MEM) {
        uint32_t cur = p47_read_long_le(0x118);
        if (cur != s_p47_vec46_last && s_p47_vec46_log_cnt < 30) {
            debug_log("[P47-A-DIAG-3] vec46 (RAM 0x118) changed frame=%d: 0x%08x -> 0x%08x\n",
                      frame_num, s_p47_vec46_last, cur);
            s_p47_vec46_last = cur;
            s_p47_vec46_log_cnt++;
        }
    }

    /* P47-A-DIAG-4: $07FC IOCS ABORT pointer change watch */
    if (MEM) {
        uint32_t cur = p47_read_long_le(0x07FC);
        if (cur != s_p47_abort_ptr_last && s_p47_abort_log_cnt < 30) {
            debug_log("[P47-A-DIAG-4] $07FC ABORT ptr changed frame=%d: 0x%08x -> 0x%08x\n",
                      frame_num, s_p47_abort_ptr_last, cur);
            s_p47_abort_ptr_last = cur;
            s_p47_abort_log_cnt++;
        }
    }

    /* P31-DIAG: CGROM領域へのPC侵入検出 */
    if (MEM) {
        static int s_cgrom_entered = 0;
        uint32_t pc = m68000_get_reg(M68K_PC);
        if (pc >= 0x640000U && pc <= 0x6BFFFFU) {
            if (!s_cgrom_entered) {
                debug_log("[P31-DIAG] frame=%d CGROM PC entry: PC=0x%08x SSP=0x%08x\n",
                          frame_num, pc, m68000_get_reg(M68K_SP));
                s_cgrom_entered = 1;
            }
        } else {
            s_cgrom_entered = 0;
        }
    }

    /* P31-DIAG / P47-A-DIAG-5: RTEスタブ実行検出 → スタック内容ダンプ
     * P47-A: removed s_rte_logged single-shot flag, now always logs but rate-limited
     * P47-A-DIAG-1: byte-order bug fixed via p47_read_stack_frame() helper */
    if (MEM) {
        uint32_t pc = m68000_get_reg(M68K_PC);
        if (pc == P29_RTE_STUB_ADDR) {
            uint32_t ssp = m68000_get_reg(M68K_SP);
            uint16_t stk_sr;
            uint32_t stk_pc;
            p47_read_stack_frame(ssp, &stk_sr, &stk_pc);
            /* Rate limit: log every hit while frame<=30, then 1 per 10 frames up to 100 total */
            if ((frame_num <= 30 || (frame_num % 10 == 0)) && s_p47_rte_log_cnt < 100) {
                debug_log("[P47-A-DIAG-5] frame=%d RTE stub hit: SSP=0x%08x stk_SR=0x%04x stk_PC=0x%08x (post-byte-order-fix)\n",
                          frame_num, ssp, stk_sr, stk_pc);
                if (stk_pc & 1) {
                    debug_log("[P47-A-DIAG-5] WARNING: stk_PC=0x%08x is ODD (post-byte-order-fix; suggests Address Error chain)\n",
                              stk_pc);
                }
                s_p47_rte_log_cnt++;
            }
        }
    }

    /* P31-DIAG: frame=2-3 PC遷移トレース */
    {
        static uint32_t s_trace_count = 0;
        if (frame_num >= 2 && frame_num <= 3) {
            uint32_t pc = m68000_get_reg(M68K_PC);
            if ((s_trace_count % 100) == 0) {
                debug_log("[P31-DIAG-TRACE] frame=%d cnt=%u PC=0x%08x SR=0x%04x\n",
                          frame_num, s_trace_count,
                          pc, (uint16_t)m68000_get_reg(M68K_SR));
            }
            s_trace_count++;
        }
    }

    /* P33-DIAG: memory dump near PC=0x657401 (first occurrence only) */
    {
        static int s_p33_dump_done = 0;
        if (!s_p33_dump_done && MEM) {
            uint32_t cur_pc = m68000_get_reg(M68K_PC);
            if (cur_pc >= 0x657000U && cur_pc <= 0x658000U) {
                uint32_t base = cur_pc & 0xFFFFF0U;
                debug_log("[P33-DIAG-DUMP] PC=0x%06x MEM[0x%06x..+15]:", cur_pc, base);
                for (int _di = 0; _di < 16; _di += 2) {
                    debug_log(" %04x", (unsigned)*(uint16_t*)&MEM[base + _di]);
                }
                debug_log("\n");
                debug_log("[P33-DIAG-DUMP] MEM[0x657400..+31]:");
                for (int _di = 0; _di < 32; _di += 2) {
                    debug_log(" %04x", (unsigned)*(uint16_t*)&MEM[0x657400 + _di]);
                }
                debug_log("\n");
                s_p33_dump_done = 1;
            }
        }
    }

    // P25: Hラインループ構造 (upstream WinX68k_Exec() 準拠)
    /* P641(D-65): 1フィールドのCPUクロック予算を CRTC レジスタ(R00/R04/R20/HRL)
     * から動的に導出する。旧実装は CRTC_Regs[0x29] bit4 だけを見た固定2値
     * (VSYNC_HIGH 180310 / VSYNC_NORM 162707)で、レジスタを書き換えても実際の
     * 垂直リフレッシュレートが追従しなかった。MPX68K PR#26 の
     * WinX68k_FieldCycles10M() 相当。
     * ★CRTC_GetFrameClocks() は分数(端数)状態を進めるため、1フレームにつき
     *   1回だけ呼ぶこと。0/負が返る想定は無いが、万一のときは従来の固定2値へ
     *   退避する(px68k-libretro 個人フォーク PR の WinX68k_Exec ゼロガード踏襲)。 */
    int32_t clk_total;
    int32_t crtc_vline_total  = 0;
    int32_t crtc_frame_clocks = CRTC_GetFrameClocks(&crtc_vline_total);
    if (crtc_frame_clocks > 0) {
        clk_total = crtc_frame_clocks;
    } else {
        crtc_vline_total = 0;    /* 退避時は従来どおり VLINE_TOTAL 側を使う */
        if (CRTC_Regs[0x29] & 0x10)
            clk_total = VSYNC_HIGH;  // 180310: 15kHz高解像度モード
        else
            clk_total = VSYNC_NORM;  // 162707: 31kHz標準モード
    }
    /* P641: クロック倍率スケーリング前(名目10MHz基準)の値を保持する。
     * mx68k_get_vsync_hz() がこれを 10e6 で割って垂直周波数を返す。 */
    g_p641_frame_clocks_10m = clk_total;

    /* P180: MX's c68k core (Core/c68k/c68kexec.c) counts REAL MC68000 cycles — it does
     * NOT apply the 1/5-MHz scaling that MPX's WinX68k_Exec comment assumes. P149 copied
     * MPX's clkdiv = clock*5, which over-ran the CPU 5x (a standard X68000 benchmark showed
     * 826% at 16MHz vs the correct ~160%). Use clkdiv = clock so clk_total = base*(clock/10)
     * gives the true MHz (10->base*1.0=100%, 16->base*1.6=160%). Timer feed usedclk =
     * clk_total*10/clkdiv = base is invariant, so MFP/RTC/OPM/ADPCM tick rate is unchanged.
     * clkdiv MUST be > 0 — usedclk = ClkUsed/clkdiv would divide-by-zero if g_clock_mhz==0
     * (mx68k_set_clock_mhz has no clamp; Swift config could pass 0). crash-0 is a merge gate. */
    int32_t clkdiv = (g_clock_mhz > 0 ? g_clock_mhz : 16);      /* P180: was *5. 16MHz -> 16 */
    clk_total = (int32_t)(((int64_t)clk_total * clkdiv) / 10);  /* CPU budget = base*(clock/10) */

    /* P641: clk_total と対になる走査線数。CRTC_GetFrameClocks() は、ゲストが
     * CRTC レジスタを順に書き換えている途中(組合せが一時的に不正)でも直前の
     * 完全な設定の走査線数を返すため、予算(分子)と走査線数(分母)が食い違わない
     * ——MPX68K PR#26 が VLINE_TOTAL を active_vline_total へ置き換えたのと同じ
     * 理由。退避経路では従来の VLINE_TOTAL ゼロガードをそのまま使う。 */
    int32_t vline_total_val = (crtc_vline_total > 0) ? crtc_vline_total
                            : ((VLINE_TOTAL > 0) ? VLINE_TOTAL : 567);

    /* P26-DIAG: IPL範囲でのPC固着を診断（frame 0〜5のみ） */
    if (frame_num <= 5) {
        uint32_t cur_pc = C68k_Get_PC(&C68K);
        if (cur_pc >= 0xFC0000 && cur_pc < 0x1000000) {
            uint32_t offset = cur_pc & 0x1FFFE;
            debug_log("[P26-DIAG] pre-frame=%d PC=%06x IPL[%05x]=%02x%02x\n",
                      frame_num, cur_pc, offset, IPL[offset], IPL[offset+1]);
        }
    }

    // P22.5-DMA: Dump ch0 state before H-line loop (frames 0-20 only)
    if (frame_num >= 0 && frame_num <= 20) {
        debug_log("[P22.5-DMA] pre-CPU frame=%d ch0: CSR=%02x CCR=%02x OCR=%02x SCR=%02x MAR=%06x MTC=%04x DAR=%06x\n",
                  frame_num, DMA[0].CSR, DMA[0].CCR, DMA[0].OCR, DMA[0].SCR,
                  DMA[0].MAR, DMA[0].MTC, DMA[0].DAR);
    }

    // P25: Hラインループ – upstream WinX68k_Exec() の do/while(vline<VLINE_TOTAL) に対応
    int32_t total_executed = 0;
    int KeyIntCnt   = 0;
    int MouseIntCnt = 0;

    /* P385: CLOCK_SLICE sub-line chunk size — px68k原典 winx68k.cpp:319 の値。
     * 各走査線のCPU予算をこのサイクル数以下のチャンクに刻んで実行し、IRQ/timer の
     * 粒度を ≤200 cyc に保つ (c68k は m68000_execute の呼び出し境界でしか IRQ を
     * 検査しない)。
     * P149 は MPX winx68k.cpp:353 の 1500 (MPX が性能目的で px68k の 200 から
     * 引き上げた値) を採っていたが、MPX はそれを clkdiv = clock*5 と組で使っており、
     * MX は P180 で clkdiv を px68k 式 (= clock) に戻したためこの組が崩れていた。
     * 200 に戻すことで clkdiv と CLOCK_SLICE が再び px68k 原典と同じ組になる。
     * なお P385 の走査線境界クランプ (下記 do ループ先頭) により、契約
     * 「1反復はその走査線を跨がない」は CLOCK_SLICE の数値的余裕には依存しない。 */
#ifndef CLOCK_SLICE
#define CLOCK_SLICE 200
#endif
    /* P149: MPX ClkUsed remainder accumulator (winx68k.cpp global). static so the
     * ≤clkdiv remainder carries across chunks, scanlines AND frames — byte-identical
     * to MPX. Do NOT reset per frame. Single-threaded (CVDisplayLink frame loop). */
    static int32_t ClkUsed = 0;

    /* P37-DIAG: フレーム先頭SSP記録 */
    static uint32_t s_p37_frame_start_ssp = 0;
    static int      s_p37_vsync_count = 0;
    if (frame_num < 20) {
        s_p37_frame_start_ssp = m68000_get_reg(M68K_MSP);
        s_p37_vsync_count = 0;
        debug_log("[P37-DIAG] frame=%d FRAME-START SSP=0x%08x\n",
                  frame_num, s_p37_frame_start_ssp);
    }

    /* P153: frame-local CPU budget. Named frame_icount (NOT ICount) to avoid
     * shadowing the Core global int32_t ICount (m68000_bridge.c:72), which
     * Core mfp.c:175 reads as hpos = ICount % HSYNC_CLK for the MFP GPIP H-SYNC
     * status bit.
     * P158: MX now mirrors that global (ICount = clk_total at frame start,
     * ICount -= n per chunk, below) so mfp.c's hpos sweeps like MPX and the
     * MFP GPIP H-SYNC bit toggles — fixing the H-SYNC wait-loop spin. */
    /* P153: MPX WinX68k_Exec whole-frame seed (winx68k.cpp:386-410).
     * frame_icount is a frame-LOCAL budget (MX carries no cross-frame ICount; each
     * frame runs exactly clk_total, as the old clk_per_line*VLINE+remainder did).
     * Invariant across the loop: clk_count + frame_icount == clk_total. */
    int32_t frame_icount = clk_total;       /* MPX: ICount += clk_total (fresh each frame) */
    ICount = clk_total;                     /* P158: mirror Core global ICount (mfp.c H-SYNC hpos = ICount % HSYNC_CLK) */
    int32_t clk_count = 0;                  /* MPX: clk_count = -ICount_old -> 0 here      */
    int32_t clk_next  = clk_total / vline_total_val;   /* first line boundary (vl==0)      */
    int32_t clk_line_start = 0;   /* P657: 現在走査線の開始位置(スケール済)。line_len の算出に使う */
    int     hsync     = 1;
    int     vl        = 0;
    int32_t line_usedclk = 0;               /* MPX clk_line: per-line normalized clock     */

#if P385_ENABLE
    /* P385: 走査線チャンク不変条件「n==0 のチャンクは発生しない」の常設監視。
     * zero だけでは「本当に0回」と「プローブ未到達」が区別できないため、
     * 分母 (chunks / cum_chunks) を必ず併記する。 */
    int32_t p385_chunks = 0, p385_zero = 0, p385_first_zero_vl = -1;
    static unsigned long long s_p385_cum_chunks = 0, s_p385_cum_zero = 0;
#endif

#if P657_PROBE_ENABLE
    /* P657 測定1: CRTC_HorizontalFrontPorch() の発火回数と、その内訳・分母。
     * 「0件」の解釈が一意に落ちるよう、分子(fp_calls/rc_exec)と分母(chunks)を
     * 必ず同一行に出す:
     *   chunks==0            → プローブ未到達
     *   fp_calls==0 && chunks>0 → 境界計算が誤り(境界に到達していない)
     *   fp_calls>0 && rc_exec==0 → ゲストが動作ポート bit3 を立てていない
     * 派生値は出さず、すべて生カウント。取得時刻は行頭の f=<frame_num>。 */
    int32_t p657_chunks = 0, p657_fp_calls = 0, p657_rc_exec = 0;
    int32_t p657_rc_noop_plane = 0, p657_rc_noop_same = 0;
    /* P657 測定3: 境界計算の入力そのもの(生値)。スナップショット経路を通さず
     * フレームループ内でその場の値を読む(P373 の鮮度欠陥を繰り返さないため)。 */
    int32_t p657_line_len_snap = 0;
#endif

#if P214_ENABLE
    p214_r1_frame_reset();   /* P214: 行ごと変化検出をフレーム頭でリセット（read-only） */
#endif

    /* P294: フレーム前処理(back バッファ選択・disp 幾何確定・memset・集計 reset)を
     * exec ループ直前で1回。以降 draw_line が表示行を per-scanline で描き込む。 */
    mx68k_render_begin();

    do {
        /* P385: 1反復が走査線境界を跨がないようクランプする。参照実装(px68k :366 /
         * MPX :479)はこれを持たず、暗黙の数値条件 CLOCK_SLICE <= clk_total/VLINE_TOTAL
         * にのみ依存している。MX は P180 で clkdiv を px68k 式に戻した際 CLOCK_SLICE を
         * MPX 値(1500)のまま残したためこの条件が破れ、vl>=193 で n=0 となり
         * フレーム後半のCPU実行が丸ごと失われていた([P299-SPRVLINE] vl_max=192 で実測)。
         * 本クランプは数値的余裕に依存しないため将来の設定変更でも再発しない。 */
        int32_t line_left = clk_next - clk_count;   /* 現在ラインの残余 */
        int32_t n = frame_icount;
        if (n > CLOCK_SLICE) n = CLOCK_SLICE;
        if (n > line_left)   n = line_left;
        if (n < 0)           n = 0;   /* 防御: 負値は ClkUsed を逆走させる */

        /* P657-V: px68k-libretro(uraraworks) PR#2 逐語移植
         * (merge commit cd88e2ea3337777062117a14daea56e3b18953f4)。
         * CPU実行スライスが表示開始/表示終了境界をまたがないようにクランプする。
         *
         * ★空間の不変条件(PR#2 の libretro.c が原文コメントで要求):
         *     "Keep these display boundaries aligned with GetGPIP() in x68k/mfp.c."
         *   ここの display_start / display_end は Core/px68k/x68k/mfp.c:181 の
         *   GPIP 比較式と**同一の変数・同一の式・同一の整数型**でなければならない。
         *   両者が同じ空間にいる限り、ラスタ周期の絶対値が実機と何倍ずれていても、
         *   ゲストが GPIP で観測する H-SYNC 遷移と、ラスタコピーが実際に走る契機とは
         *   1対1のロックステップになる。
         *
         *   P657 初版はここだけを clkdiv スケール済の line_len 空間へ移し
         *   (mfp.c は名目10MHz の ICount % HSYNC_CLK のまま)、この不変条件を破った。
         *   16MHz では比が 509/317 = 1.61 となり、ゲストが発行した src/dst 組の
         *   約38%がコピーされずスクロール時の二重写しになった(ユーザーの
         *   hands-on 確認、2026-08-21)。本修正は PR#2 の空間へ戻す。
         *
         *   残存する既知の乖離: HSYNC_CLK は名目10MHz、ICount は clkdiv スケール済
         *   なので、発火の絶対周期は実ラスタの clkdiv/10 倍速のままである。これは
         *   参照実装と共有する不整合であり(Docs/09 D-68 の後続候補=案S)、
         *   本サイクルでは是正しない。 */
        int32_t until_display_end = -1;
#if P657_PROBE_ENABLE
        {
            /* 測定3: line_len は境界計算に使われなくなったが、受け入れた残存スケール
             * 不整合(line_len ÷ HSYNC_CLK)を記録するため生値の採取は継続する。 */
            int32_t line_len_now = clk_next - clk_line_start;
            if (line_len_now > 0) p657_line_len_snap = line_len_now;
        }
#endif
        if (HSYNC_CLK > 0 && CRTC_Regs[0x01] != 0) {
            /* mfp.c:180-181 と同型: int32_t 演算・同一の切り捨て順序。
             * ICount はフレーム先頭で clk_total、チャンクごとに -= n されるため
             * 走査中は常に非負(n は frame_icount 以下にクランプ済、:3394)。 */
            int32_t hpos          = (int32_t)(ICount % HSYNC_CLK);
            int32_t display_start = (int32_t)CRTC_Regs[0x07] * HSYNC_CLK / CRTC_Regs[0x01];
            int32_t display_end   = (int32_t)CRTC_Regs[0x05] * HSYNC_CLK / CRTC_Regs[0x01];
            int32_t until_display_start;
            until_display_end   = (hpos > display_end)
                                ? (hpos - display_end)
                                : (hpos + HSYNC_CLK - display_end);
            until_display_start = (hpos > display_start)
                                ? (hpos - display_start)
                                : (hpos + HSYNC_CLK - display_start);
            if (until_display_start > 0 && n > until_display_start) n = until_display_start;
            if (until_display_end   > 0 && n > until_display_end)   n = until_display_end;
            if (n < 0) n = 0;
        }

#if P657_PROBE_ENABLE
        p657_chunks++;   /* 測定1の分母 */
#endif

#if P385_ENABLE
        p385_chunks++;
        if (n == 0) { p385_zero++; if (p385_first_zero_vl < 0) p385_first_zero_vl = vl; }
#endif

        /* ===== LINE-START (hsync) block — 1 回/ライン ===== */
        if (hsync) {
            hsync = 0;
            line_usedclk = 0;

            // HSYNC 割り込み（毎ライン、upstream: MFP_Int(0) at hsync)
            mx68k_diag_mfp_int(0, "hsync"); /* P47-D-DIAG-G */

#if P384_ENABLE
            /* P384: VSYNC(irq=9、IPRB側)のマスク/保留/サービス状態の周期ダンプ。
             * 派生値を含まない生レジスタ値のみ。読み取りのみ。 */
            if (vl == 0 && (g_mx68k_frame_num % 300 == 0)) {
                debug_log("[P384-VSYNCMASK] f=%d IERB=0x%02x IMRB=0x%02x IPRB=0x%02x ISRB=0x%02x\n",
                          g_mx68k_frame_num,
                          (unsigned)MFP[MFP_IERB], (unsigned)MFP[MFP_IMRB],
                          (unsigned)MFP[MFP_IPRB], (unsigned)MFP[MFP_ISRB]);
            }
#endif

            // VLINE 更新 (upstream 準拠)
            if (vl >= (int)CRTC_VSTART && vl < (int)CRTC_VEND)
                VLINE = ((int32_t)(vl - (int)CRTC_VSTART) * (int32_t)CRTC_VStep) / 2;
            else
                VLINE = (int32_t)-1;
            VLINEBG = VLINE;
            x68k_vline = vl;

#ifndef P358_ENABLE
#define P358_ENABLE 1   /* OverTake VC21変動タイミング検証用、一時的に有効化 */
#endif
#if P358_ENABLE
            if ((VLINE == 50 || VLINE == 100 || VLINE == 150 || VLINE == 200 || VLINE == 250) &&
                (g_mx68k_frame_num % 60 == 0)) {
                debug_log("[P358-START] f=%d VLINE=%d VC21=%02x C16=%02x C17=%02x\n",
                          g_mx68k_frame_num, VLINE, VCReg2[1], CRTC_Regs[0x16], CRTC_Regs[0x17]);
            }
#endif

#if P378_ENABLE
            /* P378: 表示期間中に BG/スプライトのゲートレジスタが本当に一度も
             * 動かないかを、ハッシュではなく値そのもので確認する(既存 [P214]
             * chg_vc の行末サンプルでは vblank 中や行内で戻る変化を取りこぼす)。
             * 読み取りのみ・エミュレーション状態は一切変更しない。
             * フレーム終端の判定は下の VSYNC ブロックと同じラップ補正を流用する:
             * CRTC_VEND >= vline_total_val の構成では走査線ループ
             * do{}while(vl < vline_total_val) が vl==CRTC_VEND に到達しないため、
             * 素朴な比較では打ち切りカウンタが永久に進まない。 */
            {
                static int s_p378_gatewatch_frames = 0;
                /* P380 訂正: VCReg2[1]==0x6f はカウントダウン画面でも成立し
                 * レース中固有の条件になっていなかった。BG0 表示イネーブル
                 * ビット(BG_Regs[9] bit0)そのものをトリガにする。 */
                if ((BG_Regs[9] & 1) && s_p378_gatewatch_frames < 10) {
                    int p378_endline = ((int)CRTC_VEND >= (int)vline_total_val)
                                       ? ((int)CRTC_VEND - (int)vline_total_val)
                                       : ((int)vline_total_val - 1);
                    int p378_mid = ((int)CRTC_VSTART + (int)CRTC_VEND) / 2;
                    if (vl == 0 || vl == (int)CRTC_VSTART || vl == p378_mid ||
                        vl == (int)CRTC_VEND || vl == p378_endline) {
                        debug_log("[P378-GATEWATCH] f=%d vl=%d BG8=%02x BG9=%02x BG11=%02x VC2_1=%02x\n",
                                  g_mx68k_frame_num, vl,
                                  (unsigned)BG_Regs[8], (unsigned)BG_Regs[9],
                                  (unsigned)BG_Regs[0x11], (unsigned)VCReg2[1]);
                        if (vl == p378_endline) s_p378_gatewatch_frames++;
                    }
                }
            }
#endif

            // CRTC IntLine 割り込み (MFP_AER bit6 == 0 のとき)
            if (!(MFP[MFP_AER] & 0x40) && (vl == (int)CRTC_IntLine)) {
                mx68k_diag_mfp_int(1, "crtc-aer0"); /* P47-D-DIAG-G */
#if P295_ENABLE
                s_p295_raster_irq_fires++;
#endif
            }

            // VSYNC 割り込み (upstream 準拠)
            if (MFP[MFP_AER] & 0x10) {
                if (vl == (int)CRTC_VSTART) {
                    /* P40-FIX: P28-FIX の SR IPL 操作を削除。CPU コア (c68k) が CHECK_INT で IRQ マスクを自動処理するため、外部からの SR 操作は不要かつ有害。SR IPL=6 でハンドラ実行中に次の IRQ6 が遮断されるのは正しい動作。 */
                    /* P33-DIAG: VSYNC fire state (each branch counts independently, up to 5 each = max 15 total) */
                    {
                        static int s_p33_vsync_fire_count = 0;
                        if (s_p33_vsync_fire_count < 5) {
                            uint32_t _sr  = m68000_get_reg(M68K_SR);
                            uint32_t _ssp = m68000_get_reg(M68K_MSP);
                            debug_log("[P33-DIAG-VSYNC] frame=%d vl=%d MFP_VR=0x%02x IERB=0x%02x IMRB=0x%02x "
                                      "IPRB=0x%02x ISRB=0x%02x IRQLine=%d SR=0x%04x SSP=0x%08x (fire#%d)\n",
                                      frame_num, vl, MFP[MFP_VR], MFP[MFP_IERB], MFP[MFP_IMRB],
                                      MFP[MFP_IPRB], MFP[MFP_ISRB],
                                      C68K.IRQLine, (unsigned)_sr, _ssp,
                                      s_p33_vsync_fire_count + 1);
                            s_p33_vsync_fire_count++;
                        }
                    }
                    if (frame_num < 20) { s_p37_vsync_count++; }
                    mx68k_diag_mfp_int(9, "vsync-A"); /* P47-D-DIAG-G */
                }
            } else {
                if ((int)CRTC_VEND >= (int)vline_total_val) {
                    if (vl == (int)CRTC_VEND - (int)vline_total_val) {
                        /* P40-FIX: P28-FIX の SR IPL 操作を削除。CPU コア (c68k) が CHECK_INT で IRQ マスクを自動処理するため、外部からの SR 操作は不要かつ有害。SR IPL=6 でハンドラ実行中に次の IRQ6 が遮断されるのは正しい動作。 */
                        /* P33-DIAG: VSYNC fire state (each branch counts independently, up to 5 each = max 15 total) */
                        {
                            static int s_p33_vsync_fire_count = 0;
                            if (s_p33_vsync_fire_count < 5) {
                                uint32_t _sr  = m68000_get_reg(M68K_SR);
                                uint32_t _ssp = m68000_get_reg(M68K_MSP);
                                debug_log("[P33-DIAG-VSYNC] frame=%d vl=%d MFP_VR=0x%02x IERB=0x%02x IMRB=0x%02x "
                                          "IPRB=0x%02x ISRB=0x%02x IRQLine=%d SR=0x%04x SSP=0x%08x (fire#%d)\n",
                                          frame_num, vl, MFP[MFP_VR], MFP[MFP_IERB], MFP[MFP_IMRB],
                                          MFP[MFP_IPRB], MFP[MFP_ISRB],
                                          C68K.IRQLine, (unsigned)_sr, _ssp,
                                          s_p33_vsync_fire_count + 1);
                                s_p33_vsync_fire_count++;
                            }
                        }
                        if (frame_num < 20) { s_p37_vsync_count++; }
                        mx68k_diag_mfp_int(9, "vsync-B"); /* P47-D-DIAG-G */
                    }
                } else {
                    if (vl == (int)vline_total_val - 1) {
                        /* P40-FIX: P28-FIX の SR IPL 操作を削除。CPU コア (c68k) が CHECK_INT で IRQ マスクを自動処理するため、外部からの SR 操作は不要かつ有害。SR IPL=6 でハンドラ実行中に次の IRQ6 が遮断されるのは正しい動作。 */
                        /* P33-DIAG: VSYNC fire state (each branch counts independently, up to 5 each = max 15 total) */
                        {
                            static int s_p33_vsync_fire_count = 0;
                            if (s_p33_vsync_fire_count < 5) {
                                uint32_t _sr  = m68000_get_reg(M68K_SR);
                                uint32_t _ssp = m68000_get_reg(M68K_MSP);
                                debug_log("[P33-DIAG-VSYNC] frame=%d vl=%d MFP_VR=0x%02x IERB=0x%02x IMRB=0x%02x "
                                          "IPRB=0x%02x ISRB=0x%02x IRQLine=%d SR=0x%04x SSP=0x%08x (fire#%d)\n",
                                          frame_num, vl, MFP[MFP_VR], MFP[MFP_IERB], MFP[MFP_IMRB],
                                          MFP[MFP_IPRB], MFP[MFP_ISRB],
                                          C68K.IRQLine, (unsigned)_sr, _ssp,
                                          s_p33_vsync_fire_count + 1);
                                s_p33_vsync_fire_count++;
                            }
                        }
                        if (frame_num < 20) { s_p37_vsync_count++; }
                        mx68k_diag_mfp_int(9, "vsync-C"); /* P47-D-DIAG-G */
                    }
                }
            }
#if P136_ENABLE
            p136_poll(P136_PRE_CPU, vl);    /* 前 line 末〜この line CPU 実行前 */
#endif
        }

        /* ===== per-CHUNK: CPU + timers — 毎イテレーション（MPX 474-497） =====
         * P149/P153: CLOCK_SLICE ≤1500 sub-line chunk mirroring MPX WinX68k_Exec
         * inner do{} (winx68k.cpp:412-494). After Edit A's clkdiv scaling the CPU
         * budget can exceed CLOCK_SLICE, so the whole frame runs in ≤1500-cyc chunks
         * and MFP/RTC receive the clkdiv-normalized clock per chunk. This keeps
         * IRQ/timer granularity MPX-equivalent (c68k checks IRQs only at execute
         * call boundaries). */
        int32_t ex = m68000_execute(n);     /* executed cycles: bookkeeping only */
        total_executed += ex;
        /* P657: 水平フロントポーチ到達 → ラスタコピーの唯一の実行契機。
         * PR#2 は m(= n - m68000_ICountBk)で判定するが、MX は m ≡ n
         * (m68000_ICountBk は常に0、下記 P152 コメント)。
         * MFP_Timer() より前に置くのは PR#2 と同じ順序。 */
#if P657_FRONTPORCH_ENABLE
        if (until_display_end > 0 && n >= until_display_end) {
#if P657_PROBE_ENABLE
            /* 測定1の内訳。Core パッチ側からは取得できないため Bridge 側で
             * 同じ条件を読み取って分類する(いずれも crtc.h 公開の
             * グローバルで、ここでは読み取りのみ)。 */
            p657_fp_calls++;
            if (CRTC_Mode & 8) {
                if (!(CRTC_Regs[0x2b] & 0x0f))                 p657_rc_noop_plane++;
                else if (CRTC_Regs[0x2c] == CRTC_Regs[0x2d])   p657_rc_noop_same++;
                else                                           p657_rc_exec++;
            }
#endif
#if P670_PROBE_ENABLE
            /* P670 (D-69): 実際に転送が起きた回数の累積(= [P670-RCDROP] の分子)。
             * 上の p657_rc_exec と同じ分岐条件だが、P657_PROBE_ENABLE の値に
             * 依存して黙って 0 のままになることがないよう独立に評価する。
             * 既存のフレームローカル p657_rc_exec と違いリセットしない。 */
            if ((CRTC_Mode & 8) && (CRTC_Regs[0x2b] & 0x0f)
                && CRTC_Regs[0x2c] != CRTC_Regs[0x2d]) {
                g_p670_rc_exec_cum++;
            }
#endif
            CRTC_HorizontalFrontPorch();
        }
#endif
        /* P152: charge the timer by the REQUESTED slice n, NOT executed ex.
         * MPX WinX68k_Exec feeds MFP/RTC m = n - m68000_ICountBk, and
         * m68000_ICountBk ≡ 0 (init 0; its only writes are in #if 0 dead code),
         * so m = n. At interrupt/chunk boundaries c68k under-runs the slice
         * (ex < n); charging ex under-fed Timer-C ~3.3% and fired the FF0B48
         * calibration late (icount 308493 vs MPX 308239). Charge n and drain
         * frame_icount by n to match MPX exactly (winx68k.cpp:484-490). */
        ClkUsed += n * 10;
        int32_t usedclk = ClkUsed / clkdiv;
        ClkUsed -= usedclk * clkdiv;
        line_usedclk += usedclk;
        MFP_Timer(usedclk);   /* per-chunk feed → IRQ/timer granularity ≤1500 cyc */
#if P404_ENABLE
        /* P404-TCWATCH: D-23調査。MFP Timer C の「分母」(= MFP_Timer() 呼出し回数)と
         * 制御レジスタ生値を 30 フレーム毎に無条件ダンプする。Timer C 沈黙時に
         * (i) TCDCR で停止 / (ii) IERB・IMRB でマスク / (iii) tick 自体が来ていない
         * の 3 通りを 1 本のログから排他的に判定できるようにするための読み取り専用プローブ。 */
        {
            static unsigned s_p404_mfp_timer_calls = 0;
            static int s_p404_last_log_frame = -1;
            s_p404_mfp_timer_calls++;
            if (g_mx68k_frame_num % 30 == 0 && g_mx68k_frame_num != s_p404_last_log_frame) {
                int delta_f = (s_p404_last_log_frame >= 0)
                            ? (g_mx68k_frame_num - s_p404_last_log_frame) : -1;
                debug_log("[P404-TCWATCH] f=%d df=%d TCDCR=%02x TCDR=%02x "
                          "IERB=%02x IMRB=%02x IPRB=%02x mfp_timer_calls=%u\n",
                          g_mx68k_frame_num, delta_f,
                          (unsigned)MFP[MFP_TCDCR], (unsigned)MFP[MFP_TCDR],
                          (unsigned)MFP[MFP_IERB], (unsigned)MFP[MFP_IMRB],
                          (unsigned)MFP[MFP_IPRB], s_p404_mfp_timer_calls);
                s_p404_mfp_timer_calls = 0;
                s_p404_last_log_frame = g_mx68k_frame_num;
            }
        }
#endif
#if P404_ENABLE
        /* P404-TCEDGE: D-23調査。TCDCR のプリスケーラ(bit4-6)が 0↔非0 に遷移した
         * 瞬間だけ発火する遷移検出。遷移が無ければ出力ゼロ = 「その区間で TCDCR は
         * 変化していない」という一意の解釈になり、上の周期ダンプと相互に無信号の
         * 意味を補強する。読み取りのみ・ゲスト状態は一切変更しない。 */
        {
            static int s_p404_tcdcr_active = -1;   /* -1=未初期化 */
            int now_active = (MFP[MFP_TCDCR] & 0x70) ? 1 : 0;
            if (s_p404_tcdcr_active >= 0 && now_active != s_p404_tcdcr_active) {
                uint32_t pc_now = m68000_get_reg(M68K_PC);
                debug_log("[P404-TCEDGE] f=%d TCDCR %02x->%s PC=0x%06x\n",
                          g_mx68k_frame_num,
                          (unsigned)MFP[MFP_TCDCR],
                          now_active ? "active" : "inactive",
                          (unsigned)pc_now);
            }
            s_p404_tcdcr_active = now_active;
        }
#endif
        RTC_Timer(usedclk);   /* RP5C15 1Hz/16Hz alarm, right after MFP (winx68k.cpp:493-494) */
        frame_icount -= n;    /* MPX: ICount -= m, m == n */
        ICount -= n;          /* P158: mirror Core global ICount so mfp.c H-SYNC bit sweeps (MPX winx68k.cpp:488) */
        clk_count    += n;

        /* ===== LINE-END block — 1 回/ライン（MPX 499-537） ===== */
        if (clk_count >= clk_next) {
#if P136_ENABLE
            p136_poll(P136_CPU_EXEC, vl);   /* CPU 実行区間直後 */
#endif

            /* P37-DIAG: 各Hライン実行後のSSP変化記録（SSP変化時のみ出力、全ライン対象） */
            if (frame_num < 20 && vl < 600) {
                static uint32_t s_p37_prev_ssp = 0;
                uint32_t _p37_ssp = m68000_get_reg(M68K_MSP);
                if (_p37_ssp != s_p37_prev_ssp) {
                    uint32_t _p37_pc = m68000_get_reg(M68K_PC);
                    int32_t  _p37_delta = (int32_t)(s_p37_frame_start_ssp - _p37_ssp);
                    debug_log("[P37-DIAG] frame=%d vl=%d POST-EXEC SSP=0x%08x "
                              "frame-delta=%+d PC=0x%06x IERA=0x%02x IPRB=0x%02x ISRB=0x%02x\n",
                              frame_num, vl, _p37_ssp, _p37_delta,
                              _p37_pc, MFP[MFP_IERA], MFP[MFP_IPRB], MFP[MFP_ISRB]);
                    if (MEM && _p37_ssp >= 0x400 && _p37_ssp < (uint32_t)(12*1024*1024 - 6)) {
                        /* P47-A-DIAG-1: byte-order bug fixed via p47_read_stack_frame() */
                        uint16_t _p37_stk_sr;
                        uint32_t _p37_stk_pc;
                        p47_read_stack_frame(_p37_ssp, &_p37_stk_sr, &_p37_stk_pc);
                        debug_log("[P37-DIAG] frame=%d vl=%d STK-TOP: SR=0x%04x RetPC=0x%08x (post-byte-order-fix)\n",
                                  frame_num, vl, _p37_stk_sr, _p37_stk_pc);
                    }
                    s_p37_prev_ssp = _p37_ssp;
                }
            }

            /* P33-DIAG: frame=0 BIOS initialization trace (every 200 H-lines) */
            {
                static uint32_t s_p33_bios_cnt = 0;
                if (frame_num == 0) {
                    if ((s_p33_bios_cnt % 200) == 0 && s_p33_bios_cnt < 2000) {
                        uint32_t cur_pc  = m68000_get_reg(M68K_PC);
                        uint32_t cur_sr  = m68000_get_reg(M68K_SR);
                        uint32_t cur_ssp = m68000_get_reg(M68K_MSP);
                        uint32_t vvec = MEM ? ((((uint32_t)*(uint16_t*)&MEM[0x118]) << 16) |
                                                ((uint32_t)*(uint16_t*)&MEM[0x11A])) : 0;
                        debug_log("[P33-DIAG-BIOS] cnt=%04u vl=%d PC=0x%06x SR=0x%04x SSP=0x%08x VSYNC_vec=0x%08x\n",
                                  s_p33_bios_cnt, vl, cur_pc, (unsigned)cur_sr, cur_ssp, vvec);
                    }
                    s_p33_bios_cnt++;
                }
            }

            /* P149: MFP タイマー feed relocated into the CLOCK_SLICE chunk loop above
             * (per-chunk MFP_Timer(usedclk)). The P117_EXEC_CYCLE_FEED branches are
             * retired — the normalized per-chunk feed supersedes both. The per-line
             * poll/sampler snapshots below stay (read-only, sampled once per line). */
#if P136_ENABLE
            p136_poll(P136_MFP_TIMER, vl);
#endif

#if P82A_ENABLE
            /* P82-A-3: detect Timer-C IPRB[bit5=0x20] rising edge.
             * MFP_Timer fires MFP_Int(10) which sets IPRB |= 0x20.
             * We sample IPRB before and after MFP_Timer to catch the moment. */
            {
                static uint8_t s_p82a3_iprb_prev = 0;
                static int s_p82a3_count = 0;
                uint8_t iprb_now = MFP[MFP_IPRB];
                if ((iprb_now & 0x20u) && !(s_p82a3_iprb_prev & 0x20u) &&
                    s_p82a3_count < P82A_TIMERC_RISE_CAP) {
                    uint32_t cur_pc  = m68000_get_reg(M68K_PC);
                    uint32_t cur_sr  = m68000_get_reg(M68K_SR);
                    uint32_t d0      = m68000_get_reg(M68K_D0);
                    uint32_t d7      = m68000_get_reg(M68K_D7);
                    debug_log("[P82-A-3] Timer-C fired: IPRB=0x%02x IERB=0x%02x IMRB=0x%02x "
                              "TCDCR=0x%02x TCDR=0x%02x "
                              "PC=0x%06x SR=0x%04x D0=0x%08x D7=0x%08x "
                              "frame=%d vl=%d (count=%d)\n",
                              (unsigned)iprb_now, (unsigned)MFP[MFP_IERB],
                              (unsigned)MFP[MFP_IMRB], (unsigned)MFP[MFP_TCDCR],
                              (unsigned)MFP[MFP_TCDR],
                              (unsigned)cur_pc, (unsigned)cur_sr,
                              (unsigned)d0, (unsigned)d7,
                              frame_num, vl, s_p82a3_count + 1);
                    s_p82a3_count++;
                }
                s_p82a3_iprb_prev = iprb_now;
            }
#endif /* P82A_ENABLE */

            /* P47-C-3 / P149: RTC_Timer (RP5C15 1Hz/16Hz alarm) feed relocated into the
             * CLOCK_SLICE chunk loop above (per-chunk RTC_Timer(usedclk)), right after
             * MFP_Timer as MPX does (winx68k.cpp:493-494). P117 branches retired. */
#if P136_ENABLE
            p136_poll(P136_RTC_TIMER, vl);
#endif

            // MFP TimerA + CRTC IntLine (AER bit6 == 1 のとき) – upstream の clk_count>=clk_next ブロック
            MFP_TimerA();
            if ((MFP[MFP_AER] & 0x40) && (vl == (int)CRTC_IntLine)) {
                mx68k_diag_mfp_int(1, "crtc-aer1"); /* P47-D-DIAG-G */
#if P295_ENABLE
                s_p295_raster_irq_fires++;
#endif
            }
#if P136_ENABLE
            p136_poll(P136_MFP_TIMERA, vl);
#endif

            // DMA 実行 (upstream: DMA_Exec(0)/1/2 のみ、3 は存在しない)
            // P153 DELTA 2: DMA は per-line 据置（per-chunk へ移さない — Timer-C fix を隔離）
            /* P164: capture DMA ch0 full register block around DMA_Exec(0) to see why
             * an armed channel (MTC!=0) with FDC data ready does not drain. Read-only,
             * bounded, stall-window gated. Call count/order unchanged (single call). */
            {
                static int s_p164 = 0;
                if (frame_num >= 55 && frame_num <= 80 && DMA[0].MTC != 0 && s_p164 < 24) {
                    uint16_t mtc_before = DMA[0].MTC;
                    uint8_t  csr_before = DMA[0].CSR;
                    int rdy = FDC_IsDataReady();
                    int cond = ((DMA[0].CSR & 0x08) && !(DMA[0].CCR & 0x20) &&
                                !(DMA[0].CSR & 0x80) && DMA[0].MTC &&
                                (((DMA[0].OCR & 3) != 2) || rdy)) ? 1 : 0;
                    debug_log("[P164-DMA0] f=%d PRE CSR=0x%02x CCR=0x%02x OCR=0x%02x DCR=0x%02x "
                              "SCR=0x%02x MTC=0x%04x MAR=0x%06x DAR=0x%06x rdy=%d whilecond=%d\n",
                              frame_num, (unsigned)DMA[0].CSR, (unsigned)DMA[0].CCR,
                              (unsigned)DMA[0].OCR, (unsigned)DMA[0].DCR, (unsigned)DMA[0].SCR,
                              (unsigned)DMA[0].MTC, (unsigned)DMA[0].MAR, (unsigned)DMA[0].DAR,
                              rdy, cond);
#if P367_ENABLE
                    uint32_t p367_dst0_a = (DMA[0].OCR & 0x80) ? DMA[0].MAR : DMA[0].DAR;
                    uint16_t p367_mtc0_a = DMA[0].MTC;
#endif
                    DMA_Exec(0);   /* the real call, now instrumented */
#if P367_ENABLE
                    {
                        uint32_t p367_dst1_a = (DMA[0].OCR & 0x80) ? DMA[0].MAR : DMA[0].DAR;
                        p367_dma_write_check(0, p367_dst0_a, p367_mtc0_a,
                                             p367_dst1_a, DMA[0].MTC, vl);
                    }
#endif
                    debug_log("[P164-DMA0] f=%d POST CSR=0x%02x->0x%02x MTC=0x%04x->0x%04x (drained=%d)\n",
                              frame_num, (unsigned)csr_before, (unsigned)DMA[0].CSR,
                              (unsigned)mtc_before, (unsigned)DMA[0].MTC,
                              (int)(mtc_before - DMA[0].MTC));
                    /* P165: sample the bytes the DMA just wrote to memory, to see if real
                     * disk content flows (varies per block) or repeats/garbage. */
                    {
                        uint32_t mar = DMA[0].MAR & 0x00FFFFFF;
                        uint32_t base = (mar >= 8) ? (mar - 8) : 0;
                        if (MEM) {
                            debug_log("[P165-DMADATA] f=%d MAR=0x%06x mem[-8..-1]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                      frame_num, (unsigned)mar,
                                      MEM[base+0], MEM[base+1], MEM[base+2], MEM[base+3],
                                      MEM[base+4], MEM[base+5], MEM[base+6], MEM[base+7]);
                        }
                    }
                    s_p164++;
                } else {
#if P367_ENABLE
                    uint32_t p367_dst0_b = (DMA[0].OCR & 0x80) ? DMA[0].MAR : DMA[0].DAR;
                    uint16_t p367_mtc0_b = DMA[0].MTC;
#endif
                    DMA_Exec(0);
#if P367_ENABLE
                    {
                        uint32_t p367_dst1_b = (DMA[0].OCR & 0x80) ? DMA[0].MAR : DMA[0].DAR;
                        p367_dma_write_check(0, p367_dst0_b, p367_mtc0_b,
                                             p367_dst1_b, DMA[0].MTC, vl);
                    }
#endif
                }
            }
            /* P181: DMAC ch0 is FDD-dedicated; ACT bit set = real disk R/W in progress
             * (seek/select/poll leave it clear). Light the access lamp only on actual transfer. */
            if (DMA[0].CSR & 0x08u) mx68k_fdd_note_rw();
#if P136_ENABLE
            p136_poll(P136_DMA0, vl);
#endif
#if P367_ENABLE
            {
                uint32_t p367_dst0_c1 = (DMA[1].OCR & 0x80) ? DMA[1].MAR : DMA[1].DAR;
                uint16_t p367_mtc0_c1 = DMA[1].MTC;
#endif
            DMA_Exec(1);
#if P367_ENABLE
                uint32_t p367_dst1_c1 = (DMA[1].OCR & 0x80) ? DMA[1].MAR : DMA[1].DAR;
                p367_dma_write_check(1, p367_dst0_c1, p367_mtc0_c1,
                                     p367_dst1_c1, DMA[1].MTC, vl);
            }
#endif
#if P136_ENABLE
            p136_poll(P136_DMA1, vl);
#endif
#if P367_ENABLE
            {
                uint32_t p367_dst0_c2 = (DMA[2].OCR & 0x80) ? DMA[2].MAR : DMA[2].DAR;
                uint16_t p367_mtc0_c2 = DMA[2].MTC;
#endif
            DMA_Exec(2);
#if P367_ENABLE
                uint32_t p367_dst1_c2 = (DMA[2].OCR & 0x80) ? DMA[2].MAR : DMA[2].DAR;
                p367_dma_write_check(2, p367_dst0_c2, p367_mtc0_c2,
                                     p367_dst1_c2, DMA[2].MTC, vl);
            }
#endif
#if P136_ENABLE
            p136_poll(P136_DMA2, vl);
#endif

            // OPM / ADPCM タイマー (毎ライン)
            // P149: feed the per-line clkdiv-normalized clock (MPX clk_line = line_usedclk),
            // NOT raw sc — after Edit A's clkdiv scaling, raw sc would run FM/ADPCM too fast.
            OPM_Timer((uint32_t)line_usedclk);
            ADPCM_PreUpdate((uint32_t)line_usedclk);
            /* P488: MIDI ボードのタイマー更新 + 送出キューのフラッシュ。
             * 呼出位置は参照実装 px68k-libretro libretro/winx68k.cpp:483 と同一
             * (ADPCM_PreUpdate → OPM_Timer → MIDI_Timer → Mcry_PreUpdate、
             *  同一クロック単位 clk_line = line_usedclk を渡す)。
             * ★未装着時は呼ばない — 未装着時の挙動は完全に従来どおり。 */
            if (g_midi_installed) {
                MIDI_Timer((int32_t)line_usedclk);
                MIDI_DelayOut((uint32_t)g_midi_delay_ms);   /* P490: 設定可変(ライブ反映) */
            }
            /* P483: Mercury PCM のサンプル供給(参照実装 px68k x11/winx68k.cpp:469 /
             * px68k-libretro libretro/winx68k.cpp:485 と同一位置・同一引数)。
             * これが無いと Mcry_SampleCnt が常に 0 のままで DMA ch2 の ready CB
             * (dmac.c:401 DMA_SetReadyCB(2, Mcry_IsReady))が恒久 false になる。
             * ★未装着時は呼ばない — 未装着時の挙動は完全に従来どおり。 */
            /* P634 (D-43): 実機 Mercury-Unit V4 の LRCK はオーディオマスタークロック
             * n384FS で駆動される同期カウンタ出力であり、CPU/DMAC の転送とは無関係に
             * 自走する(実機 CPLD の ABEL 原本 Mercury-Unit-v4.0-Artemis.abl:
             * `LRCK.clk = n384FS ;`)。px68k は LRCK 相当を Mcry_LRTiming の
             * 「転送の副作用」としてしか進めないため、LR エッジ待ちで転送を開始する
             * ソフト(PCM8PP)が構造的デッドロックに陥る。
             * ここでは Core の Mcry_LRTiming には触れず、観測用の自走位相のみを
             * 別に持つ。単位は Mcry_PreUpdate と同一(10^7 clock = 1 秒)、
             * レートは 2*fs(LRCK は DAC のフレーム同期 = 1 サンプルフレームで
             * 1 周期 = 2 トグル)。★未装着時は呼ばない。 */
            /* P636 (サウンドモニタ): Mercury PCM の活動検出。P635 の CPU 書込み
             * フック方式は DMAC 駆動の書込み(PCM8PP は DMAC ch2 経由)を構造的に
             * 観測できなかったため、書込み経路によらず Core が保持する最新
             * サンプル値 Mcry_OutDataL/R をここで毎スキャンラインポーリングする。
             * read-only、★未装着時は呼ばない。 */
            if (g_mercury_installed) {
                Mcry_PreUpdate((int32_t)line_usedclk);
                p634_lrck_advance((int32_t)line_usedclk);
                p636_mercury_pcm_poll_samples();
            }
#if P136_ENABLE
            p136_poll(P136_OPM_ADPCM, vl);
#endif

            // キーボード割り込み (upstream: KeyIntCnt > VLINE_TOTAL/4 で発火)
            KeyIntCnt++;
            if (KeyIntCnt > (int)vline_total_val / 4) {
                KeyIntCnt = 0;
                mx68k_keyboard_int();
            }
#if P136_ENABLE
            p136_poll(P136_KEYBOARD_INT, vl);   /* Keyboard_Int ブロック (条件付き) 直後 */
#endif
            // マウス割り込み (upstream: MouseIntCnt > VLINE_TOTAL/8 で SCC_IntCheck)
            MouseIntCnt++;
            if (MouseIntCnt > (int)vline_total_val / 8) {
                MouseIntCnt = 0;
                SCC_IntCheck();
            }
#if P136_ENABLE
            p136_poll(P136_SCC_INTCHECK, vl);   /* SCC_IntCheck ブロック (条件付き) 直後 */
            p136_poll(P136_LINE_TAIL, vl);      /* per-line loop body 末 */
#endif
#if P214_ENABLE
            p214_r1_sample_line(vl);   /* P214-R1: 行末の映像状態を採取（read-only） */
#endif
            /* P294: この走査線を即描画(MPX winx68k.cpp:544-554 相当)。VLINE は
             * LINE-START(:1970)で表示行に設定済み、非表示行は VLINE<0。表示中に
             * ゲームが Sprite_Regs/scroll を書き換えても行ごとに正しく反映される。 */
            if (VLINE >= 0) {                       /* MPX: vline∈[CRTC_VSTART,CRTC_VEND) */
                if (CRTC_VStep == 1) {              /* HighReso 256dot 2ライン読み */
                    if (vl % 2)                     /* raw 走査線パリティ(VLINE ではなく vl) */
                        mx68k_draw_display_line();
                } else if (CRTC_VStep == 4) {       /* 低解像 512dot: 1物理走査線 = 表示2行 */
                    mx68k_draw_display_line();
                    VLINE++; VLINEBG = VLINE;
                    mx68k_draw_display_line();
                } else {                            /* High 512dot / Low 256dot: 1回 */
                    mx68k_draw_display_line();
                }
            }
            vl++;
            clk_line_start = clk_next;                                                    /* P657 */
            clk_next = (int32_t)(((int64_t)clk_total * (vl + 1)) / vline_total_val);  /* MPX 535 */
            hsync = 1;
        }
    } while (vl < (int)vline_total_val);        /* MPX: while(vline<VLINE_TOTAL) */

#if P657_PROBE_ENABLE
    /* [P657-RCFIRE]: 測定1(フロントポーチ発火の分子・内訳・分母)・
     * 測定2(GPIP 差し替えヒットとその分母 mfp_reads)・
     * 測定3(境界計算の入力の生値7点)を 300 フレームに1回、1行で出す。
     * 派生値(display_start/display_end)は出さない — 生値のみ。
     * line_len と HSYNC_CLK は §0★但し書きの実測降格措置のため必ず同一行。 */
    if ((g_mx68k_frame_num % 300) == 0) {
        debug_log("[P657-RCFIRE] f=%d fp_calls=%d rc_exec=%d rc_noop_plane=%d "
                  "rc_noop_same=%d chunks=%d gpip_hits_cum=%llu mfp_reads_cum=%llu "
                  "line_len=%d HSYNC_CLK=%d clkdiv=%d vline_total_val=%d "
                  "htotal=%d R05=%d R07=%d fp_gate=%d\n",
                  g_mx68k_frame_num,
                  p657_fp_calls, p657_rc_exec, p657_rc_noop_plane,
                  p657_rc_noop_same, p657_chunks,
                  g_p657_gpip_hits, g_p657_mfp_reads,
                  p657_line_len_snap, (int)HSYNC_CLK, clkdiv, vline_total_val,
                  (int)CRTC_Regs[1], (int)CRTC_Regs[0x05], (int)CRTC_Regs[0x07],
                  (int)P657_FRONTPORCH_ENABLE);
    }
#endif

#if P670_PROBE_ENABLE
    /* [P670-RCDROP] (D-69): ラスタコピー「取りこぼし」仮説の直接観測。
     * 分子 rc_exec_cum(実際に転送が起きた回数)と分母 r22_writes_cum(ゲストが
     * R22 に src/dst 対を発行した回数)を必ず同一行へ並記する。両者が一致すれば
     * 仮説は棄却、rc_exec_cum が有意に少なければ取りこぼしの直接観測となる
     * (0件・少数の解釈が一意に落ちる)。どちらも生値・累積で派生値は出さない。
     * 出力は 300 フレーム毎 + 分母が増えたフレームは即時追加 — コンソール
     * スクロールの短時間バーストを 300 フレームサンプリングで落とさないため
     * (既存 p657_rc_exec がまさにそれを取りこぼした反省)。 */
    {
        static unsigned long long s_p670_r22_writes_cum_last_logged = 0;
        if ((g_mx68k_frame_num % 300) == 0
            || g_p670_r22_writes_cum != s_p670_r22_writes_cum_last_logged) {
            debug_log("[P670-RCDROP] f=%d rc_exec_cum=%llu r22_writes_cum=%llu\n",
                      g_mx68k_frame_num,
                      g_p670_rc_exec_cum, g_p670_r22_writes_cum);
            s_p670_r22_writes_cum_last_logged = g_p670_r22_writes_cum;
        }
    }
#endif

#if P385_ENABLE
    /* P385: cum_* は累積なので、60フレームおきの出力でも稀な n==0 を取りこぼさない
     * (出力間隔とイベント検出が独立)。派生値 line_budget は生値 clk_total /
     * vline_total と同一行に併記し、標本自身がどのクロック設定で採られたかを示す。 */
    s_p385_cum_chunks += (unsigned long long)p385_chunks;
    s_p385_cum_zero   += (unsigned long long)p385_zero;

    /* P408: CPUモニタ可視化用スナップショット。ログ出力ゲート(次行の if)より
     * 手前に置き、60フレームおきではなく毎フレーム更新すること
     * (P382/P383 の状態スタンプ鮮度バグと同型の再発を避けるため)。 */
    g_p408_clock_slice_snapshot = CLOCK_SLICE;
    g_p408_clkdiv_snapshot      = clkdiv;
    g_p408_clk_total_snapshot   = clk_total;
    g_p408_vline_total_snapshot = vline_total_val;
    g_p408_chunks_last_frame    = p385_chunks;
    g_p408_cum_zero_snapshot    = (int32_t)s_p385_cum_zero;

    if (frame_num < 5 || (frame_num % 60) == 0) {
        debug_log("[P385-CHUNK] f=%d chunks=%d zero=%d first_zero_vl=%d last_vl=%d "
                  "cum_chunks=%llu cum_zero=%llu "
                  "clk_total=%d vline_total=%d line_budget=%d CLOCK_SLICE=%d clkdiv=%d\n",
                  frame_num, p385_chunks, p385_zero, p385_first_zero_vl, vl,
                  s_p385_cum_chunks, s_p385_cum_zero,
                  clk_total, vline_total_val,
                  (vline_total_val > 0 ? clk_total / vline_total_val : -1),
                  CLOCK_SLICE, clkdiv);
    }
#endif

    int32_t executed = total_executed;

#if P63_PROBE_ENABLE
    /* Probe-C: per-frame FDD_IsReady(0) state log.
     * FDD_IsReady returns 1 when SetDelay[0]==0 (disk fully settled).
     * Expected TRUE by frame ~3 after disk insert. If still 0 at frame 10+,
     * FDD_SetFDInt drain is not working (H2 confirmed). */
    {
        static int s_p63_fddready_frames  = 0;
        static int s_p63_fddready_was_true = 0;
        if (s_p63_fddready_frames < P63_FDDREADY_LOG_MAX &&
            !s_p63_fddready_was_true) {
            int fdd_rdy = FDD_IsReady(0);
            debug_log("[P63-FDDREADY] frame=%d ready=%d\n",
                      frame_num, fdd_rdy);
            s_p63_fddready_frames++;
            if (fdd_rdy) s_p63_fddready_was_true = 1;
        }
    }
#endif /* P63_PROBE_ENABLE */
#if P64_PROBE_ENABLE
    /* Probe-B: snapshot the TRAP#15 vector at the very first frame to
     * establish a baseline. Compare with Probe-A's value at the time of
     * the error to see if the vector was updated during boot. */
    {
        static int s_p64_snap_done = 0;
        if (!s_p64_snap_done) {
            uint32_t snap_vec = p47_read_long_le(0x000000bcU);
            debug_log("[P64-TRAP15SNAP] frame=%d trap15_vec=0x%08x\n",
                      frame_num, (unsigned)snap_vec);
            s_p64_snap_done = 1;
        }
    }
#endif /* P64_PROBE_ENABLE */
#if P65_PROBE_ENABLE
    {
        static int s_p65_sram_snap_done = 0;
        if (!s_p65_sram_snap_done) {
            uint32_t sv = ((uint32_t)SRAM[0x1e] << 24) |
                          ((uint32_t)SRAM[0x1f] << 16) |
                          ((uint32_t)SRAM[0x20] << 8)  |
                           (uint32_t)SRAM[0x21];
            uint8_t  sb = SRAM[0x26];
            debug_log("[P65-SRAM-SNAP] frame=%d sram_001e=0x%08x sram_0026=0x%02x\n",
                      frame_num, (unsigned)sv, (unsigned)sb);
            s_p65_sram_snap_done = 1;
        }
    }
#endif /* P65_PROBE_ENABLE */

    // P22.5-DMA: Dump ch0 state after H-line loop (frames 0-20 only)
    if (frame_num >= 0 && frame_num <= 20) {
        debug_log("[P22.5-DMA] post-CPU frame=%d ch0: CSR=%02x CCR=%02x OCR=%02x SCR=%02x MAR=%06x MTC=%04x DAR=%06x\n",
                  frame_num, DMA[0].CSR, DMA[0].CCR, DMA[0].OCR, DMA[0].SCR,
                  DMA[0].MAR, DMA[0].MTC, DMA[0].DAR);
    }

    /* P48-D: One-shot forensic dump of low-RAM stall region + SSP top at
     * frame=50. Zero-cost when not triggered. Guarded so the SSP dump skips
     * if SSP is in mapped I/O / >=0xC00000 (C-3: prevent OOB host read). */
    {
        static int s_p48d_dumped = 0;
        if (frame_num == 50 && !s_p48d_dumped && MEM) {
            s_p48d_dumped = 1;
            debug_log("[P48-D-DUMP] MEM[0x1FC0..0x1FFF] @frame=50:\n");
            for (uint32_t a = 0x1FC0; a < 0x2000; a += 16) {
                debug_log("[P48-D-DUMP]   %04X: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                          a,
                          *(uint16_t*)&MEM[a +  0], *(uint16_t*)&MEM[a +  2],
                          *(uint16_t*)&MEM[a +  4], *(uint16_t*)&MEM[a +  6],
                          *(uint16_t*)&MEM[a +  8], *(uint16_t*)&MEM[a + 10],
                          *(uint16_t*)&MEM[a + 12], *(uint16_t*)&MEM[a + 14]);
            }
            uint32_t ssp = m68000_get_reg(M68K_MSP) & 0x00FFFFFFu;
            if (ssp >= 0xC00000u) {
                /* C-3: SSP outside 12MB RAM range — host MEM[] read would OOB. */
                debug_log("[P48-D-DUMP] SSP=0x%06x outside RAM range, stack dump skipped\n", ssp);
            } else if (ssp + 32 > 12u * 1024u * 1024u) {
                debug_log("[P48-D-DUMP] SSP=0x%06x too close to RAM end, stack dump skipped\n", ssp);
            } else {
                debug_log("[P48-D-DUMP] SSP=0x%06x stk[0..32]:\n", ssp);
                for (uint32_t i = 0; i < 32; i += 16) {
                    debug_log("[P48-D-DUMP]   +%02x: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                              i,
                              *(uint16_t*)&MEM[ssp + i +  0],
                              *(uint16_t*)&MEM[ssp + i +  2],
                              *(uint16_t*)&MEM[ssp + i +  4],
                              *(uint16_t*)&MEM[ssp + i +  6],
                              *(uint16_t*)&MEM[ssp + i +  8],
                              *(uint16_t*)&MEM[ssp + i + 10],
                              *(uint16_t*)&MEM[ssp + i + 12],
                              *(uint16_t*)&MEM[ssp + i + 14]);
                }
            }
        }
    }

    /* P45-DIAG: FDD_SetFDInt()前後のIRQ1状態監視 */
    {
        static int s_p45_irq1_cnt = 0;

        uint8_t irq1_pre  = IRQH_IRQ[1];
        uint8_t ioc_stat  = IOC_IntStat;
        int     rdy0      = FDD_IsReady(0);
        int     rdy1      = FDD_IsReady(1);

        /* P23-DIAG互換: 最初10フレームのみ既存ログを維持 */
        if (frame_num <= 10) {
            debug_log("[P23-FDD] frame=%d FDD_IsReady(0)=%d IOC_IntStat=0x%02x\n",
                      frame_num, rdy0, ioc_stat);
        }

        /* FDD insert/eject interrupt timing (must be called every frame) */
        FDD_SetFDInt();
#if P136_ENABLE
        p136_poll(P136_FDD_SETFDINT, -1);   /* per-frame (line loop 外): sentinel line=-1 */
#endif

        uint8_t irq1_post = IRQH_IRQ[1];
        int     rdy0_post = FDD_IsReady(0);

        /* 最初50フレームは毎フレームログ（IRQ1変化時は常に出力） */
        if (frame_num < 50 || irq1_pre != irq1_post) {
            debug_log("[P45-DIAG-PRE] frame=%d rdy0=%d->%d rdy1=%d "
                      "IOC_IntStat=0x%02x IRQH_IRQ[1]=%d->%d\n",
                      frame_num, rdy0, rdy0_post, rdy1,
                      (unsigned)ioc_stat, (int)irq1_pre, (int)irq1_post);
        }

        /* IRQ1が0→1に変化: IRQH_Int(1,&FDD_Int)発火確認 */
        if (irq1_pre == 0 && irq1_post == 1 && s_p45_irq1_cnt < 10) {
            debug_log("[P45-DIAG-IRQ1-FIRED] frame=%d IRQH_IRQ[1]: 0->1 "
                      "IOC_IntStat=0x%02x rdy0=%d rdy1=%d "
                      "(IRQH_Int(1,&FDD_Int)発火)\n",
                      frame_num, (unsigned)ioc_stat, rdy0, rdy1);
            s_p45_irq1_cnt++;
        }

        /* FDDレディだがIRQ1未発火（IOC_IntStat & 2 = 0の疑い） */
        if (rdy0 == 1 && irq1_pre == 0 && irq1_post == 0 &&
            frame_num >= 3 && frame_num < 60) {
            if (!(ioc_stat & 2)) {
                debug_log("[P45-DIAG-NO-IRQ1-IOC] frame=%d FDD ready だが "
                          "IOC_IntStat & 2 = 0 (0x%02x) でIRQH_Int未呼び出し\n",
                          frame_num, (unsigned)ioc_stat);
            }
        }
    }

#ifndef P514_ENABLE
#define P514_ENABLE 1   /* D-39切り分け診断: Pal_TrackContrast()のフェード実行有無を計測。
                         * でたな!!ツインビー・パロディウスだ!・沙羅曼蛇の3タイトルで
                         * 共通してBudget超過が観測された(2026-08-08 hands-on実測)ことを受け、
                         * 「全ゲスト書込みで一律発火する残置プローブ」説より「この3タイトル
                         * 共通のKONAMIロゴ演出(パレットフェード)に紐づく処理コスト」説を
                         * 優先的に検証する。読み取り専用・Core無改変。 */
#endif
#if P514_ENABLE
    static uint32_t s_p514_frame_count = 0;
    static uint32_t s_p514_fade_active_count = 0;
    static uint32_t s_p514_edge_log_count = 0;
    {
        uint8_t p514_sp1 = SysPort[1];
        uint8_t p514_cv  = Contrast_Value;
        int p514_fade_active = (p514_sp1 != p514_cv);
        s_p514_frame_count++;
        if (p514_fade_active) {
            s_p514_fade_active_count++;
            if (s_p514_edge_log_count < 20) {
                debug_log("[P514-CONTRAST-EDGE] frame=%d SysPort1=%u Contrast_Value=%u "
                          "fade_active=1 (count=%u)\n",
                          frame_num, (unsigned)p514_sp1, (unsigned)p514_cv,
                          ++s_p514_edge_log_count);
            }
        }
        if (s_p514_frame_count % 60 == 0) {
            debug_log("[P514-CONTRAST-HB] frame=%d SysPort1=%u Contrast_Value=%u "
                      "fade_active_now=%d total_frames=%u fade_active_total=%u\n",
                      frame_num, (unsigned)p514_sp1, (unsigned)p514_cv, p514_fade_active,
                      s_p514_frame_count, s_p514_fade_active_count);
        }
    }
#endif /* P514_ENABLE */
    Pal_TrackContrast();

    // ---- audio generation ----
    // Generate one frame worth of samples (44100/60 = 735)
    // OPM_Timer / ADPCM_PreUpdate は Hラインループ内で呼び済み – ここでは audio 出力のみ
    /* P211b: couple audio generation to the guest VSYNC rate. P211 paces run_frame()
     * to 55.46/61.46Hz; with the old fixed /60 (735/frame) that under-supplied the ring
     * (55.46*735=40763 < 44100) -> ADPCM dropouts. round(44100/vsync) keeps
     * (paced_freq * audio_frames) == AUDIO_SAMPLE_RATE regardless of the paced rate.
     * NOTE: uses the SAME mx68k_get_vsync_hz() as the Swift run_frame accumulator, so
     * the two always agree (their product is 44100/sec). */
    /* P465-C (D-6): round(44100/vhz) は毎フレーム独立に丸めるため端数が毎回捨てられ、
     * 15kHzモード(55.46Hz)では約9.4フレーム/秒の慢性的な生成不足(31kHzモードでは
     * 逆方向の過剰)が蓄積し、リングバッファのアンダーラン=ゼロ埋めスプライスの
     * 発生トリガになっていた(P464 [P464-RINGFILL] の fill_hist 実測でドリフト量が
     * 理論値と定量一致)。DDA/Bresenham型のフラクショナルアキュムレータへ変更し、
     * 端数を次フレームへ持ち越すことで長期平均を厳密に AUDIO_SAMPLE_RATE へ収束させる。
     * アキュムレータからの減算はクランプ「前」の切り捨て値で行う(通常の55.46/61.46Hzでは
     * audio_frames は 795/718 近辺でありクランプは発動しない)。 */
    double vhz = mx68k_get_vsync_hz();
    static double s_p465_audio_frame_accum = 0.0;
    /* P626 (D-62): レート基準を設定値へ。22050Hz 設定時は AudioUnit 出力も 22050Hz
     * なので、生成量も 22050/vhz でなければリングが恒常的に満杯になる。 */
    s_p465_audio_frame_accum += (double)g_audio_sample_rate_hz / vhz;
    int audio_frames = (int)s_p465_audio_frame_accum;
    s_p465_audio_frame_accum -= (double)audio_frames;
    if (audio_frames > AUDIO_FRAMES_MAX) audio_frames = AUDIO_FRAMES_MAX;  /* buffer guard */
    if (audio_frames < 1) audio_frames = 1;
    int16_t tmp_opm[AUDIO_FRAMES_MAX * 2];
    int16_t tmp_adpcm[AUDIO_FRAMES_MAX * 2];
    int16_t tmp_mix[AUDIO_FRAMES_MAX * 2];

    memset(tmp_opm, 0, sizeof(tmp_opm));
    // OPM_Update writes interleaved stereo L/R into buffer[0..length*2-1]
    OPM_Update(tmp_opm, audio_frames, tmp_opm, tmp_opm + audio_frames * 2);

    memset(tmp_adpcm, 0, sizeof(tmp_adpcm));
    ADPCM_Update(tmp_adpcm, audio_frames, tmp_adpcm, tmp_adpcm + audio_frames * 2);

    /* P484 (サウンドモニタ): 波形プレビュー用スナップショット。
     * 条件分岐なし・毎フレーム実行(パネル非表示時も)。コストは 256 要素以下の
     * int16 コピーのみで、既存の tmp_mix 合成ループと同オーダー・無視できる。
     * 読み出し側(Swift)をパネル表示中のみ高頻度化する設計とし、書込み側は
     * 常時稼働のシンプルな設計に保つ。 */
    {
        int wf_start = (audio_frames > MX68K_ADPCM_WAVEFORM_SAMPLES)
                       ? (audio_frames - MX68K_ADPCM_WAVEFORM_SAMPLES) : 0;
        int wf_n     = audio_frames - wf_start;   /* 通常は256、稀に256未満 */
        for (int i = 0; i < wf_n; i++) {
            g_adpcm_waveform[i] = tmp_adpcm[(wf_start + i) * 2];  /* L ch */
        }
        /* audio_frames < 256 の稀なケース(ポーズ復帰直後等)では末尾の残り要素は
         * 前フレームの値が残る(古いスナップショットの末尾が薄く混ざるだけで実害軽微、
         * 波形プレビューという表示専用機能の性質上、厳密な連続性は要求しない)。 */
    }

#if P460_ADPCM_STEP_ENABLE
    /* P460 (D-6診断): tmp_adpcm のゼロ→非ゼロ遷移を検出し、遷移直前後のサンプル値と
     * 段差を実測する("機構D"仮説の実データ化)。挙動には一切影響しない読み取り専用プローブ。
     * static 変数でフレーム境界を跨いで直前サンプルを保持する(L/R 別)。
     * ★フレーム内に複数のゼロ→非ゼロ遷移があれば全て記録する(1フレーム1件に限定しない)
     * ——記録上限は s_p460_transitions によるグローバル通算200件キャップのみ。
     * frame_num は本関数スコープ内で既に static int として宣言・毎フレーム
     * インクリメント済み(:1950/:3012 付近)のため新規カウンタを起こさず再利用し、
     * 他プローブのログとの frame= 相互参照を容易にする。
     * status は ADPCM_Read(0xE92001) の生値のまま記録する(真偽値へデコードしない)。 */
    static int16_t s_p460_prev[2] = {0, 0};   /* 直前フレーム末尾のL/Rサンプル */
    static uint64_t s_p460_frames_nonzero = 0;
    static uint64_t s_p460_transitions = 0;
    static uint64_t s_p461_stop_transitions = 0;
    static uint64_t s_p460_verdict_done = 0;

    int p460_nonzero_in_frame = 0;
    for (int i = 0; i < audio_frames * 2; i++) {
        if (tmp_adpcm[i] != 0) p460_nonzero_in_frame++;
    }
    if (p460_nonzero_in_frame > 0) s_p460_frames_nonzero++;

    /* L/Rそれぞれについて、フレーム内で発生した全てのゼロ→非ゼロ遷移を記録する。 */
    for (int ch = 0; ch < 2; ch++) {
        int16_t prev = s_p460_prev[ch];
        for (int i = ch; i < audio_frames * 2; i += 2) {
            int16_t cur = tmp_adpcm[i];
            if (prev == 0 && cur != 0 && s_p460_transitions < 200) {
                uint8_t p460_status = ADPCM_Read(0xE92001);
                int32_t p460_step = (int32_t)cur - (int32_t)prev;
                debug_log("[P460-ADPCMSTEP] ch=%d frame=%d prev=%d cur=%d step_abs=%ld "
                          "nonzero_in_frame=%d/%d status=0x%02x nonzero_frames=%llu\n",
                          ch, frame_num, prev, cur,
                          (long)(p460_step < 0 ? -p460_step : p460_step),
                          p460_nonzero_in_frame, audio_frames * 2, p460_status,
                          (unsigned long long)s_p460_frames_nonzero);
                s_p460_transitions++;
            } else if (prev != 0 && cur == 0 && s_p461_stop_transitions < 200) {
                /* P461 (D-6診断): 「非ゼロ→ゼロ」(停止・打ち切り)方向の段差を実測。
                 * P460のhands-on実測で「開く方向」の遷移が長時間無音明けではなく
                 * 波形の自然なゼロ交差である疑いが強まった一方、実際にクリックが
                 * 報告された放置期間中は「開く方向」の遷移が1件も記録されなかった。
                 * Core出力ゲート(adpcm.c:173-184)が停止の瞬間に非ゼロ値を強制的に
                 * 0へ叩き落とす経路をこちらで直接計測する。 */
                uint8_t p461_status = ADPCM_Read(0xE92001);
                int32_t p461_step = (int32_t)prev;  /* cur は必ず0なので段差=直前値そのもの */
                debug_log("[P461-ADPCMSTOP] ch=%d frame=%d prev=%d cur=%d step_abs=%ld "
                          "nonzero_in_frame=%d/%d status=0x%02x nonzero_frames=%llu\n",
                          ch, frame_num, prev, cur,
                          (long)(p461_step < 0 ? -p461_step : p461_step),
                          p460_nonzero_in_frame, audio_frames * 2, p461_status,
                          (unsigned long long)s_p460_frames_nonzero);
                s_p461_stop_transitions++;
            }
            prev = cur;
        }
        s_p460_prev[ch] = prev;
    }

    /* 一定フレーム数経過後、集計サマリを1回だけ出力(Self-Falsifiability Gate対応の分母)。 */
    if (!s_p460_verdict_done && frame_num >= 1800) {  /* 約30秒 @60fps */
        debug_log("[P460-ADPCMSTEP-SUMMARY] total_frames=%d nonzero_frames=%llu transitions=%llu "
                   "stop_transitions=%llu\n",
                   frame_num, (unsigned long long)s_p460_frames_nonzero,
                   (unsigned long long)s_p460_transitions,
                   (unsigned long long)s_p461_stop_transitions);
        s_p460_verdict_done = 1;
    }
#endif

    /* P465-A' (D-6): P462/P463のデクリック段は発火条件が「厳密にゼロ」であるため、
     * ADPCMのデコード出力が非ゼロの定数に張り付いた場合(P464実測: adpcm_status が
     * 0xc0=再生中のまま mean_abs_adpcm=11154.0 が27ウィンドウ≒32秒にわたり小数点まで
     * 完全一致 = Coreの補間履歴が最後にデコードした値のまま凍結)には一度も実行され
     * なかった。P463の減衰時定数を127倍変えても聴感が無変化だったのは、当該コード
     * パスが一度も通っていなかったためと機械的に説明できる。
     * ここでは「一定期間まったく変化しない非ゼロ値」= DC固着 を独立した状態として
     * 検出し、真の無音と同じ±1/サンプルのランプでゼロへ収束させる。
     * ★重要(Round-1 Code Review finding 1): 真の無音とDC固着を同じ
     * s_p463_silence_count に相乗りさせると、正当な再生中の短い定常区間を誤検出した
     * 場合にカウンタが汚染され、後続の本物の音符へ不要な強制クロスフェードが
     * カスケードする。これを構造的に排除するため両者を独立した3分岐として扱う。 */
#define P465_STUCK_THRESHOLD_SAMPLES 128
    static int16_t s_p465_prev_src[2] = {0, 0};
    static int32_t s_p465_stuck_run = 0;
    static bool    s_p465_was_stuck = false;  /* ★B1修正: 分岐2経由の一発ラッチ */

    /* P463 (D-6追加改善): P462の「リリース減衰(128サンプルかけてゼロへ)」を、
     * XM6式の「±1/サンプルでゼロへ向けた超低速ランプ」(xm6/vm/adpcm.cpp:771-778)
     * に是正する。P462はゼロへの収束が速すぎ(2.9ms)、その変化自体が残存クリックの
     * 原因だったと考えられる。単に「無期限保持(減衰なし)」にすると、今度は
     * 意図的な再開時に保持値→新信号への段差が生じてしまう(Round-1 Requirements
     * Reviewが発見)ため、再開時は「その時点の(部分的に減衰した)保持値」から
     * 新信号へクロスフェードする設計とする。
     * 単発の自然なゼロ交差(P460実測で確認済み、通常1〜数サンプル)では、
     * P463_SILENCE_THRESHOLD_SAMPLES未満のためクロスフェードは発火せず、
     * P462と同じ「envはUNITY付近のまま」の挙動を維持する(回帰なし)。 */
    static int32_t s_p463_xfade = P462_ENV_UNITY; /* Q15 (0..P462_ENV_UNITY)。0=保持値100%、UNITY=新信号100% */
    static int16_t s_p462_hold[2] = {0, 0};     /* L/R それぞれの保持値(意図的停止中は徐々に0へ減衰) */
    static int32_t s_p463_silence_count = 0;    /* 連続無音サンプル数 */

    for (int i = 0; i < audio_frames; i++) {
        int16_t srcL = tmp_adpcm[i * 2];
        int16_t srcR = tmp_adpcm[i * 2 + 1];
        bool p462_nonzero = (srcL != 0) || (srcR != 0);
        /* P465-A': DC固着検出。非ゼロかつ直前サンプルとL/R両方が完全一致した回数を数える。 */
        bool p465_unchanged = p462_nonzero
            && (srcL == s_p465_prev_src[0]) && (srcR == s_p465_prev_src[1]);
        s_p465_prev_src[0] = srcL;
        s_p465_prev_src[1] = srcR;
        s_p465_stuck_run = p465_unchanged ? (s_p465_stuck_run + 1) : 0;
        /* ★xfade>=UNITY(クロスフェード完了・定常再生中)をDC固着判定の必須条件に
         * することで、P462_ATTACK_SAMPLESとの数値関係に依存せず構造的に安全にする
         * (Round-1 Code Review finding 2への対応・記号表参照)。クロスフェード
         * 進行中はholdが凍結されているため、その間にstuck判定を発火させて
         * holdを減衰させると、クロスフェード自身の凍結起点が動いてしまい別の
         * アーティファクトを生む——xfade>=UNITY条件がこれを防ぐ。 */
        bool p465_stuck_decay = p462_nonzero
            && (s_p465_stuck_run >= P465_STUCK_THRESHOLD_SAMPLES)
            && (s_p463_xfade >= P462_ENV_UNITY);

        if (!p462_nonzero) {
            /* 分岐1: 真の無音。既存P462/P463のロジックを完全に無変更で維持。 */
            s_p463_silence_count++;
            if (s_p463_silence_count >= P463_SILENCE_THRESHOLD_SAMPLES) {
                /* 意図的な停止が確定した後のみ、XM6式±1/サンプルでゼロへランプ。
                 * 確定前(単発ゼロ交差の可能性がある間)は保持値を変化させない
                 * (P462のCase Bと同じ「変化なし」挙動を維持)。 */
                if (s_p462_hold[0] > 0)      s_p462_hold[0] -= P463_DECAY_STEP;
                else if (s_p462_hold[0] < 0) s_p462_hold[0] += P463_DECAY_STEP;
                if (s_p462_hold[1] > 0)      s_p462_hold[1] -= P463_DECAY_STEP;
                else if (s_p462_hold[1] < 0) s_p462_hold[1] += P463_DECAY_STEP;
            }
            tmp_adpcm[i * 2]     = s_p462_hold[0];
            tmp_adpcm[i * 2 + 1] = s_p462_hold[1];
        } else if (p465_stuck_decay) {
            /* 分岐2: DC固着(非ゼロだが変化しない)。s_p463_silence_countには触れない
             * (Round-1 Code Review finding 1対応、誤検出カスケード防止)が、
             * ★B1修正: 「本物の固着から復帰した」ことを分岐3へ伝えるための
             * ラッチのみセットする(silence_countのような累積カウンタではなく
             * 一発フラグ——誤検出防止の構造は維持したまま復帰時の保護を追加)。
             * 分岐切替(通常→固着)時点で hold は既に固着値そのもの(直前まで
             * xfade>=UNITY により毎サンプル hold=src 追従していたため)なので
             * ここでの減衰開始に不連続はない。 */
            s_p465_was_stuck = true;
            if (s_p462_hold[0] > 0)      s_p462_hold[0] -= P463_DECAY_STEP;
            else if (s_p462_hold[0] < 0) s_p462_hold[0] += P463_DECAY_STEP;
            if (s_p462_hold[1] > 0)      s_p462_hold[1] -= P463_DECAY_STEP;
            else if (s_p462_hold[1] < 0) s_p462_hold[1] += P463_DECAY_STEP;
            tmp_adpcm[i * 2]     = s_p462_hold[0];
            tmp_adpcm[i * 2 + 1] = s_p462_hold[1];
        } else {
            /* 分岐3: 通常再生・単発ゼロ交差明けの再開。
             * 意図的な停止(連続無音がSILENCE_THRESHOLD_SAMPLES以上続いた)明けの
             * 再開のみ、envを0へ戻して新規クロスフェードを開始する。単発のゼロ
             * 交差ではenvはUNITY付近のまま(P462と同じ挙動、回帰なし)。
             * ★B1修正: 分岐2(DC固着)を経由していた場合(s_p465_was_stuck)も、
             * 無音復帰と同じくxfade=0を強制しクロスフェードをやり直す——分岐2の間に
             * holdが減衰している可能性があるため、直接out=src切替では復帰の瞬間に
             * 段差が生じる(Round-2 Requirements Review指摘)。 */
            if (s_p463_silence_count >= P463_SILENCE_THRESHOLD_SAMPLES
                || s_p465_was_stuck) {
                s_p463_xfade = 0;
            }
            s_p463_silence_count = 0;
            s_p465_was_stuck = false;
            if (s_p463_xfade < P462_ENV_UNITY) {
                s_p463_xfade += P462_ENV_UNITY / P462_ATTACK_SAMPLES;
                if (s_p463_xfade > P462_ENV_UNITY) s_p463_xfade = P462_ENV_UNITY;
            }
            /* クロスフェード: env=0 なら出力=hold(保持値からそのまま)、
             * env=UNITY なら出力=src(新信号そのまま)。通常再生中(env==UNITY)は
             * (UNITY-env)==0 となり実質 src そのまま(P462と同一、劣化なし)。 */
            int32_t outL = ((int32_t)s_p462_hold[0] * (P462_ENV_UNITY - s_p463_xfade)
                            + (int32_t)srcL * s_p463_xfade) >> 15;
            int32_t outR = ((int32_t)s_p462_hold[1] * (P462_ENV_UNITY - s_p463_xfade)
                            + (int32_t)srcR * s_p463_xfade) >> 15;
            tmp_adpcm[i * 2]     = (int16_t)outL;
            tmp_adpcm[i * 2 + 1] = (int16_t)outR;
            /* ★Round-2 Code Review/Requirements Review共通指摘(必須修正):
             * クロスフェード進行中(env<UNITY)にholdを毎サンプル追従させると、
             * クロスフェードの「凍結された起点」が1サンプル目で新信号の生値に
             * 上書きされてしまい、意図した64サンプルの緩やかな遷移が実質1サンプルへ
             * 潰れ、機構D相当の段差が再導入される(2エージェント独立に同一バグを検出)。
             * env==UNITY(クロスフェード完了・定常再生中)の間だけholdを追従させ、
             * クロスフェード中はholdを凍結したままにする。 */
            if (s_p463_xfade >= P462_ENV_UNITY) {
                s_p462_hold[0] = srcL;
                s_p462_hold[1] = srcR;
            }
        }
    }

    for (int i = 0; i < audio_frames * 2; i++) {
        int32_t s = (int32_t)tmp_opm[i] + (int32_t)tmp_adpcm[i];
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        tmp_mix[i] = (int16_t)s;
    }
    /* P483: Mercury は OPM_Update/ADPCM_Update(4引数・リングラップ対応)と異なり、
     * 「既に埋まったバッファへ加算」する 2 引数の規約(mercury.c:90-126)なので
     * 合成後に呼ぶ。内部で ±32767 にクリップ済み。length はフレーム数(ステレオ対)で、
     * 書き込む int16 個数は 2*length = tmp_mix の使用範囲と一致
     * (fmgen.h:15 FM_SAMPLETYPE=int16 のため M288_Update の Mix も 2*length)。
     * ★未装着時は呼ばない — Mcry_Update はバッファ空のとき DMA_Exec(2) を直接
     * 叩く(mercury.c:99-102)ので、DMA ch2 を Mercury 以外が使う構成で
     * 副作用が出ないようにするための安全要件。 */
    if (g_mercury_installed) Mcry_Update(tmp_mix, audio_frames);

#if P464_RINGFILL_ENABLE
    /* P464 (D-6再調査): 生成側の計上。充填率ヒストグラムは audio_ring_write() 内部の
     * 1箇所でのみ計上済み(ここで write_pos/read_pos を読み直すと「書込み後」の値に
     * なってしまうため重複実装しない)。ここでは戻り値から求まるドロップ量と、
     * ADPCM / OPM それぞれの振幅平均の分子・分母のみを積算する。 */
    {
        int p464_requested = audio_frames * 2;
        /* P624: P555 のターボ中ミュート分岐を撤去し、生成側は常に素の
         * audio_ring_write() を呼ぶ(= P555 以前と同一)。ターボ倍率の扱いは
         * すべて消費側(mx68k_audio_read のデシメーション)へ移した。 */
        int p464_written   = audio_ring_write(tmp_mix, p464_requested);
        int p464_dropped   = p464_requested - p464_written;
        if (p464_dropped > 0) {
            s_p464_overrun_writes++;
            s_p464_samples_dropped += (unsigned long long)p464_dropped;
        }
        for (int i = 0; i < p464_requested; i++) {
            int32_t a = (int32_t)tmp_adpcm[i]; if (a < 0) a = -a;
            int32_t o = (int32_t)tmp_opm[i];   if (o < 0) o = -o;
            s_p464_abs_sum_adpcm += (unsigned long long)a;
            s_p464_abs_sum_opm   += (unsigned long long)o;
        }
        s_p464_abs_sample_count += (unsigned long long)p464_requested;
    }

    /* 固定ウィンドウ(64フレーム)ごとに1行だけ、エミュレーションスレッド上から出力。
     * P463 §2の参照先拘束の反省(「先頭200件グローバルcap」ではユーザーの実際の
     * idle 区間を捉えられているか判別できない)を踏まえ、累積カウンタ + 固定
     * ウィンドウ方式とし、1行が「その約1秒間に実際に何が起きたか」を単独で
     * 証明できる形にする。派生値(mean_abs_*)は必ず window_frames と frame=
     * (採取時フレーム番号 = 鮮度)を同じ行に併記する。 */
    if ((g_mx68k_frame_num % P464_RINGFILL_WINDOW_FRAMES) == 0) {
        /* ★CoreAudio スレッド側が更新する3個は「読取り+ゼロ化」を単一命令で行う。
         * load → log → store 0 の2手順に分けると、その間の1回のインクリメントが
         * 失われ得る(Code Review 指摘#2)。 */
        unsigned long long p464_cb = atomic_exchange_explicit(
            &s_p464_callbacks_total, 0ULL, memory_order_acq_rel);
        unsigned long long p464_ur = atomic_exchange_explicit(
            &s_p464_underrun_callbacks, 0ULL, memory_order_acq_rel);
        unsigned long long p464_zf = atomic_exchange_explicit(
            &s_p464_samples_zero_filled, 0ULL, memory_order_acq_rel);
        uint8_t p464_status = ADPCM_Read(0xE92001);   /* 生バイト(デコード前) */
        double p464_mean_adpcm = 0.0, p464_mean_opm = 0.0;
        if (s_p464_abs_sample_count > 0) {
            p464_mean_adpcm = (double)s_p464_abs_sum_adpcm / (double)s_p464_abs_sample_count;
            p464_mean_opm   = (double)s_p464_abs_sum_opm   / (double)s_p464_abs_sample_count;
        }
        debug_log("[P464-RINGFILL] frame=%d window_frames=%d callbacks_total=%llu "
                  "underrun_callbacks=%llu samples_zero_filled=%llu overrun_writes=%llu "
                  "samples_dropped=%llu fill_hist=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
                  "adpcm_status=0x%02x mean_abs_adpcm=%.1f mean_abs_opm=%.1f vhz=%.2f "
                  "audio_frames=%d\n",
                  g_mx68k_frame_num, P464_RINGFILL_WINDOW_FRAMES,
                  p464_cb, p464_ur, p464_zf,
                  s_p464_overrun_writes, s_p464_samples_dropped,
                  s_p464_fill_hist[0], s_p464_fill_hist[1], s_p464_fill_hist[2],
                  s_p464_fill_hist[3], s_p464_fill_hist[4], s_p464_fill_hist[5],
                  s_p464_fill_hist[6], s_p464_fill_hist[7],
                  p464_status, p464_mean_adpcm, p464_mean_opm, vhz, audio_frames);
        /* 生成側(このスレッドからのみ更新)は通常代入でリセットしてよい。 */
        s_p464_overrun_writes   = 0;
        s_p464_samples_dropped  = 0;
        s_p464_abs_sum_adpcm    = 0;
        s_p464_abs_sum_opm      = 0;
        s_p464_abs_sample_count = 0;
        for (int b = 0; b < P464_RINGFILL_BUCKETS; b++) s_p464_fill_hist[b] = 0;
    }
#else
    audio_ring_write(tmp_mix, audio_frames * 2);
#endif

#if P136_ENABLE
    p136_poll(P136_FRAME_TAIL, -1);   /* per-frame (line loop 外): frame_num++ 直前・sentinel line=-1 */
#endif
#if P492_ENABLE
    /* P492 (D-43診断): I/O ウィンドウ別 read ヒストグラムを出力する。
     * frame_num++ の前 = g_mx68k_frame_num が当該フレームの番号のままの時点。
     * 出力の 300 フレームゲートは関数側が持つ。 */
    p492_io_histogram_dump();
#endif
#if P602_ENABLE
    /* P602 (D-57): 例外ベクタ / 奇数PC・$6xFF / 未マップI/O read の 3 集計行を
     * 出力する。P492 と同じ位置(frame_num++ の前 = g_mx68k_frame_num が当該
     * フレームの番号のままの時点)・同じ 300 フレーム周期(ゲートは関数側)。 */
    p602_periodic_dump();
#endif
    /* P509 (D-48): FDC コマンドヒストグラム + 強制 READY モデルの出力。
     * P492 と同じ 300 フレーム周期(ゲートは関数側)。 */
    p509_fdc_hist_dump();
    /* P510 (D-32): 外付け SCSI の SCTL 生値 + SSTS 読み出し総数 + 割込み要求数。
     * [P510-EXTIRQ] の発火有無とは独立に無条件で出す分母行(ゲートは関数側)。 */
    p510_ext_scsi_dump();
    frame_num++;
#if P82XG_ENABLE
    /* P82-X-G: FDC/IOC register-access trace VERDICT one-shot. Emitted once
     * when frame>=95 is first reached (the certain post-window point; the
     * observation window is frames 80-90). frame 87 で boot が停止しても CPU は
     * スピンループを回し続け frame_num は進むため frame 95 は確実に到達する。
     * p82xg_emit_verdict() 内部に s_p82xg_verdict_done ガードがあるので、
     * 多重呼び出しになっても 1 回だけダンプする (read-only)。 */
    if (frame_num >= 95) {
        p82xg_emit_verdict();
    }
#endif
#if P82XH_ENABLE
    /* P82-X-H: FDC ISR (vec 0x60) execution-path trace VERDICT one-shot.
     * Emitted once when frame>=96 is first reached (the certain post-window
     * point; the observation window is frames 80-95). frame 87 で boot が
     * 停止しても CPU はスピンループを回し続け frame_num は進むため frame 96
     * は確実に到達する。p82xh_emit_verdict() 内部に s_p82xh_verdict_done
     * ガードがあるので多重呼び出しになっても 1 回だけダンプする (read-only)。 */
    if (frame_num >= 96) {
        p82xh_emit_verdict();
    }
#endif
#if P82XK_ENABLE
    /* P82-X-K: good FDC ST0=0x20 と panic 終端 0xff063c の間の boot-path
     * 分岐点切り分け VERDICT one-shot。frame>=96 到達で 1 回だけ出力（観測窓は
     * frames 80-95）。frame 87 で boot が停止しても CPU はスピンループを回し
     * 続け frame_num は進むため frame 96 は確実に到達する。
     * p82xk_emit_verdict() 内部に s_p82xk_verdict_done ガードがあるので
     * 多重呼び出しになっても 1 回だけダンプする (read-only)。 */
    if (frame_num >= 96) {
        p82xk_emit_verdict();
    }
#endif
#if P82XL_ENABLE
    /* P82-X-L: Recalibrate->ReadData transition VERDICT one-shot。frame>=96
     * 到達で 1 回だけ出力（観測窓は frames 80-95）。boot が停止しても CPU は
     * スピンループを回し続け frame_num は進むため frame 96 は確実に到達する。
     * p82xl_emit_verdict() 内部に p82xl_verdict_done ガードがあるので多重
     * 呼び出しになっても 1 回だけダンプする (read-only)。 */
    if (frame_num >= 96) {
        p82xl_emit_verdict();
    }
#endif
#if P82XM_ENABLE
    /* P82-X-M: boot-sector DMA delivery / IPLROM bootability-reject VERDICT
     * one-shot。frame>=96 到達で 1 回だけ出力（観測窓は frames 80-95）。boot が
     * 停止しても CPU はスピンループを回し続け frame_num は進むため frame 96 は
     * 確実に到達する。p82xm_emit_verdict() 内部に p82xm_g_verdict_done ガード
     * があるので多重呼び出しになっても 1 回だけダンプする (read-only)。
     * p82xm_g_verdict_done は m68000_bridge.c の TU-local static —
     * EmulatorBridge.c から参照しない (P82-X-L/-H/-K 全先例と同型)。 */
    if (frame_num >= 96) {
        p82xm_emit_verdict();
    }
#endif
#if P82XN_ENABLE
    /* P82-X-N: per-frame CP-A snapshot scheduler — slot 0 at frame 5 (pre-DMA
     * failsafe) and slots 1/2/3 at STR=1 + 2/+4/+6 frames respectively. The
     * STR observer lives in trace_Memory_WriteB/W; p82xn_cpa_tick reads
     * p82xn_str_seen_frame and acts. Idempotent (slot bit-mask guards). */
    p82xn_cpa_tick();
    /* P82-X-N: VERDICT one-shot at frame>=96 — same gating pattern as
     * P82-X-K/-L/-M. p82xn_verdict_done (TU-local to m68000_bridge.c) keeps
     * the dump to a single emit even on repeated calls. */
    if (frame_num >= 96) {
        p82xn_emit_verdict();
    }
#endif
#if P82XO_ENABLE
    /* P82-X-O: per-frame CP-O-3 anchor driver — anchor 0 at frame 5
     * (pre-override), anchor 1 at frame 30 (override-stable). anchor 2 is
     * taken from p82xo_on_vector_fetch synchronously with CP-O-1 latch. */
    p82xo_tick();
    /* P82-X-O: VERDICT one-shot at frame>=96 — same gating idiom as P82XN.
     * p82xo_verdict_done (TU-local to m68000_bridge.c) keeps the dump to a
     * single emit even on repeated calls. */
    if (frame_num >= 96) {
        p82xo_emit_verdict();
    }
#endif
#if P82XP_ENABLE
    /* P82-X-P: per-frame CP-P-D D-2 BusErrHandling sticky-edge poll — catches
     * BusError events that miss the trace callbacks (DMAC/SCSI/MIDI 等 trace
     * 非経由経路). VERDICT one-shot at frame>=96 — same gating idiom as P82XO.
     * p82xp_verdict_done (TU-local to m68000_bridge.c) keeps the dump to a
     * single emit even on repeated calls. */
    p82xp_tick();
    if (frame_num >= 96) {
        p82xp_emit_verdict();
    }
#endif
    /* P82-X-Q verdict (Plan §4 / §6). One-shot at frame >= 96 from the
     * dump-side. Internally guarded so multiple calls are no-ops. */
    if (frame_num >= 96) {
        p82xq_emit_verdict();
    }
    /* P82-X-R (Round 5) per-frame tick (CP-R-4 DMAC NIV/EIV snap at
     * anchor frame=90 + CP-R-5 BusErr extern sticky-edge per-frame
     * poll) and one-shot verdict at frame≥96. Internally guarded so
     * extra calls are no-ops. Read-only — Plan §3 / §5 / §6. */
    p82xr_tick();
    if (frame_num >= 96) {
        p82xr_emit_verdict((uint32_t)frame_num);
    }
    /* P82-X-T per-frame tick — drives CP-T-2 (DMA-CCR-GATE edge),
     * CP-T-3 (IOC-INTSTAT-GATE bit7 rising-edge), CP-T-5 (FDC trajectory
     * 1 row/frame in window 80-95), and CP-T-6 (NIV/EIV stage A frame 5 +
     * stage B frame 90). All emit one-shot or capped — read-only.
     * Plan: /tmp/mx68k_P82-X-T_plan.md §3. */
    p82xt_tick();
    /* P82-X-U per-frame tick — drives CP-U-1 (FDC MSR/bufready sampler,
     * window 80-95), CP-U-3 (PC band classify, window 60-95), and CP-U-5
     * ($C90 result-buffer one-shot at frame=90). CP-U-2/U-4 live in the
     * per-chunk hook (m68000_bridge.c). CP-U-6 lives in trace_Memory_WriteB.
     * All emit one-shot or capped — read-only.
     * Plan: /tmp/mx68k_P82-X-U_plan.md §3. */
    p82xu_tick();
    /* P82-X-V per-frame tick — drives CP-V-2 (DMAC NIV/EIV vector-table
     * snap at frame=5 stage A + frame=90 stage B, per Codex Q2 vectored
     * verdict) and CP-V-5 (IRQH all-levels + IPL + MFP IPRA/B + DMA0
     * CSR/CCR sampler, window 80-95, cap 16). CP-V-3/V-4 live in the
     * per-chunk hook (m68000_bridge.c). All emit one-shot or capped —
     * read-only. Plan: /tmp/mx68k_P82-X-V_plan.md §3. */
    p82xv_tick();
    /* P37-DIAG: フレーム末尾のNEST-IRQカウントとSSP総変化記録 */
    if (frame_num <= 20) {
        uint32_t _p37_end_ssp = m68000_get_reg(M68K_MSP);
        int32_t  _p37_total_delta = (int32_t)(s_p37_frame_start_ssp - _p37_end_ssp);
        debug_log("[P37-DIAG] frame=%d FRAME-END SSP=0x%08x "
                  "total-delta=%+d NEST-IRQ(VSYNC)=%d\n",
                  frame_num - 1, _p37_end_ssp, _p37_total_delta, s_p37_vsync_count);
    }
    /* P47-A-DIAG-6: SR.IPL and MFP ISR registers per-frame end (frame<=30) for
     * direct observation of IRQ6 nesting and IPL transitions. */
    if (frame_num <= 30) {
        uint32_t sr  = m68000_get_reg(M68K_SR);
        uint8_t  ipl = (uint8_t)((sr >> 8) & 7);
        uint32_t pc  = m68000_get_reg(M68K_PC) & 0xFFFFFFu;  /* P82-F-1: per-frame PC */
        /* P82-G: D0/D7 + A5/A6. 0xFF063C executes MOVE.L D7,(2,A6) inside an
         * A6-relative command-line/path parser; D7 (and D0) carry the IOCS
         * error/result code, A6 is the parameter block, A5 the source string. */
        uint32_t d0  = m68000_get_reg(M68K_D0);
        uint32_t d7  = m68000_get_reg(M68K_D7);
        uint32_t a5  = m68000_get_reg(M68K_A5);
        uint32_t a6  = m68000_get_reg(M68K_A6);
        debug_log("[P47-A-DIAG-6] frame=%d SR=0x%04x IPL=%d "
                  "MFP_ISRA=0x%02x MFP_ISRB=0x%02x MFP_IPRA=0x%02x MFP_IPRB=0x%02x "
                  "MFP_IERA=0x%02x MFP_IERB=0x%02x MFP_TCDCR=0x%02x PC=0x%06x "
                  "D0=0x%08x D7=0x%08x A5=0x%08x A6=0x%08x\n",
                  frame_num - 1, (unsigned)sr, ipl,
                  MFP[MFP_ISRA], MFP[MFP_ISRB],
                  MFP[MFP_IPRA], MFP[MFP_IPRB],
                  MFP[MFP_IERA], MFP[MFP_IERB],
                  MFP[MFP_TCDCR], (unsigned)pc,
                  (unsigned)d0, (unsigned)d7, (unsigned)a5, (unsigned)a6);
    }
    /* P47-D-DIAG-I-VECTBL: dump vec slots $00-$140 at frame=1, 5, 10, 20 to
     * confirm IPL-ROM has overwritten the default $ff05e4 with proper handlers.
     * Spec §3.4 / §6.1 DIAG-F.
     * Slots of interest:
     *   $b8 (vec#$2e TRAP#14)  — should become 0x00ff0632
     *   $7c (vec#$1f NMI/L7)   — should become 0x00ff05c8
     *   $100-$13f (vec#$40-$4f MFP) — should become specific handlers,
     *                                 NOT 0x00ff05e4 */
    {
        static int s_p47d_vectbl_dumped[4] = {0, 0, 0, 0};
        int slot = -1;
        if      (frame_num == 1)  slot = 0;
        else if (frame_num == 5)  slot = 1;
        else if (frame_num == 10) slot = 2;
        else if (frame_num == 20) slot = 3;
        if (slot >= 0 && !s_p47d_vectbl_dumped[slot]) {
            s_p47d_vectbl_dumped[slot] = 1;
            debug_log("[P47-D-DIAG-I-VECTBL] frame=%d snapshot:\n", frame_num);
            debug_log("[P47-D-DIAG-I-VECTBL]   vec#$02 ($08)=0x%08x  vec#$03 ($0c)=0x%08x  vec#$04 ($10)=0x%08x\n",
                      p47_read_long_le(0x08), p47_read_long_le(0x0C), p47_read_long_le(0x10));
            debug_log("[P47-D-DIAG-I-VECTBL]   vec#$08 ($20)=0x%08x  vec#$0f ($3c)=0x%08x  vec#$1f ($7c)=0x%08x\n",
                      p47_read_long_le(0x20), p47_read_long_le(0x3C), p47_read_long_le(0x7C));
            debug_log("[P47-D-DIAG-I-VECTBL]   vec#$2e ($b8)=0x%08x (TRAP#14, expect 0x00ff0632)\n",
                      p47_read_long_le(0xB8));
            for (int v = 0x40; v <= 0x4F; v++) {
                uint32_t handler = p47_read_long_le((uint32_t)v * 4);
                debug_log("[P47-D-DIAG-I-VECTBL]   vec#$%02x ($%03x)=0x%08x%s\n",
                          v, v * 4, handler,
                          handler == 0x00ff05e4 ? " <-- DEFAULT (panic)" : "");
            }
        }
    }
    /* P33-DIAG: SSP per-frame delta tracking */
    {
        static uint32_t s_p33_last_ssp = 0;
        uint32_t cur_ssp = m68000_get_reg(M68K_MSP);
        if (frame_num <= 15) {
            int32_t delta = (s_p33_last_ssp != 0)
                            ? (int32_t)(s_p33_last_ssp - cur_ssp) : 0;
            uint32_t cur_pc = m68000_get_reg(M68K_PC);
            debug_log("[P33-DIAG-SSP] frame=%d SSP=0x%08x delta=%+d PC=0x%06x IPRB=0x%02x ISRB=0x%02x\n",
                      frame_num, cur_ssp, delta, cur_pc, MFP[MFP_IPRB], MFP[MFP_ISRB]);
            /* P34-DIAG / P35-DIAG: delta != 0 の場合、スタック状態を確認 */
            if (delta != 0) {
                uint32_t ram_limit = (uint32_t)(12*1024*1024 - 6);  /* 0xBFFFF9 */
                if (MEM && cur_ssp >= 0x400 && cur_ssp < ram_limit
                    && s_p34_stack_dump_count < 5) {
                    /* P47-A-DIAG-1: byte-order bug fixed via p47_read_stack_frame() */
                    uint16_t stk_sr;
                    uint32_t stk_pc;
                    p47_read_stack_frame(cur_ssp, &stk_sr, &stk_pc);
                    debug_log("[P34-DIAG-STK] frame=%d delta=%+d SSP=0x%08x "
                              "stk_SR=0x%04x stk_PC=0x%08x (odd=%d) (dump#%d) (post-byte-order-fix)\n",
                              frame_num, delta, cur_ssp,
                              stk_sr, stk_pc, (stk_pc & 1) ? 1 : 0,
                              s_p34_stack_dump_count + 1);
                    if (stk_pc >= 0x000FFF00U && stk_pc <= 0x000FFF10U) {
                        debug_log("[P34-DIAG-STK] stk_PC is near RTE stub — stub should be executing\n");
                    } else if (stk_pc & 1) {
                        debug_log("[P34-DIAG-STK] WARNING: stk_PC=0x%08x is ODD (post-byte-order-fix; suggests Address Error chain)\n",
                                  stk_pc);
                    }
                    s_p34_stack_dump_count++;
                } else if (!(cur_ssp >= 0x400 && cur_ssp < ram_limit)
                           && s_p35_oob_dump_count < 5) {
                    /* RAM範囲外: SSP値のみ記録（MEM直接アクセス禁止） */
                    uint32_t cur_pc_now = m68000_get_reg(M68K_PC);
                    debug_log("[P35-DIAG-STK-OOB] frame=%d delta=%+d SSP=0x%08x "
                              "OUT_OF_RAM (ram_limit=0x%08x) PC=0x%08x (dump#%d)\n",
                              frame_num, delta, cur_ssp, ram_limit, cur_pc_now,
                              s_p35_oob_dump_count + 1);
                    s_p35_oob_dump_count++;
                }
            }
        }
        s_p33_last_ssp = cur_ssp;
    }
    /* P47-A-DIAG-2: BIOS panic entry trap. Detect PC at any of three known panic
     * points (0xff0632 = lea, 0xff0638 = bsr completed, 0xff063c = halt loop)
     * and dump 32 longwords of supervisor stack to identify the panic call chain. */
    {
        uint32_t pc = m68000_get_reg(M68K_PC);
        if (s_p47_panic_logged < 3 &&
            (pc == 0xff0632 || pc == 0xff0638 || pc == 0xff063c)) {
            uint32_t ssp = m68000_get_reg(M68K_MSP);
            uint32_t sr  = m68000_get_reg(M68K_SR);
            uint32_t usp = m68000_get_reg(M68K_SP);
            debug_log("[P47-A-DIAG-2] BIOS PANIC ENTRY pc=0x%08x ssp=0x%08x usp=0x%08x sr=0x%04x frame=%d\n",
                      pc, ssp, usp, sr, frame_num);
            uint32_t ram_limit = (uint32_t)(12*1024*1024 - 4);
            if (MEM && (ssp & 1) == 0 && ssp >= 0x400 && ssp + 4 < ram_limit) {
                for (int i = 0; i < 32; i++) {
                    uint32_t addr = ssp + i * 4;
                    if (addr + 3 >= ram_limit) break;
                    uint16_t hi = *(uint16_t*)&MEM[addr];
                    uint16_t lo = *(uint16_t*)&MEM[addr + 2];
                    uint32_t val = ((uint32_t)hi << 16) | lo;
                    debug_log("[P47-A-DIAG-2]   ssp+%3d (0x%08x): 0x%08x\n",
                              i*4, addr, val);
                }
            } else {
                debug_log("[P47-A-DIAG-2]   stack dump skipped: ssp out of RAM range or odd\n");
            }
            s_p47_panic_logged++;
        }
    }

    /* P166: boot progress metric — advancing (new RAM code reached / text printed)
     * vs frozen (fixed loop)? Per-frame PC high-water mark + text-printed check,
     * logged every 30 frames over a long run. Read-only. */
    {
        static uint32_t s_p166_max_ram_pc      = 0;
        static uint32_t s_p166_last_logged_max = 0;
        uint32_t p166_pc = (uint32_t)m68000_get_reg(M68K_PC) & 0xFFFFFF;
        /* RAM-region code (loaded program), excluding low vectors and ROM/IO */
        if (p166_pc >= 0x002000 && p166_pc < 0xc00000 && p166_pc > s_p166_max_ram_pc)
            s_p166_max_ram_pc = p166_pc;
        if (frame_num >= 30 && (frame_num % 30) == 0) {
            int tdw_nz = 0;
            for (int i = 0; i < 1024 * 512; i++) { if (TextDrawWork[i]) { tdw_nz++; if (tdw_nz >= 100) break; } }
            debug_log("[P166-PROGRESS] frame=%d PC=0x%06x max_ram_pc=0x%06x new_ram=%d TextDrawWork_nz=%d\n",
                      frame_num, (unsigned)p166_pc, (unsigned)s_p166_max_ram_pc,
                      (s_p166_max_ram_pc > s_p166_last_logged_max) ? 1 : 0, tdw_nz);
            /* P516: g_p169_tvram_wb は #if P169_ENABLE ガードの外で常時カウント
             * されている真の実測値なので無条件に出力する。一方 g_p169_tvram_ww は
             * P169_ENABLE ガードの内側でしか加算されないため、無効時は「実測して
             * いない」ことが読み手に伝わるよう 0 ではなく DISABLED を出力する
             * (自己反証可能性ゲート: 0件と未測定を文字列レベルで区別する)。 */
            debug_log("[P169-CRTC] frame=%d CRTC2a=0x%02x 2b=0x%02x 2e=0x%02x 2f=0x%02x "
#if P169_ENABLE
                      "tvram_wb=%u tvram_ww=%u\n",
#else
                      "tvram_wb=%u tvram_ww=DISABLED\n",
#endif
                      frame_num, (unsigned)CRTC_Regs[0x2a], (unsigned)CRTC_Regs[0x2b],
                      (unsigned)CRTC_Regs[0x2e], (unsigned)CRTC_Regs[0x2f],
#if P169_ENABLE
                      (unsigned)g_p169_tvram_wb, (unsigned)g_p169_tvram_ww);
#else
                      (unsigned)g_p169_tvram_wb);
#endif
            s_p166_last_logged_max = s_p166_max_ram_pc;
        }
    }

    /* P41-DIAG: PC=0xff063c 固着検出と FDC/IOC/DMA/MFP 状態診断 */
    {
        static uint32_t s_p41_stuck_pc    = 0;
        static int      s_p41_stuck_count = 0;
        static int      s_p41_mfp_dumped  = 0;

        if (frame_num >= 10) {
            uint32_t cur_pc  = (uint32_t)m68000_get_reg(M68K_PC);
            uint32_t cur_sr  = (uint32_t)m68000_get_reg(M68K_SR);
            uint32_t cur_ssp = (uint32_t)m68000_get_reg(M68K_MSP);

            if (cur_pc == s_p41_stuck_pc) {
                s_p41_stuck_count++;
            } else {
                if (s_p41_stuck_count >= 10) {
                    debug_log("[P41-DIAG] PC UNSTUCK: was 0x%06x for %d frames, now 0x%06x\n",
                              s_p41_stuck_pc, s_p41_stuck_count, cur_pc);
                }
                s_p41_stuck_pc    = cur_pc;
                s_p41_stuck_count = 1;
            }

            int is_stuck = (s_p41_stuck_count >= 10);
#if P69_PROBE_ENABLE
            /* P69-A Probe-C: 停止直前(frame 8..30)も FDC 状態を毎フレーム
             * 観測する。0xff0eca wait ループ本体(frame 10-20)を丸ごと捕捉。
             * FDC_Read(0xe94001) は状態を変更しない(副作用なし)。 */
            static int s_p69_prewin_cnt = 0;
            int in_prewin = (frame_num >= 8 && frame_num <= 30 &&
                             s_p69_prewin_cnt < 23);
            int read_fdc  = is_stuck || in_prewin;
            if (in_prewin) s_p69_prewin_cnt++;
#else
            int read_fdc  = is_stuck;
#endif
            /* FDC_Read はログ量抑制のため固着確定後/P69窓内のみ呼ぶ */
            uint8_t fdc_status     = read_fdc ? FDC_Read(0xe94001) : 0;
            int     fdc_data_ready = read_fdc ? FDC_IsDataReady()  : 0;
            int do_diag = (frame_num % 5 == 0) ||
                          (is_stuck && s_p41_stuck_count <= 15)
#if P69_PROBE_ENABLE
                          || in_prewin
#endif
                          ;

            if (do_diag) {
                debug_log("[P41-DIAG] frame=%d PC=0x%06x SR=0x%04x SSP=0x%08x "
                          "stuck=%d FDC_ST=0x%02x FDC_RDY=%d "
                          "IOC_IntStat=0x%02x IOC_IntVect=0x%02x "
                          "DMA0_CSR=0x%02x DMA0_CCR=0x%02x DMA0_MTC=0x%04x\n",
                          frame_num, cur_pc, cur_sr, cur_ssp,
                          s_p41_stuck_count,
                          (unsigned)fdc_status, fdc_data_ready,
                          (unsigned)IOC_IntStat, (unsigned)IOC_IntVect,
                          (unsigned)DMA[0].CSR, (unsigned)DMA[0].CCR,
                          (unsigned)DMA[0].MTC);
            }

            if (is_stuck && !s_p41_mfp_dumped) {
                debug_log("[P41-DIAG-MFP] frame=%d (stuck@0x%06x) "
                          "TACR=0x%02x TBCR=0x%02x TCDCR=0x%02x "
                          "TADR=0x%02x TBDR=0x%02x TCDR=0x%02x TDDR=0x%02x "
                          "IERA=0x%02x IERB=0x%02x IMRA=0x%02x IMRB=0x%02x "
                          "IPRA=0x%02x IPRB=0x%02x ISRA=0x%02x ISRB=0x%02x\n",
                          frame_num, cur_pc,
                          MFP[MFP_TACR],  MFP[MFP_TBCR],  MFP[MFP_TCDCR],
                          MFP[MFP_TADR],  MFP[MFP_TBDR],  MFP[MFP_TCDR],  MFP[MFP_TDDR],
                          MFP[MFP_IERA],  MFP[MFP_IERB],  MFP[MFP_IMRA],  MFP[MFP_IMRB],
                          MFP[MFP_IPRA],  MFP[MFP_IPRB],  MFP[MFP_ISRA],  MFP[MFP_ISRB]);
                debug_log("[P41-DIAG-REGS] frame=%d (stuck@0x%06x) "
                          "D0=0x%08x D1=0x%08x D2=0x%08x D3=0x%08x "
                          "A0=0x%08x A1=0x%08x A7=0x%08x\n",
                          frame_num, cur_pc,
                          (unsigned)m68000_get_reg(M68K_D0), (unsigned)m68000_get_reg(M68K_D1),
                          (unsigned)m68000_get_reg(M68K_D2), (unsigned)m68000_get_reg(M68K_D3),
                          (unsigned)m68000_get_reg(M68K_A0), (unsigned)m68000_get_reg(M68K_A1),
                          (unsigned)m68000_get_reg(M68K_A7));
                if (MEM && cur_ssp >= 0x400 && cur_ssp < (uint32_t)(12*1024*1024 + 4 - 12)) {
                    /* P47-A-DIAG-1: byte-order bug fixed — read each word as host-native LE16 */
                    debug_log("[P41-DIAG-STK] frame=%d SSP=0x%08x "
                              "stk[0]=0x%04x stk[1]=0x%04x stk[2]=0x%04x "
                              "stk[3]=0x%04x stk[4]=0x%04x stk[5]=0x%04x (post-byte-order-fix)\n",
                              frame_num, cur_ssp,
                              (unsigned)*(uint16_t*)&MEM[cur_ssp     ],
                              (unsigned)*(uint16_t*)&MEM[cur_ssp +  2],
                              (unsigned)*(uint16_t*)&MEM[cur_ssp +  4],
                              (unsigned)*(uint16_t*)&MEM[cur_ssp +  6],
                              (unsigned)*(uint16_t*)&MEM[cur_ssp +  8],
                              (unsigned)*(uint16_t*)&MEM[cur_ssp + 10]);
                    /* Also expose stk_SR + stk_PC reconstructed (LE16 native) for direct check */
                    uint16_t _p41_stk_sr;
                    uint32_t _p41_stk_pc;
                    p47_read_stack_frame(cur_ssp, &_p41_stk_sr, &_p41_stk_pc);
                    debug_log("[P41-DIAG-STK] frame=%d SSP=0x%08x stk_SR=0x%04x stk_PC=0x%08x (odd=%d)\n",
                              frame_num, cur_ssp, _p41_stk_sr, _p41_stk_pc,
                              (_p41_stk_pc & 1) ? 1 : 0);
                }
                s_p41_mfp_dumped = 1;
            }
        }
    }
    /* P12-TRACE: frame<=10毎フレーム、frame>=600はクラッシュ解析のため毎フレーム詳細ログ、それ以外は30フレームごと */
    {
        int do_log = (frame_num <= 10) ||
                     (frame_num >= 40 && frame_num <= 55) ||  /* P21-DIAG: frame=47 crash zone */
                     (frame_num >= 600) ||
                     (frame_num % 30 == 0);
        int do_detail = (frame_num >= 40 && frame_num <= 55) || (frame_num >= 600);
        if (do_log) {
            MX68KStatus st;
            mx68k_get_status(&st);
            if (do_detail) {
                uint32_t cpu_status = C68K.Status;
                debug_log("[MX68K] frame=%d pc=%06x sr=%04x ssp=%08x a7=%08x"
                          " cpu_st=%02x cycles=%d fdd0=%d fdd1=%d\n",
                          frame_num, st.pc, st.sr, st.isp, st.a[7],
                          cpu_status, executed, FDD_IsReady(0), FDD_IsReady(1));
            } else {
                debug_log("[MX68K] frame=%d pc=%06x sr=%04x cycles=%d fdd0=%d fdd1=%d\n",
                          frame_num, st.pc, st.sr, executed, FDD_IsReady(0), FDD_IsReady(1));
            }
        }
    }

    /* P294: フレーム後処理(frame-summary probe + 集計ログ + framebuffer publish)。
     * per-scanline 描画は exec ループ内の draw_display_line で既に完了している。
     * P173: emulation スレッドで実行 — cross-thread VLINE race 無し。 */
    mx68k_render_end();

    /* P276: D-3是正 — GVRAMをサンプル(render_frame)した後にクリアする
     * (サンプル→クリア。px68k本家/MPX68K実装と同じ順序)。
     * 元はHラインループ直後(フレーム末より前)にあり、render_frameより
     * 先にGVRAMをクリアしてしまっていた(クリア→サンプルの誤った順序)。
     *
     * P218: CRTC 高速クリアのライフサイクル（frame 末）。
     * 参照: upstream px68k x11/winx68k.cpp:488-503 /
     *       MPX68K X68000 Shared/px68k/x11/winx68k.cpp:577-592（byte 一致）。
     * MX はループを Bridge に再実装した際にこのブロックを移植し損ねていた
     * (Core/px68k/winx68k.c は 46 行スタブ)。結果 CRTC_FastClr は永久に 0、
     * CRTC_Mode bit1 は永久に 1、GVRAM_FastClear() は呼び出し元ゼロ。 */
#if P468_ENABLE
    /* [P468-FCPOLL] D-35症状2: [P467-FASTCLR] が 0 行だったという既存の解釈
     * (「条件が一度も真にならなかった」)にポジティブコントロールを与える分母。
     * 直後の `if (CRTC_Mode & 2)` の判定結果を、真偽どちらであっても 60 フレームに
     * 1 回そのまま出力する。これにより「条件が偽だった」と「そもそもこのブロックに
     * 到達していない(= 1 行も出ない)」を、後からログだけで一意に区別できる
     * (P467解析§1末尾で申告した自己反証性の欠陥の解消)。
     * 完全に読み取り専用 —— 直後の if 条件・CRTC_Mode・CRTC_FastClr のいずれにも
     * 触れず、既存の制御フローを一切変更しない。 */
    if (g_mx68k_frame_num % 60 == 0) {
        debug_log("[P468-FCPOLL] f=%d CRTC_Mode=0x%02x mode_bit1=%d CRTC_FastClr=%d\n",
                  g_mx68k_frame_num, (unsigned)CRTC_Mode,
                  ((CRTC_Mode & 2) != 0) ? 1 : 0, (int)CRTC_FastClr);
    }
#endif
    if (CRTC_Mode & 2) {            /* 高速クリア要求が出ているときだけ動く */
        if (CRTC_FastClr) {         /* FastClr=1 且つ CRTC_Mode&2 なら 終了 */
            CRTC_FastClr--;
            if (!CRTC_FastClr)
                CRTC_Mode &= 0xfd;
        } else {                    /* FastClr 開始 */
            if (CRTC_Regs[0x29] & 0x10)
                CRTC_FastClr = 1;
            else
                CRTC_FastClr = 2;
            TVRAM_SetAllDirty();
#if P534_ENABLE
            /* P534 [P534-GVWCOL]: 高速クリアの発火数。P467_ENABLE の値に
             * 関わらず GVRAM_FastClear() を呼ぶ両経路を 1 箇所でカバーする
             * ため、#if P467_ENABLE の前に置く。読み取り専用の計数のみ。 */
            s_p534_fcfire++;
#endif
#if P467_ENABLE
            /* [P467-FASTCLR] D-35症状2: 高速クリアが実際に発火しているか、
             * 発火してもマスクで一部が残るのかを切り分けるための実測。
             * GVRAM_FastClear() 呼出しの前後で非ゼロ word 数(分子)と総 word 数
             * (分母)を両方記録し、トリガ条件が依存する生値
             * (CRTC_Mode / CRTC_FastClr / CRTC_Regs[0x29] / CRTC_Regs[0x2b]&15 /
             *  CRTC_FastClrMask / CRTC_FastClrLine)を同じ行に並べる。
             * すべて読み取り専用 —— GVRAM・レジスタへの書込みは一切しない。
             * ★Code Review条件2: 静的カウンタで最初の500件のみ記録する
             * (CRTC_Mode&2 が毎フレーム立つタイトルでログが際限なく増えるのを防ぐ
             *  安全弁。走査自体はカウンタ判定の前に行うためオーバーヘッドは不変)。 */
            {
                unsigned p467_total_b = 0, p467_total_a = 0;
                unsigned p467_nz_before = p467_gvram_nonzero_words(&p467_total_b);
                GVRAM_FastClear();
                unsigned p467_nz_after = p467_gvram_nonzero_words(&p467_total_a);
                static unsigned s_p467_fastclr_fire_count = 0;
                if (s_p467_fastclr_fire_count < 500) {
                    s_p467_fastclr_fire_count++;
                    debug_log("[P467-FASTCLR] f=%d fire=%u nz_before=%u/%u nz_after=%u/%u "
                              "CRTC_Mode=%02x CRTC_FastClr_after_set=%d CRTC_R29=%02x "
                              "R2b_lo4=%x FastClrMask=%04x FastClrLine=%u\n",
                              g_mx68k_frame_num, s_p467_fastclr_fire_count,
                              p467_nz_before, p467_total_b, p467_nz_after, p467_total_a,
                              (unsigned)CRTC_Mode, (int)CRTC_FastClr,
                              (unsigned)CRTC_Regs[0x29], (unsigned)(CRTC_Regs[0x2b] & 15),
                              (unsigned)CRTC_FastClrMask, (unsigned)CRTC_FastClrLine);
                }
            }
#else
            GVRAM_FastClear();
#endif
        }
    }
}

// ---- FDD ----
int mx68k_fdd_insert(int drive, const char* path) {
    if (drive < 0 || drive > 3) return -1;   /* P684: 2 台 → 4 台 */
    if (!path) return -1;
    debug_log("[MX68K] mx68k_fdd_insert: drive=%d path=%s\n", drive, path);
    /* P443 (D-7): FDD_SetFD() を先に呼び、g_fdd_path[] への書込みはその後に行う
     * (並べ替えのみ、ロジック変更なし)。FDD_SetFD() 内で s_fdd_present[] が
     * 立つため、run_frame()(CVDisplayLink スレッド)側の「メディア無し
     * かつパス有り → クリア」チェックが、この関数(main thread)の実行中に
     * 走っても新しいパスを誤って消すことがない。FDD_SetFD() は g_fdd_path[] に
     * 依存しないので順序入れ替えは安全。 */
    FDD_SetFD(drive, (char*)path, 0);
    strncpy(g_fdd_path[drive], path, sizeof(g_fdd_path[drive]) - 1);
    g_fdd_path[drive][sizeof(g_fdd_path[drive]) - 1] = '\0';

    /* P48-A (★ 案1): Drain SetDelay synchronously from 3 to 1 via two
     * successive FDD_SetFDInt() calls. The third 1->0 transition (which is
     * the one that actually dispatches IRQH_Int(1,&FDD_Int)) is left to the
     * existing every-frame FDD_SetFDInt() call inside mx68k_run_frame()
     * (see line ~1049). That call fires on frame=1 while P23-FIX has
     * IOC_IntStat=0x0E (bit1 set), so IRQ1 is guaranteed to be queued via
     * the regular px68k code path — no synthetic dispatch is performed here.
     *
     * Subsumes P46-FIX-A (single FDD_SetFDInt() drain 3->2). */
    s_p48a_drain_count++;
    FDD_SetFDInt();   /* SetDelay 3 -> 2 */
    FDD_SetFDInt();   /* SetDelay 2 -> 1 (frame=1 will complete 1 -> 0 + IRQ) */
    debug_log("[P48-A-DRAIN] drive=%d SetDelay drained 3->1 (frame=1 will fire IRQ) "
              "FDD_IsReady(%d)=%d IOC_IntStat=0x%02x IRQH_IRQ[1]=%d "
              "(drain_count=%d)\n",
              drive, drive, FDD_IsReady(drive),
              (unsigned)IOC_IntStat, (int)IRQH_IRQ[1],
              s_p48a_drain_count);

    /* P49-B-FDD-INT-PULSE: optional one-shot bit6 set of IOC_IntStat at
     * FDD insertion. Spec §3.1: bit6 = FDD INT (DISK IN edge). Spec §5.2
     * says it should fire on DISK IN 0->1; px68k base core does not
     * implement this OR (Code-inv §B.2). Gated OFF by default — see
     * the P49B_FDD_INT_PULSE_ENABLE block-comment near the top of this
     * file for the full enable-side prerequisites (paired bit6-clear
     * path is mandatory).
     *
     * NOTE on placement: this stub stays exactly here at the
     * mx68k_fdd_insert() call site. Spec §5.2's "edge" semantics are
     * approximated by "OR bit6 once per FDD insert call". If the stub is
     * ever moved away from mx68k_fdd_insert(), eject+reinsert mid-run
     * would miss the edge — keep it here (Requirements Review §6 spec
     * note). */
#if P49B_FDD_INT_PULSE_ENABLE
    {
        uint8_t prev = IOC_IntStat;
        IOC_IntStat |= 0x40u;  /* FDD INT source bit (Spec §3.1 bit6) */
        debug_log("[P49-B-FDD-INT-PULSE] one-shot bit6 set: "
                  "IOC_IntStat 0x%02x -> 0x%02x (drive=%d)\n",
                  (unsigned)prev, (unsigned)IOC_IntStat, drive);
        /* Existing IRQH_Int(1,&FDD_Int) drain via P48-A delivers IRQ1. */
    }
#endif
    return 0;
}

void mx68k_fdd_eject(int drive) {
    if (drive < 0 || drive > 3) return;   /* P684: 2 台 → 4 台 */
    g_fdd_path[drive][0] = '\0';
    FDD_EjectFD(drive);
}

/* P271: FDライトプロテクト。Core FDD_SetReadOnly()は一方向セット(解除は
 * eject 時の ROnly=0 クリアのみ、fdd.c:120)なので、この関数は「挿入直後に
 * protect=true ならセットする」という順序で呼ぶことを前提とする(mountFDD 側で
 * insert 直後に呼ぶ)。挿入済みディスクのプロテクトを OFF に戻すには eject+再挿入が
 * 必要(実機のディスク側の爪を切り替えるのに一旦取り出す物理動作と対応)。 */
void mx68k_fdd_set_write_protect(int drive, int protect) {
    if (drive < 0 || drive > 3) return;   /* P684: 2 台 → 4 台 */
    if (protect) {
        FDD_SetReadOnly(drive);
        debug_log("[P271] FDD%d write-protect ON\n", drive);
    }
    /* protect==0 は no-op(Core に解除関数が無いため)。呼び出し側(mountFDD)は
     * 挿入のたびに新しい状態で呼ぶので、通常フローでは問題にならない。 */
}

bool mx68k_fdd_is_write_protected(int drive) {
    if (drive < 0 || drive > 3) return false;   /* P684: 2 台 → 4 台 */
    return FDD_IsReadOnly(drive) != 0;
}

bool mx68k_fdd_is_inserted(int drive) {
    return (FDD_IsReady(drive) != 0);
}

bool mx68k_fdd_is_active(int drive) {
    (void)drive;
    return (FDD_IsReading != 0);
}

const char* mx68k_fdd_get_path(int drive) {
    if (drive < 0 || drive > 3) return NULL;   /* P684: 2 台 → 4 台 */
    return g_fdd_path[drive];
}

/* ======================================================================
 * P458 (D-29): HDD イメージのマウント検証を「拡張子ベース」から
 * 「実ファイルサイズベース」へ。
 *
 * SASI HDD の実機容量制約は 10/20/40MB の3値のみ。値の一次情報源は
 * XM6 vm/disk.cpp:1878-1920 SASIHD::Open()。同一値が本リポジトリの
 * Bridge/scsi_disk.cpp の SASIHD::Open (P249 で移植済みだが未配線の
 * 死にコード, :2076-2084) にも既に存在する — 定数を新規に起こしたのでは
 * なく、既に移植済みのロジックの値をそのまま踏襲した。
 * 外付け SCSI 側 (512B 倍数 / 10MB以上 / 4095MiB以下) の一次情報源は
 * XM6 vm/disk.cpp:1999-2010、同値が Bridge/scsi_disk.cpp:2185-2196 にある。
 *
 * <sys/stat.h> はこのファイル冒頭で既に include 済み。
 * ==================================================================== */
#define P458_SASI_10MB 0x9f5400L
#define P458_SASI_20MB 0x13c9800L
#define P458_SASI_40MB 0x2793000L
#define P458_SCSI_MIN  0x9f5400L
#define P458_SCSI_MAX  0xfff00000L

/* P458: ファイルサイズを取得(stat失敗時は-1)。 */
static long p458_hdd_file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* P458: SASI HDD として妥当なサイズか(10/20/40MB完全一致のみ)。 */
static bool p458_sasi_size_valid(long size) {
    return size == P458_SASI_10MB || size == P458_SASI_20MB || size == P458_SASI_40MB;
}

/* P458: 外付けSCSI HDD として妥当なサイズか(512B倍数かつ10MB~4095MiB範囲)。 */
static bool p458_scsi_size_valid(long size) {
    if (size & 0x1ff) return false;
    if (size < P458_SCSI_MIN) return false;
    if (size > P458_SCSI_MAX) return false;
    return true;
}

/* ====================================================================
 * P687 (D-29 B): ディスクイメージ先頭8バイトの読み取り。
 *
 * SCSI 用にフォーマットされた X68000 ディスクは先頭 8 バイトに自己記述
 * シグネチャ "X68SCSI1"(= 58 36 38 53 43 53 49 31)を持つ
 * (一次情報源: Docs/Inside X68000_text.pdf p.468 図9「SCSIデバイス
 *  パラメータ」。裏取り: Core/px68k/x68k/scsi.c:611 の Human_ipl() が
 *  Memory_ReadD(0x02000)==0x58363853 && Memory_ReadD(0x02004)==0x43534931
 *  で同じ 8 バイトを検査している。★同ファイル :551 の SCSI_ipl() は
 *  0x02001 という異なるオフセットを使っており一次情報源にしない)。
 * SASI 側には署名の概念自体が無い(Core/px68k/x68k/sasi.c に検査コードは
 * 存在せず、Inside X68000 の SASI 章にもフォーマット・署名の節が無い)ため、
 * この検査は SASI スロットへの誤挿入検出にのみ使える片方向の判定である。
 *
 * 読み取り失敗(オープン不可 / 8バイト未満)は false を返し、out はゼロ埋め
 * する。呼び出し側は「署名なし」として素通りさせる — P458 のサイズ検証と
 * 同じく、I/O 失敗自体を拒否理由にはしない設計。
 * ==================================================================== */
static bool p687_read_head8(const char* path, uint8_t out[8]) {
    memset(out, 0, 8);
    if (!path) return false;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    size_t n = fread(out, 1, 8, fp);
    fclose(fp);
    if (n != 8) { memset(out, 0, 8); return false; }
    return true;
}

/* ======================================================================
 * P200: HDD (SASI .hdf) mount
 *
 * Config.HDImage[] holds the SASI disk-image path per slot. sasi.c opens
 * that path on demand (256B/sector, offset SASI_Sector<<8, raw image, no
 * header); an empty string means "no drive" and sasi.c returns the no-drive
 * status itself. The SASI Read/Write handlers are already wired into the
 * Core memory tables, so mounting is purely storing the path — sasi.c
 * re-opens it fresh on the very next sector access (no cache), no reset
 * needed.
 *
 * device-ID mapping: logical unit 0..7 -> physical index unit*2 (SASI device
 * 0 -> HDImage[0], device 1 -> HDImage[2], … device 7 -> HDImage[14]).
 * LUN1 slots (odd indices) are not used — Human68k probes the SASI bus per
 * device ID, so a second drive must be a distinct device (index 2), not LUN1
 * (index 1), or it stays invisible.
 *
 * P455: unit count 2 -> MX68K_SASI_UNIT_COUNT (8). The upper bound is the Core
 * SELECT presence test (sasi.c:426) reading HDImage[dev*2+1], i.e. index 15 for
 * device 7, which exactly fills prop.h:19's HDImage[16]; see the _Static_assert
 * next to s_sasi_hdd_path[].
 *
 * Thread note: like mx68k_fdd_insert, this writes the path on the main thread.
 * A torn read by the emulation thread at worst makes File_Open fail -> the
 * benign no-drive status; no reset is scheduled or needed here.
 * ==================================================================== */
int mx68k_hdd_insert(int unit, const char* path) {
    if (unit < 0 || unit >= MX68K_SASI_UNIT_COUNT) return -1;
    /* P268: SCSI 機では内蔵 SASI が配線されないので SASI HDD insert を拒否
     * (配線確定機種で判定 — pending の未リセット機種では誤拒否しない)。 */
    if (g_wired_machine_type == 4) {
        debug_log("[P268] hdd_insert refused: wired machineType=SCSI\n");
        return -2;
    }
    if (!path) return -1;
    /* P458 (D-29): 実ファイルサイズが SASI の実機容量(10/20/40MB)と一致しなければ
     * 拒否する。拡張子ではフォーマットを判別できないため(.hdf/.hds は同一の生
     * データ形式)、リネームした 512B セクタのイメージが無言でセクタ境界のずれた
     * まま読み書きされるのを防ぐ。Config.HDImage[] への書込み**前**にゲートする。 */
    long p458_size = p458_hdd_file_size(path);
    if (p458_size < 0) {
        debug_log("[P458] hdd_insert refused: stat failed path=%s\n", path);
        return -3;
    }
    if (!p458_sasi_size_valid(p458_size)) {
        debug_log("[P458] hdd_insert refused: size=%ld not a valid SASI capacity (10/20/40MB) path=%s\n",
                   p458_size, path);
        return -3;
    }
    /* P687 (D-29 B): SCSI 用にフォーマットされたイメージが SASI スロットへ
     * 誤挿入されるのを拒否する。実機では SASI/SCSI は別バス・別コネクタで
     * この誤挿入の物理経路が存在しないが、GUI でパスを自由に指定できる
     * エミュレータでは起こりうる(P458 のサイズ検証と同じ位置づけの MX 独自
     * 安全機構)。逆方向(SASI 用イメージの SCSI 誤挿入)は検出しない —
     * 「署名が無い」ことは SASI イメージ以外にもデータ専用 SCSI ディスク等
     * 複数の解釈を許すため、拒否条件にできない。
     * ★判定に使った実測 8 バイトを必ずログへ残す(「拒否した」だけでは
     *   定数取り違えがあったときにログから判別できないため)。 */
    uint8_t head8[8];
    if (p687_read_head8(path, head8) && memcmp(head8, "X68SCSI1", 8) == 0) {
        debug_log("[P687] hdd_insert refused: head8=%02x%02x%02x%02x%02x%02x%02x%02x "
                  "matches X68SCSI1 signature (SCSI-formatted image inserted into "
                  "SASI slot) path=%s\n",
                  head8[0], head8[1], head8[2], head8[3],
                  head8[4], head8[5], head8[6], head8[7], path);
        return -4;
    }
    int idx = unit * 2;
    if (strlen(path) >= sizeof(Config.HDImage[idx])) return -1;
    debug_log("[MX68K] mx68k_hdd_insert: unit=%d idx=%d path=%s\n", unit, idx, path);
    strncpy(Config.HDImage[idx], path, sizeof(Config.HDImage[idx]) - 1);
    Config.HDImage[idx][sizeof(Config.HDImage[idx]) - 1] = '\0';
    /* P447 (C2'): 実際に Config.HDImage[] へ書けた**後**にシャドウも更新する。
     * 早期 return(-1/-2)の経路では更新しない — マウントされていないものを
     * 記録しないため。この1行があるので、pushConfig を通らない設定画面の
     * Select…/D&D 経路(P239 の即時反映 UX)でもシャドウが同期し、直後の ⌘R で
     * ディスクが無言で消えることがない。 */
    strlcpy(s_sasi_hdd_path[unit], Config.HDImage[idx],
            sizeof(s_sasi_hdd_path[unit]));
    /* P239/P502: no reset needed — sasi.c still looks Config.HDImage[] up by
     * path on every sector access, so the new path takes effect on the very next
     * SASI sector I/O (verified via source read + hands-on test, P238 followup).
     * P502 added a Bridge-side fd cache under those File_* calls, so "no cache"
     * is no longer literally true; the conclusion is unchanged because the
     * invalidation hook below (g_pending_sasi_cache_invalidate) makes
     * mx68k_run_frame() close every cached fd at the next frame boundary — a
     * stale descriptor for the previous image can never be reused. */
    g_pending_sasi_cache_invalidate = 1;
    return 0;
}

int mx68k_hdd_eject(int unit) {
    if (unit < 0 || unit >= MX68K_SASI_UNIT_COUNT) return -1;
    /* P268/P503 (b): insert と対称の多層防御ゲート。現在の UI
     * (SASISettingsView.swift の .disabled(isSCSIMachine)、P457)が
     * wired==SCSI 中は Eject ボタン自体を塞いでいるため実害の再現手順は
     * 無いが、将来 UI 経路が増えた場合に備えて C API 契約としての非対称
     * (insert だけがゲートされ eject は無条件成功)を無くす。 */
    if (g_wired_machine_type == 4) {
        debug_log("[P268] hdd_eject refused: wired machineType=SCSI\n");
        return -2;
    }
    int idx = unit * 2;
    debug_log("[MX68K] mx68k_hdd_eject: unit=%d idx=%d\n", unit, idx);
    Config.HDImage[idx][0] = '\0';
    /* P447 (C2'): eject も同じ真実源を更新する。これが無いと、Eject 直後の ⌘R で
     * 古い非空シャドウから取り外したはずのディスクが復活する。 */
    s_sasi_hdd_path[unit][0] = '\0';
    /* P239/P502: no reset needed — sasi.c still looks Config.HDImage[] up by
     * path on every sector access, so the cleared path takes effect on the very
     * next SASI sector I/O. P502 added a Bridge-side fd cache under those File_*
     * calls, so "no cache" is no longer literally true; the conclusion is
     * unchanged because the invalidation hook below
     * (g_pending_sasi_cache_invalidate) makes mx68k_run_frame() close every
     * cached fd at the next frame boundary — the ejected image's descriptor is
     * released rather than lingering. */
    g_pending_sasi_cache_invalidate = 1;
    return 0;
}

/* P447 (C2'): config 保存値をシャドウへ載せるための setter(Swift の pushConfig
 * から呼ぶ)。起動直後、まだ一度も mx68k_hdd_insert() が呼ばれていない状態でも
 * 「SASI 機へ戻したときに復元すべきパス」を確定させる必要があるため、
 * mx68k_hdd_insert() の中のシャドウ更新とは別に、この入口も持つ。
 *
 * ★機種ゲートは付けない — 機種判定は Bridge 側(mx68k_reset_hard の再適用)が
 * 一手に担う。ゲートを Swift 側に置くと、pushConfig だけが機種を知っていて
 * Bridge が知らないという非対称(本サイクルが直している欠陥そのもの)が再発する。
 *
 * 内蔵 SCSI の mx68k_set_scsi_in_disk_path() と対称。path==NULL / 空文字は
 * 「未装着」としてシャドウをクリアする。 */
void mx68k_set_hdd_path(int unit, const char* path) {
    if (unit < 0 || unit >= MX68K_SASI_UNIT_COUNT) return;
    if (path && path[0]) {
        strlcpy(s_sasi_hdd_path[unit], path, sizeof(s_sasi_hdd_path[unit]));
    } else {
        s_sasi_hdd_path[unit][0] = '\0';
    }
}

bool mx68k_hdd_is_inserted(int unit) {
    if (unit < 0 || unit >= MX68K_SASI_UNIT_COUNT) return false;
    return Config.HDImage[unit * 2][0] != '\0';
}

/* ======================================================================
 * P241 Stage A: external SCSI (CZ-6BS1) disk mount.
 *
 * SCSI ID 0..6 maps directly to Config.SCSIEXHDImage[id] (prop.h: the index is
 * the SCSI ID itself, unlike SASI's unit*2). id 7 is the host X68000 and is not
 * mountable. Same shape as mx68k_hdd_insert: store the path only — scsi.c
 * re-opens the image per block access with no cache (scsi.c File_Open), so the
 * new path takes effect on the very next SCSI block I/O; no reset needed.
 *
 * Thread note: like mx68k_hdd_insert, this writes the path on the main thread;
 * a torn read by the emulation thread at worst makes File_Open fail -> the
 * benign no-device status.
 * ==================================================================== */
int mx68k_scsi_insert(int id, const char* path) {
    if (id < 0 || id > 6) return -1;
    /* P268: SCSI 機では外付 CZ-6BS1 を排他(scope)。配線確定機種で判定。
     * 起動時の外付復元ループ(Swift)は init 前に走り g_wired_machine_type は
     * まだ初期値 0 なので、そちらの排他は Swift 側 config 値ゲートが担当する。 */
    if (g_wired_machine_type == 4) {
        debug_log("[P268] scsi_insert refused: wired machineType=SCSI\n");
        return -2;
    }
    if (!path) return -1;
    /* P458 (D-29): 外付け SCSI は 512B 倍数かつ 10MB 以上 4095MiB 以下のみ受理する
     * (XM6 vm/disk.cpp:1999-2010 と同じ範囲)。Config.SCSIEXHDImage[] への
     * 書込み**前**にゲートする。 */
    long p458_size = p458_hdd_file_size(path);
    if (p458_size < 0) {
        debug_log("[P458] scsi_insert refused: stat failed path=%s\n", path);
        return -3;
    }
    if (!p458_scsi_size_valid(p458_size)) {
        debug_log("[P458] scsi_insert refused: size=%ld out of SCSI range path=%s\n",
                   p458_size, path);
        return -3;
    }
    if (strlen(path) >= sizeof(Config.SCSIEXHDImage[id])) return -1;
    debug_log("[MX68K] mx68k_scsi_insert: id=%d path=%s\n", id, path);
    strncpy(Config.SCSIEXHDImage[id], path, sizeof(Config.SCSIEXHDImage[id]) - 1);
    Config.SCSIEXHDImage[id][sizeof(Config.SCSIEXHDImage[id]) - 1] = '\0';
    return 0;
}

void mx68k_scsi_eject(int id) {
    if (id < 0 || id > 6) return;
    debug_log("[MX68K] mx68k_scsi_eject: id=%d\n", id);
    Config.SCSIEXHDImage[id][0] = '\0';
}

bool mx68k_scsi_is_inserted(int id) {
    if (id < 0 || id > 6) return false;
    return Config.SCSIEXHDImage[id][0] != '\0';
}

/* ====================================================================
 * P247 Stage 1: Internal SCSI (SUPER+ built-in SPC MB89352) skeleton.
 * Path/ROM held in Bridge only; NOT yet wired to real I/O. Real SPC
 * transfer logic and $FC0000 IPL mapping arrive in Stage 2/4.
 * ==================================================================== */
/* P251: s_scsi_in_rom / s_scsi_in_rom_loaded は Stage 2c で外部リンケージへ
 * 変更(scsi_spc_bridge.cpp が ROM を、scsi_in_bridge.c が loaded フラグを
 * それぞれ extern 参照する)。他 TU からは読み取りのみ。 */
uint8_t s_scsi_in_rom[0x2000];
bool    s_scsi_in_rom_loaded = false;
static char    s_scsi_in_image[8][4096];   /* SCSI ID 0..7 (7=host予約、実質0..6) */

/* P251(Fable5 audit #2): 実SCSI配線の初回有効化で SCSI::Reset() が sram.dat の
 * メモリスイッチ領域を恒久更新する前に、ワンタイムのバックアップを取る。
 * コピー先が既に存在すれば上書きしない(「本当の初回状態」を保全)。 */
void mx68k_backup_sram_before_scsi_wiring(void) {
    static bool done = false;
    if (done) return;
    done = true;
    const char* home = getenv("HOME");
    if (!home || !home[0]) return;
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s/Library/Application Support/MX68K/sram.dat", home);
    snprintf(dst, sizeof(dst), "%s/Library/Application Support/MX68K/sram.dat.pre_scsi.bak", home);
    FILE* fdst_probe = fopen(dst, "rb");
    if (fdst_probe) { fclose(fdst_probe); return; }   /* 既存なら上書きしない */
    FILE* fsrc = fopen(src, "rb");
    if (!fsrc) { debug_log("[P251] sram backup skipped: no sram.dat yet\n"); return; }
    FILE* fdst = fopen(dst, "wb");
    if (!fdst) { fclose(fsrc); debug_log("[P251] sram backup: cannot create .bak\n"); return; }
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) fwrite(buf, 1, n, fdst);
    fclose(fsrc);
    fclose(fdst);
    debug_log("[P251] sram.dat backed up to sram.dat.pre_scsi.bak\n");
}

/* 内蔵SCSI(SPC MB89352)IPL ROM(SCSIINROM.DAT、8KB)をBridge静的バッファへ
 * ロードして保持するのみ(Stage1では$FC0000への実マッピングはしない—
 * Core側rm_ipl経由のIPL[]直読みのため、Stage2/4で対応)。欠落は非致命。
 * LE16スワップはStage2でのc68k fetch接続に備えて揃えておく(Stage Aの
 * SCSIEXROM.DATロードと同一パターン)。 */
void mx68k_set_scsi_in_rom_path(const char* path) {
    s_scsi_in_rom_loaded = false;
    memset(s_scsi_in_rom, 0, sizeof(s_scsi_in_rom));
    if (!path || !path[0]) return;
    FILE* fp = fopen(path, "rb");
    if (!fp) { debug_log("[MX68K] SCSIINROM: %s -> fopen failed\n", path); return; }
    uint8_t tmp[0x2000];
    size_t n = fread(tmp, 1, sizeof(tmp), fp);
    fclose(fp);
    if (n < 0x1FE0) { debug_log("[MX68K] SCSIINROM: short read %zu\n", n); return; }
    for (size_t i = 0; i + 1 < n; i += 2) {
        s_scsi_in_rom[i]     = tmp[i + 1];
        s_scsi_in_rom[i + 1] = tmp[i];
    }
    s_scsi_in_rom_loaded = true;
    debug_log("[MX68K] SCSIINROM: %s -> OK (%zu bytes, held only, not yet mapped)\n", path, n);
}

/* P253: 内蔵SCSI(SPC MB89352)HDディスクイメージのパスを保持。
 * scsi_real_install_construct()(scsi_spc_bridge.cpp)が mx68k_get_scsi_in_disk_path()
 * で読み取り、Reset()->Construct() 前に SetDiskPath() へ流す。id=SCSI ID 0..6。 */
static char s_scsi_in_disk_path[7][1024];   /* ID0-6全配線 */
void mx68k_set_scsi_in_disk_path(int id, const char* path) {
    if (id < 0 || id > 6) return;
    if (path) { strlcpy(s_scsi_in_disk_path[id], path, sizeof(s_scsi_in_disk_path[id])); }
    else s_scsi_in_disk_path[id][0] = '\0';
    /* P687 (D-29 C): 内蔵 SCSI は「設定を受け付けた」時点と「実際にオープン
     * された」時点がハードリセットを挟んで離れている(SCSISettingsView.swift
     * :436-438 に明記の既存設計、ライブ反映化はしない)。この 1 行があると、
     * 実オープン結果を出す [P687] scsi Open failed と対にして、どちらの段階で
     * 止まっているかをログだけで判別できる。 */
    debug_log("[P687] scsi_in_disk_path set: id=%d path=%s (applies after next hard reset)\n",
              id, s_scsi_in_disk_path[id]);
}
const char* mx68k_get_scsi_in_disk_path(int id) {
    if (id < 0 || id > 6) return "";
    return s_scsi_in_disk_path[id];
}

/* ====================================================================
 * P668: SCSI MO(ID5 固定スロット)のディスクパスを保持する Bridge シャドウ。
 * scsi_real_install_construct() が mx68k_get_mo_path() で読み、
 * SCSI::SetMOPath() -> Reset() -> Construct() 経由で実オープンされる。
 * ==================================================================== */
static char s_mo_path[1024];

/* XM6:vm/disk.cpp:2131-2187(SCSIMO::Open)が受理する4容量。完全一致のみ。
 * 検算: 248826*512 / 446325*512 / 1041500*512 / 310352*2048 = 各値。
 * ★BlankImageService.swift(Swift 側の空イメージ生成)と**バイト単位で一致**
 *   していなければならない。片方だけ変更してはならない。 */
#define P668_MO_128MB  0x0797F400L   /*  127,398,912 = 248,826   x  512 */
#define P668_MO_230MB  0x0D9EEA00L   /*  228,518,400 = 446,325   x  512 */
#define P668_MO_540MB  0x1FC8B800L   /*  533,248,000 = 1,041,500 x  512 */
#define P668_MO_640MB  0x25E28000L   /*  635,600,896 = 310,352   x 2048 */

static bool p668_mo_size_valid(long size) {
    return size == P668_MO_128MB || size == P668_MO_230MB
        || size == P668_MO_540MB || size == P668_MO_640MB;
}

int mx68k_mo_insert(const char* path) {
    if (!path) return -1;
    if (strlen(path) >= sizeof(s_mo_path)) return -1;
    /* P458 の SCSI HD ゲートと同型: シャドウへ書き込む**前**にサイズを検査する。
     * ★機種ゲート(P268 の g_wired_machine_type == 4 拒否)は付けない — MO は
     *   内蔵/外付けどちらの SPC 構成でも同じ ID5 スロットに載るため。 */
    long p668_size = p458_hdd_file_size(path);
    if (p668_size < 0) {
        debug_log("[P668] mo_insert refused: stat failed path=%s\n", path);
        return -3;
    }
    if (!p668_mo_size_valid(p668_size)) {
        debug_log("[P668] mo_insert refused: size=%ld is not 128/230/540/640MB path=%s\n",
                   p668_size, path);
        return -3;
    }
    /* P674: 失敗時に巻き戻せるよう、シャドウ更新前に旧パスを保持する。 */
    char prev[sizeof(s_mo_path)];
    strlcpy(prev, s_mo_path, sizeof(prev));
    strlcpy(s_mo_path, path, sizeof(s_mo_path));   /* シャドウ = 次回ハードリセット用 */
    int live = scsi_real_mo_open(path);
    debug_log("[P674] mx68k_mo_insert: live=%d path=%s size=%ld\n", live, path, p668_size);
    if (live == 0) return 0;                        /* 即時反映 */
    if (live == -1 || live == -2) return 1;          /* SPC未構築/MO未装着 = 次回⌘Rで反映 */
    strlcpy(s_mo_path, prev, sizeof(s_mo_path));     /* ロック/オープン失敗はシャドウも巻き戻す */
    return (live == -3) ? -5 : -4;
}

int mx68k_mo_eject(void) {
    int live = scsi_real_mo_eject(0);
    debug_log("[P674] mx68k_mo_eject: live=%d\n", live);
    if (live == -3) return -5;          /* ゲストロック中、シャドウは変更しない */
    s_mo_path[0] = '\0';                /* live==0(成功)/-1/-2(SPC未構築/MO未装着)いずれもシャドウは空にする */
    return (live == 0) ? 0 : 1;         /* 0=即時反映 / 1=シャドウのみ(次回⌘R) */
}

bool mx68k_mo_is_inserted(void) {
    return s_mo_path[0] != '\0';
}

/* pushConfig 用。mx68k_set_scsi_in_disk_path と同型でサイズ検証を行わない
 * (検証は mx68k_mo_insert 側の責務。config に残った実体無しパスは Swift 側で
 *  存在チェックしてから push される)。 */
void mx68k_set_mo_path(const char* path) {
    if (path) { strlcpy(s_mo_path, path, sizeof(s_mo_path)); }
    else s_mo_path[0] = '\0';
}

const char* mx68k_get_mo_path(void) {
    return s_mo_path;
}

/* ====================================================================
 * P676: SCSI CD-ROM(ID6 固定スロット)のイメージパスを保持する Bridge シャドウ。
 * scsi_real_install_construct() が mx68k_get_cd_path() で読み、
 * SCSI::SetCDPath() -> Reset() -> Construct() 経由で実オープンされる。
 * 構造は P668/P674 の MO と同一。相違点はサイズ検証だけで、MO の
 * 「4容量の完全一致」に対し CD は**範囲**判定になる。
 * ==================================================================== */
static char s_cd_path[1024];

/* XM6:vm/disk.cpp:2772-2858(SCSICD::OpenIso)が受理するサイズ条件。
 * RAW(MODE1/2352): サイズが 0x930 の倍数、かつ 912579600 以下
 *   検算: 912579600 / 0x930 = 388,000 blocks(約74分相当)
 * 非RAW(ISO 2048B/sector): サイズが 2048 の倍数、かつ 0x2bed5000 以下
 *   検算: 0x2bed5000 >> 11 = 359,705 blocks
 * どちらの経路も最低 0x800(1セクタ分)を要する(XM6:vm/disk.cpp:2788)。 */
#define P676_CD_RAW_SECTOR   0x930L        /* 2352 = 12(sync)+4(header)+2048+288(EDC/ECC) */
#define P676_CD_RAW_MAX      912579600L
#define P676_CD_ISO_SECTOR   2048L
#define P676_CD_ISO_MAX      0x2BED5000L   /* 736,675,840 */
#define P676_CD_MIN          0x800L        /* 2048 = 1 sector */

static bool p676_cd_size_valid(long size) {
    if (size < P676_CD_MIN) return false;
    if ((size % P676_CD_RAW_SECTOR) == 0 && size <= P676_CD_RAW_MAX) return true;
    if ((size % P676_CD_ISO_SECTOR) == 0 && size <= P676_CD_ISO_MAX) return true;
    return false;
}

/* ★Mode1 判定(header[3] == 0x01)はここでは行わない — ファイル内容の読み取りが
 * 必要であり、SCSICD::OpenIso が既に厳密に行うため二重実装は移植の忠実性を損なう。
 * ライブ挿入経路では scsi_real_cd_open が -4 を返すことで検出でき、その理由は
 * プローブ [P676-CD] openiso: の reason= / hdr3= から読み取れる。
 * シャドウのみの経路(SPC 未構築時)では次回ハードリセットの Construct で
 * Open 失敗となり、ドライブは装着されるがメディア無しとして振る舞う
 * (P668 の MO と同じ。リムーバブル機器として正しい)。 */
int mx68k_cd_insert(const char* path) {
    if (!path) return -1;
    if (strlen(path) >= sizeof(s_cd_path)) return -1;
    long p676_size = p458_hdd_file_size(path);
    if (p676_size < 0) {
        debug_log("[P676] cd_insert refused: stat failed path=%s\n", path);
        return -3;
    }
    if (!p676_cd_size_valid(p676_size)) {
        debug_log("[P676] cd_insert refused: size=%ld is neither a 2352B nor a 2048B "
                  "sector multiple within range path=%s\n", p676_size, path);
        return -3;
    }
    /* 失敗時に巻き戻せるよう、シャドウ更新前に旧パスを保持する(P674 と同型)。 */
    char prev[sizeof(s_cd_path)];
    strlcpy(prev, s_cd_path, sizeof(prev));
    strlcpy(s_cd_path, path, sizeof(s_cd_path));   /* シャドウ = 次回ハードリセット用 */
    int live = scsi_real_cd_open(path);
    debug_log("[P676] mx68k_cd_insert: live=%d path=%s size=%ld\n", live, path, p676_size);
    if (live == 0) return 0;                        /* 即時反映 */
    if (live == -1 || live == -2) return 1;          /* SPC未構築/CD未装着 = 次回⌘Rで反映 */
    strlcpy(s_cd_path, prev, sizeof(s_cd_path));     /* ロック/オープン失敗はシャドウも巻き戻す */
    return (live == -3) ? -5 : -4;
}

int mx68k_cd_eject(void) {
    int live = scsi_real_cd_eject(0);
    debug_log("[P676] mx68k_cd_eject: live=%d\n", live);
    if (live == -3) return -5;          /* ゲストロック中、シャドウは変更しない */
    s_cd_path[0] = '\0';                /* live==0(成功)/-1/-2(SPC未構築/CD未装着)いずれもシャドウは空にする */
    return (live == 0) ? 0 : 1;         /* 0=即時反映 / 1=シャドウのみ(次回⌘R) */
}

bool mx68k_cd_is_inserted(void) {
    return s_cd_path[0] != '\0';
}

/* pushConfig 用。mx68k_set_mo_path と同型でサイズ検証を行わない
 * (検証は mx68k_cd_insert 側の責務)。 */
void mx68k_set_cd_path(const char* path) {
    if (path) { strlcpy(s_cd_path, path, sizeof(s_cd_path)); }
    else s_cd_path[0] = '\0';
}

const char* mx68k_get_cd_path(void) {
    return s_cd_path;
}

/* P510: 外付け CZ-6BS1 のディスクイメージパス。内蔵と違い専用のシャドウ配列を
 * 持たず Config.SCSIEXHDImage[] がそのまま真実源(mx68k_scsi_insert/eject が
 * 直接読み書きする)なので、薄いラッパで足りる。id=SCSI ID 0..6。 */
const char* mx68k_get_scsi_ext_disk_path(int id) {
    if (id < 0 || id > 6) return "";
    return Config.SCSIEXHDImage[id];
}

/* P274: 内蔵SCSI(SPC, ID0-6全配線)がステータスバー上「マウント済み」と
 * 見なせるか。P273の二重ゲート(機種SCSI・ROM済)を満たし、
 * かつID0-6のいずれかにディスクパスが設定されている場合のみ true。 */
bool mx68k_scsi_in_disk_present(void) {
    if (g_wired_machine_type != 4 || !s_scsi_in_rom_loaded) return false;
    for (int id = 0; id < 7; id++) {
        if (s_scsi_in_disk_path[id][0] != '\0') return true;
    }
    return false;
}

/* P269: 外付けCZ-6BS1のいずれかのIDにディスクがマウントされていればtrue。
 * P268によりmachineType=SCSIでは常にfalse(mx68k_scsi_insertが拒否するため
 * Config.SCSIEXHDImageは空のまま)。 */
bool mx68k_scsi_ext_is_inserted(void) {
    for (int id = 0; id <= 6; id++) {
        if (mx68k_scsi_is_inserted(id)) return true;
    }
    return false;
}

/* P253: g_machine_type は static(内部リンケージ)のため、m68000_bridge.c の
 * fetch overlay ヘルパから値渡しで参照するための getter。4 == SCSI。 */
int mx68k_get_machine_type(void) {
    return g_machine_type;
}

/* P457 (D-34): 配線確定機種の getter。g_machine_type(設定直後の pending 値)と
 * 対になる読み取り専用アクセサで、mx68k_reset_hard() 内でのみ更新される
 * g_wired_machine_type を返す。4 == SCSI。設定 UI の有効/無効判定はこちらを
 * 使い、insert 側のゲート(P268)と基準を揃える。 */
int mx68k_get_wired_machine_type(void) {
    return g_wired_machine_type;
}

/* 内蔵SCSI(SUPER+ SPC)ディスクイメージパスの保持のみ(Stage1)。実I/Oは
 * Stage2でSPCロジックが実装されるまで機能しない。id=SCSI ID 0..6。 */
int mx68k_scsi_in_insert(int id, const char* path) {
    if (id < 0 || id > 6) return -1;
    if (!path) return -1;
    if (strlen(path) >= sizeof(s_scsi_in_image[id])) return -1;
    strncpy(s_scsi_in_image[id], path, sizeof(s_scsi_in_image[id]) - 1);
    s_scsi_in_image[id][sizeof(s_scsi_in_image[id]) - 1] = '\0';
    debug_log("[MX68K] mx68k_scsi_in_insert: id=%d path=%s (held only, Stage2 pending)\n", id, path);
    return 0;
}
void mx68k_scsi_in_eject(int id) {
    if (id < 0 || id > 6) return;
    s_scsi_in_image[id][0] = '\0';
}
bool mx68k_scsi_in_is_inserted(int id) {
    if (id < 0 || id > 6) return false;
    return s_scsi_in_image[id][0] != '\0';
}

/* P204: guest SRAM $ED0029 (XEiJ SRAM_EJECT) bit0 = eject FD at power-off.
   SRAM[] is byte-swapped (adr^1) so guest $ED0029 -> SRAM[0x28]. */
bool mx68k_sram_eject_on_poweroff(void) { return (SRAM[0x28] & 0x01) != 0; }

/* ======================================================================
 * P198: State save / load (Phase 2 #4)
 *
 * A versioned snapshot of the emulated machine's guest-visible state.
 * Best-effort V1 (no reference implementation): fmgen/ADPCM internal DSP
 * state and FDC sector phase are NOT captured (guest re-writes re-sync them).
 *
 * Non-static globals that lack a header declaration (defined in bg.c). Verified
 * non-static in Core/px68k/x68k/bg.c:13-14 (full memcpy, both directions).
 * ==================================================================== */
extern uint8_t BG[0x8000];
extern uint8_t Sprite_Regs[0x800];

#define MX68K_STATE_MAGIC   "MX68KSAV"       /* 8 bytes, no NUL */
#define MX68K_STATE_TRAILER 0x4B383653u       /* "S68K" trailer sentinel */
/* P479 (D-41症状1): 1u -> 2u. The OPM shadow block is a new, mandatory block in
 * the fixed block order, so the file layout changed. Bumping the version makes
 * a pre-P479 file fail with the explicit version check (rc=-11) instead of
 * mis-parsing into the trailer/size checks with a confusing error. Pre-P479
 * .mxstate files can no longer be loaded — by design. */
#define MX68K_STATE_VERSION 2u

/* Shared field visitor: identical field order/size for save and load so the
 * two directions can never drift. save=1 packs emulator->buf; save=0 unpacks
 * buf->emulator. Returns the byte length of the block (structural, value-
 * independent — so it also serves as the "measure" for load-side validation). */
#define STATE_FIELD(v)      do { if (buf) { if (save) memcpy(buf + off, &(v), sizeof(v)); \
                                            else       memcpy(&(v), buf + off, sizeof(v)); } \
                                  off += (uint32_t)sizeof(v); } while (0)
#define STATE_FIELDN(p, n)  do { if (buf) { if (save) memcpy(buf + off, (p), (n)); \
                                            else       memcpy((p), buf + off, (n)); } \
                                  off += (uint32_t)(n); } while (0)

/* --- composite blocks (all-memcpy fields; CPU is handled separately) --- */

static uint32_t state_crtc_block(uint8_t* buf, int save) {
    uint32_t off = 0;
    STATE_FIELDN(CRTC_Regs, 48);
    STATE_FIELD(CRTC_Mode);
    STATE_FIELD(CRTC_VSTART); STATE_FIELD(CRTC_VEND);
    STATE_FIELD(CRTC_HSTART); STATE_FIELD(CRTC_HEND);
    STATE_FIELD(TextDotX);    STATE_FIELD(TextDotY);
    STATE_FIELD(TextScrollX); STATE_FIELD(TextScrollY);
    STATE_FIELDN(VCReg0, 2);  STATE_FIELDN(VCReg1, 2);  STATE_FIELDN(VCReg2, 2);
    STATE_FIELD(CRTC_IntLine);
    STATE_FIELD(CRTC_FastClr);
    /* CRTC_DispScan dropped: not linked into MX build + derived display-scan
     * value recomputed each frame (safe to omit from the snapshot). */
    STATE_FIELD(CRTC_FastClrLine);
    STATE_FIELD(CRTC_FastClrMask);
    STATE_FIELD(CRTC_VStep);
    STATE_FIELD(HSYNC_CLK);
    STATE_FIELDN(GrphScrollX, 4 * sizeof(uint32_t));
    STATE_FIELDN(GrphScrollY, 4 * sizeof(uint32_t));
    return off;
}

static uint32_t state_pal_block(uint8_t* buf, int save) {
    uint32_t off = 0;
    STATE_FIELDN(Pal_Regs, 1024);
    STATE_FIELD(Contrast_Value);
    return off;
}

static uint32_t state_mfp_block(uint8_t* buf, int save) {
    uint32_t off = 0;
    STATE_FIELDN(MFP, 24);
    /* Timer_Count dropped: not linked into MX build + MFP timer counters
     * reload from their data registers after load (acceptable best-effort). */
    STATE_FIELD(LastKey);
    STATE_FIELD(keyLED);
    STATE_FIELD(keyREP_DELAY);
    STATE_FIELD(keyREP_TIME);
    return off;
}

static uint32_t state_ioc_block(uint8_t* buf, int save) {
    uint32_t off = 0;
    STATE_FIELD(IOC_IntStat);
    STATE_FIELD(IOC_IntVect);
    STATE_FIELDN(SysPort, 7);
    STATE_FIELD(MouseX); STATE_FIELD(MouseY); STATE_FIELD(MouseSt);
    STATE_FIELD(ADPCM_Clock);
    STATE_FIELD(ADPCM_ClockRate);
    return off;
}

/* P479 (D-41症状1): the Bridge-side OPM register shadow (see Bridge/opm_shadow.h).
 * The OPM chip exposes no register read-back, so this shadow — recorded from the
 * CPU write hook — is the only available snapshot of the tone parameters. Block
 * size is 256+256+5 = 517 B, well within the 2048 B scratch buffer. */
static uint32_t state_opm_block(uint8_t* buf, int save) {
    uint32_t off = 0;
    STATE_FIELDN(g_opm_shadow, 256);
    STATE_FIELDN(g_opm_written, 256);
    STATE_FIELD(g_opm_reg19_pmd);         STATE_FIELD(g_opm_reg19_amd);
    STATE_FIELD(g_opm_reg19_pmd_written); STATE_FIELD(g_opm_reg19_amd_written);
    STATE_FIELD(g_opm_curreg);
    return off;
}

static uint32_t state_bgregs_block(uint8_t* buf, int save) {
    uint32_t off = 0;
    STATE_FIELDN(BG_Regs, 0x12);
    STATE_FIELD(BG0ScrollX); STATE_FIELD(BG0ScrollY);
    STATE_FIELD(BG1ScrollX); STATE_FIELD(BG1ScrollY);
    STATE_FIELD(BG_AdrMask);
    STATE_FIELD(BG_CHRSIZE);
    STATE_FIELD(BG_BG0TOP); STATE_FIELD(BG_BG1TOP);
    STATE_FIELD(BG_HAdjust);
    STATE_FIELD(BG_VLINE);
    STATE_FIELD(VLINEBG);
    STATE_FIELD(Mcry_LRTiming);
    return off;
}

static uint32_t state_timing_block(uint8_t* buf, int save) {
    uint32_t off = 0;
    STATE_FIELD(HLINE_TOTAL);
    STATE_FIELD(VLINE_TOTAL);
    STATE_FIELD(VLINE);
    STATE_FIELD(x68k_vline);
    STATE_FIELDN(IRQH_IRQ, 8);
    STATE_FIELD(g_mx68k_frame_num);
    return off;
}

/* CPU block: 21 x uint32 = 84 bytes, fixed order.
 * D0-D7, A0-A7, SR, USP, PC, C68K.Status, C68K.IRQLine. */
#define STATE_CPU_BYTES (21u * 4u)

/* P198: TextDrawWork is a DERIVED text-plane buffer rebuilt write-through inside
 * TVRAM_Write(); the bulk TVRAM memcpy on load bypasses that rebuild, so the text
 * plane must be serialized directly to avoid stale content after load. */
#define STATE_TDW_BYTES (1024u * 1024u + 100u)

/* P198: BGCHR8/BGCHR16 are DERIVED, decoded PCG pattern buffers, rebuilt
 * write-through inside BG_Write() (bg.c:262-267) from the raw BG[] pattern RAM.
 * The bulk BG[] memcpy on load bypasses that rebuild, so — exactly like
 * TextDrawWork above — the decoded sprite/BG patterns must be serialized directly
 * or sprite rendering shows stale patterns until the guest next rewrites PCG.
 * These globals have external linkage but are not declared in bg.h, so declare
 * them here (Core is not modified). Sizes match bg.c: 8*8*256 / 16*16*256. */
extern uint8_t BGCHR8[8 * 8 * 256];
extern uint8_t BGCHR16[16 * 16 * 256];
#define STATE_BGCHR8_BYTES  (8u * 8u * 256u)     /* 16384 */
#define STATE_BGCHR16_BYTES (16u * 16u * 256u)   /* 65536 */

static void state_cpu_save(uint8_t* buf) {
    uint32_t w[21]; int i = 0;
    for (int r = M68K_D0; r <= M68K_D7; r++) w[i++] = m68000_get_reg(r);
    for (int r = M68K_A0; r <= M68K_A7; r++) w[i++] = m68000_get_reg(r);
    w[i++] = m68000_get_reg(M68K_SR);
    w[i++] = m68000_get_reg(M68K_USP);
    w[i++] = m68000_get_reg(M68K_PC);
    w[i++] = (uint32_t)C68K.Status;
    w[i++] = (uint32_t)C68K.IRQLine;
    memcpy(buf, w, STATE_CPU_BYTES);
}

static void state_cpu_load(const uint8_t* buf) {
    uint32_t w[21];
    memcpy(w, buf, STATE_CPU_BYTES);
    /* Order is load-bearing (C68k_Set_USP branches on flag_S):
     * D0-D7, A0-A6, SR, then A7 + USP, raw Status/IRQLine, PC LAST. */
    int i = 0;
    for (int r = M68K_D0; r <= M68K_D7; r++) m68000_set_reg(r, w[i++]);
    /* A0-A6 now, A7 after SR */
    for (int r = M68K_A0; r <= M68K_A6; r++) m68000_set_reg(r, w[i++]);
    uint32_t a7  = w[i++];
    uint32_t sr  = w[i++];
    uint32_t usp = w[i++];
    uint32_t pc  = w[i++];
    uint32_t status  = w[i++];
    int32_t  irqline = (int32_t)w[i++];
    m68000_set_reg(M68K_SR, sr);
    m68000_set_reg(M68K_A7, a7);
    m68000_set_reg(M68K_USP, usp);
    C68K.Status  = status;
    C68K.IRQLine = irqline;
    /* derived fetch/basepc rebuilt from PC below via set_reg(PC) + cpu_setOPbase24 */
    m68000_set_reg(M68K_PC, pc);
}

/* size-tagged block writer */
static int state_wblk(FILE* f, const void* p, uint32_t n) {
    if (fwrite(&n, 4, 1, f) != 1) return -1;
    if (n && fwrite(p, 1, n, f) != n) return -1;
    return 0;
}

/* write a scalar (helper for header fields) */
static int state_w32(FILE* f, uint32_t v) { return fwrite(&v, 4, 1, f) == 1 ? 0 : -1; }

static int do_save_state(const char* path) {
    if (!path || !MEM) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -2;

    int rc = 0;
    uint8_t scratch[2048];   /* must fit the largest composite block
                              * (palette = 1025 B; P479 OPM shadow = 517 B) */
    /* P472: size the RAM block from the physical allocation, not from
     * g_memory_size_mb — MEM is always 12 MB, so a smaller setting used to make
     * the snapshot capture only the leading part of guest RAM (D-11). */
    uint32_t mem_bytes = MX68K_RAM_BYTES;

    /* header */
    if (fwrite(MX68K_STATE_MAGIC, 1, 8, f) != 8) { rc = -3; goto done; }
    if (state_w32(f, MX68K_STATE_VERSION))       { rc = -3; goto done; }
    if (state_w32(f, 0u /*flags*/))              { rc = -3; goto done; }

    /* config header */
    if (state_w32(f, (uint32_t)g_machine_type))  { rc = -3; goto done; }
    if (state_w32(f, (uint32_t)g_memory_size_mb)){ rc = -3; goto done; }
    if (state_w32(f, (uint32_t)g_clock_mhz))     { rc = -3; goto done; }
    if (state_w32(f, (uint32_t)(g_fpu_enabled ? 1 : 0))) { rc = -3; goto done; }
    for (int d = 0; d < 2; d++) {
        uint32_t len = (uint32_t)strlen(g_fdd_path[d]);
        if (state_w32(f, len)) { rc = -3; goto done; }
        if (len && fwrite(g_fdd_path[d], 1, len, f) != len) { rc = -3; goto done; }
    }

    /* blocks (fixed order) */
    /* 1. CPU */
    state_cpu_save(scratch);
    if (state_wblk(f, scratch, STATE_CPU_BYTES)) { rc = -4; goto done; }
    /* 2. MEM */
    if (state_wblk(f, MEM, mem_bytes))           { rc = -4; goto done; }
    /* 3. TVRAM / 3b. TextDrawWork (derived text plane) / 4. GVRAM / 5. SRAM */
    if (state_wblk(f, TVRAM, 0x80000))           { rc = -4; goto done; }
    if (state_wblk(f, TextDrawWork, STATE_TDW_BYTES)) { rc = -4; goto done; }
    if (state_wblk(f, GVRAM, 0x80000))           { rc = -4; goto done; }
    /* P495: SRAM は常に物理最大の 64KB を保存する(D-11/P472 と同型)。64KB 設定の
     * 有効/無効に関わらず、上位 48KB($ED4000-$EDFFFF)の現在値もあわせて保存する。
     * ブロックは長さ付き自己記述式のため、旧 16KB 形式のステートは読込み側の
     * ブロック単位分岐だけで引き続き読める(MX68K_STATE_VERSION は据え置き)。 */
    if (state_wblk(f, sram_ext_snapshot64(), 0x10000)) { rc = -4; goto done; }
    /* 6. BG RAM / 7. Sprite regs / 7b. decoded PCG pattern buffers */
    if (state_wblk(f, BG, 0x8000))               { rc = -4; goto done; }
    if (state_wblk(f, Sprite_Regs, 0x800))       { rc = -4; goto done; }
    if (state_wblk(f, BGCHR8, STATE_BGCHR8_BYTES))   { rc = -4; goto done; }
    if (state_wblk(f, BGCHR16, STATE_BGCHR16_BYTES)) { rc = -4; goto done; }
    /* 8. CRTC / 9. Palette / 10. MFP / 11. DMAC / 12. IOC misc / 13. BG regs / 14. timing
     * / 15. OPM shadow (P479) */
    {
        uint32_t n;
        n = state_crtc_block(scratch, 1);   if (state_wblk(f, scratch, n)) { rc = -4; goto done; }
        n = state_pal_block(scratch, 1);    if (state_wblk(f, scratch, n)) { rc = -4; goto done; }
        n = state_mfp_block(scratch, 1);    if (state_wblk(f, scratch, n)) { rc = -4; goto done; }
        if (state_wblk(f, DMA, (uint32_t)sizeof(DMA))) { rc = -4; goto done; }  /* dmac_ch[4] */
        n = state_ioc_block(scratch, 1);    if (state_wblk(f, scratch, n)) { rc = -4; goto done; }
        n = state_bgregs_block(scratch, 1); if (state_wblk(f, scratch, n)) { rc = -4; goto done; }
        n = state_timing_block(scratch, 1); if (state_wblk(f, scratch, n)) { rc = -4; goto done; }
        n = state_opm_block(scratch, 1);    if (state_wblk(f, scratch, n)) { rc = -4; goto done; }
    }

    /* trailer */
    if (state_w32(f, MX68K_STATE_TRAILER)) { rc = -5; goto done; }

done:
    fclose(f);
    if (rc != 0) remove(path);   /* do not leave a truncated snapshot */
    return rc;
}

/* Load cursor over the in-memory file buffer. */
typedef struct { const uint8_t* base; size_t len, pos; int err; } state_rdcur;

/* read one size-tagged block; validates the tag fits the buffer (no mutation). */
static const uint8_t* state_rblk(state_rdcur* c, uint32_t* outn) {
    if (c->err) return NULL;
    if (c->pos + 4 > c->len) { c->err = 1; return NULL; }
    uint32_t n; memcpy(&n, c->base + c->pos, 4); c->pos += 4;
    if (c->pos + (size_t)n > c->len) { c->err = 1; return NULL; }
    const uint8_t* d = c->base + c->pos; c->pos += n; *outn = n;
    return d;
}

static int do_load_state(const char* path) {
    if (!path || !MEM) return -1;

    /* All locals declared up front so the early-exit gotos never jump over a
     * declaration (keeps strict C / -Werror happy). */
    FILE* f = NULL;
    long fsz = 0;
    uint8_t* fb = NULL;
    int rc = 0;
    state_rdcur c;
    uint32_t version = 0, flags = 0;
    int32_t s_machine = 0, s_mem = 0, s_clock = 0, s_fpu = 0;
    char s_fdd[2][4096];
    uint32_t exp_mem = 0;
    uint32_t trailer = 0;
    const uint8_t *blk_cpu = NULL, *blk_mem = NULL, *blk_tvram = NULL, *blk_tdw = NULL,
                  *blk_gvram = NULL, *blk_sram = NULL, *blk_bg = NULL, *blk_spr = NULL,
                  *blk_bgchr8 = NULL, *blk_bgchr16 = NULL,
                  *blk_crtc = NULL, *blk_pal = NULL, *blk_mfp = NULL, *blk_dma = NULL,
                  *blk_ioc = NULL, *blk_bgr = NULL, *blk_tim = NULL, *blk_opm = NULL;
    uint32_t n_cpu = 0, n_mem = 0, n_tvram = 0, n_tdw = 0, n_gvram = 0, n_sram = 0,
             n_bg = 0, n_spr = 0, n_bgchr8 = 0, n_bgchr16 = 0, n_crtc = 0, n_pal = 0,
             n_mfp = 0, n_dma = 0, n_ioc = 0, n_bgr = 0, n_tim = 0, n_opm = 0;

    /* --- read whole file into memory --- */
    f = fopen(path, "rb");
    if (!f) return -2;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
    fsz = ftell(f);
    if (fsz <= 0 || fsz > (long)(64 * 1024 * 1024)) { fclose(f); return -2; }
    rewind(f);
    fb = (uint8_t*)malloc((size_t)fsz);
    if (!fb) { fclose(f); return -2; }
    if (fread(fb, 1, (size_t)fsz, f) != (size_t)fsz) { free(fb); fclose(f); return -2; }
    fclose(f);

    c.base = fb; c.len = (size_t)fsz; c.pos = 0; c.err = 0;

    /* --- PASS 1: parse + validate everything, NO emulator mutation --- */
    if (c.len < 16 || memcmp(c.base, MX68K_STATE_MAGIC, 8) != 0) { rc = -10; goto out; }
    c.pos = 8;
    memcpy(&version, c.base + c.pos, 4); c.pos += 4;
    memcpy(&flags,   c.base + c.pos, 4); c.pos += 4;
    (void)flags;
    if (version != MX68K_STATE_VERSION) { rc = -11; goto out; }

    /* config header */
    if (c.pos + 16 > c.len) { rc = -10; goto out; }
    memcpy(&s_machine, c.base + c.pos, 4); c.pos += 4;
    memcpy(&s_mem,     c.base + c.pos, 4); c.pos += 4;
    memcpy(&s_clock,   c.base + c.pos, 4); c.pos += 4;
    memcpy(&s_fpu,     c.base + c.pos, 4); c.pos += 4;
    /* P481 (D-42): -15 (not -11) — rc=-11 is reserved for "state file written by
     * an older, incompatible MX68K version" so the UI can say exactly that; an
     * out-of-range memory size is an unrelated condition and needs its own code. */
    if (s_mem <= 0 || s_mem > 12) { rc = -15; goto out; }
    for (int d = 0; d < 2; d++) {
        if (c.pos + 4 > c.len) { rc = -10; goto out; }
        uint32_t plen; memcpy(&plen, c.base + c.pos, 4); c.pos += 4;
        if (plen >= sizeof(s_fdd[d]) || c.pos + plen > c.len) { rc = -10; goto out; }
        memcpy(s_fdd[d], c.base + c.pos, plen); s_fdd[d][plen] = '\0'; c.pos += plen;
    }

    /* block index (ptr+len), captured during validation */
    blk_cpu   = state_rblk(&c, &n_cpu);
    blk_mem   = state_rblk(&c, &n_mem);
    blk_tvram = state_rblk(&c, &n_tvram);
    blk_tdw   = state_rblk(&c, &n_tdw);
    blk_gvram = state_rblk(&c, &n_gvram);
    blk_sram  = state_rblk(&c, &n_sram);
    blk_bg    = state_rblk(&c, &n_bg);
    blk_spr   = state_rblk(&c, &n_spr);
    blk_bgchr8  = state_rblk(&c, &n_bgchr8);
    blk_bgchr16 = state_rblk(&c, &n_bgchr16);
    blk_crtc  = state_rblk(&c, &n_crtc);
    blk_pal   = state_rblk(&c, &n_pal);
    blk_mfp   = state_rblk(&c, &n_mfp);
    blk_dma   = state_rblk(&c, &n_dma);
    blk_ioc   = state_rblk(&c, &n_ioc);
    blk_bgr   = state_rblk(&c, &n_bgr);
    blk_tim   = state_rblk(&c, &n_tim);
    blk_opm   = state_rblk(&c, &n_opm);    /* P479 */
    if (c.err) { rc = -12; goto out; }

    /* trailer */
    if (c.pos + 4 > c.len) { rc = -12; goto out; }
    memcpy(&trailer, c.base + c.pos, 4); c.pos += 4;
    if (trailer != MX68K_STATE_TRAILER) { rc = -13; goto out; }

    /* exact per-block size validation (structural; measured from save direction) */
    /* P472: expect the full physical RAM size, not the saved g_memory_size_mb —
     * MEM is always 12 MB, so a smaller value used to leave the remainder of
     * guest RAM unrestored after a load (D-11). Pre-P472 state files saved with
     * a <12 MB setting now fail the n_mem check below (rc=-14) instead of
     * silently restoring only part of RAM. */
    exp_mem = MX68K_RAM_BYTES;
    if (n_cpu   != STATE_CPU_BYTES)          { rc = -14; goto out; }
    if (n_mem   != exp_mem)                  { rc = -14; goto out; }
    if (n_tvram != 0x80000)                  { rc = -14; goto out; }
    if (n_tdw   != STATE_TDW_BYTES)          { rc = -14; goto out; }
    if (n_gvram != 0x80000)                  { rc = -14; goto out; }
    /* P495: 16KB 形式(P495 以前に保存されたステート)と 64KB 形式(P495 以後)の
     * 両方を受理する。バージョン番号は上げない——ブロックが長さ付き自己記述式
     * なので、このブロック単位の分岐だけで後方互換が成立する。 */
    if (n_sram != 0x4000 && n_sram != 0x10000) { rc = -14; goto out; }
    if (n_bg    != 0x8000)                   { rc = -14; goto out; }
    if (n_spr   != 0x800)                    { rc = -14; goto out; }
    if (n_bgchr8  != STATE_BGCHR8_BYTES)     { rc = -14; goto out; }
    if (n_bgchr16 != STATE_BGCHR16_BYTES)    { rc = -14; goto out; }
    if (n_crtc  != state_crtc_block(NULL, 1)) { rc = -14; goto out; }
    if (n_pal   != state_pal_block(NULL, 1))  { rc = -14; goto out; }
    if (n_mfp   != state_mfp_block(NULL, 1))  { rc = -14; goto out; }
    if (n_dma   != (uint32_t)sizeof(DMA))     { rc = -14; goto out; }
    if (n_ioc   != state_ioc_block(NULL, 1))  { rc = -14; goto out; }
    if (n_bgr   != state_bgregs_block(NULL, 1)) { rc = -14; goto out; }
    if (n_tim   != state_timing_block(NULL, 1)) { rc = -14; goto out; }
    if (n_opm   != state_opm_block(NULL, 1))  { rc = -14; goto out; }

    /* --- PASS 2: apply (structure fully validated) --- */
    /* config mismatch -> reconfigure + hard reset before restoring RAM/regs */
    if (s_machine != g_machine_type || s_mem != g_memory_size_mb ||
        s_clock != g_clock_mhz || (s_fpu != 0) != g_fpu_enabled) {
        mx68k_set_machine_type(s_machine);
        mx68k_set_memory_size(s_mem);
        mx68k_set_clock(s_clock);
        mx68k_set_fpu_enabled(s_fpu != 0);
        mx68k_reset_hard();
    }

    /* bulk RAM/regs (memcpy) */
    memcpy(MEM,         blk_mem,   exp_mem);
    memcpy(TVRAM,       blk_tvram, 0x80000);
    memcpy(TextDrawWork, blk_tdw,  STATE_TDW_BYTES);
    memcpy(GVRAM,       blk_gvram, 0x80000);
    memcpy(SRAM,        blk_sram,  0x4000);   /* 低位 16KB は形式に関わらず常にここにある */
    /* P495: 上位 48KB。順序厳守——sram_ext_* は Fetch シャドウ再構築時に SRAM[] を
     * 読むため、低位 16KB の memcpy が先に完了している必要がある。 */
    if (n_sram == 0x10000) {
        /* 64KB 形式: 上位 48KB もステートに含まれている */
        sram_ext_restore_upper48(blk_sram + 0x4000);
    } else {
        /* 16KB 形式(P495 以前に保存された旧ステート): 上位 48KB の記録が無いため
         * ゼロクリアして確定的な状態にする(P494 の既存関数を再利用)。 */
        sram_ext_clear();
    }
    memcpy(BG,          blk_bg,    0x8000);
    memcpy(Sprite_Regs, blk_spr,   0x800);
    memcpy(BGCHR8,      blk_bgchr8,  STATE_BGCHR8_BYTES);
    memcpy(BGCHR16,     blk_bgchr16, STATE_BGCHR16_BYTES);
    memcpy(DMA,         blk_dma,   sizeof(DMA));
    state_crtc_block((uint8_t*)blk_crtc, 0);
    state_pal_block((uint8_t*)blk_pal, 0);
    state_mfp_block((uint8_t*)blk_mfp, 0);
    state_ioc_block((uint8_t*)blk_ioc, 0);
    state_bgregs_block((uint8_t*)blk_bgr, 0);
    state_timing_block((uint8_t*)blk_tim, 0);
    /* P479: restores the Bridge-side shadow only; the replay into the chip
     * happens further below, after the P474 key-off loop. */
    state_opm_block((uint8_t*)blk_opm, 0);

    /* derived-state rebuild (before CPU regs, per plan). P473: this must call
     * Pal32_ChangeContrast() directly, NOT Pal_TrackContrast().
     * Pal_TrackContrast() (palette.c:160) starts with the guard
     * "if (SysPort[1] == Contrast_Value) return;" -- and immediately after a
     * state load those two are always equal, because SysPort[1] (restored by
     * state_ioc_block) and Contrast_Value (restored by state_pal_block) both
     * come from the same save-time snapshot. So the guard always fires and the
     * rebuild never runs. Pal32_ChangeContrast() (palette.c:174) skips that
     * guard and unconditionally rebuilds Pal32/GrphPal32/TextPal32 from the
     * just-restored Pal_Regs -- the one-shot rebuild a state load needs, rather
     * than the gradual per-frame CRT-like convergence Pal_TrackContrast() is
     * designed for. */
    Pal32_ChangeContrast(Contrast_Value);
    TVRAM_SetAllDirty();
    memset(s_compose_fb, 0, sizeof(s_compose_fb));   /* P535: ロード前フレームの残像を持ち越さない */

    /* P474 (D-38): force the sound chips silent on state load.
     *
     * WHY: none of the 17 blocks written by do_save_state() carries OPM
     * (YM2151) or ADPCM chip state -- the only audio-adjacent fields saved are
     * ADPCM_Clock/ADPCM_ClockRate inside state_ioc_block(), which are the chip
     * clock divider, not playback state. And nothing in this function resets
     * the sound chips either (ADPCM_Init()/OPM reset only run on the
     * config-mismatch path above, via mx68k_reset_hard()). So a note or sample
     * that was sounding at load time keeps sounding afterwards: the guest music
     * driver we just restored holds the internal state of the (silent) save
     * point, so it has no reason to emit the key-off that would stop it.
     *
     * OPM: write the KEYON register (0x08) once per channel with the operator
     * slot mask (data>>3) left at 0, which key-offs all 4 operators of that
     * channel (opm.cpp:183-185, ch[data&7].KeyControl(data>>3)).
     *
     * ** CALLING CONVENTION (corrected 2026-07-31 after the first P474 attempt
     * measurably did nothing): the first argument of
     * OPM_Write(uint32_t adr, uint8_t data) is NOT a register number -- it is
     * the X68000 I/O bus port select (0 = address latch, 1 = data write).
     * OPM_Write() (fmg_wrap.cpp:141-143) is only a thin C wrapper forwarding
     * to MyOPM::WriteIO() (fmg_wrap.cpp:58-68), which is the two-stage latch:
     * "if (adr&1) { ...; SetReg(CurReg, data); } else { CurReg = data; }".
     * The guest write path uses the same two-stage sequence: mem_wrap.c:345
     * maps $E90001 (addr&3==1) to OPM_Write(0, val) and :347 maps $E90003
     * (addr&3==3) to OPM_Write(1, val).
     * The first attempt wrote OPM_Write(0x08, ch); since 0x08&1 == 0 it fell
     * into the address-latch branch every time, SetReg() was never called and
     * no key-off was ever issued. The correct form is the pair below.
     *
     * Known minor side effect (accepted): after this loop, the Core-internal
     * CurReg (private, not restorable from the Bridge) is left pointing at
     * 0x08 -- CurReg is only updated by an address-latch write (adr=0), and
     * the channel number appears only in the data write (adr=1), so what
     * remains is always register 0x08, never "the last channel number". If a
     * load interrupts the guest mid-sequence (address latched, data write
     * pending), that pending data write would land on register 0x08
     * (KEYON/KEYOFF) instead of its intended register. The timing window is
     * extremely narrow and the consequence is a single stray key event, so
     * this is accepted rather than invoking the narrow Core-modification
     * exception (recorded as a known limitation in Docs/09).
     *
     * OPM_Reset() is deliberately NOT used: it would also clear the timer A/B
     * settings, and many X68000 music drivers use the OPM timer interrupt as
     * their tempo source -- wiping it would be a worse regression than the bug
     * (music that never resumes after a load). Key-off alone silences the
     * voices while preserving tone, timer and all other register state.
     *
     * ADPCM: ADPCM_Init(g_audio_sample_rate_hz) is the exact call already used at mx68k_init()
     * and mx68k_reset_hard() (existing pattern, not a new one). It clears
     * ADPCM_Playing, the buffer pointers and the interpolation history, and
     * does NOT touch ADPCM_ClockRate/ADPCM_Clock -- those are separate
     * variables (adpcm.c:39-40 vs the assignment at :347) and were already
     * restored moments ago by the state_ioc_block() call above. */
    for (int ch = 0; ch < 8; ch++) {
        OPM_Write(0, 0x08);              /* addr latch: KEYON reg */
        OPM_Write(1, (uint8_t)ch);       /* data: slot mask=0 -> KeyOff all 4 operators of this channel */
    }

    /* P479 (D-41症状1): replay the shadowed OPM tone registers so the chip
     * matches what was actually saved, instead of whatever it drifted to
     * between save and load. KEYON (0x08) is excluded -- the KEYOFF loop
     * above already silences all channels; replaying 0x08 would re-trigger
     * notes. Registers 0x01/0x14 are replayed with specific bits masked off
     * to avoid one-shot side effects (LFO phase reset / timer IRQ-flag
     * clear) that would not have happened at the actual save moment.
     * Register 0x1B (CT/W, LFO waveform) is deliberately EXCLUDED -- writing
     * it unconditionally also fires FDC_SetForceReady() via fmg_wrap.cpp's
     * WriteIO (CurReg==0x1b branch), which would stomp the FDC's live ready
     * flag with a stale save-time bit unrelated to the actual FDD state.
     * The tone-quality cost (LFO waveform not restored across a load) is
     * accepted as a known minor limitation to avoid that risk (Code Review
     * finding, see Docs/09 D-41). */
    {
        static const uint8_t kOpmReplayOrder[] = {
            0x01, 0x0F, 0x10, 0x11, 0x12, 0x14, 0x18, 0x19,
            0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
            0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
            0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
            0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f
            /* 0x40-0xFF (TL/AR/DR/SR/RR) added by range loop below.
             * 0x1B intentionally omitted -- see comment above. */
        };
        for (size_t i = 0; i < sizeof(kOpmReplayOrder); i++) {
            uint8_t reg = kOpmReplayOrder[i];
            if (reg == 0x19) continue;   /* handled separately below (2 slots) */
            if (!g_opm_written[reg]) continue;
            uint8_t data = g_opm_shadow[reg];
            if (reg == 0x01) data &= (uint8_t)~0x02;
            if (reg == 0x14) data &= (uint8_t)~0x30;
            OPM_Write(0, reg);
            OPM_Write(1, data);
        }
        if (g_opm_reg19_pmd_written) { OPM_Write(0, 0x19); OPM_Write(1, (uint8_t)(g_opm_reg19_pmd | 0x80)); }
        if (g_opm_reg19_amd_written) { OPM_Write(0, 0x19); OPM_Write(1, (uint8_t)(g_opm_reg19_amd & 0x7f)); }
        for (int reg = 0x40; reg <= 0xFF; reg++) {
            if (!g_opm_written[reg]) continue;
            OPM_Write(0, (uint8_t)reg);
            OPM_Write(1, g_opm_shadow[reg]);
        }
        /* restore the two-stage latch itself last -- also fixes the P474
         * known limitation where CurReg was left stuck at 0x08 after the
         * KEYOFF loop (see the P474 comment above). Note: if the saved
         * CurReg happened to be 0x1B, this final write still only latches
         * the address (adr=0), it does not perform the data write that
         * triggers WriteIO's CurReg==0x1b side effects -- safe. */
        OPM_Write(0, g_opm_curreg);
    }

    ADPCM_Init(g_audio_sample_rate_hz);   /* P626: 設定サンプルレート(既定 44100) */
    /* P477 (D-41症状2): ADPCM_Init() は内部で ADPCM_SetPan(0x0b) を呼び、
     * L/R 両方の出力を強制ミュートする(adpcm.c:355、Pan bit0/bit1=ミュート
     * フラグ、0x0b=両方セット)。復帰はゲストが PPI PortC 下位4bitを直前と
     * 異なる値に書き込んだ時だけ発火するが、ロードでは ppi.PortC 自体が
     * 巻き戻らない(そもそもセーブ対象外)ため、ゲストのドライバは「パンは
     * 既に設定済み」と認識し再送しない可能性が高く、消音のまま残る
     * (D-41症状2)。実際の(ミュートされていない)Panは PPI_Read() で
     * 読み出せる現在の ppi.PortC 下位4bitに反映されているため、それを
     * 使って ADPCM_SetPan を再送し、強制ミュートを解除する。 */
    ADPCM_SetPan(PPI_Read(0xE9A005) & 0x0f);
    /* 上の ADPCM_SetPan は Pan の上位2bit(クロック選択)が変化した場合、
     * ADPCM_Clock/ADPCM_ClockRate も書き換える(adpcm.c:313-321) —
     * state_ioc_block() による復元値(4714行で復元済み)を上書きして
     * しまうため、同じバッファで再適用し復元値を再度確定させる。
     * state_ioc_block(buf, save=0) は読み出し専用の副作用なしヘルパで
     * あり、同一バッファに対して複数回呼んでも安全(冪等)。 */
    state_ioc_block((uint8_t*)blk_ioc, 0);

    /* CPU regs restored LAST (PC last inside) */
    state_cpu_load(blk_cpu);
    cpu_setOPbase24((uint32_t)m68000_get_reg(M68K_PC));

    /* FDD re-mount if the saved image path differs from the current one.
     * mx68k_fdd_insert is safe to call on the emulation thread (it only stores
     * the path + loads the image via the Core FDD API). */
    for (int d = 0; d < 2; d++) {
        if (strcmp(s_fdd[d], g_fdd_path[d]) != 0) {
            if (s_fdd[d][0] != '\0') mx68k_fdd_insert(d, s_fdd[d]);
            else                     mx68k_fdd_eject(d);
        }
    }

out:
    free(fb);
    return rc;
}

int mx68k_save_state(const char* path) {
    if (!path) return -1;
    strncpy(g_pending_state_path, path, sizeof(g_pending_state_path) - 1);
    g_pending_state_path[sizeof(g_pending_state_path) - 1] = '\0';
    atomic_store(&g_pending_save, 1);
    return 0;   /* queued; runs at the next run_frame boundary */
}

int mx68k_load_state(const char* path) {
    if (!path) return -1;
    strncpy(g_pending_state_path, path, sizeof(g_pending_state_path) - 1);
    g_pending_state_path[sizeof(g_pending_state_path) - 1] = '\0';
    atomic_store(&g_pending_load, 1);
    return 0;   /* queued; runs at the next run_frame boundary */
}

/* P481 (D-42): completion readout for the queued save/load (see g_state_op_seq).
 * Callers snapshot mx68k_state_op_seq() before queueing, then poll until it
 * changes; at that point mx68k_last_state_rc()/_kind() describe that operation. */
unsigned int mx68k_state_op_seq(void)    { return atomic_load(&g_state_op_seq); }
int          mx68k_last_state_rc(void)   { return atomic_load(&g_state_op_rc); }
int          mx68k_last_state_kind(void) { return atomic_load(&g_state_op_kind); }

void mx68k_schedule_hard_reset(void) {
    g_pending_hard_reset = 1;
}

void mx68k_schedule_soft_reset(void) {
    g_pending_soft_reset = 1;
}

/* P454: SRAM ゼロクリアを次の run_frame 境界へ予約する。 */
void mx68k_schedule_sram_clear(void) {
    g_pending_sram_clear = 1;
}

// ---- settings ----
void mx68k_set_machine_type(int type) {
    g_machine_type = type;
}

void mx68k_set_memory_mb(int mb) {
    g_memory_size_mb = mb;
}

void mx68k_set_memsw_auto_update(bool enabled) {
    g_memsw_auto_update = enabled;
}

bool mx68k_get_memsw_auto_update(void) {
    return g_memsw_auto_update;
}

void mx68k_set_scsi_ext_board_installed(bool installed) {
    g_scsi_ext_board_installed = installed;
}

/* P85-A (MAJOR-1): export the file-static g_memory_size_mb so the P85A MEMSIZE
 * probe in m68000_bridge.c can read it without an `extern int g_memory_size_mb`
 * (which would fail to link against an internal-linkage symbol). Read-only. */
int mx68k_get_memory_size_mb(void) {
    return g_memory_size_mb;
}

void mx68k_set_clock_mhz(int mhz) {
    g_clock_mhz = mhz;
}

static char g_iplrom30_path[4096];

/* P241 Stage A: external SCSI (CZ-6BS1) IPL ROM. SCSIIPL[] is the non-static
 * global defined in Core/px68k/x68k/scsi.c (0x2000 = 8KB); referenced here via
 * extern so Core stays unmodified. */
extern uint8_t SCSIIPL[];               /* scsi.c: external CZ-6BS1 IPL ROM 0x2000 */
static bool s_scsi_ext_rom_loaded = false;

/* Load the external SCSI (CZ-6BS1) IPL ROM (SCSIEXROM.DAT, 8KB) into SCSIIPL[].
 * Optional: if the file is absent the external-SCSI boot path is simply disabled
 * (not a fatal error). Size 0x2000 (0x1FE0 also accepted, WinX68k/XM6 compat).
 * ★SCSIIPL is stored LE16-byte-swapped: both scsi.c's data read (adr^1) and the
 * c68k LE-native fetch of $EA0000 require it — same technique as the IPLROM
 * s_ipl_fetch build above. */
void mx68k_set_scsi_ext_rom_path(const char* path) {
    s_scsi_ext_rom_loaded = false;
    memset(SCSIIPL, 0, 0x2000);
    if (!path || !path[0]) return;
    /* P510: P246 ガード(SCSI ID0 にディスクイメージがマウントされている時だけ
     * SCSIIPL[] をロードする)は撤去済み。旧ガードの理由は「Core/px68k の MB89352
     * SPC 実装が REQ/ACK ハンドシェイクを持たない不完全なスタブで、ROM のバス待ち
     * ループが抜けられない」ことだったが、P510 で外付け SCSI のレジスタ窓
     * $EA0000-$EA001F を移植済み XM6 SPC 実装へ配線したためこの理由は解消した。
     * 実機では、ボードが装着されていればディスクの有無に関わらず ROM は常に
     * $EA0020-$EA1FFF へ見える(Outside X68000 印刷 p.132)。 */
    FILE* fp = fopen(path, "rb");
    if (!fp) { debug_log("[MX68K] SCSIEXROM: %s -> fopen failed\n", path); return; }
    uint8_t tmp[0x2000];
    size_t n = fread(tmp, 1, 0x2000, fp);
    fclose(fp);
    if (n < 0x1FE0) { debug_log("[MX68K] SCSIEXROM: short read %zu\n", n); return; }
    for (size_t i = 0; i + 1 < n; i += 2) {   /* LE16 swap store */
        SCSIIPL[i]     = tmp[i + 1];
        SCSIIPL[i + 1] = tmp[i];
    }
    s_scsi_ext_rom_loaded = true;
    debug_log("[MX68K] SCSIEXROM: %s -> OK (%zu bytes)\n", path, n);
}

void mx68k_set_bios_path(const char* iplrom, const char* cgrom) {
    bool ipl_ok = false;
    bool cg_ok = false;
    if (iplrom) {
        FILE* fp = fopen(iplrom, "rb");
        if (fp) {
            if (!IPL) IPL = (uint8_t*)malloc(0x40000);
            if (IPL) {
                memset(IPL, 0, 0x40000);
                ipl_ok = (fread(IPL, 1, 0x20000, fp) == 0x20000);
            }
            fclose(fp);
            /* P17-FIX-B: FETCHテーブル用LE16スワップ済みコピーを作成 */
            /* C68K_BYTE_SWAP_OPT有効時FETCH_WORD=*(u16*)PC(LE native)のため、BE格納のIPL[]をLE16スワップ */
            if (ipl_ok && IPL) {
                for (int i = 0; i < 0x20000; i += 2) {
                    s_ipl_fetch[i]     = IPL[i + 1];
                    s_ipl_fetch[i + 1] = IPL[i];
                }
            }
            debug_log( "[MX68K] IPLROM: %s -> %s\n", iplrom, ipl_ok ? "OK" : "FAIL");
        } else {
            debug_log( "[MX68K] IPLROM: %s -> fopen failed\n", iplrom);
        }
    }
    if (cgrom) {
        FILE* fp = fopen(cgrom, "rb");
        if (fp) {
            if (!FONT) FONT = (uint8_t*)malloc(0xC0000);
            if (FONT) {
                memset(FONT, 0, 0xC0000);
                cg_ok = (fread(FONT, 1, 0xC0000, fp) == 0xC0000);
                if (cg_ok) {
                    /* P174: byte-swap FONT to LE-native. CGROM.DAT is stored big-endian,
                     * but MX reads FONT via the LE-native *(uint16_t*)&FONT[] convention
                     * (Core mem_wrap.c:510/555/557, Bridge m68000_bridge.c:21569/21571/22017,
                     * and the c68k fetch region at m68000_bridge.c:25463). Without this swap
                     * every font-row read is byte-swapped -> full-width chars show their
                     * left/right 8px halves swapped and half-width chars overlap. FONT is a
                     * read-only ROM with a single (LE) read convention, so an in-place swap
                     * is uniformly correct. */
                    for (uint32_t i = 0; i < 0xC0000; i += 2) {
                        uint8_t t = FONT[i];
                        FONT[i]     = FONT[i + 1];
                        FONT[i + 1] = t;
                    }
                }
            }
            fclose(fp);
            debug_log( "[MX68K] CGROM: %s -> %s\n", cgrom, cg_ok ? "OK" : "FAIL");
        } else {
            debug_log( "[MX68K] CGROM: %s -> fopen failed\n", cgrom);
        }
    }
    s_bios_loaded = ipl_ok && cg_ok;
    debug_log( "[MX68K] BIOS loaded: %s\n", s_bios_loaded ? "YES" : "NO");
}

void mx68k_set_fpu_enabled(bool enabled) {
    g_fpu_enabled = enabled;
}

/* P483: Mercury Unit の装着設定。設定値のみを更新し、配線は init /
 * mx68k_reset_hard でラッチする(mx68k_set_machine_type と同じ様式)。 */
void mx68k_set_mercury_enabled(bool enabled) { g_mercury_enabled = enabled; }

/* P686 (D-70): 外付け FDD ユニットの装着設定。設定値のみを更新し、配線は
 * init / ハードリセットのラッチで確定する(Mercury / MIDI と同型)。 */
void mx68k_set_ext_fdd_enabled(bool enabled) { g_ext_fdd_enabled = enabled; }

/* P488: MIDI ボード(CZ-6BM1)の装着設定。設定値のみを更新し、配線は init /
 * ハードリセットのラッチで確定する(Mercury と同型)。 */
void mx68k_set_midi_enabled(bool enabled) { g_midi_enabled = enabled; }

/* P493: 内蔵 SRAM 64KB 化(実機改造相当)の設定。設定値のみを更新し、配線
 * (MemRead/WriteTable[0x6A-0x6F] の差し替え)は init / ハードリセットで確定する。 */
void mx68k_set_sram_64k_enabled(bool enabled) { g_sram_64k_enabled = enabled; }

/* P642: Windrv(Mac フォルダのホスト共有)。いずれも「設定値」のみを更新し、
 * 配線(g_windrv_installed とマウントルートの正規化)は init / ハードリセットの
 * windrv_init() で確定する(Mercury / MIDI / SRAM64K と同型)。 */
void mx68k_set_windrv_enabled(bool enabled) { g_windrv_enabled = enabled; }

void mx68k_set_windrv_host_path(const char* path) {
    if (path == NULL) {
        g_windrv_host_path[0] = '\0';
        return;
    }
    /* 固定長バッファへ必ず NUL 終端でコピーする(strcpy は使わない)。
     * 収まらない長さのパスは「未選択」と同じ扱いにする —— 切り詰めた
     * 別のディレクトリを黙って共有してしまう事故を避けるため。 */
    if (strlen(path) >= sizeof(g_windrv_host_path)) {
        g_windrv_host_path[0] = '\0';
        debug_log("[P642-WINDRV] host path too long (%zu bytes) — treated as unset\n",
                  strlen(path));
        return;
    }
    snprintf(g_windrv_host_path, sizeof(g_windrv_host_path), "%s", path);
}

bool mx68k_get_windrv_installed(void) { return g_windrv_installed != 0; }

/* P647: 書込み許可も同型 —— 設定値のみを更新し、配線(g_windrv_write_wired)は
 * windrv_init() で `g_windrv_installed && g_windrv_write_enabled` としてラッチする。 */
void mx68k_set_windrv_write_enabled(bool enabled) { g_windrv_write_enabled = enabled; }

bool mx68k_get_windrv_write_wired(void) { return g_windrv_write_wired != 0; }

/* P626 (D-62): 音声サンプルレートの設定。設定値のみを更新し、Core 側チップへの
 * 反映は init / ハードリセットで確定する(Mercury / MIDI / SRAM64K と同型)。
 * ホワイトリスト外の値は安全側として既定の 44100 へ丸める — 不正値がそのまま
 * チップ初期化へ渡ると、音程と生成量が同時に壊れるため。
 *
 * P627: 選択肢を {22050, 44100} から {22050, 44100, 48000, 62500, 88200, 96000} の
 * 6 値へ上方拡張。62500 は参照実装 XM6 本家の既定値であり、fmgen の OPM レート換算式
 * rateratio = ((clock/64) << 7) / rate が 62500 でだけ整数丸め誤差ゼロ(=128 ちょうど)
 * になるという技術的優位性を持つ。
 * ★このホワイトリストが唯一の真実源である。Swift 側(EmulatorViewModel /
 *   AudioSettingsView)は同じ値集合を複製せず、適用結果を
 *   mx68k_get_audio_sample_rate() で読み戻して AudioUnit 出力レートに使う
 *   — 二重管理による「Picker では選べるのに出力だけ黙って 44100 に丸められ、
 *   チップと AudioUnit のピッチが食い違う」類の不具合を構造的に防ぐため。 */
void mx68k_set_audio_sample_rate(int hz) {
    static const int kAllowedRates[] = { 22050, 44100, 48000, 62500, 88200, 96000 };
    int v = AUDIO_SAMPLE_RATE;
    for (size_t i = 0; i < sizeof(kAllowedRates) / sizeof(kAllowedRates[0]); i++) {
        if (hz == kAllowedRates[i]) { v = hz; break; }
    }
    if (v != hz) {
        debug_log("[P626-RATE] invalid sample rate %d -> clamped to %d\n", hz, v);
    }
    g_audio_sample_rate_hz = v;
}

/* P627: Bridge 側で実際に適用されたサンプルレートの読み戻し。Swift 側は
 * AudioUnit の出力レートをこの値に追従させることで、上記ホワイトリストを
 * 複製せずに済む(単一真実源)。 */
int mx68k_get_audio_sample_rate(void) { return g_audio_sample_rate_hz; }

/* P490 UX fix: 「MIDI 未配線(ハードリセット待ち)」と「配線済みだが実デバイス0台」を
 * Swift 側で区別できるようにする(前者はハードリセットで解決しうるが、後者は
 * 何度リセットしても変わらない — 案内文言を出し分けるための判別 API)。 */
bool mx68k_midi_is_wired(void) { return g_midi_installed != 0; }

/* ---- P490 (MIDI Stage 2): リセット送信 / 音源種別 / 送信遅延 / デバイス選択 ---- */

/* 起動・ハードリセット時にリセット SysEx を送出するか。配線は init /
 * ハードリセットのラッチで Config.MIDI_Reset へ反映される。 */
void mx68k_set_midi_reset_enabled(bool enabled) { g_midi_reset_enabled = enabled; }

/* 音源種別(0=LA/1=GM/2=GS/3=XG)。範囲外は無視して現在値を維持する
 * (midi.c:82-84 の MIDI_ResetType[] は index 4 が MIDI_NOTUSED のため)。 */
void mx68k_set_midi_reset_type(int type) {
    if (type < 0 || type > 3) return;
    g_midi_reset_type = type;
}

/* 送信遅延(ms)。上限 1000 は DelayBuf[4096] ÷ 3125B/s ≒ 1.31 秒からの安全マージン。 */
void mx68k_set_midi_delay_ms(int ms) {
    if (ms < 0)    ms = 0;
    if (ms > 1000) ms = 1000;
    g_midi_delay_ms = ms;
}

/* menu_items[MIDI_OUTDEV_ROW]/[MIDI_INDEV_ROW] は midi_darwin.c の
 * mid_outDevList()/mid_inDevList() が MIDI_Init() 実行時に埋め直す。
 * よってデバイス一覧は MIDI 装着 + init/ハードリセット後にのみ有効
 * (Mercury/MIDI の「配線はハードリセットで確定」パターンと同型)。 */
static int midi_dev_count(int row) {
    /* P490 crash fix: MIDI_Init()(midi.c:318-333)は実デバイスが 0 台のとき
     * menu_items[row][0] に "No Device Found." または "inactive." という
     * プレースホルダ文字列を書く。これを実デバイス 1 個と誤カウントすると、
     * 存在しない index 0 が mx68k_midi_set_output_device() の範囲検査を
     * 素通りして midOutChg() に到達してしまう(欠陥 B の実体)。 */
    if (menu_items[row][0][0] != '\0' &&
        (strcmp(menu_items[row][0], "No Device Found.") == 0 ||
         strcmp(menu_items[row][0], "inactive.") == 0)) {
        return 0;
    }
    int n = 0;
    while (n < MIDI_DEV_MAX && menu_items[row][n][0] != '\0') n++;
    return n;
}

static bool midi_dev_name(int row, int idx, char *buf, int len) {
    if (!g_midi_installed || idx < 0 || idx >= MIDI_DEV_MAX || buf == NULL || len <= 0) return false;
    if (menu_items[row][idx][0] == '\0') return false;
    strncpy(buf, menu_items[row][idx], (size_t)(len - 1));
    buf[len - 1] = '\0';
    return true;
}

int mx68k_midi_get_output_device_count(void) {
    if (!g_midi_installed) return 0;
    return midi_dev_count(MIDI_OUTDEV_ROW);
}

bool mx68k_midi_get_output_device_name(int idx, char *buf, int len) {
    return midi_dev_name(MIDI_OUTDEV_ROW, idx, buf, len);
}

void mx68k_midi_set_output_device(int idx) {
    if (!g_midi_installed) return;
    if (idx < 0 || idx >= mx68k_midi_get_output_device_count()) return;
    midOutChg((uint32_t)idx, 0);
    g_midi_out_device_index = idx;
}

int mx68k_midi_get_input_device_count(void) {
    if (!g_midi_installed) return 0;
    return midi_dev_count(MIDI_INDEV_ROW);
}

bool mx68k_midi_get_input_device_name(int idx, char *buf, int len) {
    return midi_dev_name(MIDI_INDEV_ROW, idx, buf, len);
}

void mx68k_midi_set_input_device(int idx) {
    if (!g_midi_installed) return;
    if (idx < 0 || idx >= mx68k_midi_get_input_device_count()) return;
    midInChg((uint32_t)idx);
    g_midi_in_device_index = idx;
}

/* P693: MIDI Viewer 向け — YM3802 レジスタ状態のスナップショット。
 *
 * 対象 7 項目は midi.c の非 static グローバルだが midi.h に extern 宣言が
 * あるのは MIDI_R35 / MIDI_IntEnable だけなので、残りはここで宣言する
 * (MIDI_MODULE を同じ理由でファイル前方に extern しているのと同型)。
 *
 * ★スレッド安全性: 7 項目とも書き手はゲスト CPU の I/O アクセス経由、
 *   すなわちエミュレーションスレッドのみ(MIDI_Write の各 case、
 *   Core/px68k/x68k/midi.c:665,677,704,715,720,739,750-752 と
 *   MIDI_Timer 経路の MIDI_Buffered--)。本関数の呼び出し元も同じ
 *   エミュレーションスレッド(EmulatorEngine.fetchMonitorsAndPerfStats)
 *   なのでロック不要。MIDI_IntEnable / MIDI_R35 は mid_In_callback() から
 *   *読まれる* が書かれないため、この判断には影響しない。
 *
 * ★MIDI_IntFlag / MIDI_IntVect は意図的に含めない(EmulatorBridge.h の
 *   MX68K_MIDIRegs コメント参照 — D-73 の既知競合に読み手を足さないため)。
 *
 * read-only: Core 側の状態は一切変更しない。 */
extern uint8_t  MIDI_RegHigh;
extern uint8_t  MIDI_Vector;
extern uint8_t  MIDI_R05;
extern uint8_t  MIDI_R55;
extern uint32_t MIDI_Buffered;

void mx68k_get_midi_regs(MX68K_MIDIRegs* out) {
    if (!out) return;
    out->reg_high     = MIDI_RegHigh;
    out->vector       = MIDI_Vector;
    out->int_enable   = MIDI_IntEnable;
    out->r05          = MIDI_R05;
    out->r35          = MIDI_R35;
    out->r55          = MIDI_R55;
    out->tx_fifo_used = MIDI_Buffered;
}

void mx68k_set_bios_path_030(const char* iplrom30_path) {
    if (iplrom30_path) {
        strncpy(g_iplrom30_path, iplrom30_path, sizeof(g_iplrom30_path) - 1);
        g_iplrom30_path[sizeof(g_iplrom30_path) - 1] = '\0';
    } else {
        g_iplrom30_path[0] = '\0';
    }
}

void mx68k_set_sound_enabled(bool en) {
    (void)en;
}

/* P512: 保持変数 g_opm_volume / g_adpcm_volume の定義は mx68k_reset_hard() から
 * 参照するためファイル前方(モジュール変数群)に置いてある。 */
void mx68k_set_opm_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 16) vol = 16;
    g_opm_volume = vol;
    OPM_SetVolume((uint8_t)vol);   /* P512: was empty stub; wire to Core OPM volume (0-16), same scale as ADPCM. */
}

void mx68k_set_adpcm_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 16) vol = 16;
    g_adpcm_volume = vol;   /* P512: remember for the hard-reset re-apply path. */
    ADPCM_SetVolume((uint8_t)vol);   /* P210: was empty stub; wire to Core ADPCM volume (0-16). */
}

/* P624: ターボモード中の音声再生レート設定(P555 の mx68k_set_turbo_audio_mute を置換)。
 * 保持変数 g_turbo_audio_ratio_q16 / g_turbo_audio_reset_gen の定義はファイル前方の
 * モジュール変数群にある(g_adpcm_volume と同じ理由)。
 *
 * ★スレッド安全設計: 本関数の呼出し元は Swift メインスレッド、デシメーション位相状態の
 * 実際の読み書きは CoreAudio 実時間スレッド(mx68k_audio_read 内)であり別スレッドである。
 * したがってここから位相状態を直接ゼロ化してはならない(非 atomic な部分書込みが混ざり
 * クリック・発振の原因になる)。単一書き手則を保つため、ここでは倍率を atomic に書き、
 * リセット世代カウンタを +1 するだけに留める。実際のゼロ初期化は mx68k_audio_read()
 * 冒頭の世代チェックが CoreAudio スレッド上で行う。 */
void mx68k_set_turbo_audio_rate(int ratio_q16) {
    /* 0 はミュートを意味する予約値(ノーウェイト用)。負値は不正なので最も安全な
     * 等倍(= P624 以前と同一の bypass 経路)へ丸める。 */
    if (ratio_q16 < 0) ratio_q16 = MX68K_TURBO_AUDIO_RATE_UNITY;
    atomic_store_explicit(&g_turbo_audio_ratio_q16, ratio_q16, memory_order_release);
    atomic_fetch_add_explicit(&g_turbo_audio_reset_gen, 1, memory_order_release);
}

/* P557: FD アクセス高速化(XM6 の Config::floppy_speed 相当の ON/OFF)。
 * Core/px68k/x68k/fdd.c は fdd.c 専用の -include(Bridge/fdd_timing_shim.h)により
 * usleep() の代わりにこの関数を呼ぶ。Core は無改変。
 *
 * ★このファイル冒頭で FDD_TIMING_SHIM_NO_MACROS を定義しているため、以下の
 *   usleep() は素のシステムコールであって自分自身への再帰ではない。 */
int mx68k_fdd_usleep_shim(useconds_t microseconds) {
    if (g_fd_fast_access) {
        /* XM6 fast mode 相当の 64µs 固定(fdc.cpp の 128 hus × 0.5µs)。
         * 参照実装 3 種(MPX68K / px68k 本家 / px68k-libretro)のように遅延を
         * 完全に 0 にはしない — XM6 が「高速化 ON」でも 64µs を残している設計を
         * そのまま踏襲する意図的な選択。 */
        return usleep(64);
    }
    return usleep(microseconds);   /* OFF(既定)= 従来どおり素通し */
}

void mx68k_set_fd_fast_access(int enabled) {
    g_fd_fast_access = enabled ? 1 : 0;
}

void mx68k_sram_save(void) {
    ensure_app_support_dir();
    const char* home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/Library/Application Support/MX68K/sram.dat", home);
    FILE* fp = fopen(path, "wb");
    if (fp) {
        fwrite(SRAM, 1, 0x4000, fp);
        fclose(fp);
    }
}

void mx68k_sram_load(void) {
    const char* home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/Library/Application Support/MX68K/sram.dat", home);
    FILE* fp = fopen(path, "rb");
    if (fp) {
        fread(SRAM, 1, 0x4000, fp);
        fclose(fp);
    }
}

/* P505 (D-46): SRAM署名を含む91バイト($ED0000-$ED005A)をIPL-ROM内蔵の
 * デフォルトテーブル(guest $FF0842、IPL[]配列内オフセット0x10842)から
 * 直接コピーする。従来(P142d)は署名部分を意図的に無効(0埋め)のまま残し、
 * IPL-ROM自身の自己修復ロジックに91バイトを補完させる設計だった——しかし
 * Clear SRAM操作の度にこの無効状態が再武装され、Hard Reset直後にBridgeが
 * 書いたHD_MAX等の値をIPL-ROMの自己修復が0で上書きしてしまう競合を起こす
 * (D-46、詳細は.mx68k_cycles/P504_verify_inv.md)。
 * 署名を最初から有効にしておけばIPL-ROMの自己修復自体が発火しなくなり、
 * この競合が構造的に解消する(XM6は同種のUI操作を持たないため、この
 * 競合が元々発生しない——.mx68k_cycles/P505_code_inv.md参照)。
 * 定数のハードコードではなくロード済みIPL[]からの複写とし、期待する
 * 署名8バイトと不一致(または IPL 未ロード)の場合は旧来のP142d手動シード
 * (署名は無効のまま、自己修復に委ねる)へ安全側フォールバックする。 */
static void sram_seed_defaults(void) {
    static const uint32_t SIG_TABLE_OFFSET = 0x10842;
    static const uint8_t EXPECTED_SIG[8] = {0x82,0x77,0x36,0x38,0x30,0x30,0x30,0x57};
    int i, match = (IPL != NULL);

    memset(SRAM, 0x00, 0x4000);

    if (match) {
        for (i = 0; i < 8; i++) {
            if (IPL[SIG_TABLE_OFFSET + i] != EXPECTED_SIG[i]) { match = 0; break; }
        }
    }

    if (match) {
        for (i = 0; i < 91; i++) {
            SRAM[i ^ 1] = IPL[SIG_TABLE_OFFSET + i];
        }
        /* IPL-ROM自己修復は91バイトの他にclr.w $ed0100も行う(FF0452)。
         * 91バイトの範囲外なので明示的に0クリアしておく。 */
        SRAM[0x100 ^ 1] = 0x00;
        SRAM[0x101 ^ 1] = 0x00;
        debug_log("[P505-SRAMSEED] template=IPL[0x%x] match=1 hdmax=0x%02x ed0010_13=0x%02x%02x%02x%02x\n",
                  SIG_TABLE_OFFSET, SRAM[0x5A^1],
                  SRAM[0x10^1], SRAM[0x11^1], SRAM[0x12^1], SRAM[0x13^1]);
    } else {
        /* フォールバック: 従来のP142d手動シード(署名は無効のまま残し、
         * IPL-ROMの自己修復に91バイト全体を補完させる——本修正が無かった
         * 場合と同じ挙動)。 */
        SRAM[0x08^1] = 0x00; SRAM[0x09^1] = 0xC0;
        SRAM[0x0A^1] = 0x00; SRAM[0x0B^1] = 0x00;
        SRAM[0x10^1] = 0x00; SRAM[0x11^1] = 0x01;
        SRAM[0x12^1] = 0xED; SRAM[0x13^1] = 0x00;
        SRAM[0x18^1] = 0x00; SRAM[0x19^1] = 0x00;
        SRAM[0x1A^1] = 0x00; SRAM[0x1B^1] = 0x00;
        SRAM[0x70^1] = 0x00;
        SRAM[0x72^1] = 0x00; SRAM[0x73^1] = 0x00;
        debug_log("[P505-SRAMSEED] template=IPL[0x%x] match=0 reason=%s fallback=legacy_p142d\n",
                  SIG_TABLE_OFFSET, IPL ? "sig_mismatch" : "ipl_not_loaded");
    }
}

void mx68k_sram_clear(void) {
    /* P505 (D-46): 全ゼロ化から「工場出荷時(IPL-ROM 内蔵既定値)へ戻す」へ
     * 意味論を変更。署名が有効なまま残るため、以後の Hard Reset で IPL-ROM の
     * 自己修復が発火せず、Bridge が書くメモリスイッチ値を上書きしない。 */
    sram_seed_defaults();
    /* P494-①: 64KB 設定時、上位 48KB($ED4000-$EDFFFF)は Bridge 所有バッファに
     * あるため memset(SRAM,...) では消えない。「Clear SRAM」は全 SRAM を消す、
     * という期待に合わせてここで併せてクリアする(命令フェッチ用シャドウも
     * sram_ext_clear() の中で追随させる)。 */
    sram_ext_clear();
}

// ---- framebuffer ----
static inline uint32_t px68k_color_to_rgba(uint32_t c)
{
    // px68k stores colors as 0xRRGGBB00 (R@bits24-31, G@16-23, B@8-15).
    // P184: the Metal texture is .bgra8Unorm, so emit B,G,R,A byte order (BGRA)
    // to match it. (Previously emitted R,G,B,A → R/B swapped on display: blue
    // rendered as orange. White/green are swap-invariant, which is why text
    // looked correct and only color-heavy graphics revealed the swap.)
    // c=0xRRGGBB00 → (c>>8)=0x00RRGGBB → bytes B,G,R + A=0xFF = BGRA.
    return ((c >> 8) & 0x00FFFFFF) | 0xFF000000;
}

#if P305_ENABLE
/* P305: D-4実測用。指定フレームの内部フレームバッファ(RGBA8888、
 * Metal転送前)を生の画素データとしてファイル保存する。read-only
 * (自身が新規生成するファイルへの書込みのみ、エミュレーション状態・
 * 既存の戻り値ロジックには一切影響しない)。ディレクトリは初回のみ
 * mkdir(既存ならEEXISTを無視)。 */
static void p305_dump_frame(int frame_num, const uint8_t *fb, int w, int h) {
    static int s_p305_dir_ready = 0;
    if (!s_p305_dir_ready) {
        mkdir("/tmp/mx68k_p305_dump", 0755);   /* 既存ならEEXIST、無視してよい */
        s_p305_dir_ready = 1;
    }
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mx68k_p305_dump/f%06d_w%d_h%d.rgba",
             frame_num, w, h);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(fb, 1, (size_t)w * (size_t)h * 4, fp);
        fclose(fp);
        debug_log("[P305-FBDUMP] f=%d w=%d h=%d path=%s\n", frame_num, w, h, path);
    }
}
#endif

#if P312_ENABLE
/* P312: D-4実測用。Sprite_Regsの全128スロット実効範囲(0xEB0000-0xEB03FF、
 * 1024バイト、Core Sprite_DrawLineMcrが実際に参照する範囲)を生バイナリで
 * ファイル保存する。read-only(Sprite_Regsは一切書き込まない)。P305の
 * フレームバッファダンプと同一フレーム番号・同一タイミングで呼ばれ、
 * 対になるスプライトテーブル状態を記録する。 */
static void p312_dump_sprite_table(int frame_num) {
    static int s_p312_dir_ready = 0;
    if (!s_p312_dir_ready) {
        mkdir("/tmp/mx68k_p312_dump", 0755);   /* 既存ならEEXIST、無視してよい */
        s_p312_dir_ready = 1;
    }
    extern uint8_t Sprite_Regs[0x800];
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mx68k_p312_dump/f%06d_spr1024.bin", frame_num);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(Sprite_Regs, 1, 1024, fp);
        fclose(fp);
        debug_log("[P312-SPRDUMP] f=%d path=%s\n", frame_num, path);
    }
}
#endif

/* P571 (D-51): R20 下位バイト(CRTC_Regs[0x29])から、その画面モードの
 * 「標準表示ウィンドウ」= R02/R03/R06/R07 の標準設定値を引く。
 * 一次情報源: テクニカルデータブック 印刷p.26 表2-12「各画面モードにおける
 * CRTCレジスタ設定値」/ 印刷p.28「R20(下位バイト)」ビット表。
 *   D04    = 水平偏向周波数 (0:15.98kHz "標準解像度" / 1:31.5kHz "高解像度")
 *   D01-D00= 水平表示ドット数 (00:256 / 01:512 / 10:768 / 11:未定義)
 * R06/R07 の標準値は水平周波数だけで決まる(垂直側のモード差は CRTC_VStep が
 * すべて吸収する。Core: crtc.c:308-319 参照)ので、垂直は 2 通りしかない。
 * 表2-12 の値は「計算値−1」だが、R02/R03・R06/R07 は呼び元が R03-R02 / R07-R06 の
 * 「差」しか使わないため −1 は相殺される。
 * P589 (D-54): R00(水平トータル)の標準値も併せて返す。こちらは呼び元が
 * CRTC 実測 R00 との「直接の等値比較」に使う(差分ではないので相殺は成立しない)ため、
 * 表2-12 の R00 欄の値をそのまま(レジスタに書かれる値として)返している。
 * P595 (D-55): R04(垂直トータル)の標準値も返す。R00 と同じく呼び元が CRTC 実測
 * R04 との「直接の等値比較」に使う(高解像度系=567・標準解像度系=259、表2-12)。
 * 戻り値 0 = 表2-12 に定義の無いビット組合せ(呼び元はレターボックスを行わない)。 */
static int mx68k_crtc_std_window(uint8_t r20lo,
                                 int *r00_std,
                                 int *r02_std, int *r03_std,
                                 int *r04_std,
                                 int *r06_std, int *r07_std)
{
    const int hires = (r20lo & 0x10) != 0;   /* D04 */
    const int hdots = (r20lo & 0x03);        /* D01-D00 */

    if (hires) { *r06_std = 40; *r07_std = 552; *r04_std = 567; }  /* 高解像度(31.5kHz) */
    else       { *r06_std = 16; *r07_std = 256; *r04_std = 259; }  /* 標準解像度(15.98kHz) */

    if (hires) {
        switch (hdots) {
        case 0: *r02_std = 6;  *r03_std = 38;  *r00_std = 45;  break;   /* 256 dot  R00=0x2D */
        case 1: *r02_std = 17; *r03_std = 81;  *r00_std = 91;  break;   /* 512 dot  R00=0x5B */
        case 2: *r02_std = 28; *r03_std = 124; *r00_std = 137; break;   /* 768 dot  R00=0x89 */
        default: return 0;                              /* 11: 表2-12 に無し */
        }
    } else {
        switch (hdots) {
        case 0: *r02_std = 0; *r03_std = 32; *r00_std = 37; break;      /* 256 dot  R00=0x25 */
        case 1: *r02_std = 5; *r03_std = 69; *r00_std = 75; break;      /* 512 dot  R00=0x4B */
        default: return 0;                              /* 15.98kHz の 768/未定義は無し */
        }
    }
    return 1;
}

/* P294: フレーム前処理 — exec ループ直前で1回。back バッファ選択・disp 幾何確定
 * (フレーム頭で固定=ストライド一貫性)・memset・集計 reset。以降 draw_display_line が
 * 表示行を per-scanline で描き込む。旧 mx68k_render_frame() の (A) 部 + 集計 reset。
 * P173: emulation スレッドで実行(cross-thread VLINE race 無し)。 */
static void mx68k_render_begin(void) {
    int back = 1 - atomic_load_explicit(&s_fb_front, memory_order_relaxed);
    s_render_back = back;
    s_render_fb = s_framebuffer[back];
    s_render_orig_textdotx = TextDotX;
#if P533_ENABLE
    /* P533 [P533-XSNAP]: フレーム頭のスナップショット。全て読み取りのみ。
     * s_p533_chsize_calls はここで 0 に戻し、フレーム中の
     * WinDraw_ChangeSize() 呼出し回数を数える。 */
    s_p533_chsize_calls = 0;
    s_p533_tdx_b = TextDotX;
    s_p533_tdy_b = TextDotY;
    s_p533_palhash_b = p214_fnv1a(P214_FNV_BASIS, Pal_Regs, sizeof(Pal_Regs));
    s_p533_sp1_b = SysPort[1];
    s_p533_cv_b = Contrast_Value;
#endif
#if P570_ENABLE
    /* P570 [P570-GEOM]: フレーム頭の出力ジオメトリ・スナップショット。
     * disp_w/disp_h のラッチ(直下)より前に採取するので、ここで読む値は
     * クランプ前の生の CRTC 由来値である。全て読み取りのみ。 */
    s_p570_tdx_b   = TextDotX;
    s_p570_tdy_b   = TextDotY;
    s_p570_hs_b    = CRTC_HSTART;
    s_p570_he_b    = CRTC_HEND;
    s_p570_vs_b    = CRTC_VSTART;
    s_p570_ve_b    = CRTC_VEND;
    s_p570_r28_b   = CRTC_Regs[0x28];
    s_p570_r29_b   = CRTC_Regs[0x29];
    s_p570_vstep_b = CRTC_VStep;
    s_p570_r00_b   = ((int32_t)CRTC_Regs[0x00] << 8) | CRTC_Regs[0x01];
    s_p570_r04_b   = ((int32_t)CRTC_Regs[0x08] << 8) | CRTC_Regs[0x09];
#endif
    int disp_w = TextDotX;
    int disp_h = TextDotY;
    if (disp_w <= 0) disp_w = 768;
    if (disp_w > 1024) disp_w = 1024;
    if (disp_h <= 0) disp_h = 512;
    if (disp_h > 1024) disp_h = 1024;
    /* P294: TextDotX の 1024 クランプは begin では行わない。exec 中 TextDotX を
     * クランプしたまま置くと CPU/CRTC ロジックへ波及しうるため、描画直前だけ
     * mx68k_draw_display_line() 内で局所的に save/clamp/restore する。disp_w は
     * ここで確定した値をフレーム全走査線で不変に使う(ストライド一貫性)。 */
    s_render_disp_w = disp_w;
    s_render_disp_h = disp_h;

    /* P595 (D-55): 出力表示ジオメトリを「標準表示窓=1.0」を単位とする連続スケール
     * (h_scale/v_scale)+オフセット(off_x/off_y)として確定する。P571(D-51)の
     * 離散レターボックス(ノミナル面へのパディング貼付け)と P589(D-54)の R00 等値
     * ゲートを、この 1 本の式へ統合したもの。実機は表示ウィンドウを狭めても絵を
     * 引き伸ばさず、画面モードのノミナルラスタの中で映像範囲が狭まり外側が
     * ボーダー(黒)になるだけ(テクニカルデータブック 印刷p.26 DISPTMG図、
     * ハードウェアスケーラは非存在)。
     * 既定 = 恒等(P212 と完全同一)。std_window が無効/妥当性外なら恒等のまま。 */
    double hs_ = 1.0, vs_ = 1.0, ox_ = 0.0, oy_ = 0.0;
    int geo_mode = 0;   /* 0=恒等 1=標準R00/R04(P571式) 2=非標準R00(対称中央寄せ) 9=妥当性外/R04非標準 */
    {
        int r00s, r02s, r03s, r04s, r06s, r07s;
        if (mx68k_crtc_std_window(CRTC_Regs[0x29], &r00s, &r02s, &r03s,
                                   &r04s, &r06s, &r07s)) {
            const int r00_actual = ((int)CRTC_Regs[0x00] << 8) | CRTC_Regs[0x01];
            const int r04_actual = ((int)CRTC_Regs[0x08] << 8) | CRTC_Regs[0x09];
            /* P597 (D-55): 水平は「1ライン中の実表示時間の比」で正規化する。R00(水平トータル)を
             * 無視すると、走査速度を変える非標準ラスタ(超連射68K: R00=69対標準91)で幅を
             * 25%取り違える(ユーザーのXM6 TypeG実測、.mx68k_cycles/P596_render_regression_inv.md
             * §32-39で確定)。kh=(r00s+1)/(r00_actual+1)。r00_actual==r00sのときkhは厳密に1.0
             * (整数同値のdouble除算)となり、標準ラスタでの出力はP212/P571/P589とビット同一。 */
            double kh = 1.0;
            if (r00_actual > 0 && r00s > 0)
                kh = (double)(r00s + 1) / (double)(r00_actual + 1);
            hs_ = (double)((int)CRTC_HEND - (int)CRTC_HSTART) / (double)(r03s - r02s) * kh;
            vs_ = (double)((int)CRTC_VEND - (int)CRTC_VSTART) / (double)(r07s - r06s);
            /* 水平オフセット: R00標準ならP571式(意図的な窓ずらし、Phalanx等)を維持、
             * 非標準なら対称中央寄せ。倍率のみM2化し、位置は現行維持
             * (TypeGの非対称配置は規則不明・誤差3.6%のため追従しない、P596investigation §36)。 */
            if (r00_actual == r00s) {
                ox_ = ((double)CRTC_HSTART - (double)r02s) / (double)(r03s - r02s);
            } else {
                ox_ = (1.0 - hs_) / 2.0;
            }
            /* 垂直: R04非標準は未実行経路(既知タイトルなし)のため恒等へフォールバック。
             * R04標準時のみ P571 由来の窓ずらしオフセットを維持する。 */
            if (r04_actual == r04s) {
                oy_ = ((double)CRTC_VSTART - (double)r06s) / (double)(r07s - r06s);
            } else {
                hs_ = 1.0; vs_ = 1.0; ox_ = 0.0; oy_ = 0.0;
            }
            if (r04_actual == r04s &&
                hs_ > 0.0 && vs_ > 0.0 && hs_ <= 1.5 && vs_ <= 1.5 &&
                ox_ >= -0.5 && oy_ >= -0.5 &&
                ox_ + hs_ <= 1.5 && oy_ + vs_ <= 1.5) {
                geo_mode = (r00_actual == r00s) ? 1 : 2;
            } else {
                hs_ = 1.0; vs_ = 1.0; ox_ = 0.0; oy_ = 0.0; geo_mode = 9;
            }
        }
    }
    /* hs_/vs_/ox_/oy_/geo_mode は mx68k_render_begin() のローカルであり、別関数
     * mx68k_render_end() からは参照できない。既存の s_render_back と同じ手法で
     * file-scope のブリッジ変数へ複写し、end 側の publish で読む。 */
    s_render_hscale  = (float)hs_;
    s_render_vscale  = (float)vs_;
    s_render_offx    = (float)ox_;
    s_render_offy    = (float)oy_;
    s_render_geomode = geo_mode;

    /* P535: 旧 memset(s_render_fb, 0, 4MB) を撤廃。描画先は永続合成バッファ
     * s_compose_fb に移り、公開バッファ s_render_fb は render_end の転送で
     * 表示領域全体が上書き(未描画行はゼロ埋め)されるため全消去は不要。 */
    memset(s_row_drawn, 0, sizeof(s_row_drawn));   /* P535: 1024B/frame。撤廃するmemsetの0.024% */

    s_render_fb_call_count++;
    s_render_log_this = (s_render_fb_call_count <= 5 || s_render_fb_call_count % 60 == 0);
    if (s_render_log_this) {
        debug_log("[MX68K] mx68k_get_framebuffer: call=%d TextDotX=%d TextDotY=%d VCReg0[1]=%02x VCReg1[1]=%02x VCReg2[1]=%02x\n",
                  s_render_fb_call_count, TextDotX, TextDotY, VCReg0[1], VCReg1[1], VCReg2[1]);
    }

    s_render_nonzero_pixels = 0;
#if P543_ENABLE
    s_p543_lines_drawn = 0;
    s_p543_lines_mx_draws = 0;
    s_p543_lines_would_block = 0;
    s_p543_first_block_vline = -1;
    s_p543_last_block_vline = -1;
#endif
#if P217_PROBE
    s_p217_n_bgzero = 0;
    s_p217_y_min = -1;
    s_p217_y_max = -1;
#endif
}

/* P294: フレーム後処理 — exec ループ終了直後で1回。旧 (B) frame-summary probe +
 * (D) 後処理 probe + framebuffer publish。per-scanline 描画は draw_display_line で
 * 既に完了済み。フレーム単位の集計値は begin/draw_line が更新した file-scope static
 * を読むだけ(以下でローカルにエイリアスする)。 */
static void mx68k_render_end(void) {
    int fb_call_count = s_render_fb_call_count;
    int log_this      = s_render_log_this;
    int disp_w        = s_render_disp_w;
    int disp_h        = s_render_disp_h;
    int back          = s_render_back;
    int orig_textdotx = s_render_orig_textdotx;
    int nonzero_pixels = s_render_nonzero_pixels;
    (void)orig_textdotx;   /* P294: TextDotX 復元を廃止したため参照のみ無し(§2.5) */

    /* P340: 旧 mx68k_render_frame() の関数ローカル static。P294 の3分割で
     * これらを参照する箇所(P293 バースト判定・P305/P312 のフレーム末 dump)が
     * すべて render_end 側へ来たため、宣言も render_end に置く(意味論は
     * フレームをまたいで保持する点も含め main と同一)。 */
#if P305_ENABLE
    static int s_p305_dump_countdown = 0;   /* >0の間、毎フレームダンプする */
    static int s_p305_burst_count    = 0;   /* 最初の3バーストのみダンプ(ディスク使用量を抑制) */
#endif

#if P214_ENABLE
    /* P214: 直前フレームの計測を 60 フレームに 1 行だけ出力（20 秒走行で ~20 行）。
     * R2 は R1 と同じ per-frame 意味論にするため毎フレーム reset する。read-only。 */
    {
        char r2buf[320];
        p214_r2_format_and_reset(r2buf, (int)sizeof(r2buf));
#if P292_ENABLE
        {
            /* D-4調査: r2bufは既にBGRバケット(0xEB0000-0xEB0811書込み統計)を
             * 含んでいる(m68000_bridge.cのp214_r2_format_and_resetが毎フレーム
             * 計算・整形・リセット済み)。新規の計測ロジックを追加せず、この
             * 文字列から"BGR n=... lines=... disp=..."部分をパースするだけ。
             * read-only。 */
            unsigned bgr_n = 0, bgr_lines = 0, bgr_disp = 0;
            const char *p = strstr(r2buf, "BGR n=");
            if (p) sscanf(p, "BGR n=%u lines=%u disp=%u", &bgr_n, &bgr_lines, &bgr_disp);
            debug_log("[P292-MIDSCAN] f=%d bgr_n=%u bgr_disp=%u bgr_lines=%u chg_spr=%u\n",
                      fb_call_count, bgr_n, bgr_disp, bgr_lines, s_p214_chg_spr);
        }
#endif
#if P293_ENABLE
        {
            int p293_min = -1, p293_max = -1;
            unsigned p293_n = 0;
            p293_peek_and_reset(&p293_min, &p293_max, &p293_n);
            debug_log("[P293-SPRWRITE] f=%d slot_min=%d slot_max=%d n=%u\n",
                      fb_call_count, p293_min, p293_max, p293_n);
#if P305_ENABLE
            if (p293_min == 0 && p293_max == 63 && s_p305_burst_count < 1
                && fb_call_count > 5000) {
                s_p305_burst_count++;
                s_p305_dump_countdown = 3000;   /* P309: frame>5000以降の最初の1バーストのみ、連続3000フレーム(約54秒)
                                                  * ダンプ(P308実測で300フレームでは既知の症状発生region(+約1947フレーム
                                                  * 先)に届かなかったため拡大) */
            }
#endif
        }
#endif
#if P299_ENABLE
        {
            int p299_vl_min, p299_vl_max;
            unsigned p299_n_in, p299_n_out;
            p299_peek_and_reset(&p299_vl_min, &p299_vl_max, &p299_n_in, &p299_n_out);
            if (p299_n_in || p299_n_out) {
                debug_log("[P299-SPRVLINE] f=%d vl_min=%d vl_max=%d n_in=%u n_out=%u vstart=%u vend=%u\n",
                          fb_call_count, p299_vl_min, p299_vl_max, p299_n_in, p299_n_out,
                          (unsigned)CRTC_VSTART, (unsigned)CRTC_VEND);
            }
        }
#endif
#if P300_ENABLE
    {
        uint16_t p300_before_posx[64], p300_before_posy[64];
        int p300_have_before;
        p300_peek_and_reset(p300_before_posx, p300_before_posy, &p300_have_before);
        if (p300_have_before) {
            extern uint8_t Sprite_Regs[0x800];
            typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) P300SprEntC;
            const P300SprEntC *sct = (const P300SprEntC *)Sprite_Regs;
            int n_changed = 0;
            char buf[2048];   /* Code Review round1指摘: 64スロット全変化(P293/P299実測での
                                * 常態)を想定すると64*21≒1344バイト > 旧1024で切り詰めが発生し、
                                * tear line付近のスロットが欠落域に入ると仮説検証不能になるため
                                * 2048へ拡大 */
            int off = 0;
            buf[0] = '\0';
            for (int n = 0; n < 64; n++) {
                uint16_t new_y = sct[n].posy & 0x3ff;
                if (new_y != p300_before_posy[n]) {
                    n_changed++;
                    if (off < (int)sizeof(buf) - 40) {
                        int w = snprintf(buf + off, sizeof(buf) - (size_t)off,
                                          " n=%d oy=%u ny=%u", n, p300_before_posy[n], new_y);
                        if (w > 0) off += w;
                    }
                }
            }
            if (n_changed > 0) {
                debug_log("[P300-SPRYDIFF] f=%d n_changed=%d%s\n", fb_call_count, n_changed, buf);
            }
        }
    }
#endif
#if P337_ENABLE
    {
        static uint8_t s_p337_prev_pat0 = 0xFF;
        static uint8_t s_p337_prev_pat1 = 0xFF;
        static int     s_p337_initialized = 0;
        extern uint8_t Sprite_Regs[0x800];
        typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) P337SprEnt;
        const P337SprEnt *sct = (const P337SprEnt *)Sprite_Regs;
        uint8_t pat0 = sct[0].ctrl & 0xff;
        uint8_t pat1 = sct[1].ctrl & 0xff;

        if (!s_p337_initialized) {
            s_p337_prev_pat0 = pat0;
            s_p337_prev_pat1 = pat1;
            s_p337_initialized = 1;
        } else {
            if (pat0 != s_p337_prev_pat0) {
                char found[512]; int off = 0; found[0] = '\0';
                int n_found = 0;
                for (int n = 0; n < 128; n++) {
                    if ((sct[n].ctrl & 0xff) == s_p337_prev_pat0) {
                        n_found++;
                        if (off < (int)sizeof(found) - 16) {
                            int w = snprintf(found + off, sizeof(found) - (size_t)off, " %d", n);
                            if (w > 0) off += w;
                        }
                    }
                }
                debug_log("[P337-SHIPSEARCH] f=%d slot=0 old_pat=0x%02x new_pat=0x%02x found_n=%d found_at=[%s]\n",
                          fb_call_count, s_p337_prev_pat0, pat0, n_found, found);
            }
            if (pat1 != s_p337_prev_pat1) {
                char found[512]; int off = 0; found[0] = '\0';
                int n_found = 0;
                for (int n = 0; n < 128; n++) {
                    if ((sct[n].ctrl & 0xff) == s_p337_prev_pat1) {
                        n_found++;
                        if (off < (int)sizeof(found) - 16) {
                            int w = snprintf(found + off, sizeof(found) - (size_t)off, " %d", n);
                            if (w > 0) off += w;
                        }
                    }
                }
                debug_log("[P337-SHIPSEARCH] f=%d slot=1 old_pat=0x%02x new_pat=0x%02x found_n=%d found_at=[%s]\n",
                          fb_call_count, s_p337_prev_pat1, pat1, n_found, found);
            }
            s_p337_prev_pat0 = pat0;
            s_p337_prev_pat1 = pat1;
        }
    }
#endif
        if (fb_call_count % 60 == 0) {
            /* S: upstream windraw.c:664-673 の VLINEBG スケーリング指数 */
            int s1 = ((BG_Regs[0x11] & 4) ? 2 : 1) - ((BG_Regs[0x11] & 16) ? 1 : 0);
            int s2 = ((CRTC_Regs[0x29] & 4) ? 2 : 1) - ((CRTC_Regs[0x29] & 16) ? 1 : 0);
            debug_log("[P214] f=%d VST=%d VEND=%d VStep=%d IntLine=%d rint_en=%d"
                      " | R1 chg_vc=%u chg_pal=%u chg_spr=%u"
                      " | R2 %s"
                      " | S VC0[1]=%02x VC1[0]=%02x VC1[1]=%02x VC2[0]=%02x VC2[1]=%02x"
                      " BG8=%02x BG9=%02x BG11=%02x C29=%02x s1=%d s2=%d BGF=%02x C0D=%02x\n",
                      fb_call_count, (int)CRTC_VSTART, (int)CRTC_VEND, (int)CRTC_VStep,
                      (int)CRTC_IntLine, (MFP[MFP_IERA] & 0x40) ? 1 : 0,
                      s_p214_chg_vc, s_p214_chg_pal, s_p214_chg_spr,
                      r2buf,
                      VCReg0[1], VCReg1[0], VCReg1[1], VCReg2[0], VCReg2[1],
                      BG_Regs[8], BG_Regs[9], BG_Regs[0x11], CRTC_Regs[0x29], s1, s2,
                      BG_Regs[0x0f], CRTC_Regs[0x0d]);
            p278_dump_sprites(fb_call_count);
        }
    }
#endif

#if P290_ENABLE
    {
        /* D-4調査: s_p214_prev_spr はこのフレームの最終表示行時点の
         * Sprite_Regsハッシュ(p214_r1_sample_line が行末ごとに更新し、
         * p214_r1_frame_reset はこの値自体はクリアしないため、フレーム
         * 終了時点でそのフレームの最終状態を保持している)。read-only。 */
        int changed = s_p290_have_frame && (s_p214_prev_spr != s_p290_prev_frame_spr);
        int active = 0;
        {
            typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) P290SprEnt;
            extern uint8_t Sprite_Regs[0x800];
            const P290SprEnt *sct = (const P290SprEnt *)Sprite_Regs;
            for (int n = 0; n < 128; n++) {
                uint16_t px = sct[n].posx & 0x3ff, py = sct[n].posy & 0x3ff;
                if (px == 0 && py == 0) continue;   /* P278と同じ簡易フィルタ */
                active++;
            }
        }
        debug_log("[P290-SPRDIFF] f=%d changed=%d spr_hash=%08x active=%d\n",
                  fb_call_count, changed, s_p214_prev_spr, active);
        s_p290_prev_frame_spr = s_p214_prev_spr;
        s_p290_have_frame = 1;
    }
#endif

#if P291_ENABLE
    p291_sprline_scan(fb_call_count, disp_h);
#endif

    /* ---- (D) 後処理 probe + framebuffer publish（旧 mx68k_render_frame 末尾を移設）---- */
#if P295_ENABLE
    debug_log("[P295-RASTERIRQ] f=%d reg12=%02x reg13=%02x int_line=%u fires=%u\n",
              fb_call_count, CRTC_Regs[0x12], CRTC_Regs[0x13],
              (unsigned)CRTC_IntLine, s_p295_raster_irq_fires);
    s_p295_raster_irq_fires = 0;
#endif

#if P217_PROBE
    {
        static int s_p217_dumped = 0;
        if (!s_p217_dumped) {
            s_p217_dumped = 1;
            debug_log("[P217] static: TextPal32[0]=%08x [16]=%08x [32]=%08x [48]=%08x BG_CHRSIZE=%d\n",
                      TextPal32[0], TextPal32[16], TextPal32[32], TextPal32[48], (int)BG_CHRSIZE);
        }
        if (fb_call_count % 60 == 0) {
            debug_log("[P217] f=%d n_bgzero=%d y=[%d..%d] VC1[0]=%02x VC2[1]=%02x BG9=%02x\n",
                      fb_call_count, s_p217_n_bgzero, s_p217_y_min, s_p217_y_max,
                      VCReg1[0], VCReg2[1], BG_Regs[9]);
        }
    }
#endif

    if (log_this) {
        debug_log("[MX68K] mx68k_get_framebuffer: nonzero_pixels=%d\n", nonzero_pixels);
    }

#if P543_ENABLE
    if (s_p543_lines_would_block > 0) {
        /* 乖離フレーム: 毎回無条件ログ(レート制限なし、稀事象のため) */
        debug_log("[P543-DISPWIN] f=%d lines_drawn=%d lines_mx_draws=%d "
                  "lines_would_block=%d first_vline=%d last_vline=%d\n",
                  fb_call_count, s_p543_lines_drawn, s_p543_lines_mx_draws,
                  s_p543_lines_would_block, s_p543_first_block_vline, s_p543_last_block_vline);
    } else if (fb_call_count % 300 == 0) {
        /* 自己反証可能性ゲート: プローブが実際に走っている(分母>0)ことを
         * 定期的に示す心拍ログ。乖離ゼロが「プローブ未実行」ではなく
         * 「実測して0件」であることを後から検証可能にする。 */
        debug_log("[P543-DISPWIN-HB] f=%d lines_drawn=%d lines_mx_draws=%d clean=1\n",
                  fb_call_count, s_p543_lines_drawn, s_p543_lines_mx_draws);
    }
#endif

    /* P162: text layer enabled (TXON) but zero visible pixels — diagnose
     * whether the text VRAM is empty (boot stalled before printing) vs a
     * render-path defect. Read-only, bounded (<=6 dumps, only while stuck). */
    static int s_p162_dumps = 0;
    if (log_this && (VCReg2[1] & 0x20) && nonzero_pixels == 0 && s_p162_dumps < 6) {
        s_p162_dumps++;
        int tdw_nz = 0;
        for (int i = 0; i < 1024 * 512; i++) {
            if (TextDrawWork[i]) { tdw_nz++; if (tdw_nz >= 1000) break; }
        }
        int pal_nz = 0;
        for (int i = 0; i < 16; i++) { if (TextPal32[i]) pal_nz++; }
        debug_log("[MX68K][P162] TXON set nonzero=0: TextDrawWork_nz=%d TextPal32_nz=%d "
                  "TextPal32[0..3]=%08x %08x %08x %08x TextScrollX=%d TextScrollY=%d\n",
                  tdw_nz, pal_nz, TextPal32[0], TextPal32[1], TextPal32[2], TextPal32[3],
                  TextScrollX, TextScrollY);
    }

    /* P294: TextDotX の復元は不要(begin で TextDotX 自体はクランプせず、draw_display_line
     * 内で描画直前だけ save/clamp/restore する設計のため)。ここで frame 頭の値へ戻すと
     * exec 中に CPU が更新した TextDotX を巻き戻してしまうので行わない(plan §2.5)。 */

    /* P535: 永続合成バッファ → 公開フレームバッファへ、固定ストライドで読みタイトパックで書く転送。
     * 未描画行(s_row_drawn[ry]==0)はゼロ埋めする。disp_h はフレーム頭でラッチされる一方、
     * VLINE は走査線ごとに生のCRTCレジスタから再計算されるため、フレーム途中のCRTC_VSTART/VEND
     * 書換え(ラスタ分割)により、ラッチ済み disp_h の内側にありながら一度も描画されない行が
     * 実際に発生することが本タイトル自身のログで実測されている(TextDotY 512→256 の縮小)。
     * 旧コードはそこをmemset由来の黒でpublishしていたため、ゼロ埋めは出力同一性の必須条件。
     * 合成バッファ側(s_compose_fb)は消さずに残す —— Stage Bで「未到達」と「dirtyでないので
     * 省略」を区別するために永続内容を保持する非対称設計です。
     * P595 (D-55): P571 のノミナル面貼付け(パディング+黒枠合成)は撤去した。黒帯・
     * 配置は公開ジオメトリ(s_fb_hscale/vscale/offx/offy)として表示側へ渡し、
     * 公開フレームバッファは実描画寸法(disp_w x disp_h)を素で持つ P571 以前の
     * 方式へ戻す(毎フレームの memset+memcpy コストも減る)。 */
    for (int ry = 0; ry < disp_h; ry++) {
        uint8_t *drow = s_render_fb + (size_t)ry * disp_w * 4;
        if (!s_row_drawn[ry]) {
            memset(drow, 0, (size_t)disp_w * 4);    /* 未描画行 */
            continue;
        }
        memcpy(drow,
               s_compose_fb + (size_t)ry * MX_COMPOSE_STRIDE * 4,
               (size_t)disp_w * 4);
    }

    s_fb_w[back] = disp_w;   /* P179+P595: publish size paired with buffer(実描画寸法) */
    s_fb_h[back] = disp_h;
    /* P595 (D-55): 寸法と同じ back インデックスへ表示ジオメトリも書き、下の release
     * store が張る既存の happens-before 境界でまとめて公開する(新規の同期機構は不要)。 */
    s_fb_hscale[back]  = s_render_hscale;   /* begin() 側で複写済みのブリッジ変数から読む */
    s_fb_vscale[back]  = s_render_vscale;
    s_fb_offx[back]    = s_render_offx;
    s_fb_offy[back]    = s_render_offy;
    s_fb_geomode[back] = s_render_geomode;
    atomic_store_explicit(&s_fb_front, back, memory_order_release);  /* P179: publish complete frame */
#if P303_ENABLE
    atomic_store_explicit(&s_fb_front_frame_num, fb_call_count, memory_order_release);
#endif
#if P305_ENABLE
    /* P340: 旧 mx68k_render_frame() 末尾の frame dump。per-scanline 描画完了後
     * (publish 済みの完成フレーム)を対象にする点は分割前と同じ。 */
    if (s_p305_dump_countdown > 0) {
        /* P595: 公開FBの実寸は実描画寸法(disp_w x disp_h)へ戻ったので、
         * ダンプもその寸法で行う(P571 のノミナル寸法は撤去済み)。 */
        p305_dump_frame(fb_call_count, s_framebuffer[back], disp_w, disp_h);
#if P312_ENABLE
        p312_dump_sprite_table(fb_call_count);
#endif
        s_p305_dump_countdown--;
    }
#endif
#if P533_ENABLE
    /* P533 [P533-XSNAP]: フレーム末のスナップショットを採取し、frame begin 側の
     * 値と 1 行に併記して出力する。トリガ条件なし・毎フレーム 1 行 —— 一致
     * していても必ず出力するため、「変化なし」と「プローブ未到達」を区別できる。
     * chsize_calls は begin でリセットしたカウンタをそのまま読む(_e 版は無い)。
     * 全て読み取りのみで、エミュレーション状態への書込みは一切ない。 */
    s_p533_tdx_e = TextDotX;
    s_p533_tdy_e = TextDotY;
    s_p533_palhash_e = p214_fnv1a(P214_FNV_BASIS, Pal_Regs, sizeof(Pal_Regs));
    s_p533_sp1_e = SysPort[1];
    s_p533_cv_e = Contrast_Value;
    debug_log("[P533-XSNAP] f=%d tdx_b=%d tdx_e=%d tdy_b=%d tdy_e=%d chsize_calls=%u "
              "palhash_b=%08x palhash_e=%08x sp1_b=%u sp1_e=%u cv_b=%u cv_e=%u\n",
              g_mx68k_frame_num, s_p533_tdx_b, s_p533_tdx_e, s_p533_tdy_b, s_p533_tdy_e,
              s_p533_chsize_calls, s_p533_palhash_b, s_p533_palhash_e,
              (unsigned)s_p533_sp1_b, (unsigned)s_p533_sp1_e,
              (unsigned)s_p533_cv_b, (unsigned)s_p533_cv_e);
#endif
#if P570_ENABLE
    /* P570 [P570-GEOM]: フレーム末の出力ジオメトリを採取し、frame begin 側の
     * 値と 1 行に併記して出力する。トリガ条件なし・毎フレーム 1 行・間引き
     * なし —— 値が一致していても必ず出力するので、「変化なし」と「プローブ
     * 未到達」を標本自身で区別できる。
     * 派生値(tdx/tdy → dw/dh → fbw/fbh)には必ずその計算元の生値
     * (hs/he/vs/ve・r28/r29・vstep)を同一行へ併記する。
     * rows_drawn は publish ループとは独立の読み取りループで集計する
     * (publish ループ本体へ混ぜると、将来 publish 側を最適化した際に
     * プローブが黙って壊れるため)。rows_total(=disp_h)が分母。
     * 全て読み取りのみで、エミュレーション状態への書込みは一切ない。 */
    {
        int p570_rows_drawn = 0;
        for (int ry = 0; ry < disp_h; ry++) {
            if (s_row_drawn[ry]) p570_rows_drawn++;
        }
        /* chsz は既存 P533 カウンタの再利用。P570 は P214_ENABLE/P533_ENABLE に
         * 従属しない設計なので、P533 休止時も本プローブ自体は出力し続けられる
         * よう 0 で埋める(その場合 chsz=0 は「P533 休止」を意味する)。 */
        unsigned p570_chsz = 0;
#if P533_ENABLE
        p570_chsz = s_p533_chsize_calls;
#endif
        debug_log("[P570-GEOM] f=%d dw=%d dh=%d fbw=%d fbh=%d "
                  "tdx_b=%d tdx_e=%d tdy_b=%d tdy_e=%d "
                  "hs_b=%u he_b=%u vs_b=%u ve_b=%u hs_e=%u he_e=%u vs_e=%u ve_e=%u "
                  "r00=%02x r01=%02x r08=%02x r09=%02x "
                  "r28_b=%02x r29_b=%02x r28_e=%02x r29_e=%02x "
                  "vstep_b=%u vstep_e=%u chsz=%u rows_drawn=%d rows_total=%d "
                  "r00_b=%d r04_b=%d "
                  "hscale=%.4f vscale=%.4f offxf=%.4f offyf=%.4f geo_mode=%d\n",
                  g_mx68k_frame_num, disp_w, disp_h, s_fb_w[back], s_fb_h[back],
                  s_p570_tdx_b, TextDotX, s_p570_tdy_b, TextDotY,
                  (unsigned)s_p570_hs_b, (unsigned)s_p570_he_b,
                  (unsigned)s_p570_vs_b, (unsigned)s_p570_ve_b,
                  (unsigned)CRTC_HSTART, (unsigned)CRTC_HEND,
                  (unsigned)CRTC_VSTART, (unsigned)CRTC_VEND,
                  (unsigned)CRTC_Regs[0x00], (unsigned)CRTC_Regs[0x01],
                  (unsigned)CRTC_Regs[0x08], (unsigned)CRTC_Regs[0x09],
                  (unsigned)s_p570_r28_b, (unsigned)s_p570_r29_b,
                  (unsigned)CRTC_Regs[0x28], (unsigned)CRTC_Regs[0x29],
                  (unsigned)s_p570_vstep_b, (unsigned)CRTC_VStep,
                  p570_chsz, p570_rows_drawn, disp_h,
                  (int)s_p570_r00_b, (int)s_p570_r04_b,
                  (double)s_render_hscale, (double)s_render_vscale,
                  (double)s_render_offx, (double)s_render_offy,
                  s_render_geomode);
    }
#endif
#if P534_ENABLE
    /* P534 [P534-GVWCOL]: 毎フレーム 1 行・トリガ条件なし・フレーム番号による
     * 間引きなし。前置12フィールド(f= から fcfire= まで)は MPX68K の
     * [R1-GVWCOL] と同名・同順・同書式で、タグを落とせば行単位 diff できる。
     * MX 独自の 3 フィールド(col496_zero/col496_nonzero/r28)は末尾に追記。
     * 差分は「今フレーム分」、cum_* は累積(フレーム欠落の検出用)。
     * scrX/scrY は MPX と同じくマスク前の生値を出す。 */
    {
        static unsigned long lc = 0, lw = 0, lk = 0, lz = 0, ln = 0;
        unsigned long cum_col = s_p534_col496_zero + s_p534_col496_nonzero;
        debug_log("[P534-GVWCOL] f=%d col496_511=%lu wr256=%lu calls=%lu "
                  "cum_col=%lu cum_wr256=%lu scrX=[%u,%u,%u,%u] scrY=[%u,%u,%u,%u] "
                  "CRTC_Mode=0x%02x CRTC_R29=0x%02x fcfire=%lu "
                  "col496_zero=%lu col496_nonzero=%lu r28=0x%02x\n",
                  g_mx68k_frame_num,
                  cum_col - lc, s_p534_wr256 - lw, s_p534_calls - lk,
                  cum_col, s_p534_wr256,
                  (unsigned)GrphScrollX[0], (unsigned)GrphScrollX[1],
                  (unsigned)GrphScrollX[2], (unsigned)GrphScrollX[3],
                  (unsigned)GrphScrollY[0], (unsigned)GrphScrollY[1],
                  (unsigned)GrphScrollY[2], (unsigned)GrphScrollY[3],
                  (unsigned)CRTC_Mode, (unsigned)CRTC_Regs[0x29], s_p534_fcfire,
                  s_p534_col496_zero - lz, s_p534_col496_nonzero - ln,
                  (unsigned)CRTC_Regs[0x28]);
        lc = cum_col; lw = s_p534_wr256; lk = s_p534_calls;
        lz = s_p534_col496_zero; ln = s_p534_col496_nonzero;
    }
#endif
}

/* P353: BG0/BG1の表示をモニタUIから手動でON/OFFする(デバッグ専用)。
 * ゲーム自身が読むBG_Regs[9]の実体は変更しない——描画直前に一時的に
 * ビットクリアし描画直後に復元する既存P281の安全パターンを、0.5秒
 * 自動巡回ではなくこのフラグの継続的な参照に置き換えている。デフォルト
 * は両方表示(1)、実機と同じ挙動(byte-identical)。単純なintフラグの
 * Swift(メインスレッド書込み)→描画ループ(CVDisplayLinkスレッド読取り)
 * 間の無同期共有は、既存のg_clock_mhz(mx68k_set_clock、EmulatorBridge.c
 * 267行目)と同じ確立済みパターン。 */
static int s_p353_bg0_visible = 1;
static int s_p353_bg1_visible = 1;

void mx68k_set_bg_layer_visible(int layer, int visible) {
    if (layer == 0) s_p353_bg0_visible = visible ? 1 : 0;
    else if (layer == 1) s_p353_bg1_visible = visible ? 1 : 0;
}

int mx68k_get_bg_layer_visible(int layer) {
    if (layer == 0) return s_p353_bg0_visible;
    if (layer == 1) return s_p353_bg1_visible;
    return 1;   /* 未知のlayerはデフォルトの「表示」を返す */
}

#if P351_ENABLE
/* mx68k_get_bg_page_rgba(P348)と同じ再利用パターンでBG0の全1024行を
 * スキャンし、代表色が変化する境界だけをログする。ユーザーの実測
 * (BG0Y=489・VLINE=50→参照行539)が空色帯か路面縞模様かを直接特定する
 * 目的(OverTake調査)。5秒に1回発火。呼出し元(mx68k_draw_display_line
 * 末尾、合成完了後)でBG_LineBuf32/Text_TrFlagを汚しても、当該VLINEの
 * 実合成は既に完了済みで、次走査線は毎回フレッシュにmemsetされるため
 * 無害。 */
static void p351_bg0_rowscan_probe(void) {
    if (g_mx68k_frame_num % 300 != 0) return;
    extern void bg_drawline_loopx16(uint16_t, uint32_t, uint32_t, int32_t, int32_t);
    extern void bg_drawline_loopx8(uint16_t, uint32_t, uint32_t, int32_t, int32_t);
    int size = BG_CHRSIZE * 64;
    uint16_t bgtop = BG_BG0TOP;
    int32_t save_vlinebg = VLINEBG, save_bgvline = BG_VLINE;
    int save_textdotx = TextDotX;
    debug_log("[P351-BG0VLINE] f=%d BG_VLINE=%d BG_CHRSIZE=%d\n",
              g_mx68k_frame_num, BG_VLINE, BG_CHRSIZE);
    BG_VLINE = 0;
    TextDotX = size;
    char rowbuf[1800]; int rp = 0;
    uint32_t last_repr = 0xFFFFFFFFu;   /* 番兵: 実際のARGB値と衝突しない */
    for (int line = 0; line < size; line++) {
        VLINEBG = line;
        memset(BG_LineBuf32 + 16, 0, sizeof(uint32_t) * size);
        memset(Text_TrFlag + 16, 0, size);
        if (BG_CHRSIZE == 8) bg_drawline_loopx8(bgtop, 0, 0, 0, 0);
        else                 bg_drawline_loopx16(bgtop, 0, 0, 0, 0);
        uint32_t repr = 0;   /* 0 = この行は不透明ピクセル無し */
        for (int x = 0; x < size; x++) {
            if ((Text_TrFlag[16 + x] & 2) && BG_LineBuf32[16 + x] != 0) {
                repr = BG_LineBuf32[16 + x];
                break;
            }
        }
        if (repr != last_repr && rp < 1750) {
            rp += snprintf(rowbuf + rp, sizeof(rowbuf) - rp, "%d:%06x,",
                            line, px68k_color_to_rgba(repr) & 0x00FFFFFFu);
            last_repr = repr;
        }
    }
    debug_log("[P351-BG0ROW] f=%d bands=%s\n", g_mx68k_frame_num, rowbuf);
    VLINEBG = save_vlinebg; BG_VLINE = save_bgvline; TextDotX = save_textdotx;
}
#endif

#if P430_ENABLE
/* [P430-BGIDX0] D-5調査(BGパレットindex0透明判定欠落説)の実測カウンタ。
 * mx68k_draw_display_line は走査線1本ごとに呼ばれるので、1フレーム分を
 * ここへ積算し、フレーム境界(g_mx68k_frame_num の変化)で1行だけ出力する。
 * 全て読み取り結果の集計のみ——エミュレーション状態には一切書き戻さない。 */
static int s_p430_last_frame  = -1;  /* 積算中のフレーム番号(-1 = 未開始) */
static int s_p430_scanned     = 0;   /* 分母1: 走査した画素総数 */
static int s_p430_bg_drawn    = 0;   /* 分母2: BG面が描いた画素数(Text_TrFlag&2) */
static int s_p430_marker_hit  = 0;   /* 分子: 値が index0 マーカーと一致した画素数 */
/* has_bg は BG0 単独ではなく BG0+BG1+スプライトが共有する Text_TrFlag bit1
 * に基づく合成判定(EmulatorBridge.c の合成ループ参照)。BG0 のみの数では
 * ないため、marker_hit との比較時はこの共有性を前提に読むこと。 */
static int s_p430_has_bg      = 0;
/* 経路フラグ: BG描画分岐に入らないフレームでも記録できるよう、分岐の外
 * (bg_above_text 算出直後)で毎走査線に更新する。 */
static int s_p430_bg_above_text = -1;
static int s_p430_bg0_on        = -1;
static int s_p430_sp_on         = -1;
/* 出力頻度制御: 値に変化があったフレーム、または60フレーム毎のみ出力。 */
static int s_p430_prev_bg_above_text = -2;
static int s_p430_prev_bg0_on        = -2;
static int s_p430_prev_sp_on         = -2;
static int s_p430_prev_scanned       = -1;
static int s_p430_prev_bg_drawn      = -1;
static int s_p430_prev_marker_hit    = -1;
static int s_p430_prev_has_bg        = -1;

/* フレーム境界で1フレーム分の集計を出力し、カウンタを初期化する。
 * mx68k_draw_display_line の先頭(分岐に入る前)から毎走査線呼ばれるため、
 * BG描画分岐が一度も実行されないフレームも取りこぼさない。 */
static void p430_frame_boundary(void) {
    if (g_mx68k_frame_num == s_p430_last_frame) return;
    if (s_p430_last_frame >= 0) {
        int changed = (s_p430_bg_above_text != s_p430_prev_bg_above_text) ||
                      (s_p430_bg0_on        != s_p430_prev_bg0_on)        ||
                      (s_p430_sp_on         != s_p430_prev_sp_on)         ||
                      (s_p430_scanned       != s_p430_prev_scanned)       ||
                      (s_p430_bg_drawn      != s_p430_prev_bg_drawn)      ||
                      (s_p430_marker_hit    != s_p430_prev_marker_hit)    ||
                      (s_p430_has_bg        != s_p430_prev_has_bg);
        if (changed || (s_p430_last_frame % 60 == 0)) {
            debug_log("[P430-BGIDX0] f=%d bg_above_text=%d bg0_on=%d sp_on=%d"
                      " scanned=%d bg_drawn=%d marker_hit=%d has_bg=%d(BG0+BG1+SP合算)\n",
                      s_p430_last_frame, s_p430_bg_above_text, s_p430_bg0_on,
                      s_p430_sp_on, s_p430_scanned, s_p430_bg_drawn,
                      s_p430_marker_hit, s_p430_has_bg);
            s_p430_prev_bg_above_text = s_p430_bg_above_text;
            s_p430_prev_bg0_on        = s_p430_bg0_on;
            s_p430_prev_sp_on         = s_p430_sp_on;
            s_p430_prev_scanned       = s_p430_scanned;
            s_p430_prev_bg_drawn      = s_p430_bg_drawn;
            s_p430_prev_marker_hit    = s_p430_marker_hit;
            s_p430_prev_has_bg        = s_p430_has_bg;
        }
    }
    s_p430_scanned = s_p430_bg_drawn = s_p430_marker_hit = s_p430_has_bg = 0;
    s_p430_bg_above_text = s_p430_bg0_on = s_p430_sp_on = -1;
    s_p430_last_frame = g_mx68k_frame_num;
}
#endif

/* P501-D45: BG/テキスト面半透明の50%合成。MX Core 自身の32bit版TR実装
 * (Core/px68k/x68k/gvram.c:792-798、Grp_DrawLine8TR 内)と同一算法。
 * 参照実装3種(MPX68K/px68k本家/px68k-libretro)は16bitパレット版であり、
 * その Ibit(16bit固有の輝度補正ビット)関連項は MX Core に存在しないため移植しない
 * (Core/px68k/x68k/palette.h:25 で Ibit/Pal_Ix2 はコメントアウト済み)。 */
static inline uint32_t p501_blend_half(uint32_t a, uint32_t b) {
    a >>= 8; b >>= 8;
    uint32_t r = ((((a & 0x00ff0000) + (b & 0x00ff0000)) >> 1) & 0x00ff0000)
               | ((((a & 0x0000ff00) + (b & 0x0000ff00)) >> 1) & 0x0000ff00)
               | ((((a & 0x000000ff) + (b & 0x000000ff)) >> 1) & 0x000000ff);
    return (r << 8) & Pal32_FullMask;
}

/* P294: 表示行1本(現在の VLINE 行)を描画。exec ループ LINE-END の
 * p214_r1_sample_line 直後から、CRTC_VStep 分岐で呼ばれる(MPX winx68k.cpp:544-554)。
 * 旧 mx68k_render_frame() の全走査線ループ本体1回分。VLINE は LINE-START で設定済み。 */
static void mx68k_draw_display_line(void) {
    if (VLINE < 0 || VLINE >= s_render_disp_h) return;   /* 表示範囲・バッファ境界ガード */
#if P430_ENABLE
    p430_frame_boundary();
#endif
    int y = VLINE;
    VLINEBG = VLINE;                 /* bg.c 参照。x68k_vline は触らない(§2.3-2: P214-R2 サンプラ整合) */
    uint8_t *fb = s_compose_fb;   /* P535: 永続合成バッファへ描く */
    int disp_w  = s_render_disp_w;
    int disp_h  = s_render_disp_h;
    int fb_call_count = s_render_fb_call_count;
    (void)disp_h; (void)fb_call_count;   /* P281/P282/P283 無効時は診断 probe のみが参照 */
#if P467_ENABLE
    /* [P467-GVXPROF] D-35症状2: VLINE=150 の走査線について GVRAM の生ワード値を
     * x=0,16,32,...,240 の17点でダンプする(導出後の画素色ではなく生データ)。
     * 「消えた/残った」の境界がワード境界・ページ境界(0x20000単位)・CRTC矩形
     * 境界のどれと一致するかを、後からログだけで判定できるようにするため。
     *
     * ★Code Review条件1: アドレス導出は Core/px68k/x68k/gvram.c:64-71 の
     * GVRAM_FastClear() の走査式を verbatim に踏襲する(独自再導出はしない)。
     *   gvram.c:64  offy = (GrphScrollY[0] & 0x1ff) << 10;
     *   gvram.c:66  offx = GrphScrollX[0] & 0x1ff;
     *   gvram.c:67  p    = (uint16_t *)(GVRAM + offy + offx * 2);
     *   gvram.c:71  offx = (offx + 1) & 0x1ff;      (1画素ごと)
     *   gvram.c:74  offy = (offy + 0x400) & 0x7fc00; (1行ごと)
     * これを閉形式にしたものが下の offy_row / offx_col:
     *   offy(row) = (((GrphScrollY[0] & 0x1ff) + row) & 0x1ff) << 10
     *   offx(col) = ((GrphScrollX[0] & 0x1ff) + col) & 0x1ff
     * 読み出しアドレスは GVRAM + offy(150) + offx(col)*2。
     *
     * 範囲内であることの根拠(このマスク処理が境界外参照を防ぐ):
     *   offy      <= 0x1ff << 10   = 0x7FC00
     *   offx * 2  <= 0x1ff * 2     = 0x003FE
     *   最大バイトオフセット = 0x7FFFE、16bit読み出しの最終バイトは 0x7FFFF で
     *   GVRAM[0x80000](gvram.h:6)の範囲内に収まる。offy は 0x400 の倍数・
     *   offx*2 は偶数なのでアドレスは常に 2 バイト整列。
     * 参照専用 —— GVRAM への書込みは行わない。 */
    if ((VLINE == 150) && (fb_call_count % 10 == 0)) {
        uint32_t p467_offy = ((((GrphScrollY[0] & 0x1ff) + 150u) & 0x1ff) << 10);
        char p467_buf[512];
        p467_buf[0] = '\0';   /* 17点 × 約18文字 = 約306文字で収まるが、途中break時も終端を保証 */
        int p467_len = 0;
        for (int p467_col = 0; p467_col <= 240; p467_col += 16) {
            uint32_t p467_offx = (((GrphScrollX[0] & 0x1ff) + (uint32_t)p467_col) & 0x1ff);
            uint32_t p467_addr = p467_offy + p467_offx * 2u;
            uint16_t p467_word = *(const uint16_t *)(GVRAM + p467_addr);
            int p467_n = snprintf(p467_buf + p467_len, sizeof(p467_buf) - (size_t)p467_len,
                                  "%s%d:0x%05x=0x%04x",
                                  (p467_len ? "," : ""), p467_col,
                                  (unsigned)p467_addr, (unsigned)p467_word);
            if (p467_n < 0 || (size_t)p467_n >= sizeof(p467_buf) - (size_t)p467_len) break;
            p467_len += p467_n;
        }
        debug_log("[P467-GVXPROF] f=%d VLINE=150 scrX=%u scrY=%u offy=0x%05x pts=%s\n",
                  fb_call_count, (unsigned)GrphScrollX[0], (unsigned)GrphScrollY[0],
                  (unsigned)p467_offy, p467_buf);
    }
#endif
#if P468_ENABLE
    /* [P468-GVXPROF2] D-35症状2: 仮説 H-B(page1 側スクロールレジスタが一度も
     * 観測されていない)の切り分け用。[P467-GVXPROF] と同一のゲート
     * (VLINE==150 && fb_call_count%10==0)に相乗りし、
     *   (a) GrphScrollX[0..3] / GrphScrollY[0..3] / VCReg2[0] / VCReg2[1] /
     *       CRTC_Regs[0x29] を「生値のまま」
     *   (b) page1(奇数バイト面)側の生ワードダンプ
     * を出力する。派生値・要約は一切行わない —— H-B の判定は読者がこれらの
     * 生値の時系列変化を直接見て行うため。
     *
     * アドレス導出は Core/px68k/x68k/gvram.c:306-321 の Grp_DrawLine8(page, opaq)
     * を page=1 固定で verbatim に複製したもの(独自再導出はしない):
     *   gvram.c:306  page &= 1;                          -> page = 1
     *   gvram.c:308  y = GrphScrollY[page*2] + VLINE;     -> GrphScrollY[2] + 150
     *   gvram.c:310-313 if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) { y += VLINE; }
     *   gvram.c:314  y = ((y & 0x1ff) << 10) + page;      -> ... + 1
     *   gvram.c:317  x = GrphScrollX[page*2] & 0x1ff;     -> GrphScrollX[2] & 0x1ff
     *   gvram.c:321  srcp = (uint16_t *)(GVRAM + y + x * 2);  1 画素ごとに srcp++
     * これを col 番目の点の閉形式にしたものが下の p468_ybase / p468_offx:
     *   ybase     = (((GrphScrollY[2] + 150 (+150)) & 0x1ff) << 10) + 1
     *   offx(col) = ((GrphScrollX[2] & 0x1ff) + col) & 0x1ff
     *   addr(col) = ybase + offx(col) * 2
     * (offx の 0x1ff 折り返しは gvram.c:324 以降の 2 分割ループと等価)
     * 添字は page*2 = 2(主スクロール位置)であって page = 1 ではない —— ここを
     * 誤ると常に page0 のレジスタを読み、H-B が偽の結論になる。
     *
     * 範囲内であることの根拠:
     *   ybase    <= (0x1ff << 10) + 1 = 0x7FC01
     *   offx * 2 <= 0x1ff * 2         = 0x003FE
     *   最大バイトオフセット = 0x7FFFF で GVRAM[0x80000](gvram.h:6)の範囲内。
     * page=1 では addr が常に奇数になるため、16bit 値は未整列 uint16_t 参照では
     * なく gvram.c:27 の GET_WORD_W8(リトルエンディアン: src[0] | src[1]<<8)と
     * 同じバイト単位読み出しで再現する。addr+1 が sizeof(GVRAM) に達する場合
     * だけ上位バイトを 0 として扱う(実機の Grp_DrawLine8 は折り返しループ側で
     * この 1 バイトに到達しない)。
     * 参照専用 —— GVRAM・レジスタへの書込みは一切行わない。 */
    if ((VLINE == 150) && (fb_call_count % 10 == 0)) {
        uint32_t p468_y = GrphScrollY[2] + 150u;
        if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) p468_y += 150u;
        uint32_t p468_ybase = ((p468_y & 0x1ffu) << 10) + 1u;   /* +1 = page(=1) */
        uint32_t p468_x0 = GrphScrollX[2] & 0x1ffu;
        char p468_buf[512];
        p468_buf[0] = '\0';   /* 17点 × 約18文字 = 約306文字で収まるが、途中break時も終端を保証 */
        int p468_len = 0;
        for (int p468_col = 0; p468_col <= 240; p468_col += 16) {
            uint32_t p468_offx = (p468_x0 + (uint32_t)p468_col) & 0x1ffu;
            uint32_t p468_addr = p468_ybase + p468_offx * 2u;
            unsigned p468_lo = (unsigned)GVRAM[p468_addr];
            unsigned p468_hi = ((p468_addr + 1u) < (uint32_t)sizeof(GVRAM))
                             ? (unsigned)GVRAM[p468_addr + 1u] : 0u;
            unsigned p468_word = p468_lo | (p468_hi << 8);
            int p468_n = snprintf(p468_buf + p468_len, sizeof(p468_buf) - (size_t)p468_len,
                                  "%s%d:0x%05x=0x%04x",
                                  (p468_len ? "," : ""), p468_col,
                                  (unsigned)p468_addr, p468_word);
            if (p468_n < 0 || (size_t)p468_n >= sizeof(p468_buf) - (size_t)p468_len) break;
            p468_len += p468_n;
        }
        debug_log("[P468-GVXPROF2] f=%d VLINE=150 scrX=[%u,%u,%u,%u] scrY=[%u,%u,%u,%u] "
                  "VCReg2_0=0x%02x VCReg2_1=0x%02x CRTC_R29=0x%02x "
                  "p1_ybase=0x%05x p1_pts=%s\n",
                  fb_call_count,
                  (unsigned)GrphScrollX[0], (unsigned)GrphScrollX[1],
                  (unsigned)GrphScrollX[2], (unsigned)GrphScrollX[3],
                  (unsigned)GrphScrollY[0], (unsigned)GrphScrollY[1],
                  (unsigned)GrphScrollY[2], (unsigned)GrphScrollY[3],
                  (unsigned)VCReg2[0], (unsigned)VCReg2[1],
                  (unsigned)CRTC_Regs[0x29], (unsigned)p468_ybase, p468_buf);
    }
#endif
#if P358_ENABLE
    if ((VLINE == 50 || VLINE == 100 || VLINE == 150 || VLINE == 200 || VLINE == 250) &&
        (g_mx68k_frame_num % 60 == 0)) {
        debug_log("[P358-END] f=%d VLINE=%d VC21=%02x\n",
                  g_mx68k_frame_num, VLINE, VCReg2[1]);
    }
#endif
#ifndef P359_ENABLE
#define P359_ENABLE 1   /* OverTake BG0タイルマップバイト実測用、一時的に有効化 */
#endif
#if P359_ENABLE
    if ((VLINE == 200) && (g_mx68k_frame_num % 60 == 0) && (BG_Regs[9] & 1)) {
        /* bg.c:392-403 (BG_CHRSIZE==8) / 432-443 (==16) と同じ計算式を
         * 読み取り専用で再現する。bg.cは変更しない。
         * ★Code Review round1指摘の反映: CHRSIZE==16のadjustは呼出し元の
         * gdフラグ次第で0になる(bg.c:481 gd=1→BG_HAdjust / 493-494 gd=0→0)。
         * gdは`bg_above_text`分岐で決まる(EmulatorBridge.c:4553 gd=0 / 4572
         * gd=1)。この挿入位置は`bg_above_text`計算(関数内で後で行われる、
         * 約4414行目)より前なので、同じ1行の式をこの場でも独立に再計算する
         * (読み取りのみ、副作用なし)。CHRSIZE==8側は両gdともBG_HAdjust
         * 固定(bg.c:475,487)なので分岐不要。 */
        int p359_bg_above_text = (((VCReg1[0] & 0x30) >> 2) < (VCReg1[0] & 0x0c));
        uint32_t edx, ecx;
        int32_t step;
        if (BG_CHRSIZE == 8) {
            edx = BG_BG0TOP + (((BG0ScrollY + VLINEBG - BG_VLINE) & 0x1f8) << 4);
            ecx = ((BG0ScrollX - BG_HAdjust) & 0x1f8) >> 2;
            step = TextDotX >> 3;
        } else {
            int32_t adjust16 = p359_bg_above_text ? 0 : BG_HAdjust;   /* Code Review round1 */
            edx = BG_BG0TOP + (((BG0ScrollY + VLINEBG - BG_VLINE) & 0x3f0) << 3);
            ecx = ((BG0ScrollX - adjust16) & 0x3f0) >> 3;
            step = TextDotX >> 4;
        }
        char buf[512]; int bp = 0;
        int n = (step < 40) ? step : 40;   /* ログ行が長くなりすぎないよう上限 */
        for (int i = 0; i <= n; i++) {
            /* ★Code Review round1指摘の反映: BG[ecx+edx]はflip/パレット属性
             * バイト(bg.c:409-421のbl、パレット上位ニブルになる)、
             * BG[ecx+edx+1]がキャラクタ/パターン番号(bg.c:407/447のsi、
             * BGCHR8/16へのインデックス)——当初案はラベルが逆だった。 */
            uint8_t attr = BG[(ecx & 0x7f) + edx];
            uint8_t pat  = BG[(ecx & 0x7f) + edx + 1];
            if (bp < 500) bp += snprintf(buf + bp, sizeof(buf) - bp, "%02x/%02x ", attr, pat);
            ecx += 2;
        }
        debug_log("[P359-BGTILE] f=%d VLINE=%d CHRSIZE=%d BG0X=%u BG0Y=%u HAdj=%d BGVL=%d"
                  " VLBG=%d BG0TOP=%04x TextDotX=%d bg_above_text=%d n=%d tiles(attr/pat)=%s\n",
                  g_mx68k_frame_num, VLINE, BG_CHRSIZE, BG0ScrollX, BG0ScrollY, BG_HAdjust,
                  BG_VLINE, VLINEBG, BG_BG0TOP, TextDotX, p359_bg_above_text, n, buf);
    }
#endif
#ifndef P360_ENABLE
#define P360_ENABLE 1   /* OverTake BG0単体不透明画素カウンタ実測用、一時的に有効化 */
#endif
#if P360_ENABLE
    if ((VLINE == 200) && (g_mx68k_frame_num % 60 == 0) && (BG_Regs[9] & 1) && (BG_CHRSIZE == 8)) {
        /* bg.c:400-407(タイル取得)・409-421(方向分岐)・379-390(NG展開)を
         * 読み取り専用で複製。bg.cは呼ばない・変更しない。
         * bg_above_textは前ブロック(P359)と同じ理由でこの場で独立再計算する。 */
        int p360_bg_above_text = (((VCReg1[0] & 0x30) >> 2) < (VCReg1[0] & 0x0c));
        int32_t adjust = BG_HAdjust;   /* CHRSIZE==8は両gdともBG_HAdjust固定(bg.c:475,487、P359 Code Review round1で確認済み) */
        uint32_t ebp = ((BG0ScrollY + VLINEBG - BG_VLINE) & 7) << 3;
        uint32_t edx = BG_BG0TOP + (((BG0ScrollY + VLINEBG - BG_VLINE) & 0x1f8) << 4);
        uint32_t ecx = ((BG0ScrollX - adjust) & 0x1f8) >> 2;
        int step = TextDotX >> 3;
        int n = (step < 40) ? step : 40;
        int opaque_count = 0;
        int color_opaque_count = 0;   /* P361: パレット変換後の色値が非ゼロの画素数 */
        for (int i = 0; i <= n; i++) {
            uint8_t bl  = BG[(ecx & 0x7f) + edx];
            uint16_t si = (uint16_t)BG[(ecx & 0x7f) + edx + 1] << 6;
            uint8_t bl_shifted = (uint8_t)(bl << 4);   /* P361: bg.c:381と同じ */
            const uint8_t *esi; int32_t d;
            if (bl < 0x40)                  { esi = &BGCHR8[si + ebp];        d = +1; }
            else if ((uint8_t)(bl - 0x40) & 0x80) { esi = &BGCHR8[si + 0x3f - ebp]; d = -1; }
            else if ((int8_t)bl >= 0x40)     { esi = &BGCHR8[si + ebp + 7];    d = -1; }
            else                              { esi = &BGCHR8[si + 0x38 - ebp]; d = +1; }
            for (int j = 0; j < 8; j++, esi += d) {
                uint8_t nib = *esi & 0xf;
                if (nib) {
                    opaque_count++;
                    uint8_t dat = nib | bl_shifted;   /* P361: bg.c:385と同じ */
                    if (TextPal32[dat] != 0) color_opaque_count++;   /* P361 */
                }
            }
            ecx += 2;
        }
        debug_log("[P360-BG0OPAQUE] f=%d VLINE=%d bg_above_text=%d n_tiles=%d opaque_count=%d"
                  " color_opaque_count=%d (of %d px scanned)\n",
                  g_mx68k_frame_num, VLINE, p360_bg_above_text, n, opaque_count,
                  color_opaque_count, (n + 1) * 8);
    }
#endif
    int save_textdotx = TextDotX;    /* P294 §2.3-3: 描画直前だけ TextDotX を局所クランプ */
    if (TextDotX > 1024) TextDotX = 1024;

    {
        // Clear line buffers for this scanline.
        memset(Grp_LineBuf32, 0, sizeof(uint32_t) * disp_w);
        memset(BG_LineBuf32 + 16, 0, sizeof(uint32_t) * disp_w);
        memset(Text_TrFlag + 16, 0, disp_w);
        int sp_active = 0;   // P284: この走査線で graphic 特殊優先 (SP) が発火したか

        // Graphics layer — P184: px68k WinDraw_DrawLine 準拠 (windraw.c:514)。
        // mode = VCReg0[1]&3 (0=16c, 1/2=256c, 3=65536c), &4=1024dot。各ページは VCReg2[1] bit で gate、
        // 優先度は VCReg1[1] の 2bit フィールド、最初に描くページのみ opaq=1(不透明ベース)。
        // SP(特殊優先)/TR(半透明)変種は未対応(follow-up)。
        if (VCReg2[1] & 0x0F) {   // いずれかのグラフィックページ有効
            switch (VCReg0[1] & 3) {
                case 0: // 16 色
                    if (VCReg0[1] & 4) {          // 1024 dot 高解像
                        Grp_DrawLine4h();
                    } else {                       // 512 dot: 4 ページ
                        int opaq = 1;
                        if (VCReg2[1] & 8) { Grp_DrawLine4((VCReg1[1] >> 6) & 3, 1);    opaq = 0; }
                        if (VCReg2[1] & 4) { Grp_DrawLine4((VCReg1[1] >> 4) & 3, opaq); opaq = 0; }
                        if (VCReg2[1] & 2) { Grp_DrawLine4((VCReg1[1] >> 2) & 3, opaq); opaq = 0; }
                        if (VCReg2[1] & 1) { Grp_DrawLine4((VCReg1[1]      ) & 3, opaq); }
                    }
                    break;
                case 1:
                case 2: // 256 色 (2 ページ・VCReg1[1] 比較で GRP0/1 優先)
                    {
                        int opaq = 1;
                        if ((VCReg1[1] & 3) <= ((VCReg1[1] >> 4) & 3)) {   // GRP0 優先
                            // P284: graphic 特殊優先 (SP)。256色 page0 が SP 対象のとき
                            // ((VCReg2[0]&0x10) && (VCReg2[1]&1) — P283 プローブで実測発火確認)、
                            // page0 を Grp_DrawLine8SP(0) で SP(前半)/SP2(背半) バッファへ振り分ける。
                            // MPX windraw.c:583 準拠。SP/SP2 は Bridge が従来クリアしていないため、
                            // この経路が動くときだけ per-line で明示クリアする。
                            int grp_sp = (VCReg2[0] & 0x10) && (VCReg2[1] & 1);
                            if (grp_sp) {
                                memset(Grp_LineBuf32SP,  0, sizeof(uint32_t) * disp_w);
                                memset(Grp_LineBuf32SP2, 0, sizeof(uint32_t) * disp_w);
                                Grp_DrawLine8SP(0);
                                sp_active = 1;
                            }
                            /* P501-D45 修正A: GRP面同士の半透明(GG)。MPX windraw.c:590 準拠——
                             * (VCReg2[0]&0x1e)==0x1e かつ SP が発火している走査線でのみ、
                             * もう一方のページを TR 変種で描く。TDQ(D-45)の実測値では GG=0 の
                             * ため発火しないが、上流の GRP面同士半透明機構を MX 側にも整合させる。
                             * 注: 修正B(GT, 0x5d)とはビットレベルで排他ではない
                             * (例: VCReg2[0]=0x1F では 0x1F&0x1e==0x1e[GG成立] かつ
                             *  0x1F&0x5d==0x1D[GT成立] — 両立しうる)。バッファは別
                             * (本修正Aは Grp_LineBuf32 へ直接描画、修正Bは Grp_LineBuf32SP を
                             *  読むだけ)なので書込み競合は無いが、両方同時発火する画面での
                             * 見た目は hands-on 確認が必要。 */
                            if (VCReg2[1] & 4) {
                                if (((VCReg2[0] & 0x1e) == 0x1e) && sp_active) Grp_DrawLine8TR(1, 1);
                                else                                          Grp_DrawLine8(1, 1);
                                opaq = 0;
                            }
                            // MPX windraw.c:599: (VCReg2[0]&0x14)==0x14 のとき page0 通常描画を抑止
                            if ((VCReg2[1] & 1) && (VCReg2[0] & 0x14) != 0x14) { Grp_DrawLine8(0, opaq); }
                        } else {
                            // P417: GRP1優先分岐にもSP機構を配線(P284はGRP0優先分岐のみ
                            // 対応していた「適用範囲漏れ」の是正)。MPX windraw.c:608-630準拠。
                            // ★grp_sp条件はGRP0優先分岐と同じ VCReg2[1]&1 のまま(入れ替えない、
                            // Code ReviewがMPX実コードで確認済み)。
                            int grp_sp = (VCReg2[0] & 0x10) && (VCReg2[1] & 1);
                            if (grp_sp) {
                                memset(Grp_LineBuf32SP,  0, sizeof(uint32_t) * disp_w);
                                memset(Grp_LineBuf32SP2, 0, sizeof(uint32_t) * disp_w);
                                Grp_DrawLine8SP(1);
                                sp_active = 1;
                            }
                            /* P501-D45 修正A(GRP1優先側): MPX windraw.c:615 準拠。
                             * 条件・注意点は GRP0 優先側(上記)と同一、対象ページのみ 0。 */
                            if (VCReg2[1] & 4) {
                                if (((VCReg2[0] & 0x1e) == 0x1e) && sp_active) Grp_DrawLine8TR(0, 1);
                                else                                          Grp_DrawLine8(0, 1);
                                opaq = 0;
                            }
                            // MPX windraw.c:624: (VCReg2[0]&0x14)==0x14 のとき page1 通常描画を抑止
                            if ((VCReg2[1] & 1) && (VCReg2[0] & 0x14) != 0x14) { Grp_DrawLine8(1, opaq); }
                        }
                    }
                    break;
                case 3: // 65536 色
                    if (VCReg2[1] & 15) Grp_DrawLine16();
                    break;
            }
        }

        // BG / Sprites and text share one line buffer (BG_LineBuf32) and one flag byte
        // (Text_TrFlag: bit0=text tvram.c:259, bit1=sprite/BG bg.c:355), so the plane that
        // paints LAST owns the pixel color. P213: pick the order from the inter-plane
        // priority the way upstream does (windraw.c:645) instead of hardcoding one order —
        // with sprite in front of text (Super Xevious: VCReg1[0]=0x09 -> sp=0 tx=2) the
        // fixed order let the text plane overwrite sprite pixels.
        int text_on = (VCReg2[1] & 0x20) != 0;   // P161: text enable (VCReg2 b5 TXON)
        /* P436 (D-5): px68k上流(MPX68K windraw.c:664,681で確認)の sp 描画
         * ゲートのうち、BG_Regs[0x11]&2(h_res未定義値)判定を追加した。
         * ★P436時点のコメント「ゲート1(BG_Regs[8]&2、DISP)は実測で常に真」
         * は誤りと判明(P466、D-36として起票)——[P435-SPGATE]実測で
         * f=2400/2460(BG_Regs[8]=0x00、g_disp=0)という直接の反例を確認した。
         * [P435-SPGATE]実測: MMDSP終了後にBG_Regs[0x11]が未定義値(0xff)の
         * まま固定され、この間MX68Kのみ描画を続けていたことを確認済み。
         * ★P544 (D-36): 欠落していた DISP 項(BG_Regs[8]&0x02)を追加し、
         * 上流と同じ3条件ゲートにした(P436以来の据え置きはここで解消)。
         * 根拠: P541で参照実装3種(px68k本家/px68k-libretro/MPX68K)が
         * バイト一致で3条件ゲートを持つと確認。P542/P543のタイトル別実測で
         * 既知タイトルへの回帰なしと判断——After Burnerは4500フレーム乖離0、
         * OverTakeの唯一の乖離窓(VLINE=241-255)はGRP層が全幅を覆う
         * (n_gr=256 n_bg=0)ため視覚影響なし、他4タイトルはフレーム先頭
         * サンプリングで乖離0。 */
        int sp_on   = ((VCReg2[1] & 0x40) != 0) && ((BG_Regs[8] & 0x02) != 0)
                      && ((BG_Regs[0x11] & 0x02) == 0);   // P161/P436/P544(D-36): SPON + DISP + h_res定義済み(参照実装3種と一致)
        // Same condition as upstream windraw.c:645 ((VCReg1[0]&0x30)>>2) < (VCReg1[0]&0x0c),
        // i.e. sprite priority < text priority.
        int bg_above_text = (((VCReg1[0] & 0x30) >> 2) < (VCReg1[0] & 0x0c));

#if P543_ENABLE
        /* [P543-DISPWIN] D-36着手条件検証: 上流3条件ゲートの DISP 項
         * (BG_Regs[8]&0x02、P435-SPGATE の g1 と同一式)が落ちている間も
         * MX68K が描画を続けている走査線を、フレーム全表示期間にわたって
         * 数える。sp_on は既に g0(SPON)&& g2(h_res定義済み)を含むので、
         * sp_on && !disp_ok は「上流なら描画しない・MXは描画する」と同値。
         * 読み取りのみ —— sp_on も描画分岐も一切変更しない。 */
        {
            int disp_ok = (BG_Regs[8] & 0x02) != 0;
            s_p543_lines_drawn++;
            if (sp_on) s_p543_lines_mx_draws++;
            if (sp_on && !disp_ok) {
                s_p543_lines_would_block++;
                if (s_p543_first_block_vline < 0) s_p543_first_block_vline = VLINE;
                s_p543_last_block_vline = VLINE;
            }
        }
#endif

#if P435_ENABLE
        /* [P435-SPGATE] MPX68K(px68k上流 windraw.c:664,681)の3条件ゲートと
         * MX68K の sp_on(1条件)を同一行で比較する。読み取りのみ。 */
        if (VLINE == 0 && (fb_call_count % 60 == 0)) {
            int g0 = (VCReg2[1] & 0x40) != 0;
            int g1 = (BG_Regs[8]  & 0x02) != 0;
            int g2 = (BG_Regs[0x11] & 0x02) == 0;
            debug_log("[P435-SPGATE] f=%d g_spon=%d g_disp=%d g_hres_ok=%d "
                      "mpx_would_draw=%d mx_draws=%d BG8=%02x BG9=%02x BG10=%02x BG11=%02x\n",
                      fb_call_count, g0, g1, g2, (g0&&g1&&g2), g0,
                      BG_Regs[8], BG_Regs[9], BG_Regs[0x10], BG_Regs[0x11]);
        }
#endif

#if P430_ENABLE
        /* [P430-BGIDX0] 経路フラグの記録。BG描画分岐の外なので、分岐に入らない
         * フレーム(sp_on=0 等)でも値が残り、marker_hit=0 の解釈が一意に定まる。
         * BG_Regs[9] は分岐内で P281 が一時的に書き換えるため、ここ(書き換え前)
         * で読む。読み取りのみ。 */
        s_p430_bg_above_text = bg_above_text ? 1 : 0;
        s_p430_bg0_on        = (BG_Regs[9] & 1) ? 1 : 0;
        s_p430_sp_on         = sp_on ? 1 : 0;
#endif

#if P282_ENABLE
        if ((fb_call_count % 60 == 0) &&
            (VLINE == 50 || VLINE == 100 || VLINE == 150 || VLINE == 200 || VLINE == 250)) {
            debug_log("[P282] f=%d y=%d VCReg1_0=0x%02x bg_above_text=%d pri_sp=%d pri_tx=%d "
                      "sp_on=%d text_on=%d\n",
                      fb_call_count, y, VCReg1[0], bg_above_text,
                      (VCReg1[0]>>4)&3, (VCReg1[0]>>2)&3, sp_on, text_on);
        }
#endif

#ifndef P349_ENABLE
#define P349_ENABLE 1   /* OverTake調査: BG垂直スクロール実測用、一時的に有効化 */
#endif
#if P349_ENABLE
        if ((fb_call_count % 60 == 0) && (VLINE == 50)) {
            debug_log("[P349-BGSCROLL] f=%d BG0X=%u BG0Y=%u BG1X=%u BG1Y=%u"
                      " BG11=%02x BG0TOP=%04x BG1TOP=%04x\n",
                      fb_call_count, BG0ScrollX, BG0ScrollY, BG1ScrollX, BG1ScrollY,
                      BG_Regs[0x11], BG_BG0TOP, BG_BG1TOP);
        }
#endif

#if P283_ENABLE
        if ((fb_call_count % 60 == 0) && (y == 0)) {
            int p283_mode = VCReg0[1] & 3;
            int p283_sp_cond_16_65536 = (VCReg2[0]&0x14)==0x14;          /* 16色1024dot/65536色用 */
            int p283_sp_cond_256      = (VCReg2[0]&0x10)&&(VCReg2[1]&1); /* 256色用(After Burner想定) */
            debug_log("[P283] f=%d mode=%d VCReg0_1=0x%02x VCReg2_0=0x%02x VCReg2_1=0x%02x "
                      "sp_cond_256=%d sp_cond_16_65536=%d\n",
                      fb_call_count, p283_mode, VCReg0[1], VCReg2[0], VCReg2[1],
                      p283_sp_cond_256, p283_sp_cond_16_65536);
        }
#endif

#ifndef P357_ENABLE
#define P357_ENABLE 1   /* OverTake実画面黒帯の合成内訳実測用、一時的に有効化 */
#endif
#if P357_ENABLE
        if ((fb_call_count % 300 == 0) && (VLINE == 0)) {
            debug_log("[P357-GMODE] f=%d CRTC28=%02x mode=%d(0=16色 1=256色 2=Unknown 3=65536色)\n",
                      fb_call_count, CRTC_Regs[0x28], CRTC_Regs[0x28] & 3);
        }
#endif

        // Text layer — P197: draw the text plane OPAQUE when it is the only enabled
        // plane, so text index-0 fills the backdrop color (TextPal32[0]) instead of
        // being transparent (black). The X68000 backmost plane is the opaque base.
        // (SX-Window desktop is an index-0/index-1 dither; non-opaque lost the index-0
        // olive backdrop, turning the desktop deep-blue.) When graphics or sprite/BG
        // planes are also enabled they provide the base, so keep text transparent then.
        int text_opaq = ((VCReg2[1] & 0x0F) == 0) && ((VCReg2[1] & 0x40) == 0);
        /* P392: bg_above_text構成では上流(MPX68K x11/windraw.c:656)がGRPの有無に
         * 関わらず無条件にText_DrawLine_C(1)を呼ぶ——テキスト索引0が画面背景色
         * (TextPal32[0])を供給する実機挙動。OverTakeは空最下段バンドに索引0を
         * 使っており、上のヒューリスティックはGRP有効時に0になり背景色が失われて
         * いた([P391-BLACK]実測、.mx68k_cycles/P391_overtake_gameplay.log)。
         * bg_above_text構成ではtext→BG_DrawLineの順(:4785→:4811)で描画される
         * ため、Text_TrFlag bit1(BG/スプライトの「塗った」フラグ)がテキストの
         * 不透明描画(tvram.c:246の`=`代入)によって破壊される余地はない
         * (P391b調査でCore全体のText_TrFlag書込み箇所を確認済み)。 */
        if (bg_above_text) text_opaq = 1;

#ifndef P342_ENABLE
#define P342_ENABLE 1   /* OverTake空領域テキスト実測用、一時的に有効化 */
#endif
#if P342_ENABLE
        if (text_on && (fb_call_count % 60 == 0) &&
            (VLINE == 50 || VLINE == 100 || VLINE == 150 || VLINE == 200 || VLINE == 250)) {
            /* Text_DrawLineと同じアドレス計算(tvram.c:234-241参照、読み取りのみ) */
            int32_t ty = TextScrollY + VLINE;
            if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) ty += VLINE;
            ty = (ty & 0x3ff) << 10;
            int32_t tx = TextScrollX & 0x3ff;
            int32_t taddr = tx + ty;
            int nz = 0, total = (TextDotX < 64) ? TextDotX : 64;
            char hexbuf[200]; int hp = 0;
            for (int i = 0; i < total; i++) {
                uint8_t t = TextDrawWork[taddr + i] & 0xf;
                if (t) nz++;
                if (hp < 190) hp += snprintf(hexbuf + hp, sizeof(hexbuf) - hp, "%x", t);
            }
            debug_log("[P342-TEXTIDX] f=%d VLINE=%d TSY=%d TSX=%d taddr=%d"
                      " C16=%02x C17=%02x TextDotX=%d nz=%d/%d idx=%s\n",
                      fb_call_count, VLINE, TextScrollY, tx, taddr,
                      CRTC_Regs[0x16], CRTC_Regs[0x17], TextDotX, nz, total, hexbuf);
        }
#endif

#ifndef P346_ENABLE
#define P346_ENABLE 1   /* OverTake TextDrawWork全体スキャン用、一時的に有効化 */
#endif
#if P346_ENABLE
        if (VLINE == 0 && (fb_call_count % 300 == 0)) {
            char rowbuf[600]; int rp = 0;
            int total_nz_rows = 0;
            for (int ry = 0; ry < 1024; ry++) {
                int rownz = 0;
                for (int i = 0; i < 64; i++) {
                    if (TextDrawWork[ry * 1024 + i] & 0xf) rownz++;
                }
                if (rownz > 0) {
                    total_nz_rows++;
                    if (rp < 590) rp += snprintf(rowbuf + rp, sizeof(rowbuf) - rp, "%d,", ry);
                }
            }
            debug_log("[P346-ROWSCAN] f=%d total_nz_rows=%d rows=%s\n",
                      fb_call_count, total_nz_rows, rowbuf);
        }
#endif

        if (bg_above_text) {
            // Same order as upstream windraw.c:649/664. BG_DrawLine's opaq argument now
            // matches upstream exactly: !text_on floods the line with TextPal32[0]
            // (bg.c:499) only when the text plane is off, avoiding erasure of text drawn
            // just above when text_on=1 (D-35症状1修正、P466。旧コメントの「text_opaq
            // が単独所有」は本修正前の記述で、現在は誤り)。
            if (text_on) Text_DrawLine(text_opaq);
            if (sp_on) {
#if P281_ENABLE
                uint8_t p281_saved_regs9 = BG_Regs[9];
                if (!s_p353_bg1_visible) BG_Regs[9] &= ~0x08;   // BG1手動OFF
                if (!s_p353_bg0_visible) BG_Regs[9] &= ~0x01;   // BG0手動OFF
#endif
#if P350_ENABLE
                if (g_mx68k_frame_num % 300 == 0) {
                    debug_log("[P350-CONSUME] f=%d vl=%d BG0X=%u BG0Y=%u BG1X=%u BG1Y=%u\n",
                              g_mx68k_frame_num, VLINE, BG0ScrollX, BG0ScrollY,
                              BG1ScrollX, BG1ScrollY);
                }
#endif
#ifndef P362_ENABLE
#define P362_ENABLE 1   /* OverTake BG_DrawLine実書込み検証用、一時的に有効化 */
#endif
#if P362_ENABLE
                int p362_gate = (VLINE == 200) && (g_mx68k_frame_num % 60 == 0);
                int p362_before = 0;
                if (p362_gate) {
                    for (int x362 = 0; x362 < disp_w; x362++) {
                        if (Text_TrFlag[x362 + 16] & 2) p362_before++;
                    }
                }
#endif
                BG_DrawLine(!text_on, 0);   /* D-35症状1修正: MPX68K windraw.c:673の
                                               BG_DrawLine(!ton, 0)と一致させる。
                                               text_on=0の構成でのみ床(TextPal32[0])を
                                               敷くようになり、text_on=1では!text_on=0
                                               で現行とビット単位で同一(回帰なし)。 */
#if P362_ENABLE
                if (p362_gate) {
                    int p362_after = 0;
                    for (int x362 = 0; x362 < disp_w; x362++) {
                        if (Text_TrFlag[x362 + 16] & 2) p362_after++;
                    }
#if P281_ENABLE
                    debug_log("[P362-BGSRC] f=%d VLINE=%d before=%d after=%d delta=%d"
                              " BG_Regs9_eff=%02x p281_saved=%02x s353_bg0=%d s353_bg1=%d\n",
                              g_mx68k_frame_num, VLINE, p362_before, p362_after,
                              p362_after - p362_before, BG_Regs[9], p281_saved_regs9,
                              s_p353_bg0_visible, s_p353_bg1_visible);
#endif
                }
#endif
#if P281_ENABLE
                BG_Regs[9] = p281_saved_regs9;               // 復元
#endif
            }
        } else {
            if (sp_on) {
#if P281_ENABLE
                uint8_t p281_saved_regs9 = BG_Regs[9];
                if (!s_p353_bg1_visible) BG_Regs[9] &= ~0x08;   // BG1手動OFF
                if (!s_p353_bg0_visible) BG_Regs[9] &= ~0x01;   // BG0手動OFF
#endif
#if P350_ENABLE
                if (g_mx68k_frame_num % 300 == 0) {
                    debug_log("[P350-CONSUME] f=%d vl=%d BG0X=%u BG0Y=%u BG1X=%u BG1Y=%u\n",
                              g_mx68k_frame_num, VLINE, BG0ScrollX, BG0ScrollY,
                              BG1ScrollX, BG1ScrollY);
                }
#endif
#if P430_ENABLE
                /* [P430-BGIDX0] 呼出し直前: 各パレットブロック N(1..15)の
                 * index0 に対応する解決済み RGB 値を退避する。XM6 が
                 * REND_COLOR0 を立てる対象(パレットコード下位4bit==0)が
                 * MX68K 側でどの値として現れるかのマーカー候補。読み取りのみ。 */
                uint32_t p430_marker[16];
                for (int i = 1; i < 16; i++) p430_marker[i] = TextPal32[i * 0x10];
#endif
                /* P539: 参照実装3系統(px68k本家 x11/windraw.c:1209、px68k-libretro
                 * libretro/windraw.c:680、MPX68K x11/windraw.c:691)はいずれも
                 * BG_DrawLine(1, 1) —— 第1引数 opaq=1 で行全体を TextPal32[0]
                 * (画面背景色)で敷いてから BG/スプライトを重ねる「不透明下地」。 */
                BG_DrawLine(1, 1);
#if P430_ENABLE
                /* 呼出し直後(Text_DrawLine が同じ BG_LineBuf32 を上書きする前)に
                 * 表示範囲を走査する。分子 marker_hit と分母2種(scanned /
                 * bg_drawn)を同時に積算するので、marker_hit=0 でも「経路が
                 * 実行されていない」のか「実行されたが一致が無い」のかを区別できる。
                 * 既知の限界: 値比較のため、index 非0 の画素がたまたま同じ RGB 値を
                 * 持つ偶然の一致を数える可能性がある(一次スクリーニング用途)。 */
                for (int x430 = 0; x430 < disp_w; x430++) {
                    int off430 = x430 + 16;
                    s_p430_scanned++;
                    if (!(Text_TrFlag[off430] & 2)) continue;
                    s_p430_bg_drawn++;
                    uint32_t v430 = BG_LineBuf32[off430];
                    for (int i = 1; i < 16; i++) {
                        if (v430 == p430_marker[i]) { s_p430_marker_hit++; break; }
                    }
                }
#endif
#if P281_ENABLE
                BG_Regs[9] = p281_saved_regs9;               // 復元
#endif
            } else {
                /* P539: 参照実装の else 節(MPX x11/windraw.c:696-717)。
                 * スプライト/BGが無効でも背景色の下地は必ず敷かれる。 */
                if (text_on) {
                    for (int i = 16; i < TextDotX + 16; ++i) BG_LineBuf32[i] = TextPal32[0];
                }
                /* text_on==0 の memset は走査線先頭クリアで既に等価に済んでいる。 */
            }
            /* P539: 参照実装は Text_DrawLine_C(!bgon)。この分岐では bgon が
             * 両サブ分岐で必ず1になるため常に透過描画(0)。 */
            if (text_on) Text_DrawLine(0);
        }

        // Composite into s_framebuffer (RGBA8888, tightly packed disp_w x disp_h) — P172
        // P197: inter-plane priority from VCReg1[0] (smaller value = front-most, 0 = front).
        // Tie: text/BG plane wins (upstream order GRP<SP<TEXT). VCReg1[1] is intra-graphic-page
        // priority (used above), NOT inter-plane — the old (VCReg1[1]&1) read the wrong byte.
        uint8_t* dst = fb + (y * MX_COMPOSE_STRIDE) * 4;   /* P535: 固定ストライド */
        int pri_gr = (VCReg1[0]     ) & 3;   // graphic plane
        int pri_tx = (VCReg1[0] >> 2) & 3;   // text plane
        int pri_sp = (VCReg1[0] >> 4) & 3;   // sprite / background plane
        /* P501-D44: VC R1 の優先度値 3 は値 2 と同一ランク。生値 pri_* は温存する
         * (既存の [P282]/[P357-COMPOSE]/mx68k_get_status が生値を表示しているため)。
         * 一次情報源: XM6 vm/render.cpp:1049-1051 / px68k本家 x11/windraw.c:1275,1288,1306
         * / MPX68K x11/windraw.c:768,781,799(2独立実装が一致)。
         * 注: bg_above_text(:6390)は上流が生値比較のままなので変更しない。 */
#define VC_PRI_RANK(p)  ((p) == 3 ? 2 : (p))
        int rank_gr = VC_PRI_RANK(pri_gr);
        int rank_tx = VC_PRI_RANK(pri_tx);
        int rank_sp = VC_PRI_RANK(pri_sp);

        /* P501-D45 修正B: BG/テキスト面半透明(GT)モードの発火判定。走査線ごとに1回だけ評価。
         * 一次情報源: MPX68K x11/windraw.c:783,801,817,833,850,870(6箇所全て同一マスク 0x5d)。
         * :6860 付近の既存 SP 生値経路は (VCReg2[0]&0x5c)==0x14 で HP ビット(bit3=0x08)を
         * 0 に要求するのに対し、こちらは 0x5d マスクで HP=1 を要求する——数値上ビットレベルで
         * 排他であり、既存の D-19/D-20/D-24(After Burner・超連射68K・パロディウスだ!)が
         * 使う経路には影響しない。 */
        int tr_bg = sp_active && ((VCReg2[0] & 0x5d) == 0x1d);

        for (int x = 0; x < disp_w; x++) {
            uint32_t col = 0;
            uint32_t gcol = Grp_LineBuf32[x];
            // P284: SP 背半(偶数色)は graphic プレーン内で page1 の上に重なる(graphic 優先度のまま)
            if (sp_active && Grp_LineBuf32SP2[x]) gcol = Grp_LineBuf32SP2[x];
            int off = x + 16;
            uint32_t tcol = BG_LineBuf32[off];                 // text or bg pixel color
            // P217: on the X68000 color index 0 is transparent, so decide transparency
            // from the color value the way upstream windraw.c:271 (WD_SUB) does with
            // `w != 0`. The flag only says a plane painted the pixel, not that it is
            // opaque — bg.c:370 sets it even when it writes color 0.
            int has_text = (Text_TrFlag[off] & 1) && tcol;
            /* P539: 参照実装の WinDraw_DrawBGLine(opaq, td) は td==0 のとき
             * Text_TrFlag を見ず「値が非0なら書く」(MPX x11/windraw.c:402 _DBL_SUB2)。
             * bg_above_text==0 ⇔ td==0 が全構成で成立する(P539_plan.md 記号表参照)。
             * BG_LineBuf32 は走査線先頭のゼロクリアと本分岐の全書き込みにより当該走査線の
             * 値しか保持しないため、値のみ判定でstaleデータを拾う余地は無い。 */
            int has_bg   = tcol && (bg_above_text ? (Text_TrFlag[off] & 2) : 1);
#if P430_ENABLE
            /* [P430-BGIDX0] 最終合成に反映された画素数。Text_TrFlag bit1 は
             * BG0・BG1・スプライトが共有するため、この数は BG0 単独ではなく
             * BG0+BG1+スプライトの合算である(ログ表記にも明記)。 */
            if (has_bg) s_p430_has_bg++;
#endif
            /* P365: BG+Sprite合成バッファへ書込み — has_bgは最終的な優先度合成
             * (gcolとの比較)より前の、BG+スプライト面単体の実際の描画結果。
             * 透過画素は黒(色0)——TextDrawWork/BG Pageモニタと同じ規約。
             * P521: モニタパネル非表示中はこのストアを丸ごと省く。 */
            if (s_p521_bgsp_composite_visible) {
                *(uint32_t*)(s_bgsp_buffer + (y * disp_w + x) * 4) =
                    px68k_color_to_rgba(has_bg ? tcol : 0);
            }
#if P342_ENABLE
            /* P467: D-35症状2の遷移入口(f=2340→2400)が %60 では未観測だったため、
             * 観測密度を6倍にする(% 60 → % 10)。ゲート周期のみの変更で、
             * VLINE条件・分母(disp_w)併記の構造は一切変えない。 */
            if ((fb_call_count % 10 == 0) &&
                (VLINE == 50 || VLINE == 100 || VLINE == 150 || VLINE == 200 || VLINE == 250)) {
                static int s_p342_bgnz = 0;
                static int s_p342_last_vline = -1;
                if (VLINE != s_p342_last_vline) { s_p342_bgnz = 0; s_p342_last_vline = VLINE; }
                if (has_bg) s_p342_bgnz++;
                if (x == disp_w - 1) {
                    debug_log("[P342-BGNZ] f=%d VLINE=%d bgnz=%d/%d\n",
                              fb_call_count, VLINE, s_p342_bgnz, disp_w);
                }
            }
#endif
#if P357_ENABLE
            /* P467: 観測密度を6倍にする(% 60 → % 10)。ゲート周期のみの変更。 */
            if ((fb_call_count % 10 == 0) &&
                (VLINE == 50 || VLINE == 100 || VLINE == 150 || VLINE == 200 || VLINE == 250)) {
                static int s_p357_black = 0, s_p357_gr = 0, s_p357_tx = 0, s_p357_bg = 0;
                static int s_p357_last_vline = -1;
                if (VLINE != s_p357_last_vline) {
                    s_p357_black = s_p357_gr = s_p357_tx = s_p357_bg = 0;
                    s_p357_last_vline = VLINE;
                }
                if (gcol) s_p357_gr++;
                else if (has_text) s_p357_tx++;
                else if (has_bg) s_p357_bg++;
                else s_p357_black++;
                if (x == disp_w - 1) {
                    /* P501-D44: 生値 pri_* に加えて実効ランク rank_* を併記
                     * (派生値だけでは生値からの導出を後から検算できないため)。 */
                    debug_log("[P357-COMPOSE] f=%d VLINE=%d black=%d gr=%d tx=%d bg=%d total=%d"
                              " text_opaq=%d VC21=%02x pri_gr=%d pri_tx=%d pri_sp=%d"
                              " rank_gr=%d rank_tx=%d rank_sp=%d\n",
                              fb_call_count, VLINE, s_p357_black, s_p357_gr, s_p357_tx, s_p357_bg,
                              disp_w, text_opaq, VCReg2[1], pri_gr, pri_tx, pri_sp,
                              rank_gr, rank_tx, rank_sp);
                }
            }
#endif
#if P391_ENABLE
            /* [P391-BLACK] OverTake 空領域の黒帯 — 黒画素がどの経路で生まれるかの実測。
             * 読み取り専用(合成結果には一切影響しない)。バケットは [P357-COMPOSE] と同じ
             * if/else-if 連鎖なので n_gr+n_tx+n_bg+n_black == disp_w が構造的に保証され、
             * 分母 disp_w も併記するため「n_black=0」の解釈は1通りに定まる。
             * P467: 観測密度を6倍にする(% 60 → % 10)。ゲート周期のみの変更。 */
            if ((fb_call_count % 10 == 0) &&
                (VLINE == 50 || VLINE == 100 || VLINE == 150 || VLINE == 200 || VLINE == 250)) {
                static int s_p391_n_gr = 0, s_p391_n_tx = 0, s_p391_n_bg = 0, s_p391_n_black = 0;
                static int s_p391_k_txidx0 = 0, s_p391_k_txnz_pal0 = 0, s_p391_k_bgflag_col0 = 0;
                static int s_p391_k_txwrap = 0;
                static int s_p391_k_sp2 = 0, s_p391_k_sp2_black = 0;
                static int s_p391_black_first_x = -1;
                static int s_p391_last_vline = -1;
                if (VLINE != s_p391_last_vline) {
                    s_p391_n_gr = s_p391_n_tx = s_p391_n_bg = s_p391_n_black = 0;
                    s_p391_k_txidx0 = s_p391_k_txnz_pal0 = s_p391_k_bgflag_col0 = 0;
                    s_p391_k_txwrap = 0;
                    s_p391_k_sp2 = s_p391_k_sp2_black = 0;
                    s_p391_black_first_x = -1;
                    s_p391_last_vline = VLINE;
                }
                /* [P342-TEXTIDX](:4739-4743、原典 Core/px68k/x68k/tvram.c:234-241)の
                 * アドレス式をそのまま複製する。独自に再導出しないこと —— Bridge 側の
                 * 独立再導出が原典とズレる P381 型の事故を避けるため。値は x に依存せず
                 * 画素ごとの再計算は冗長だが誤りではない。 */
                int32_t ty = TextScrollY + VLINE;
                if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) ty += VLINE;
                ty = (ty & 0x3ff) << 10;
                int32_t tx0 = TextScrollX & 0x3ff;
                int32_t taddr0 = tx0 + ty;
                /* Text_DrawLine の横方向ラップガード(tvram.c:240 の `x = (x ^ 0x3ff) + 1;`
                 * と :243/:255 のループ条件 `(x > 0)`)。テキスト面の読み出しは行内 1024
                 * ドット境界で打ち切られるため、それ以降の画素は TextDrawWork 上では
                 * 「次の行」を指してしまい参照先が別物になる(かつ配列 1024*1024+100 の
                 * 末尾も超えうる)。範囲外は txidx を読まず k_txwrap として別計上する。 */
                int t_span = 0x400 - tx0;      /* この走査線でテキスト面が実際に読む画素数 */
                int t_in_range = (x < t_span);
                uint8_t txidx = t_in_range ? (TextDrawWork[taddr0 + x] & 0xf) : 0;

                /* k_sp2 は走査線全域の計数(H3判別用)。黒画素では gcol==0 が前提のため
                 * 黒内訳としては構造上ほぼ0にしかならず、面としての被覆を測るには全域で
                 * 数える必要がある。sp_active==0 のときは SP2 バッファが当該フレームで
                 * クリアされていない(:4649 は SP 経路でのみ実行)ので、読む際は同じ行の
                 * sp_active で必ずゲートすること。黒画素内に限った値は k_sp2_black に別途出す。 */
                if (Grp_LineBuf32SP2[x]) s_p391_k_sp2++;

                if (gcol) s_p391_n_gr++;
                else if (has_text) s_p391_n_tx++;
                else if (has_bg) s_p391_n_bg++;
                else {
                    s_p391_n_black++;
                    if (s_p391_black_first_x < 0) s_p391_black_first_x = x;
                    /* 以下は n_black の内訳(重複可) */
                    if (!t_in_range) s_p391_k_txwrap++;
                    else if (txidx == 0) s_p391_k_txidx0++;
                    else if (TextPal32[txidx] == 0) s_p391_k_txnz_pal0++;
                    if ((Text_TrFlag[off] & 2) && tcol == 0) s_p391_k_bgflag_col0++;
                    if (Grp_LineBuf32SP2[x]) s_p391_k_sp2_black++;
                }

                if (x == disp_w - 1) {
                    debug_log("[P391-BLACK] f=%d VLINE=%d disp_w=%d text_opaq=%d text_on=%d"
                              " sp_on=%d bg_above_text=%d sp_active=%d"
                              " VC0_1=%02x VC1_0=%02x VC1_1=%02x VC2_0=%02x VC2_1=%02x"
                              " TextPal0=%08x TextPal0_rgba=%08x GrphPal0=%08x GrphPal0_rgba=%08x"
                              " n_gr=%d n_tx=%d n_bg=%d n_black=%d"
                              " k_txidx0=%d k_txnz_pal0=%d k_bgflag_col0=%d k_sp2=%d"
                              " k_sp2_black=%d k_txwrap=%d TSX=%d t_span=%d black_first_x=%d\n",
                              fb_call_count, VLINE, disp_w, text_opaq, text_on,
                              sp_on, bg_above_text, sp_active,
                              VCReg0[1], VCReg1[0], VCReg1[1], VCReg2[0], VCReg2[1],
                              TextPal32[0], px68k_color_to_rgba(TextPal32[0]),
                              GrphPal32[0], px68k_color_to_rgba(GrphPal32[0]),
                              s_p391_n_gr, s_p391_n_tx, s_p391_n_bg, s_p391_n_black,
                              s_p391_k_txidx0, s_p391_k_txnz_pal0, s_p391_k_bgflag_col0,
                              s_p391_k_sp2, s_p391_k_sp2_black, s_p391_k_txwrap,
                              (int)tx0, t_span, s_p391_black_first_x);

                    /* [P391-BLACKDUMP] 黒画素が実際に出た位置を標本自身から自己特定し、
                     * その前後32画素の「結論ではなく入力値」を生ダンプする。黒画素が
                     * 無い走査線では自然に省略される。バッファ類はこの合成ループ内で
                     * 書き換えられないため、x==disp_w-1 の時点で読み直しても
                     * 各画素の走査時と同じ値になる。 */
                    if (s_p391_black_first_x >= 0) {
                        int d0 = s_p391_black_first_x - 16;
                        if (d0 < 0) d0 = 0;
                        int d1 = d0 + 32;
                        if (d1 > disp_w) { d1 = disp_w; d0 = (d1 > 32) ? (d1 - 32) : 0; }
                        char dbuf[1024]; dbuf[0] = '\0'; int dp = 0;
                        for (int dx = d0; dx < d1; dx++) {
                            uint32_t dg = Grp_LineBuf32[dx];
                            if (sp_active && Grp_LineBuf32SP2[dx]) dg = Grp_LineBuf32SP2[dx];
                            int doff = dx + 16;
                            uint32_t dt = BG_LineBuf32[doff];
                            int dht = (Text_TrFlag[doff] & 1) && dt;
                            int dhb = (Text_TrFlag[doff] & 2) && dt;
                            if (dp >= 980) break;
                            if (dx < t_span) {
                                dp += snprintf(dbuf + dp, sizeof(dbuf) - dp,
                                               "%d:%x,%x,%d,%d,%x ", dx, dg, dt, dht, dhb,
                                               TextDrawWork[taddr0 + dx] & 0xf);
                            } else {
                                /* テキスト面読み出し範囲外(横ラップ) — txidx は存在しない */
                                dp += snprintf(dbuf + dp, sizeof(dbuf) - dp,
                                               "%d:%x,%x,%d,%d,w ", dx, dg, dt, dht, dhb);
                            }
                        }
                        debug_log("[P391-BLACKDUMP] f=%d VLINE=%d black_first_x=%d range=%d..%d"
                                  " fmt=x:gcol,tcol,has_text,has_bg,txidx(w=テキスト面読出範囲外)"
                                  " %s\n",
                                  fb_call_count, VLINE, s_p391_black_first_x, d0, d1 - 1, dbuf);
                    }
                }
            }
#endif
#if P217_PROBE
            /* gate 後の has_bg ではなく生のフラグを読むこと — gate 適用後も同じ
             * 画素集合を数え続けるために必要。 */
            if ((Text_TrFlag[off] & 2) && tcol == 0 && gcol != 0) {
                s_p217_n_bgzero++;
                if (s_p217_y_min < 0) s_p217_y_min = y;
                s_p217_y_max = y;
            }
#endif
            // P213: a pixel carrying both flags is owned by whichever plane painted last,
            // so score it with that plane's priority (was: always the text plane, which made
            // sprite pixels lose to the graphic plane).
            // P501-D44: 比較はランク値(3→2 に丸めたもの)で行う。生値 pri_* は温存。
            int rank_t = bg_above_text ? (has_bg   ? rank_sp : (has_text ? rank_tx : -1))
                                       : (has_text ? rank_tx : (has_bg   ? rank_sp : -1));
            int has_t = (rank_t >= 0);

            /* P501-D45 修正B: BG/テキスト面との50%半透明合成(GT)。
             * ★優先度ガード rank_gr < rank_u は MPX windraw.c の6箇所
             * (783/801/817/833/850/870)の個別条件を一般化した**近似**であり、
             * 逐語移植ではない。MPX側の6箇所は「pri_gr<pri_tx 系」2箇所・
             * 「pri_sp>pri_gr 系」2箇所・無条件2箇所の3グループに分かれるが、
             * 共通する意図は「半透明GRP面が、それが乗る下地面より手前にある場合のみ
             * 合成する」ことであり、ここではその共通意図を単一条件で表現している。
             * MPX の page/plane 限定条件までは再現していない。
             * 注: Grp_LineBuf32SP[x] の存在フラグ(Abit32)は Grp_DrawLine8SP
             * (gvram.c:617-620)で Ibit=1 かつ v!=0 のときのみ付与されるため、
             * パレット index 0(透明)のドットは合成対象にならない——既存の
             * 「色index0=透明」規約(P217)と一貫した挙動。 */
            if (tr_bg && Grp_LineBuf32SP[x]) {
                int rank_u = has_t ? rank_t : rank_sp;   /* 下地面の実効ランク(D-44のランク丸め後) */
                if (rank_gr < rank_u) {                  /* GRPが下地より手前の場合のみ合成 */
                    tcol   = p501_blend_half(Grp_LineBuf32SP[x], has_t ? tcol : 0);
                    rank_t = rank_u; has_t = 1;
                }
            }

            if (gcol && has_t) {
                col = (rank_gr < rank_t) ? gcol : tcol;        // smaller = front; tie -> text/bg
            } else if (gcol) {
                col = gcol;
            } else if (has_t) {
                col = tcol;
            } else {
                col = tcol;                                    // typically 0
            }

            // P284: SP 前半(奇数色)は全プレーンの最前面(スプライトより手前)に描く
            // P417: MPX windraw.c:893 の条件 (VCReg2[0]&0x5c)==0x14 でゲートする
            //       (従来は sp_active のみで無条件適用だった)。
            if (sp_active && (VCReg2[0] & 0x5c) == 0x14 && Grp_LineBuf32SP[x]) col = Grp_LineBuf32SP[x];
            uint32_t rgba = px68k_color_to_rgba(col);
            *(uint32_t*)(dst + x * 4) = rgba;
            if (col) s_render_nonzero_pixels++;
        }

        /* P535: この行を実際に描画したことを記録。冒頭の
         * `VLINE < 0 || VLINE >= s_render_disp_h` ガード(この関数唯一の早期
         * return)を通過し、合成ループを実行し終えた経路にのみ置く —— 未描画行を
         * 「描画済み」と誤記録すると render_end のゼロ埋めが機能しなくなる。 */
        s_row_drawn[y] = 1;
    }

#if P351_ENABLE
    if (VLINE == 0) p351_bg0_rowscan_probe();
#endif
    TextDotX = save_textdotx;   /* P294: draw_display_line 内クランプの復元(§2.3-3) */
}

/* P595 (D-55): 寸法と表示ジオメトリを「同一スナップショット」(同一 front
 * インデックス)で返す。front を 1 回だけ acquire load し、s_fb_w/s_fb_h と
 * s_fb_hscale 等をすべてその同じインデックスで読むことで、別々の getter に
 * 分けた場合に起こりうる front/back 入替の取り違えを構造的に排除する。
 * 単位は「標準表示窓 = 1.0」。標準ラスタでは h_scale=v_scale=1.0・off=0.0。 */
const uint8_t* mx68k_get_framebuffer_geom(int* width, int* height,
                                          float* h_scale, float* v_scale,
                                          float* off_x, float* off_y) {
    int front = atomic_load_explicit(&s_fb_front, memory_order_acquire);
    if (width)   *width   = s_fb_w[front];
    if (height)  *height  = s_fb_h[front];
    if (h_scale) *h_scale = s_fb_hscale[front];
    if (v_scale) *v_scale = s_fb_vscale[front];
    if (off_x)   *off_x   = s_fb_offx[front];
    if (off_y)   *off_y   = s_fb_offy[front];
    return s_framebuffer[front];   /* P179: latest complete frame */
}

/* P595 (D-55): 診断・モニタ表示専用の geo_mode(0=恒等 1=標準R00/R04 2=非標準R00
 * 中央寄せ 9=妥当性外/R04非標準)。表示矩形の計算には使わないため、上の
 * スナップショット関数とは別に読んでよい(値の取り違えが起きても表示は狂わない)。 */
int mx68k_get_display_geo_mode(void) {
    int front = atomic_load_explicit(&s_fb_front, memory_order_acquire);
    return s_fb_geomode[front];
}

const uint8_t* mx68k_get_framebuffer(int* width, int* height) {
    /* P595: 実体は mx68k_get_framebuffer_geom()。既存呼出し元の互換性維持のため
     * 寸法だけを返す薄いラッパとして残す(P303 プローブの計数対象も従来どおり
     * この関数の呼出しのみ = 表示スレッドのテクスチャ更新経路)。 */
    const uint8_t* fb = mx68k_get_framebuffer_geom(width, height, NULL, NULL, NULL, NULL);
#if P303_ENABLE
    {
        /* P303: 表示スレッド(MTKView描画スレッド、単一呼出し元)側の
         * static。同一フレーム番号が連続何回読み取られたか(=画面に
         * 何回連続で描画されたか)を数え、フレーム切替わり時に直前
         * フレームの表示回数をログする。read-only、戻り値には一切影響しない。 */
        static int s_p303_last_frame_num = -1;
        static int s_p303_shown_count    = 0;
        int fn = atomic_load_explicit(&s_fb_front_frame_num, memory_order_acquire);
        if (fn == s_p303_last_frame_num) {
            s_p303_shown_count++;
        } else {
            if (s_p303_last_frame_num >= 0) {
                debug_log("[P303-FBSHOW] f=%d shown=%d\n",
                          s_p303_last_frame_num, s_p303_shown_count);
            }
            s_p303_last_frame_num = fn;
            s_p303_shown_count    = 1;
        }
    }
#endif
    return fb;   /* P179: latest complete frame; back buffer is being rendered */
}

// ---- audio ----
int mx68k_audio_read(int16_t* buffer, int frames) {
#if P464_RINGFILL_ENABLE
    /* P464 (D-6再調査): ここは CoreAudio 実時間スレッド。★debug_log() などの同期I/Oは
     * 絶対に呼ばない(プローブ自身が測定対象のジッター源になる)。atomicインクリメント
     * のみを行い、ログ出力は mx68k_run_frame() 側(エミュレーションスレッド)で
     * atomic_exchange により読取り+ゼロ化して行う。
     * callbacks_total は underrun_callbacks の分母として、早期return分も含めた
     * 「呼ばれた回数そのもの」を数えるため、引数ガードより前で加算する。 */
    atomic_fetch_add_explicit(&s_p464_callbacks_total, 1ULL, memory_order_relaxed);
#endif
    /* P549: サウンドモニタ用の累積カウンタ。★上の #if P464_RINGFILL_ENABLE ブロックの
     * 「外」に独立して置く(診断マクロが無効化されても UI 表示は生き続けること)。
     * P464 側と同じく、引数ガードより前で「呼ばれた回数そのもの」を数える
     * (アンダーラン率の分母になるため)。 */
    atomic_fetch_add_explicit(&s_p549_cum_callbacks_total, 1ULL, memory_order_relaxed);
    if (!buffer || frames <= 0) return 0;
    int samples_needed = frames * 2;

    /* P624: ターボ(固定倍率)時のピッチシフト再生用デシメーション状態。
     * ★これらは全て「CoreAudio 実時間スレッド専有」の関数内 static であり、
     * 他スレッドは一切書き込まない(単一書き手則)。よって _Atomic は不要。
     *   s_p624_phase   : 入力読み出し位置の小数部(Q16、0..65535)
     *   s_p624_s0/s1   : 補間に使う連続 2 入力フレーム(それぞれ L/R ペア)
     *   s_p624_have    : s0/s1 のうち有効な個数(0/1/2)。コールバックを跨いで保持し、
     *                    リング枯渇で中断しても次回そこから再開できるようにする。
     *   s_p624_seen_gen: 最後に観測したリセット世代(mx68k_set_turbo_audio_rate が +1)。 */
    static int32_t s_p624_phase    = 0;
    static int16_t s_p624_s0[2]    = {0, 0};
    static int16_t s_p624_s1[2]    = {0, 0};
    static int     s_p624_have     = 0;
    static int     s_p624_seen_gen = -1;
    {
        /* 倍率変更(= 世代カウンタの変化)を検出したら位相状態をゼロ初期化する。
         * ★ゼロ初期化を必ずこのスレッド側で行うのがスレッド安全設計の要点
         *   (セッタ側は世代を +1 するだけ。mx68k_set_turbo_audio_rate のコメント参照)。 */
        int gen = atomic_load_explicit(&g_turbo_audio_reset_gen, memory_order_acquire);
        if (gen != s_p624_seen_gen) {
            s_p624_seen_gen = gen;
            s_p624_phase = 0;
            s_p624_have  = 0;
            s_p624_s0[0] = s_p624_s0[1] = 0;
            s_p624_s1[0] = s_p624_s1[1] = 0;
        }
    }
    int p624_ratio = atomic_load_explicit(&g_turbo_audio_ratio_q16, memory_order_acquire);

    /* P625: ノーウェイト(可変速)用の自動レート推定。ノーウェイトの実速度はホスト負荷
     * 依存で不定なため Swift 側から固定倍率を渡せない。代わりにリング占有量(充填率)を
     * 観測し、目標占有量(容量の 50%)からの偏差に比例したレートへ緩やかに追従させる
     * —— 生成が速ければリングが溜まる→レートを上げて速く消費する、という負帰還ループ。
     * ★この分岐は「p624_ratio という 1 つの int に何を入れるか」を決めるだけの前処理で
     *   あり、後段の UNITY / MUTE / デシメーションの 3 分岐(P624)には一切手を加えない。
     * s_p625_auto_ratio は位相状態(s_p624_*)と同じく CoreAudio 実時間スレッド専有の
     * 関数内 static であり、他スレッドは読み書きしない → _Atomic 不要(単一書き手則)。
     * また世代リセット(g_turbo_audio_reset_gen)の対象にも含めない: 位相と違い
     * 「これまでの推定値」を引き継いだ方が切替後の収束が速いため、意図的に保持する。
     * 定数(ゲイン 7.0 = 8.0-1.0、平滑化係数 0.05、クランプ [1.0, 8.0])はいずれも
     * ホスト側の制御ループ設計値であり実機/参照実装に対応物は無い(P625 記号表参照)。 */
    static double s_p625_auto_ratio = 2.0;   /* ノーウェイト開始直後の暫定推定値 */
    if (p624_ratio == MX68K_TURBO_AUDIO_RATE_AUTO) {
        int write_pos = atomic_load_explicit(&audio_ring_write_pos, memory_order_acquire);
        int read_pos  = atomic_load_explicit(&audio_ring_read_pos,  memory_order_relaxed);
        int occ_samples = (write_pos - read_pos + AUDIO_RING_SAMPLES) % AUDIO_RING_SAMPLES;
        int occ_frames  = occ_samples / 2;
        double instant = 1.0 + 7.0 * ((double)occ_frames - (double)(AUDIO_RING_FRAMES / 2))
                                     / (double)(AUDIO_RING_FRAMES / 2);
        if (instant < 1.0) instant = 1.0;
        if (instant > 8.0) instant = 8.0;
        s_p625_auto_ratio = s_p625_auto_ratio * 0.95 + instant * 0.05;
        p624_ratio = (int)(s_p625_auto_ratio * 65536.0 + 0.5);
    }

    /* P465-変更3 (D-6): アンダーラン時の硬い memset ゼロ埋めは、直前まで出力していた
     * 値(P464実測ではDC固着した +11154)から瞬時にゼロへ落ちる段差を作り、これが
     * 実際に聞こえていたクリックの近接原因と推定される。ADPCM由来かFM(OPM)由来か、
     * 慢性ドリフトか一時的な枯渇かを問わず均一に効くよう、直前の有効サンプルから
     * P463と同じ±1/サンプル速度でゼロへランプする方式に変更する。
     * ここは CoreAudio 実時間スレッドだが追加処理は純粋な算術のみ(I/Oなし)であり、
     * [P464-RINGFILL] のH5設計原則(当該スレッドで debug_log() を呼ばない)を維持する。
     * mx68k_audio_read は同スレッドからの唯一の呼出し元(SPSC設計)のため
     * s_p465_last_good のatomic化は不要。
     * ★B2修正(Round-2で両レビュアーが独立に発見した実害バグ): audio_ring_read は
     * サンプル単位ループで break するためリングが空になる瞬間に L/R 境界を跨いで
     * 打ち切られ、got が奇数になりうる。奇数の got のまま buffer[got-2]/[got-1] を
     * L/Rペアとして読むとチャンネルが入れ替わり、かつ missing_frames の整数除算
     * 切り捨てで末尾1サンプルが未初期化のまま出力される(現行 memset は無条件に
     * 全域をゼロ埋めしていたため、これは真の回帰になりうる)。got を偶数へ切り捨てて
     * から last_good 取得・ランプ開始位置の両方に用いることで確実に回避する
     * (破棄されうる最大1サンプル ≒23マイクロ秒 は音質上無視できる)。 */
    static int16_t s_p465_last_good[2] = {0, 0};
    int got;
    if (p624_ratio == MX68K_TURBO_AUDIO_RATE_UNITY) {
        /* P624: 等倍(ターボ非活性)。★ここは P624 以前と完全に同一の経路であり、
         * 通常速度再生の音質にリグレッションを与えないための安全弁。 */
        int got_raw = audio_ring_read(buffer, samples_needed);
        got = got_raw & ~1;             /* ★B2修正: 偶数(L/Rペア境界)へ丸める */
    } else if (p624_ratio == MX68K_TURBO_AUDIO_RATE_MUTE) {
        /* P624: ミュート(ノーウェイト用。実速度がホスト負荷依存で固定倍率に落とせない
         * ため、P555 以来のミュート方式を維持する)。got=0 とすることで後段の
         * P463_DECAY_STEP ランプが直前サンプルからゼロへ滑らかに落とし、以降無音を保つ
         * —— P555 のミュート(リングが枯渇して同じランプへ落ちる)と同じ聴感になる。
         * 併せてリングを空にしておき、ミュート解除時に古い音声が再生されないようにする。 */
        audio_ring_discard_all();
        got = 0;
    } else {
        /* P624: 可変レートデシメーション(位相アキュムレータ + 隣接 2 フレーム線形補間)。
         * WebX68k src/speed.ts:87-112 resampleSpeed() のアルゴリズムを消費側へ適用したもの。
         * 出力 1 フレームごとに入力位置を ratio/65536 フレーム進めるだけなので、
         * ピッチが倍率どおり上がる(テープ早送りと同じ原理。タイムストレッチは行わない)。
         * ★入出力とも L/R フレーム単位でのみ扱う(P465-B2)。 */
        int out_frames = 0;
        while (out_frames < frames) {
            /* 補間に必要な連続 2 フレームを揃える。揃わなければリング枯渇 =
             * 後段の P463/P465 ランプ処理へ委ねる(状態は保持され次回続きから再開)。 */
            while (s_p624_have < 2) {
                int16_t f[2];
                if (!audio_ring_read_frame(f)) goto p624_ran_dry;
                if (s_p624_have == 0) { s_p624_s0[0] = f[0]; s_p624_s0[1] = f[1]; s_p624_have = 1; }
                else                  { s_p624_s1[0] = f[0]; s_p624_s1[1] = f[1]; s_p624_have = 2; }
            }
            /* 線形補間。差分は ±65535、phase は 0..65535 なので積は int32 を溢れうる
             * → int64 で計算してから >>16 する。 */
            buffer[out_frames * 2] = (int16_t)((int32_t)s_p624_s0[0] +
                (int32_t)(((int64_t)(s_p624_s1[0] - s_p624_s0[0]) * s_p624_phase) >> 16));
            buffer[out_frames * 2 + 1] = (int16_t)((int32_t)s_p624_s0[1] +
                (int32_t)(((int64_t)(s_p624_s1[1] - s_p624_s0[1]) * s_p624_phase) >> 16));
            out_frames++;
            /* 入力位置を ratio/65536 フレーム進める。整数部の繰り上がり 1 につき
             * 入力を 1 フレーム消費する(5x なら 1 出力フレームあたり 5 入力フレーム)。 */
            s_p624_phase += p624_ratio;
            while (s_p624_phase >= 65536) {
                s_p624_phase -= 65536;
                s_p624_s0[0] = s_p624_s1[0]; s_p624_s0[1] = s_p624_s1[1];
                s_p624_have  = 1;
                int16_t f[2];
                if (!audio_ring_read_frame(f)) { s_p624_phase = 0; break; }  /* 枯渇: 位相を正規化(不変条件 [0,65536) を次回呼出しへ持ち越す)してから抜ける。次周回の充填ループが goto する */
                s_p624_s1[0] = f[0]; s_p624_s1[1] = f[1];
                s_p624_have  = 2;
            }
        }
    p624_ran_dry:
        /* ★アンダーラン計装(P464/P549)の意味論を変えないため、got は常に
         * 「出力フレーム数 × 2」で表す(内部で何入力フレーム読んだかは問わない)。 */
        got = out_frames * 2;
    }
    if (got >= 2) {
        s_p465_last_good[0] = buffer[got - 2];
        s_p465_last_good[1] = buffer[got - 1];
    }
    if (got < samples_needed) {
        int16_t rampL = s_p465_last_good[0];
        int16_t rampR = s_p465_last_good[1];
        int missing_frames = (samples_needed - got) / 2;  /* got/samples_needed共に偶数→切り捨てなし */
        for (int f = 0; f < missing_frames; f++) {
            if (rampL > 0)      rampL -= P463_DECAY_STEP;
            else if (rampL < 0) rampL += P463_DECAY_STEP;
            if (rampR > 0)      rampR -= P463_DECAY_STEP;
            else if (rampR < 0) rampR += P463_DECAY_STEP;
            buffer[got + f * 2]     = rampL;
            buffer[got + f * 2 + 1] = rampR;
        }
        s_p465_last_good[0] = rampL;
        s_p465_last_good[1] = rampR;
#if P464_RINGFILL_ENABLE
        atomic_fetch_add_explicit(&s_p464_underrun_callbacks, 1ULL, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_p464_samples_zero_filled,
                                  (unsigned long long)(samples_needed - got),
                                  memory_order_relaxed);
#endif
        /* P549: 同じアンダーラン事象を、リセットされない累積カウンタにも計上する。
         * ★直上のネストした #if P464_RINGFILL_ENABLE の「外」・ただし
         * `if (got < samples_needed)` ブロックの「内」に置くこと —— 実際に
         * アンダーランが起きたときだけ加算されるという意味論を保つため。 */
        atomic_fetch_add_explicit(&s_p549_cum_underrun_callbacks, 1ULL, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_p549_cum_samples_zero_filled,
                                  (unsigned long long)(samples_needed - got),
                                  memory_order_relaxed);
    }
    return frames;
}

// ---- input ----
#if P214_ENABLE
/* P214-K（診断・既定は休止）: make/break を先頭 60 イベントだけ記録する。
 * Core keyboard.c はリピートを生成しない（KeyBuf への書込は下の 2 箇所のみ）ので、
 * break を挟まない make の連続はホスト(macOS)のオートリピート転送を意味する。read-only。 */
static unsigned s_p214_key_count = 0;
static void p214_k_log(const char *kind, uint8_t keycode) {
    if (s_p214_key_count >= 60) return;
    s_p214_key_count++;
    debug_log("[P214-K] #%u %s code=0x%02x f=%d\n",
              s_p214_key_count, kind, (unsigned)keycode, g_mx68k_frame_num);
}
#endif

void mx68k_key_down(uint8_t keycode) {
    /* P175: Core keyboard.c is a stub — push the X68000 scancode into KeyBuf here
     * (upstream send_keycode: down=code). */
#if P214_ENABLE
    p214_k_log("make ", keycode);
#endif
    uint8_t newwp = (uint8_t)((KeyBufWP + 1) & (KeyBufSize - 1));
    if (newwp != KeyBufRP) { KeyBuf[KeyBufWP] = keycode; KeyBufWP = newwp; }
}

void mx68k_key_up(uint8_t keycode) {
#if P214_ENABLE
    p214_k_log("break", keycode);
#endif
    uint8_t newwp = (uint8_t)((KeyBufWP + 1) & (KeyBufSize - 1));
    if (newwp != KeyBufRP) { KeyBuf[KeyBufWP] = (uint8_t)(keycode | 0x80); KeyBufWP = newwp; }
}

/* P195: px68k 本来の意味論(x11/mouse.c)を再現する。
 * 従来は mx68k_mouse_move が MouseX/MouseY に直接加算し、誰もクリアしないため
 * 起動以来の移動量が累積し、X68000 が毎パケットで巨大な移動量を読んでいた。
 * 正しくは「パケット要求時(Mouse_SetData)に累積分を取り出してゼロクリアし、
 * ±127 にクランプして MouseX/MouseY へ代入する」。
 * mx68k_mouse_move はメインスレッド、Mouse_SetData はエミュスレッド(SCC_Write
 * 経由)から呼ばれるため累積器は atomic(宣言は mx68k_reset_hard の直前)。 */
void mx68k_mouse_move(int dx, int dy) {
    atomic_fetch_add(&s_mouse_dx, dx);
    atomic_fetch_add(&s_mouse_dy, dy);
}

void mx68k_mouse_button(int button, bool pressed) {
    int m = (button == 0) ? 0x01 : (button == 1) ? 0x02 : 0;
    if (!m) return;
    if (pressed) atomic_fetch_or(&s_mouse_btn, m);
    else         atomic_fetch_and(&s_mouse_btn, ~m);
}

/* scc.c がマウスパケット生成時に呼ぶ(Core/px68k/x68k/mouse.c の空スタブは
 * Compile Sources から外し、この定義を使う)。x11/mouse.c と同じ処理。 */
void Mouse_SetData(void) {
    /* P196: 累積分を「ゼロクリア」せず「送出分だけ減算」して残余を次パケットへ
     * 持ち越す。ゲストのパケット要求は ~1/フレーム(60Hz)だが macOS のマウス
     * イベントは 125〜1000Hz で 1 パケット間に合算されるため、±127 超過分を捨てると
     * 速い動きでカウントが失われ、Swift 側の絶対座標追従モデル(送った分だけ届く前提)
     * が恒久的にズレる。残余保持で無損失にする(127×60=7620 counts/s で吐き出す)。 */
    int x = atomic_load(&s_mouse_dx);
    int y = atomic_load(&s_mouse_dy);
    uint8_t st = (uint8_t)(atomic_load(&s_mouse_btn) & 0x03);

    int sx = (x >  127) ? 127 : (x < -128 ? -128 : x);
    int sy = (y >  127) ? 127 : (y < -128 ? -128 : y);
    if (x >  127) st |= 0x10; else if (x < -128) st |= 0x20;
    if (y >  127) st |= 0x40; else if (y < -128) st |= 0x80;

    atomic_fetch_sub(&s_mouse_dx, sx);
    atomic_fetch_sub(&s_mouse_dy, sy);

    MouseX = (uint8_t)(int8_t)sx;
    MouseY = (uint8_t)(int8_t)sy;
    MouseSt = st;
}

void mx68k_joy_set(int port, uint8_t state) {
    GamePad_SetState((int32_t)port, state);
}

/* P500: 第2バンク(strobe high 側、多ボタンパッドの TRG3 以降)を送出する。
 * 通常の 2 ボタン利用では呼ばなくてよい(pad_btn1 は 0xFF 初期値のまま = idle)。 */
void mx68k_joy_set1(int port, uint8_t bits) {
    GamePad_SetState1((int32_t)port, bits);
}

// ---- status ----
void mx68k_get_status(MX68KStatus* status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->pc = C68k_Get_PC(&C68K);
    for (int i = 0; i < 8; i++) {
        status->d[i] = C68k_Get_DReg(&C68K, i);
        status->a[i] = C68k_Get_AReg(&C68K, i);
    }
    status->sr = (uint16_t)C68k_Get_SR(&C68K);
    status->usp = C68k_Get_USP(&C68K);
    status->isp = C68k_Get_MSP(&C68K);
    status->clock_mhz = g_clock_mhz;
    status->machine_type = g_machine_type;
    status->memory_mb = g_memory_size_mb;
    status->fpu_enabled = g_fpu_enabled;
    status->fdd0_inserted = (FDD_IsReady(0) != 0);
    status->fdd0_active = (mx68k_fdd_accessing(0) != 0);   /* P160: red = accessing */
    status->fdd1_inserted = (FDD_IsReady(1) != 0);
    status->fdd1_active = (mx68k_fdd_accessing(1) != 0);   /* P160: red = accessing */
    status->hdd_busy = (mx68k_hdd_accessing() != 0);       /* P201: HD BUSY lamp */
    /* P455: 8 ユニットの装着マスクを生値として出す(表示には使わない)。
     * ★SCSI の在席はここに OR しない — 「SASI が何台か」が読めなくなるため。
     * ★P456: 導出は mx68k_sasi_unit_mask() に一本化(式は literally 同一で
     *   hdd_inserted_mask の値は不変)。リセット経路の $ED005A 同期と
     *   同じマスクを見ていることを構造的に保証するため。 */
    uint8_t sasi_mask = mx68k_sasi_unit_mask();
    status->hdd_inserted_mask = sasi_mask;
    status->rtc_clkout_select = RTC_Regs[1][0] & 0x07;   /* P653: TIMER-LED */
    /* P203 / P455: ランプ用の集約在席フラグ。SASI 8 ユニットのいずれか OR SCSI。 */
    status->hdd0_inserted = (sasi_mask != 0)
        || mx68k_scsi_in_disk_present()                    /* P269: 内蔵SCSI */
        || mx68k_scsi_ext_is_inserted();                   /* P269: 外付けSCSI */
    status->hdd1_inserted = mx68k_hdd_is_inserted(1);      /* 意味は従来どおり不変 */
    status->paused = s_paused;
    /* P408: 実行粒度の可視化(読み取り専用)。mx68k_run_frame() が毎フレーム更新した
     * 最新スナップショットをそのまま渡す。 */
    status->clock_slice       = g_p408_clock_slice_snapshot;
    status->clkdiv            = g_p408_clkdiv_snapshot;
    status->clk_total         = g_p408_clk_total_snapshot;
    status->vline_total       = g_p408_vline_total_snapshot;
    status->chunks_last_frame = g_p408_chunks_last_frame;
    status->cum_zero          = g_p408_cum_zero_snapshot;
    /* P443 (D-7): ゲスト側イジェクトにも追従するメディア有無。Swift 側がこれを見て
     * fdd0Path/fdd1Path を突き合わせる(ランプ用の fdd0_inserted は従来通り
     * FDD_IsReady() 由来のまま変更しない)。 */
    status->fdd0_media_present = (mx68k_fdd_media_present(0) != 0);
    status->fdd1_media_present = (mx68k_fdd_media_present(1) != 0);
    /* P684: ドライブ 2/3 も同じ経路で Swift 側へ渡す。 */
    status->fdd2_media_present = (mx68k_fdd_media_present(2) != 0);
    status->fdd3_media_present = (mx68k_fdd_media_present(3) != 0);
    status->fdd2_active = (mx68k_fdd_accessing(2) != 0);   /* P689: red = accessing */
    status->fdd3_active = (mx68k_fdd_accessing(3) != 0);   /* P689: red = accessing */
}

/* P286: developer monitor panels. Read-only snapshots of existing extern globals
 * (no Core changes). Same no-lock snapshot-copy contract as mx68k_get_status. */
void mx68k_get_crtc_status(MX68KCRTCStatus* status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->hsync_khz     = (CRTC_Regs[0x29] & 0x10) ? 31.50 : 15.98;
    status->vsync_hz      = mx68k_get_vsync_hz();
    status->text_dot_x    = (uint32_t)TextDotX;
    status->text_dot_y    = (uint32_t)TextDotY;
    status->text_scroll_x = (uint32_t)TextScrollX;
    status->text_scroll_y = (uint32_t)TextScrollY;
    status->vline_total   = (uint32_t)VLINE_TOTAL;
    status->crtc_hstart   = CRTC_HSTART;
    status->crtc_hend     = CRTC_HEND;
    status->crtc_vstart   = CRTC_VSTART;
    status->crtc_vend     = CRTC_VEND;
}

/* P742: DMAC レジスタモニタ。Core の dmac_ch DMA[4](Core/px68k/x68k/dmac.h)を
 * 4ch 分そのままコピーするだけの read-only アクセサ。DMA[] へ書き込むのは
 * Core 側の DMA_Read/DMA_Write/DMA_Exec のみで、それらはエミュレーション
 * スレッド上で動く —— 本関数もエミュレーションスレッドから毎フレーム
 * (dmacVisible が立っている間だけ)呼ばれるため、追加のロックは不要。 */
void mx68k_get_dmac_status(MX68KDMACStatus* status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    for (int i = 0; i < 4; i++) {
        status->ch[i].csr = DMA[i].CSR;
        status->ch[i].cer = DMA[i].CER;
        status->ch[i].dcr = DMA[i].DCR;
        status->ch[i].ocr = DMA[i].OCR;
        status->ch[i].scr = DMA[i].SCR;
        status->ch[i].ccr = DMA[i].CCR;
        status->ch[i].mtc = DMA[i].MTC;
        status->ch[i].mar = DMA[i].MAR;
        status->ch[i].dar = DMA[i].DAR;
        status->ch[i].btc = DMA[i].BTC;
        status->ch[i].bar = DMA[i].BAR;
        status->ch[i].niv = DMA[i].NIV;
        status->ch[i].eiv = DMA[i].EIV;
        status->ch[i].mfc = DMA[i].MFC;
        status->ch[i].cpr = DMA[i].CPR;
        status->ch[i].dfc = DMA[i].DFC;
        status->ch[i].bfc = DMA[i].BFC;
        status->ch[i].gcr = DMA[i].GCR;
    }
}

/* P743: 割込み系レジスタモニタ。Core の MFP[24](Core/px68k/x68k/mfp.h)・
 * IOC_IntStat / IOC_IntVect(ioc.h)・SysPort[7](sysport.h)・C68K.IRQLine
 * (Core/c68k/c68k.h:143)をそのままコピーするだけの read-only アクセサ。
 * これらへ書き込むのは Core 側の MFP_Read/MFP_Write・IOC_Read/IOC_Write・
 * SysPort_Read/SysPort_Write・CPU コア本体のみで、いずれもエミュレーション
 * スレッド上で動く —— 本関数も intRegsVisible が立っている間だけ同スレッドから
 * 毎フレーム呼ばれるため、追加のロックは不要(P742 DMAC と同型)。
 *
 * ★アクセサ関数(MFP_Read() / IOC_Read())を経由せず生のグローバル変数を直接
 *   読むのは意図的な設計判断である。これは「実機の 68901 なら副作用があるかも
 *   しれない」という一般論ではなく、MX 自身の Core に実際に副作用が存在すること
 *   を確認済みであるため:
 *     - Core/px68k/x68k/mfp.c の MFP_Read() は Reg23(UDR)読取り時に
 *       `KeyIntFlag = 0;` を実行する(mfp.c:221-223)。
 *     - Core/px68k/x68k/ioc.c の IOC_Read() は adr==0xe9c001 読取り時に
 *       IOC_IntStat の bit 0x20(プリンタ Busy)をセット/クリアでトグルする
 *       (ioc.c:70-83)。
 *   つまりアクセサ経由でモニタ用の読取りを行うと、ゲストから見える状態
 *   (キー割込みフラグ・プリンタ Busy ビット)をモニタを開いているだけで
 *   毎フレーム変えてしまう。生変数の直接読取りはこの副作用を構造的に回避する。 */
void mx68k_get_int_regs_status(MX68KIntRegsStatus* status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));   /* 構造体パディングまで含めて初期化(P742 と同型) */
    memcpy(status->mfp, MFP, sizeof(status->mfp));
    status->ioc_int_stat = IOC_IntStat;
    status->ioc_int_vect = IOC_IntVect;
    memcpy(status->sysport, SysPort, sizeof(status->sysport));
    status->cpu_irq_line = C68K.IRQLine;
}

void mx68k_get_vc_status(MX68KVCStatus* status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->vc_reg0_0 = VCReg0[0]; status->vc_reg0_1 = VCReg0[1];
    status->vc_reg1_0 = VCReg1[0]; status->vc_reg1_1 = VCReg1[1];
    status->vc_reg2_0 = VCReg2[0]; status->vc_reg2_1 = VCReg2[1];
    status->mode   = VCReg0[1] & 3;
    status->pri_gr = (VCReg1[0]     ) & 3;
    status->pri_tx = (VCReg1[0] >> 2) & 3;
    status->pri_sp = (VCReg1[0] >> 4) & 3;
    status->text_on = (VCReg2[1] & 0x20) != 0;
    status->sp_on   = (VCReg2[1] & 0x40) != 0;
    status->sp_cond_256 = (VCReg2[0] & 0x10) && (VCReg2[1] & 1);
}

void mx68k_get_bg_status(MX68KBGStatus* status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    extern uint8_t Sprite_Regs[0x800];   /* P277以来の既存パターン: bg.h未extern・Bridge側でローカルextern宣言 */
    status->bg0_on = (BG_Regs[9] & 1) != 0;
    status->bg1_on = (BG_Regs[9] & 8) != 0;
    status->bg_chrsize = BG_CHRSIZE;
    status->bg0_scroll_x = BG0ScrollX; status->bg0_scroll_y = BG0ScrollY;
    status->bg1_scroll_x = BG1ScrollX; status->bg1_scroll_y = BG1ScrollY;
    status->bg0_top = BG_BG0TOP; status->bg1_top = BG_BG1TOP;
    typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) SprEnt;
    const SprEnt *sct = (const SprEnt *)Sprite_Regs;
    int cnt = 0;
    for (int n = 0; n < 128; n++) {
        if ((sct[n].posx & 0x3ff) != 0 || (sct[n].posy & 0x3ff) != 0) cnt++;
    }
    status->sprite_active_count = cnt;
}

/* P550: パレットモニタ。上の mx68k_get_bg_status() と同じ read-only スナップ
 * ショットコピー契約。TextPal32/GrphPal32/Contrast_Value は
 * Core/px68k/x68k/palette.h が extern 宣言済みで、このファイルは同ヘッダを
 * include しているため Bridge 側の追加 extern は不要。 */
void mx68k_get_palette_status(MX68KPaletteStatus* status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    memcpy(status->text_pal, TextPal32, sizeof(status->text_pal));
    memcpy(status->grph_pal, GrphPal32, sizeof(status->grph_pal));
    status->contrast = Contrast_Value;
}

/* P694: RTC モニタ。上の mx68k_get_palette_status() と同じ read-only スナップ
 * ショットコピー契約(ロック無し)。RTC_Regs[][] はフラットな uint8 配列で
 * 生ポインタの逆参照を含まないため、P692 のストレージモニタのような
 * emulationLock は不要 —— 既存の mx68k_get_crtc_status() 系と同じ扱い。
 *
 * ★RTC_Read() を呼ばずに同じ導出を自前で行う。RTC_Read() は引数のアドレスに
 *   応じて 1 ニブルずつしか返さない上に、副作用として RTC_Bank を書き換えるため
 *   (rtc.c:54)、モニタからは呼べない。ここでは RTC_Bank は読むだけ。
 *
 * ★各フィールドの意味とマスクは Core/px68k/x68k/rtc.c の RTC_Read()/RTC_Write()
 *   の該当 case と 1 対 1 で対応させてある(EmulatorBridge.h の
 *   MX68K_RTCStatus 定義のコメント参照)。 */
void mx68k_get_rtc_status(MX68K_RTCStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    extern uint8_t RTC_Bank;   /* rtc.c:13。RTC_Regs[][] は本ファイル上部で extern 済み */

    /* ---- 制御 / アラーム(BANK1)。RTC_Write() が実際に格納する真の状態 ---- */
    out->bank          = RTC_Bank;
    out->clkout_select = RTC_Regs[1][0] & 0x07;   /* rtc.c:142 Clock OutPut select */
    out->adj           = RTC_Regs[1][1] & 0x01;   /* rtc.c:143 ADJ */
    /* アラームの分/時/日は「1の位」「10の位」の 2 レジスタに分かれている
     * (rtc.c:144-150)。RTC_Read() の BANK1 case が使うのと同じマスクを掛けた上で
     * 10 進値へ合成する(Swift 側で BCD 復元をやり直さずに済むように)。 */
    out->alarm_min  = (uint8_t)(((RTC_Regs[1][3] & 0x07) * 10) + (RTC_Regs[1][2] & 0x0f));
    out->alarm_hour = (uint8_t)(((RTC_Regs[1][5] & 0x03) * 10) + (RTC_Regs[1][4] & 0x0f));
    out->alarm_wday = RTC_Regs[1][6] & 0x07;      /* rtc.c:148 Alarm day of week */
    out->alarm_day  = (uint8_t)(((RTC_Regs[1][8] & 0x03) * 10) + (RTC_Regs[1][7] & 0x0f));
    out->hour_mode_24 = RTC_Regs[1][10] & 0x01;   /* rtc.c:152 / RTC_Init():27 で既定 1(24h) */
    out->leap_year_ctr = RTC_Regs[1][11] & 0x03;  /* rtc.c:153。★読み戻されない(下記) */

    /* ---- BANK/TEST/ALARM 出力制御(BANK0 の 13-15)。生値のまま渡す ----
     * rtc.c:132-134 / :155-157 が BANK0 と BANK1 の同じ添字へミラー書込みするため、
     * どちらのバンクを見ても同値。値は RTC_Write() の時点でマスク済み。 */
    out->reg_bank_ctrl = RTC_Regs[0][13];
    out->reg_test      = RTC_Regs[0][14];
    out->reg_alarm_out = RTC_Regs[0][15];

    /* ---- 日時本体 = ホスト時計のスナップショット ----
     * RTC_Read() の BANK0 分岐(rtc.c:41-73)と同じ導出。localtime() は静的バッファを
     * 返すので、ここでは再入可能な localtime_r() を使う(RTC_Read() 本体は Core 側の
     * 実装のため触らない)。 */
    {
        time_t t = time(NULL);
        struct tm tmbuf;
        struct tm *tm = localtime_r(&t, &tmbuf);
        if (tm) {
            uint8_t tm24;
            if (RTC_Regs[1][10] & 0x01) {          /* 24時間制 (rtc.c:44-47) */
                tm24 = (uint8_t)(tm->tm_hour);
            } else {                                /* 12時間制 (rtc.c:49-52) */
                tm24 = (uint8_t)((tm->tm_hour) % 12);
                if ((tm->tm_hour) >= 12) tm24 = (uint8_t)(tm24 + 20);   /* PM set */
            }
            out->sec    = (uint8_t)(tm->tm_sec);
            out->minute = (uint8_t)(tm->tm_min);
            out->hour   = tm24;
            out->wday   = (uint8_t)(tm->tm_wday);
            out->mday   = (uint8_t)(tm->tm_mday);
            out->mon    = (uint8_t)(tm->tm_mon + 1);
            out->year   = (uint8_t)((tm->tm_year) - 80);   /* 1980 基準 (rtc.c:72-73) */
            /* ★RTC_Read() の BANK1 case 0x17(rtc.c:95)が実際に返す値。
             *   RTC_Regs[1][11](= leap_year_ctr、ゲストの書込み先)を一切参照せず、
             *   ホストの年から都度計算している —— 両方を並べて公開する理由。 */
            out->leap_year_effective = (uint8_t)((((tm->tm_year) - 80) % 4) & 0x03);
        }
    }
}

/* P484: サウンドモニタ(ADPCM section)。既存 mx68k_get_crtc_status() と同じ
 * 「ロックなしスナップショットコピー」契約。read-only。 */
void mx68k_get_adpcm_status(MX68K_ADPCMStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* P484: PPI ポートC shadow(m68000_bridge.c 定義)。ADPCM_Pan は adpcm.c 内で
     * static かつ getter が無いため、ゲストが PAN を設定する唯一の経路である
     * PPI ポートC を Bridge 側で shadow したものを読む。 */
    extern uint8_t g_ppi_portc_shadow;

    /* g_adpcm_waveform は既に時系列順(先頭=古い・末尾=新しい)に格納済みのため
     * 並べ替えは不要 — 単純コピーで良い。 */
    memcpy(out->waveform, g_adpcm_waveform, sizeof(out->waveform));

    int32_t peak = 0;
    for (int i = 0; i < MX68K_ADPCM_WAVEFORM_SAMPLES; i++) {
        int32_t v = (int32_t)g_adpcm_waveform[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    out->peak_level = peak;

    /* ★adpcm.c:280-284 は `return (ADPCM_Playing)?0xc0:0x40;`。
     * 同ファイル内のコメント「bit7:1=standby 0=Playing」とはコードが逆であり、
     * 実際は再生中に 0xC0(bit7セット)を返す。コメントではなくコードに合わせる。
     * ADPCM_Read() は副作用を持たない純粋な読み出し(adpcm.c:277-292)。 */
    out->playing = (ADPCM_Read(0xE92001) == 0xC0);

    /* P484b: 停止中は Peak Level を 0 へリセット(最後の値が残り続けるのを防ぐ)。
     * ★この判定は必ず上の out->playing 代入の「後」に置くこと — memset 直後では
     * out->playing が暫定的に false のままで、常に 0 になってしまう。 */
    if (!out->playing) {
        out->peak_level = 0;
    }

    /* ADPCM_ClockRate は adpcm.c の ADPCM_Clocks[] 由来で、実サンプリング周波数の
     * 12倍(ADPCM_SampleRate = 44100*12 と同じスケール、adpcm.c:31-39)。 */
    out->sample_rate_hz = (int32_t)(ADPCM_ClockRate / 12);
    out->clock_divider  = (int32_t)ADPCM_Clock;

    /* ADPCM 用 DMA はチャンネル3。CCR bit7(0x80)= STR(動作開始)フラグ
     * (dmac.c:143,183 での使われ方が一次情報源)。 */
    out->dma_active = (DMA[3].CCR & 0x80) != 0;

    out->pan = (int32_t)(g_ppi_portc_shadow & 0x0F);
}

/* P485: サウンドモニタ(OPM 8ch コンパクト表示)。上の mx68k_get_adpcm_status() と
 * 同じ「ロックなしスナップショットコピー」契約。read-only。
 *
 * ★既知の制約: CSM(Timer Control、reg 0x14)が有効な場合、実チップの KEYON 内部
 * 状態は regtc 分岐(opm.cpp:184-192、TimerA 満了経由の KeyOn 等)を通るため、
 * reg 0x08 の単純シャドウである g_opm_keyon[] は実際のキーオン/オフと乖離しうる。
 * CSM 使用ソフトは少数のため本サイクルでは reg 0x14 のシャドウ化は行わない。 */
void mx68k_get_opm_status(MX68K_OPMStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    extern uint8_t g_opm_shadow[256];
    extern uint8_t g_opm_written[256];
    extern uint8_t g_opm_keyon[8];
    /* nibble→音名index(C=0..B=11)。XEiJ SoundMonitor.java の**完全な**変換式
     * (k1=(((kc-(kc>>2))<<6|kf)+(3<<6)+(64-38))>>6、kf=0基準、o=(kk*43)>>9)で
     * 検算・確定(簡略式では結果が +3 半音ずれるため式の全項を使用すること)。
     * 3/7/11 は無効(隣接 nibble と同一ピッチになる冗長値)。
     * nibble12-15 は次オクターブ側へ繰り上がる(境界は 12 以降、15 単独ではない)。 */
    static const int8_t note_table[16] = {
        3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1, 0, 1, 2, 3
    };
    for (int ch = 0; ch < 8; ch++) {
        /* ALG/FB/PAN は g_opm_written を見ずに g_opm_shadow をそのまま読む
         * (初期値0 = ALG0/FB0/mute、それ自体が有効な状態。OPM は全レジスタ 0 で
         *  リセットされる — opm.cpp:78 `for (i=0x0;i<0x100;i++) SetReg(i,0);`)。 */
        uint8_t reg_alg_fb_pan = g_opm_shadow[0x20 + ch];
        out->ch[ch].alg = reg_alg_fb_pan & 0x07;
        out->ch[ch].fb  = (reg_alg_fb_pan >> 3) & 0x07;
        out->ch[ch].pan = (reg_alg_fb_pan >> 6) & 0x03;
        out->ch[ch].keyon = g_opm_keyon[ch];
        if (g_opm_written[0x28 + ch]) {
            uint8_t kc = g_opm_shadow[0x28 + ch];
            uint8_t nibble = kc & 0x0F;
            int8_t octave = (int8_t)((kc >> 4) & 0x07);
            if (nibble >= 12 && octave < 7) octave++;   /* 次オクターブへ繰上り */
            out->ch[ch].octave = octave;
            out->ch[ch].note   = note_table[nibble];
        } else {
            out->ch[ch].note = -1;
        }
    }
}

/* P631: OPM シンセサイザーパネル(独立ウィンドウ)用の詳細ステータス。
 * 上の mx68k_get_opm_status() と同じ「ロックなしスナップショットコピー」契約。read-only。
 * 既存の mx68k_get_opm_status() は P485 のサウンドモニタが引き続き使うため無改造。
 *
 * note/octave の算出は上の mx68k_get_opm_status() と同一の note_table / 繰上り条件を
 * 使う(表示が 2 パネル間でずれないようにするため — 式を変える場合は両方直すこと)。
 * CSM(reg $14)に関する既知の制約も上と同じくそのまま当てはまる。 */
void mx68k_get_opm_detail_status(MX68K_OPMDetailStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    extern uint8_t g_opm_shadow[256];
    extern uint8_t g_opm_written[256];
    extern uint8_t g_opm_keyon[8];

    /* mx68k_get_opm_status() と同一の表(そちらのコメントが導出の一次情報源)。 */
    static const int8_t note_table[16] = {
        3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1, 0, 1, 2, 3
    };

    /* ALG ごとのキャリア(最終出力に直接寄与するオペレータ)のビットマスク。
     * ビット位置は **レジスタ上のスロット番号**(TL レジスタ $60+(slot<<3)+ch の
     * slot)であり、fmgen 内部の op[] インデックスとは opm.cpp:278-281 の
     * slottable[4]={0,2,1,3} でずれる点に注意。
     * 導出: fmgen.cpp:823-877 Channel4::Calc() で戻り値 r に加算される op[] を
     * ALG ごとに読み取り、slottable の逆写像(op0→slot0, op1→slot2, op2→slot1,
     * op3→slot3)でレジスタスロット空間へ変換した。
     *   ALG0-3: op3            → slot3            = 0x8
     *   ALG4  : op1,op3        → slot2,slot3      = 0xC
     *   ALG5  : op1,op2,op3    → slot1,slot2,slot3= 0xE
     *   ALG6  : op1,op2,op3    → 同上             = 0xE
     *   ALG7  : op0..op3       → slot0..slot3     = 0xF
     * この値が誤っていると、実際には音量に寄与しないモジュレータの TL を読むことに
     * なり、volume_est が実音量と無関係な値になる。 */
    static const uint8_t carrier_mask[8] = {
        0x8, 0x8, 0x8, 0x8, 0xC, 0xE, 0xE, 0xF
    };

    for (int ch = 0; ch < 8; ch++) {
        MX68K_OPMDetailChannel* d = &out->ch[ch];

        /* ALG/FB/PAN は g_opm_written を見ずにシャドウをそのまま読む(初期値 0 =
         * ALG0/FB0/mute はそれ自体が有効な状態 — mx68k_get_opm_status() と同じ判断)。 */
        uint8_t reg_alg_fb_pan = g_opm_shadow[0x20 + ch];
        d->alg   = reg_alg_fb_pan & 0x07;
        d->fb    = (reg_alg_fb_pan >> 3) & 0x07;
        d->pan   = (reg_alg_fb_pan >> 6) & 0x03;
        d->keyon = g_opm_keyon[ch];

        d->kc_raw = g_opm_shadow[0x28 + ch];
        d->kf_raw = g_opm_shadow[0x30 + ch];

        /* 4 スロット分の TL 生値(bit0-6。bit7 は TL レジスタでは未使用)。 */
        for (int slot = 0; slot < 4; slot++) {
            d->tl[slot] = g_opm_shadow[0x60 + (slot << 3) + ch] & 0x7F;
        }

        d->written = g_opm_written[0x28 + ch] ? true : false;
        if (d->written) {
            uint8_t kc     = d->kc_raw;
            uint8_t nibble = kc & 0x0F;
            int8_t  octave = (int8_t)((kc >> 4) & 0x07);
            if (nibble >= 12 && octave < 7) octave++;   /* 次オクターブへ繰上り */
            d->octave = octave;
            d->note   = note_table[nibble];
        } else {
            d->note = -1;
        }

        /* ★volume_est は **推定値であり確定仕様ではない**。
         * XM6 系の「サウンドシンセサイザー」パネルが表示する V:XXX の算出式は、
         * 当該パネルが XM6 本家には存在せず(TypeG 等の後発版固有)ソースが非公開の
         * ため検証できていない。ユーザー提供スクリーンショットの 1 例(CH1 V:122)が
         * 「127 - キャリア TL」と符合したことからの推定にとどまる。
         * 実際のチップ音量は EG(エンベロープ)位相・LFO・KS 等にも依存するため、
         * ここで出るのはあくまで「レジスタ設定上の最大音量の目安」である。
         * 複数キャリアがある場合は最も大きく鳴るもの(= TL 最小)を採る。 */
        uint8_t mask = carrier_mask[d->alg];
        int min_tl = 127;
        for (int slot = 0; slot < 4; slot++) {
            if (mask & (1u << slot)) {
                if ((int)d->tl[slot] < min_tl) min_tl = (int)d->tl[slot];
            }
        }
        int vol = 127 - min_tl;
        if (vol < 0)   vol = 0;
        if (vol > 127) vol = 127;
        d->volume_est = (int32_t)vol;
    }
}

/* P491: サウンドモニタ(Mercury Unit FM 部 = YMF288)。上の mx68k_get_opm_status() と
 * 同じ「ロックなしスナップショットコピー」契約。read-only。
 *
 * 全ての値は Bridge 側 OPN シャドウ(Bridge/mercury_opn_shadow.h、書込みは
 * m68000_bridge.c の CPU 書込みフック)から導出する。チップから読み戻せない理由と
 * 2 チップ目へ到達できない制約は同ヘッダ参照。
 *
 * ★F-Num/Block は生値のまま返す(音名へは変換しない): OPM の KC/KF と違い YMF288 は
 * F-Number × Block の周波数表現で、fmgen 内部は Hz を持たず位相増分しか持たない
 * (fmgen.cpp:500-506)ため、音名式を一次情報源から確定できていない。 */
void mx68k_get_mercury_opn_status(MX68K_MercuryOPNStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->installed = g_mercury_installed ? true : false;
    if (!out->installed) return;

    out->write_count = g_mcry_opn_write_count;

    /* FM 6ch。ch0-2 はバンク0、ch3-5 はバンク1(レジスタ番号 +0x100、opna.cpp:547-548,
     * 554-555, 570-571, 577-578)。バンク内のチャンネル番号は c%3。 */
    for (int c = 0; c < 6; c++) {
        int base = (c < 3) ? 0x000 : 0x100;
        int i    = base + (c % 3);
        /* F-Number 11bit は 2 レジスタに分かれる: 下位8bit が $A0+c、上位3bit が
         * $A4+c の bit0-2、Block は同 bit3-5(opna.cpp:549-558 + fmgen.cpp:500-506)。 */
        uint8_t lo = g_mcry_opn_shadow[0xA0 + i];
        uint8_t hi = g_mcry_opn_shadow[0xA4 + i];
        out->fm[c].fnum  = (uint16_t)(lo + (uint16_t)(hi & 0x07) * 0x100);
        out->fm[c].block = (uint8_t)((hi >> 3) & 0x07);
        /* ALG/FB は $B0+c(opna.cpp:572-575)、PAN は $B4+c の bit6-7(opna.cpp:579-582)。
         * g_mcry_opn_written は見ない — 全レジスタ 0 は「ALG0/FB0/mute」という
         * それ自体有効な状態だから(P485 の OPM 側と同じ判断)。 */
        uint8_t reg_b0 = g_mcry_opn_shadow[0xB0 + i];
        out->fm[c].alg = (uint8_t)(reg_b0 & 0x07);
        out->fm[c].fb  = (uint8_t)((reg_b0 >> 3) & 0x07);
        out->fm[c].pan = (uint8_t)((g_mcry_opn_shadow[0xB4 + i] >> 6) & 0x03);
        out->fm[c].keyon = g_mcry_opn_keyon[c];
    }

    /* SSG 3ch。ミキサ $07 は負論理(psg.cpp:222-224 の `r7 = ~reg[7]`)。 */
    {
        uint8_t r7 = (uint8_t)~g_mcry_opn_shadow[0x07];
        for (int c = 0; c < 3; c++) {
            uint16_t period = (uint16_t)(g_mcry_opn_shadow[2 * c] +
                                         (uint16_t)g_mcry_opn_shadow[2 * c + 1] * 256);
            uint8_t vol = g_mcry_opn_shadow[0x08 + c];
            out->ssg[c].period   = (uint16_t)(period & 0x0FFF);   /* psg.cpp:154-170 */
            out->ssg[c].tone_on  = (r7 & (1u << c)) != 0;
            out->ssg[c].noise_on = (r7 & (1u << (c + 3))) != 0;
            out->ssg[c].volume   = (uint8_t)(vol & 0x0F);         /* psg.cpp:177-187 */
            out->ssg[c].env_on   = (vol & 0x10) != 0;
        }
        out->noise_period = (uint8_t)(g_mcry_opn_shadow[0x06] & 0x1F);   /* psg.cpp:172-175 */
    }

    /* リズム。$10 は累積(シャドウ側で |= / &=~ 追従済み)、$11 の総音量は
     * `rhythmtl = ~data & 63`(opna.cpp:2091-2093)。 */
    out->rhythmkey = g_mcry_rhythmkey;
    out->rhythm_total_level = (uint8_t)(~g_mcry_opn_shadow[0x11] & 0x3F);
}

/* P635: サウンドモニタ(Mercury Unit の PCM 部)。上の
 * mx68k_get_mercury_opn_status() と同じ「ロックなしスナップショットコピー」契約、
 * read-only(Core 状態は一切変更しない)。
 *
 * OPN 側と違いシャドウを必要としないのは、PCM の状態変数が mercury.c 内で
 * 非 static だから: 直読できる。ただし mercury.h はこれらを宣言していないため
 * (宣言されているのは Mcry_LRTiming のみ)、既存の m68000_bridge.c:428-434 と
 * 同じ方針でここに独立して extern 宣言する(同一型の重複宣言は C では合法)。
 * 型は Core/px68k/x68k/mercury.c を直読して確認済み(:25 int32_t / :27 uint8_t /
 * :29-30 int16_t)。いずれも読むだけで書き換えない。
 *
 * ★Mcry_SampleCnt(FIFO 残量)は mercury.c 内 static でゲッターも無いため
 * 表示できない。PCM が実際に動いているかは write_count(P636 以降は Bridge 側の
 * サンプル値変化カウンタ g_mcry_pcm_sample_change_count)で判定する。 */
extern int32_t Mcry_ClockRate;      /* Core/px68k/x68k/mercury.c:25 */
extern uint8_t Mcry_Status;         /* 同 :27 */
extern int16_t Mcry_OutDataL;       /* 同 :29 */
extern int16_t Mcry_OutDataR;       /* 同 :30 */

void mx68k_get_mercury_pcm_status(MX68K_MercuryPCMStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->installed = g_mercury_installed ? true : false;
    if (!out->installed) return;

    /* Mcry_Status のビット割当は mercury.c の実コードが出典:
     *   bit1 (&2) ステレオ/モノラル切替 (:160)
     *   bit2 (&4) L 有効 — 0 なら書込みデータが 0 に差し替えられる (:177, :192)
     *   bit3 (&8) R 有効 — 同上 (:163, :191) */
    out->stereo     = (Mcry_Status & 0x02) != 0;
    out->l_enabled  = (Mcry_Status & 0x04) != 0;
    out->r_enabled  = (Mcry_Status & 0x08) != 0;
    out->clock_rate = Mcry_ClockRate;
    out->last_out_l = Mcry_OutDataL;
    out->last_out_r = Mcry_OutDataR;
    /* P636: 意味が「データポートへの書込み回数」から「毎スキャンラインの
     * サンプル値ポーリングで変化を検出した回数」へ変わった(フィールド名
     * write_count は構造体の後方互換のため維持)。EmulatorBridge.h の
     * 構造体コメント参照。 */
    out->write_count = g_mcry_pcm_sample_change_count;
}

/* P549: サウンドモニタ(CoreAudio バッファのアンダーラン統計)。上の
 * mx68k_get_mercury_opn_status() と同じ「ロックなしスナップショットコピー」契約。
 * ★atomic_load のみ(atomic_exchange は使わない): P464 のウィンドウ方式と違い、
 * UI 向けの累積値は読んでも減らない。
 * ★この関数も意図的に #if P464_RINGFILL_ENABLE の外に置く(診断マクロ非依存)。
 * 3 つのアトミックを個別にロードするため、読み取りタイミングのずれで理論上
 * underrun_callbacks が callbacks_total をわずかに上回る瞬間があり得る ——
 * 割合(%)表示側でクランプすること(SoundMonitorView.swift 参照)。 */
void mx68k_get_audio_buffer_status(MX68K_AudioBufferStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->callbacks_total     = atomic_load_explicit(&s_p549_cum_callbacks_total,     memory_order_relaxed);
    out->underrun_callbacks  = atomic_load_explicit(&s_p549_cum_underrun_callbacks,  memory_order_relaxed);
    out->samples_zero_filled = atomic_load_explicit(&s_p549_cum_samples_zero_filled, memory_order_relaxed);
}

static int mx68k_collect_sprite_entries(MX68KSpriteEntry* out_entries, int max_entries, bool active_only) {
    if (!out_entries || max_entries <= 0) return 0;
    extern uint8_t Sprite_Regs[0x800];
    typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) SprEnt;
    const SprEnt *sct = (const SprEnt *)Sprite_Regs;
    int cnt = 0;
    for (int n = 0; n < 128 && cnt < max_entries; n++) {
        uint16_t px = sct[n].posx & 0x3ff, py = sct[n].posy & 0x3ff;
        if (active_only && px == 0 && py == 0) continue;   /* 既存のP278/bgStatusと同じ簡易フィルタ */
        MX68KSpriteEntry *e = &out_entries[cnt];
        e->slot       = n;
        e->x          = px;
        e->y          = py;
        e->pattern    = sct[n].ctrl & 0xff;
        e->palette_hi = (sct[n].ctrl >> 4) & 0xf0;
        e->hflip      = (sct[n].ctrl & 0xc000) == 0x4000 || (sct[n].ctrl & 0xc000) == 0xc000;
        e->vflip      = (sct[n].ctrl & 0xc000) == 0x8000 || (sct[n].ctrl & 0xc000) == 0xc000;
        e->priority   = sct[n].ply & 3;
        cnt++;
    }
    return cnt;
}

int mx68k_get_sprite_list(MX68KSpriteEntry* out_entries, int max_entries) {
    return mx68k_collect_sprite_entries(out_entries, max_entries, true);
}

int mx68k_get_sprite_table_full(MX68KSpriteEntry* out_entries, int max_entries) {
    return mx68k_collect_sprite_entries(out_entries, max_entries, false);
}

void mx68k_get_sprite_pattern_rgba(int slot, uint8_t* out_rgba) {
    if (!out_rgba || slot < 0 || slot >= 128) return;
    memset(out_rgba, 0, 16 * 16 * 4);
    extern uint8_t  Sprite_Regs[0x800];
    extern uint8_t  BGCHR16[16 * 16 * 256];
    extern uint32_t TextPal32[256];
    typedef struct { uint16_t posx, posy, ctrl, ply; } __attribute__((packed)) SprEnt;
    const SprEnt *sct = (const SprEnt *)Sprite_Regs;
    uint16_t ctrl = sct[slot].ctrl;
    uint32_t pat_base = ((uint32_t)ctrl << 8) & 0xff00;   /* bg.c:334-345と同じオフセット式 */
    int hflip = (ctrl & 0xc000) == 0x4000 || (ctrl & 0xc000) == 0xc000;
    int vflip = (ctrl & 0xc000) == 0x8000 || (ctrl & 0xc000) == 0xc000;
    uint8_t pal_hi = (uint8_t)((ctrl >> 4) & 0xf0);
    for (int row = 0; row < 16; row++) {
        int src_row = vflip ? (15 - row) : row;
        for (int col = 0; col < 16; col++) {
            int src_col = hflip ? (15 - col) : col;
            uint8_t dot = BGCHR16[pat_base + src_row * 16 + src_col] & 0x0f;
            uint32_t *px = (uint32_t*)(out_rgba + (row * 16 + col) * 4);
            if (dot == 0) {
                *px = 0;   /* 透過 */
            } else {
                uint8_t pal = dot | pal_hi;
                *px = px68k_color_to_rgba(TextPal32[pal]) ;
            }
        }
    }
}

/* P356: Text Plane Viewerでインデックス0を透過(黒背景)ではなく
 * TextPal32[0](背景色)で塗るかどうかのトグル。デフォルトON——P354/P355
 * のCode InvestigationでXM6/px68k本家の実装(単独面バッファは常に
 * pal[0]で塗る、透過は合成工程の担当)と一致させることが正しい既定と
 * 確定したため。OFFにすると旧来の透過(黒)表示に戻せる(比較用)。 */
static int s_text_plane_backdrop = 1;

void mx68k_set_text_plane_backdrop(int enabled) {
    s_text_plane_backdrop = enabled ? 1 : 0;
}

/* P343: dump the derived text-plane buffer (TextDrawWork, 1024x1024) as RGBA8888
 * so a monitor panel can view its raw contents, the same way XM6's BG monitor
 * shows a page. Read-only; palette index maps directly through TextPal32 exactly
 * as Text_DrawLine does (tvram.c:245/257). t==0 -> TextPal32[0](backdrop, P356
 * default) or transparent (P356 toggled off; same convention as
 * mx68k_get_sprite_pattern_rgba). */
void mx68k_get_text_plane_rgba(uint8_t* out_rgba) {
    if (!out_rgba) return;
    extern uint8_t  TextDrawWork[1024*1024 + 100];
    extern uint32_t TextPal32[256];
    for (int y = 0; y < 1024; y++) {
        for (int x = 0; x < 1024; x++) {
            uint8_t t = TextDrawWork[y * 1024 + x] & 0x0f;
            uint32_t *px = (uint32_t*)(out_rgba + (y * 1024 + x) * 4);
            if (t == 0) {
                *px = s_text_plane_backdrop
                    ? px68k_color_to_rgba(TextPal32[0])   /* P356: 背景色描画(既定) */
                    : 0;                                   /* 透過(旧来動作、比較用) */
            } else {
                *px = px68k_color_to_rgba(TextPal32[t]);
            }
        }
    }
}

/* P348: dump BG page 0/1 to a 1024x1024 RGBA buffer for a monitor panel, the same
 * way XM6's BG page monitor lets you see the raw tilemap+pattern content. Reuses
 * the existing Core tile decoder (bg_drawline_loopx16/x8, bg.c) one row at a time
 * instead of re-implementing its flip-bit tricks — safer and byte-identical to
 * what the real compositor would draw. Saves/restores VLINEBG/BG_VLINE/TextDotX
 * so this on-demand dump doesn't disturb the next real frame's rendering. */
void mx68k_get_bg_page_rgba(int page, uint8_t* out_rgba, int* out_size) {
    if (!out_rgba) return;
    extern void bg_drawline_loopx16(uint16_t, uint32_t, uint32_t, int32_t, int32_t);
    extern void bg_drawline_loopx8(uint16_t, uint32_t, uint32_t, int32_t, int32_t);
    memset(out_rgba, 0, 1024 * 1024 * 4);
    int size = BG_CHRSIZE * 64;   /* 512 (CHRSIZE=8) or 1024 (CHRSIZE=16) */
    if (out_size) *out_size = size;
    uint16_t bgtop = (page == 0) ? BG_BG0TOP : BG_BG1TOP;

    int32_t save_vlinebg = VLINEBG, save_bgvline = BG_VLINE;
    int save_textdotx = TextDotX;
    BG_VLINE = 0;
    TextDotX = size;
    for (int line = 0; line < size; line++) {
        VLINEBG = line;
        memset(BG_LineBuf32 + 16, 0, sizeof(uint32_t) * size);
        memset(Text_TrFlag + 16, 0, size);
        if (BG_CHRSIZE == 8) bg_drawline_loopx8(bgtop, 0, 0, 0, 0);
        else                 bg_drawline_loopx16(bgtop, 0, 0, 0, 0);
        for (int x = 0; x < size; x++) {
            /* Matches the real compositor's has_bg test exactly (EmulatorBridge.c
             * ~4494: (Text_TrFlag[off]&2) && tcol, where tcol IS BG_LineBuf32[off]) —
             * a set flag with color 0 (palette-bank-only dat) is still transparent. */
            if ((Text_TrFlag[16 + x] & 2) && BG_LineBuf32[16 + x] != 0) {
                uint32_t *px = (uint32_t*)(out_rgba + (line * 1024 + x) * 4);
                *px = px68k_color_to_rgba(BG_LineBuf32[16 + x]);
            }
        }
    }
    VLINEBG = save_vlinebg; BG_VLINE = save_bgvline; TextDotX = save_textdotx;
}

/* P348/P690: dump a graphics-page (GRP) to a 512x512 RGBA buffer. GVRAM
 * addressing mirrors the real per-mode read paths in gvram.c
 * (Grp_DrawLine4 / Grp_DrawLine8TR / Grp_DrawLine16) with the scanline scroll
 * terms dropped — a direct, scanline-independent page dump.
 * Returns 0 (buffer left zeroed) for modes this dump does not cover. */
extern uint16_t Pal16Adr[256];   /* gvram.c:22 — not declared in gvram.h */

/* P690: D11 (CRTC R20 bit3) をここに一元化する。gvram.c:94 GVRAM_Read /
 * :159 GVRAM_Write は D11 が立つ間 &3 の値に関わらず GVRAM 全域を 65536 色の
 * 生バイト配置として読み書きするため、モニタ側も同じ優先順位で判定しないと
 * 誤ったスキームで復号してしまう。mx68k_get_grp_page_rgba() と
 * mx68k_get_grp_page_count() が同じ式を使うことを保証するためのヘルパ。 */
static int grp_page_mode(void) {
    return (CRTC_Regs[0x28] & 8) ? 3 : (CRTC_Regs[0x28] & 3);
}

int mx68k_get_grp_page_rgba(int page, uint8_t* out_rgba) {
    if (!out_rgba) return 0;
    memset(out_rgba, 0, 512 * 512 * 4);
    int mode = grp_page_mode();   /* 0=16色4面 / 1,2=256色2面 / 3=65536色1面 */
    if (mode == 0 && (CRTC_Regs[0x28] & 4)) return 0;   /* 1024dot モードは対象外 */
    for (int y = 0; y < 512; y++) {
        for (int x = 0; x < 512; x++) {
            uint32_t base = (uint32_t)(y & 0x1ff) * 1024 + (uint32_t)(x & 0x1ff) * 2;
            uint32_t col = 0;
            if (mode == 0) {                     /* 16色4面(gvram.c Grp_DrawLine4 準拠) */
                int p = page & 3;
                uint8_t idx = (uint8_t)((GVRAM[base + (p >> 1)] >> (4 * (p & 1))) & 0x0f);
                if (idx) col = px68k_color_to_rgba(GrphPal32[idx]);
            } else if (mode == 3) {              /* 65536色1面(gvram.c Grp_DrawLine16 準拠) */
                uint32_t v = *(uint16_t*)(GVRAM + base);
                if (v) {
                    uint32_t v0 = (v >> 8) & 0xff;
                    v &= 0x00ff;
                    v  = Pal_Regs[Pal16Adr[v]];
                    v |= (uint32_t)Pal_Regs[Pal16Adr[v0] + 2] << 8;
                    col = px68k_color_to_rgba(Pal32[v]);
                }
            } else {                             /* 256色2面(gvram.c Grp_DrawLine8TR 準拠) */
                uint8_t idx = GVRAM[base + (page & 1)];
                if (idx) col = px68k_color_to_rgba(GrphPal32[idx]);
            }
            if (col) *(uint32_t*)(out_rgba + (y * 512 + x) * 4) = col;
        }
    }
    return 1;
}

/* P690: 現在の色モードで有効なページ数(Picker 段数の動的化用)。
 * 0=非対応(16色1024dot モード)、1=65536色、2=256色、4=16色。 */
int mx68k_get_grp_page_count(void) {
    int mode = grp_page_mode();
    if (mode == 0) return (CRTC_Regs[0x28] & 4) ? 0 : 4;
    if (mode == 3) return 1;
    return 2;
}

/* P690: D11(px68k 実装が「Nemesis 技法」と呼ぶ拡張 VRAM 配置)経路で復号したか。
 * この経路はバイト配置こそ Core の GVRAM_Read/_Write と一致させているが、
 * 色解釈(パレット合成式)の実機仕様との厳密な一致は未検証のため、UI 側で
 * 「参考表示」である旨の注記を出すのに使う。 */
bool mx68k_get_grp_page_uses_nemesis(void) {
    return (CRTC_Regs[0x28] & 8) != 0;
}

/* P365: BG+スプライト面の合成結果(GRP/テキストとの最終優先度合成"前")を
 * disp_w x disp_h の RGBA バッファとして返す。中身は毎走査線の合成ループが
 * 直接書き込んでいるため、ここでは現在の表示サイズ分だけをコピーする。 */
void mx68k_get_bgsp_composite_rgba(uint8_t* out_rgba, int* out_w, int* out_h) {
    if (out_w) *out_w = s_render_disp_w;
    if (out_h) *out_h = s_render_disp_h;
    if (!out_rgba || s_render_disp_w <= 0 || s_render_disp_h <= 0) return;
    memcpy(out_rgba, s_bgsp_buffer, (size_t)s_render_disp_w * s_render_disp_h * 4);
}

/* P373: 現在のフレーム番号。モニタパネルの状態スタンプ表示用。read-only。 */
int mx68k_get_frame_num(void) {
    return g_mx68k_frame_num;
}

/* P383: 状態スタンプ用のレジスタ生値。mx68k_get_vc_status/mx68k_get_bg_status は
 * Swift 側で ~1Hz ゲート越しのスナップショットとして保持されるため、毎レンダリング
 * 更新される frame_num と並べると最大約60フレームずれる。スタンプが必要とする3値
 * だけをゲート無しで返す。read-only。 */
uint8_t mx68k_get_vc_reg1_0_live(void) { return VCReg1[0]; }
uint8_t mx68k_get_vc_reg2_1_live(void) { return VCReg2[1]; }
uint8_t mx68k_get_bg_regs9_live(void) { return BG_Regs[9]; }

/* P211: expose the guest's current vertical-sync rate so the Swift frame driver
 * can pace mx68k_run_frame() to wall-clock. CRTC R20 (CRTC_Regs[0x29]) bit4 selects
 * the horizontal scan rate: hi-res 31.5kHz -> 55.46Hz, lo-res 15.98kHz -> 61.46Hz.
 * (10e6 / VSYNC_HIGH 180310 = 55.46; 10e6 / VSYNC_NORM 162707 = 61.46). Read-only;
 * matches the existing CRTC_Regs[0x29] & 0x10 hi-res test used for the clock total. */
double mx68k_get_vsync_hz(void) {
    /* P641(D-65): 直近フレームが実際に使った1フィールド予算(名目10MHz基準、
     * クロック倍率スケーリング前)から算出する。これにより CRTC レジスタ
     * (R00/R04/R20/HRL)の書換えに垂直周波数が追従する。最初のフレームより前は
     * 未計測(0)なので、従来の R20 bit4 固定2値へ退避する。
     * ★ここから CRTC_GetFrameClocks() を直接呼んではならない——分数状態を
     *   進めてしまうため(この関数は1フレームに複数回呼ばれる)。 */
    int32_t fc = g_p641_frame_clocks_10m;
    if (fc > 0) {
        return 10000000.0 / (double)fc;
    }
    return (CRTC_Regs[0x29] & 0x10) ? 55.46 : 61.46;
}

/* P228: ソフトウェアキーボードの LED インジケータ用。keyLED は負論理
 * (図5-7: 0=点灯・1=消灯)の7ビット + D7=コマンド識別ビット。
 * bit0=かな bit1=ローマ字 bit2=コード入力 bit3=CAPS bit4=INS
 * bit5=ひらがな bit6=全角。 */
uint8_t mx68k_get_key_led(void) {
    return keyLED;
}

/* ==== P600: メモリダンプビューア / メモリマップビューア =========================
 *
 * 設計の要は「Core のディスパッチ表(MemReadTable)を一切通さない」こと。
 * あの表の read ハンドラは実機同等の副作用を持つ(EmulatorBridge.h の
 * MX68KMemKind 宣言まわりのコメントに一次情報源を列挙)ため、モニタが値を覗いた
 * だけでゲスト状態が進む。代わりに Bridge 側で独自のリージョン表を持ち、
 * フラットな実体バッファ(MEM/GVRAM/TVRAM/FONT/IPL/SRAM/SRAM拡張)だけを直接
 * 索引する。p47_read_long_le()(このファイル上部)と同型の明示境界チェック方式。
 *
 * 区間境界は Core:x68k/mem_wrap.c:58-96 の MemReadTable[] を実地に読んで導出した。
 * 同表は (addr >> 13) & 0xff で索く 8KB 単位の表なので、index n の担当範囲は
 * $E80000 起点で言えば n=0x40 → $E80000-$E81FFF … という対応になる。
 * 「I/O 窓」と一括りにされがちな $E80000-$ECFFFF の内側にも rm_buserr のままの
 * 空き窓が複数あり($EA2000-$EADFFF / $EC0000-$ECBFFF / $ECE000-$ECFFFF)、
 * さらに SRAM 末尾と FONT 先頭の間にも 128KB の空き窓($EE0000-$EFFFFF)がある。 */

typedef enum {
    P600_STORE_NONE = 0,     /* 実体を持たない(読み出し不可の窓) */
    P600_STORE_MEM,
    P600_STORE_GVRAM,
    P600_STORE_TVRAM,
    P600_STORE_FONT,
    P600_STORE_IPL,
    P600_STORE_SRAM,
    P600_STORE_SRAM_EXT
} P600Storage;

/* 装着状態ゲート。「設定値」ではなく「配線確定値」で判定する(P457)。 */
typedef enum {
    P600_GATE_ALWAYS = 0,
    P600_GATE_SCSI_EXT,
    P600_GATE_MIDI,
    P600_GATE_MERCURY,
    P600_GATE_SRAM_EXT
} P600Gate;

typedef struct {
    uint32_t    base;
    uint32_t    size;
    uint8_t     kind;      /* ゲートが開いている場合の MX68KMemKind */
    uint8_t     storage;   /* P600Storage */
    uint8_t     gate;      /* P600Gate */
    const char* name;      /* UTF-8、MX68KRegionInfo.name[32] に収まる長さ */
} P600Region;

static const P600Region s_p600_regions[] = {
    { 0x000000u, 0xC00000u, MX68K_MEMKIND_FLAT_SWAP,     P600_STORE_MEM,      P600_GATE_ALWAYS,   "Main RAM" },
    { 0xC00000u, 0x200000u, MX68K_MEMKIND_DECODED,       P600_STORE_GVRAM,    P600_GATE_ALWAYS,   "GVRAM" },
    { 0xE00000u, 0x080000u, MX68K_MEMKIND_FLAT_SWAP,     P600_STORE_TVRAM,    P600_GATE_ALWAYS,   "TVRAM" },
    /* $E80000-$E95FFF: CRTC / パレット・VC / DMA / MFP / RTC / SysPort / OPM / ADPCM / FDC */
    { 0xE80000u, 0x016000u, MX68K_MEMKIND_IO_UNSAFE,     P600_STORE_NONE,     P600_GATE_ALWAYS,   "内蔵I/O" },
    /* $E96000-$E97FFF: 機種により SASI か内蔵SCSI(SPC)。名称は配線確定機種で切替える。 */
    { 0xE96000u, 0x002000u, MX68K_MEMKIND_IO_UNSAFE,     P600_STORE_NONE,     P600_GATE_ALWAYS,   "SASI" },
    /* $E98000-$E9FFFF: SCC / PPI / IOC */
    { 0xE98000u, 0x008000u, MX68K_MEMKIND_IO_UNSAFE,     P600_STORE_NONE,     P600_GATE_ALWAYS,   "内蔵I/O" },
    { 0xEA0000u, 0x002000u, MX68K_MEMKIND_IO_UNSAFE,     P600_STORE_NONE,     P600_GATE_SCSI_EXT, "外付けSCSI (CZ-6BS1)" },
    { 0xEA2000u, 0x00C000u, MX68K_MEMKIND_BUSERR,        P600_STORE_NONE,     P600_GATE_ALWAYS,   "未使用領域" },
    { 0xEAE000u, 0x002000u, MX68K_MEMKIND_IO_UNSAFE,     P600_STORE_NONE,     P600_GATE_MIDI,     "MIDI (CZ-6BM1)" },
    { 0xEB0000u, 0x010000u, MX68K_MEMKIND_IO_UNSAFE,     P600_STORE_NONE,     P600_GATE_ALWAYS,   "スプライト / BG" },
    { 0xEC0000u, 0x00C000u, MX68K_MEMKIND_BUSERR,        P600_STORE_NONE,     P600_GATE_ALWAYS,   "未使用領域" },
    { 0xECC000u, 0x002000u, MX68K_MEMKIND_IO_UNSAFE,     P600_STORE_NONE,     P600_GATE_MERCURY,  "Mercury Unit" },
    { 0xECE000u, 0x002000u, MX68K_MEMKIND_BUSERR,        P600_STORE_NONE,     P600_GATE_ALWAYS,   "未使用領域" },
    { 0xED0000u, 0x004000u, MX68K_MEMKIND_FLAT_SWAP,     P600_STORE_SRAM,     P600_GATE_ALWAYS,   "内蔵SRAM 16KB" },
    { 0xED4000u, 0x00C000u, MX68K_MEMKIND_FLAT_SWAP,     P600_STORE_SRAM_EXT, P600_GATE_SRAM_EXT, "SRAM拡張 48KB" },
    /* $EE0000-$EFFFFF: MemReadTable index 112-127 が全て rm_buserr の 128KB 空き窓。 */
    { 0xEE0000u, 0x020000u, MX68K_MEMKIND_BUSERR,        P600_STORE_NONE,     P600_GATE_ALWAYS,   "未使用領域" },
    { 0xF00000u, 0x0C0000u, MX68K_MEMKIND_FLAT_SWAP,     P600_STORE_FONT,     P600_GATE_ALWAYS,   "CGROM / FONT" },
    { 0xFC0000u, 0x040000u, MX68K_MEMKIND_FLAT_RAW,      P600_STORE_IPL,      P600_GATE_ALWAYS,   "IPL-ROM" },
};

#define P600_REGION_COUNT ((int)(sizeof(s_p600_regions) / sizeof(s_p600_regions[0])))

/* 表に無いアドレスは NULL を返す。呼出し側は必ず BUSERR へ倒すこと(安全側 catch-all)
 * ——上流 Core の変更で新たな空き窓が生じても、黙って「読めるように見える」方向へは
 * 倒れない。 */
static const P600Region* p600_find_region(uint32_t addr) {
    for (int i = 0; i < P600_REGION_COUNT; i++) {
        const P600Region* r = &s_p600_regions[i];
        if (addr >= r->base && (addr - r->base) < r->size) return r;
    }
    return NULL;
}

static int p600_gate_open(uint8_t gate) {
    switch (gate) {
    case P600_GATE_ALWAYS:   return 1;
    case P600_GATE_SCSI_EXT: return g_scsi_ext_board_wired ? 1 : 0;
    case P600_GATE_MIDI:     return g_midi_installed ? 1 : 0;
    case P600_GATE_MERCURY:  return g_mercury_installed ? 1 : 0;
    case P600_GATE_SRAM_EXT: return sram_ext_is_enabled() ? 1 : 0;
    default:                 return 0;
    }
}

static uint8_t p600_effective_kind(const P600Region* r) {
    if (!r) return MX68K_MEMKIND_BUSERR;
    if (!p600_gate_open(r->gate)) return MX68K_MEMKIND_NOT_INSTALLED;
    return r->kind;
}

/* P740: 逆アセンブル 1 命令。Core 同梱の Debabelizer(m68k_disassemble())は
 * オペコード語+拡張語を連続して読み進むが、その過程で領域境界を一切意識しない。
 * 開始アドレスだけを安全領域と確認しても、命令が長い場合は読取りの終端が隣接する
 * 未安全領域(例: TVRAM $E7FFFF の直後にある内蔵I/O $E80000)へはみ出しうる
 * ——そこを塞ぐため、開始アドレスが属する領域の残りバイト数が最悪ケース命令長を
 * 下回る場合は、たとえ領域内であっても m68k_disassemble() を一切呼ばずに拒否する。
 *
 * MX68K_DASM_MAX_INSN_BYTES の導出 [training knowledge]: 68000 の最長命令は
 * MOVE.L $abs32,$abs32(オペコード 2B + ソース 4B + デスティネーション 4B = 10B)。
 * これに 2 バイトの安全余裕を足して 12 とした(書籍ページでの直接裏取りは未実施)。 */
#define MX68K_DASM_MAX_INSN_BYTES 12u

uint32_t mx68k_disassemble_line(uint32_t addr, char* out_text, uint32_t out_text_len) {
    if (!out_text || out_text_len == 0) return 2;  /* 68000 命令の最小長 */

    uint32_t a = addr & 0x00FFFFFFu;   /* 24bit 空間、他の P600 アクセサと同じ規約 */
    const P600Region* r = p600_find_region(a);
    uint8_t kind = p600_effective_kind(r);

    /* ★安全ゲート: cpu_readmem24 経由の副作用(I/O ハンドシェイク進行等)を避けるため、
     * Memory Viewer(P600)が「読める」と分類する領域(FLAT_SWAP/FLAT_RAW/DECODED =
     * RAM/GVRAM/TVRAM/SRAM/CGROM/IPL-ROM)に限定し、IO_UNSAFE/BUSERR/NOT_INSTALLED は
     * m68k_disassemble() を一切呼ばずプレースホルダを返す。加えて、開始アドレスが
     * 安全領域内でもその領域の残りバイト数が最悪ケース命令長未満なら同様に拒否する。 */
    if (!(kind == MX68K_MEMKIND_FLAT_SWAP ||
          kind == MX68K_MEMKIND_FLAT_RAW  ||
          kind == MX68K_MEMKIND_DECODED) ||
        !r ||
        (a - r->base) + MX68K_DASM_MAX_INSN_BYTES > r->size) {
        snprintf(out_text, out_text_len, "(unreadable)");
        return 2;
    }

    /* d68k.c は内部の g_dasm_str[100] + g_helper_str[100] を sprintf で連結するため
     * 呼出し側バッファは 256 バイト以上を推奨(d68k.c:169-170)。 */
    char buf[256];
    buf[0] = '\0';
    int32_t len = m68k_disassemble(buf, (int32_t)a);
    if (len < 2) len = 2;  /* 防御的フロア。68000 命令は常に偶数長・最小 2 バイト */

    strncpy(out_text, buf, (size_t)out_text_len - 1);
    out_text[out_text_len - 1] = '\0';
    return (uint32_t)len;
}

/* 1 バイト読む。成功=1 / 実体が無い・境界外=0(呼出し側が 0xFF + BUSERR へ倒す)。 */
static int p600_read_flat(const P600Region* r, uint32_t addr, uint8_t* out) {
    uint32_t off = addr - r->base;
    switch (r->storage) {
    case P600_STORE_MEM:
        /* ★MEM は MX68K_RAM_BYTES の直後に 4 バイト + P565_MEM_GUARD_BYTES(64KB)の
         *   ホスト側ガードを抱えている。ゲスト空間は $BFFFFF までなので、ガードには
         *   絶対に届かせない(off < MX68K_RAM_BYTES かつ off は偶数境界内で ^1 する
         *   だけなので上限を越えない)。 */
        if (!MEM || off >= (uint32_t)MX68K_RAM_BYTES) return 0;
        *out = MEM[off ^ 1u];
        return 1;
    case P600_STORE_GVRAM:
        /* CPU 窓 $C00000-$DFFFFF の 2MB は CRTC_Regs[0x28] の色モードに応じた
         * ビットフィールド抽出で 512KB の物理プレーンへ写る。モード依存の見え方を
         * 再現すると同じアドレスの表示バイト数が設定で変わり hex ダンプとして
         * 一貫しないため、ここでは常に物理プレーンをそのまま見せる(2MB 窓には
         * 4 回ミラーされて見える)。 */
        *out = GVRAM[(off & 0x7FFFFu) ^ 1u];
        return 1;
    case P600_STORE_TVRAM:
        if (off >= 0x80000u) return 0;
        *out = TVRAM[off ^ 1u];
        return 1;
    case P600_STORE_FONT:
        if (!FONT || off >= 0xC0000u) return 0;
        *out = FONT[off ^ 1u];
        return 1;
    case P600_STORE_IPL:
        /* ★IPL だけは ^1 スワップを掛けない。Core 内に 2 つの規約が併存しており
         *   (ワード経路 rm16_main は IPL[addr & 0x1FFFF] の BE 生順、バイト経路
         *   rm_ipl は 256KB マスク + スワップ)、ROM の中身をそのまま見せたい
         *   ビューアが採るべきは前者。後者を採ると $FC0000-$FDFFFF がゼロ埋めの
         *   上位半分を指してしまう。 */
        if (!IPL) return 0;
        *out = IPL[off & 0x1FFFFu];
        return 1;
    case P600_STORE_SRAM:
        if (off >= 0x4000u) return 0;
        *out = SRAM[off ^ 1u];
        return 1;
    case P600_STORE_SRAM_EXT:
        /* P601: Fetch シャドウ(g_sram_fetch_shadow)へ触れない単バイトアクセサを
         * 直接使う。sram_ext_snapshot64() 経由だと UI スレッドが CPU コアの
         * ライブ命令フェッチ元バッファを書き換えてしまう。 */
        if (off >= 0xC000u) return 0;
        *out = sram_ext_read(addr);
        return 1;
    default:
        return 0;
    }
}

void mx68k_read_memory_bytes(uint32_t addr, uint8_t* out, uint8_t* out_kind, uint32_t len) {
    /* ★スレッド安全性: UI スレッドから実体を直読みするため、mx68k_run_frame()
     *   (CVDisplayLink スレッド)と競合して稀に値が裂けることがある。既存の
     *   CPU/Register Viewer 等と同じ「非同期スナップショット」前提のデバッグ用途
     *   として許容する(ロックを取るとエミュレーション本体を止めてしまう)。 */
    if (!out || len == 0) return;

    for (uint32_t i = 0; i < len; i++) {
        /* ★64bit で足す。uint32_t のまま足すと開始アドレスが上端近傍のとき
         *   桁溢れして小さい値へ折り返し、範囲外要求が「読める番地」に化ける。 */
        uint64_t a64 = (uint64_t)addr + (uint64_t)i;
        uint32_t a;
        const P600Region* r;
        uint8_t kind;
        uint8_t byte = 0xFFu;

        /* 24bit 空間を越えた分は折り返さず未マップ扱いにする。 */
        if (a64 > 0xFFFFFFu) {
            out[i] = 0xFFu;
            if (out_kind) out_kind[i] = MX68K_MEMKIND_BUSERR;
            continue;
        }

        a    = (uint32_t)a64;
        r    = p600_find_region(a);
        kind = p600_effective_kind(r);

        if (kind == MX68K_MEMKIND_FLAT_SWAP ||
            kind == MX68K_MEMKIND_FLAT_RAW  ||
            kind == MX68K_MEMKIND_DECODED) {
            if (!p600_read_flat(r, a, &byte)) {
                byte = 0xFFu;
                kind = MX68K_MEMKIND_BUSERR;
            }
        }

        out[i] = byte;
        if (out_kind) out_kind[i] = kind;
    }
}

int mx68k_get_region_map(MX68KRegionInfo* out, int max) {
    int n = 0;
    if (!out || max <= 0) return 0;

    for (int i = 0; i < P600_REGION_COUNT && n < max; i++) {
        const P600Region* r = &s_p600_regions[i];
        uint8_t kind = p600_effective_kind(r);
        const char* name = r->name;

        /* $E96000 の窓は機種で担当デバイスが替わる(配線確定機種 4 == 内蔵SCSI 機)。 */
        if (r->base == 0xE96000u && g_wired_machine_type == 4) name = "内蔵SCSI (SPC)";

        memset(&out[n], 0, sizeof(out[n]));
        out[n].base = r->base;
        out[n].size = r->size;
        strncpy(out[n].name, name, sizeof(out[n].name) - 1);
        switch (kind) {
        case MX68K_MEMKIND_IO_UNSAFE:     out[n].status = 1; break;
        case MX68K_MEMKIND_NOT_INSTALLED: out[n].status = 2; break;
        case MX68K_MEMKIND_BUSERR:        out[n].status = 3; break;
        default:                          out[n].status = 0; break;
        }
        n++;
    }
    return n;
}

// ---- API alias shims (EmulatorBridge.h uses shorter names) ----
void mx68k_set_memory_size(int mb) { mx68k_set_memory_mb(mb); }
void mx68k_set_clock(int mhz)      { mx68k_set_clock_mhz(mhz); }

// ---- Debug helpers ----
void mx68k_log(const char* msg) {
    debug_log("%s\n", msg);
}

void mx68k_dump_framebuffer(void) {
    debug_log("[MX68K] mx68k_dump_framebuffer called\n");
}

void mx68k_set_trace_enabled(bool enabled) {
    (void)enabled;
}

// ---- Musashi dummy stubs (not used when Config.CPU_Emu == 0) ----
void m68k_set_cpu_type(int type) { (void)type; }
void m68k_init(void) {}
void m68k_pulse_reset(void) {}
int m68k_execute(int cycles) { (void)cycles; return 0; }
void m68k_set_irq(int irqline) { (void)irqline; }
uint32_t m68k_get_reg(void *context, int regnum) { (void)context; (void)regnum; return 0; }
void m68k_set_reg(int regnum, uint32_t val) { (void)regnum; (void)val; }
