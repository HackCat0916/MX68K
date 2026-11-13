/* ===========================================================================
 *  P940: FDC/DMA 人為的遅延実験 [P940-FDLATENCY](実験的機能、既定 OFF)
 * ---------------------------------------------------------------------------
 *  仕組み(Core 無改変):
 *   - Core:x68k/dmac.c:236 の転送ループは OCR&3==2(REQG=10、外部要求転送)
 *     のときだけ IsReady[ch]() を評価し、false なら転送しない。
 *   - enabled=1 のとき、DMA_Init()(IsReady[0] を FDC_IsDataReady へ再登録する)
 *     の直後ごとに DMA_SetReadyCB(0, fdlat_is_ready) で差し替える。
 *   - FDD_Seek/Read/Write は必ず mx68k_fdd_usleep_shim() を通るので、そこで
 *     「解放時刻 = 現在ゲスト時間 + delay_ticks」をアームする。
 *   - ゲスト時間は走査線ごとに名目10MHzチックで加算する(1フレームの合計が
 *     厳密に fc になる式)。ゲートが閉じている間も CPU・MFP・VSYNC は進む。
 *
 *  ★enabled=0(環境変数未設定、既定)では DMA_SetReadyCB を一切呼ばず、
 *    各フックは静的フラグ判定で即 return する。状態への書込みはゼロ。
 *  ★macOS 限定: iOS ビルドでは環境変数を無視して常に enabled=0。
 *  ★スレッド: シム・Ready コールバック・走査線ループはいずれもエミュレーション
 *    スレッド上の同期呼出し。FDD_Seek/Read/Write の呼出し元は Core:x68k/fdc.c
 *    (ゲストの FDC コマンド実行)のみで、usleep シムの強制 include も fdd.c のみ
 *    (P940 実装時に grep で確認)。よって s_gt/s_release は素の uint64_t とする。
 *
 *  出力行(全て [P940-FDLATENCY] 接頭辞):
 *   wired(無条件1回)/ install / arm#k(上限64)/ sum(600フレーム毎+atexit)/ reset
 *   P959 追加: irq_hold#k / irq_reissue#k(各上限64)/ irq_r1_hit#k(上限16)
 *
 *  P959 IRQ 保留(MX68K_FLOPPYU_DELAY_SCOPE に "irq" を含む時のみ):
 *   - FDC の完了割込みは FDC_Write/FDC_Read 内で同期的に IRQH_Int(1, FDC_Int) される
 *     (Core:x68k/fdc.c:140、Seek/Recalibrate/ReadID 等)。0x4A アクセス前後で
 *     IRQH_IRQ[1] の 0→1 遷移かつ IRQH_CallBack[1]==FDC_Int を検出したら、
 *     IRQH_IRQCallBack(1) で取り下げ、delay_ticks 経過後に tick_line で再発行する。
 *   - レベル1は FDC/SASI/PRT 共有(Core:x68k/irqh.c:9)。アクセス前から既に
 *     IRQH_IRQ[1]==1 なら他デバイスの保留を壊さないよう触らない(merge_skip)。
 * =========================================================================== */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "fd_latency_experiment.h"
#include "dmac.h"     /* DMA[], DMA_SetReadyCB() */
#include "fdc.h"      /* FDC_IsDataReady() */
#include "irqh.h"     /* IRQH_IRQCallBack() / IRQH_Int()(Core:irqh.h) */

extern void debug_log(const char* fmt, ...);   /* Bridge/EmulatorBridge.c で定義 */
extern int  g_mx68k_frame_num;                 /* フレームカウンタ(EmulatorBridge.c で定義) */

/* P959: Core 側グローバル(読取り+既存 API 経由の操作のみ、Core 無改変) */
extern uint8_t IRQH_IRQ[8];                    /* Core:irqh.c:17、ヘッダ未宣言 */
extern void   *IRQH_CallBack[8];               /* Core:irqh.c:18、ヘッダ未宣言 */
extern int32_t FDC_Int(uint8_t irq);           /* Core:fdc.c:104、ヘッダ未宣言 */
extern uint8_t IOC_IntStat;                    /* Core:ioc.h、既存公開 */

#define P940_ENV_NAME         "MX68K_FLOPPYU_DELAY_MS"
#define P959_SCOPE_ENV_NAME   "MX68K_FLOPPYU_DELAY_SCOPE"
#define P959_IRQ_LOG_MAX      64            /* irq_hold/irq_reissue 個別行の出力上限 */
#define P959_R1_LOG_MAX       16            /* irq_r1_hit 個別行の出力上限 */
#define P959_KIND_DATA        1             /* p939_kind_of() の DATA 分類値 */
#define P940_MAX_MS           10000         /* クランプ上限(設計判断、P961 で 2000→10000) */
#define P940_TICKS_PER_MS     10000ULL      /* 名目10MHz: 1ms = 10,000 tick */
#define P940_SUM_PERIOD       600           /* 集計行の出力間隔(フレーム) */
#define P940_ARM_LOG_MAX      64            /* 個別アーム行の出力上限 */
#define P940_USEC_RW          300u          /* fdd.c の Read/Write の usleep 引数 */

#if defined(X68K_IOS)
#define P940_PLATFORM "ios"
#else
#define P940_PLATFORM "macos"
#endif

static int      s_enabled      = 0;
static int      s_delay_ms     = 0;
static uint64_t s_delay_ticks  = 0;
static int      s_last_fd_fast = 0;
static int      s_atexit_done  = 0;

/* ゲスト時間とゲート */
static uint64_t s_gt       = 0;     /* 累積ゲスト時間(名目10MHzチック) */
static uint64_t s_release  = 0;     /* この時刻以降ならゲート開 */

/* 世代カウンタ(独立に加算——同一関数で加算すると恒真式になるため分離) */
static uint32_t    s_dmainit_gen      = 0;
static uint32_t    s_inst_gen         = 0;
static const char *s_last_dmainit_site = "(none)";

/* DMA_Exec(0) 呼出し箇所での観測(分母) */
static uint64_t s_site_calls = 0;
static uint64_t s_pre_eval   = 0;
static uint64_t s_ocr_h[4]   = {0, 0, 0, 0};
static uint64_t s_dir[2]     = {0, 0};

/* 差し替え関数内のカウント */
static uint64_t s_cb_calls  = 0;
static uint64_t s_cb_rdy0   = 0;
static uint64_t s_cb_closed = 0;
static uint64_t s_cb_open   = 0;

/* アーム関連 */
static uint64_t s_arm       = 0;
static uint64_t s_arm_seek  = 0;
static uint64_t s_arm_rw    = 0;
static uint64_t s_armed_dma = 0;     /* アーム後に cb が1回以上評価されたアーム数 */
static int      s_arm_pending_eval = 0;
static uint64_t s_arm_gt    = 0;
static int      s_last_arm_f = -1;
static uint64_t s_first_eval_lag_max = 0;

/* 集計行の差分計算用 */
static int      s_sum_prev_f  = 0;
static uint64_t s_sum_prev_gt = 0;

/* P959: IRQ 保留状態とカウンタ */
static int         s_irq_scope         = 0;
static const char *s_scope_raw         = NULL;
static int         s_irq_held          = 0;
static uint64_t    s_irq_release       = 0;
static uint64_t    s_irq_detect        = 0;   /* FDC 由来の 0→1 遷移を検出した総数 */
static uint64_t    s_irq_held_n        = 0;   /* 実際に取り下げた数 */
static uint64_t    s_irq_merge_skip    = 0;   /* アクセス前から IRQH_IRQ[1]==1 で触らなかった数 */
static uint64_t    s_irq_overlap       = 0;   /* 保留中に次の FDC 割込みが来た数(素通し) */
static uint64_t    s_irq_reissue       = 0;   /* 保留明けに再発行した数 */
static uint64_t    s_irq_reissue_masked = 0;  /* 保留明け時点で IOC_IntStat bit2=0 のため再発行しなかった数 */
static uint64_t    s_irq_r1_hit        = 0;   /* 保留中の DATA 読出し数(R1 分子) */
static uint64_t    s_data_read_total   = 0;   /* DATA 読出し総数(R1 分母) */

/* P961: MSR(RQM)busy 偽装の状態とカウンタ */
static int         s_msr_scope                 = 0;
static int         s_msr_requested_without_irq = 0;   /* "msr" 指定ありだが irq 無効で無視した */
static uint64_t    s_msr_masked                = 0;   /* busy 中に STATUS 読みを加工した数 */
static uint64_t    s_status_read_total         = 0;   /* STATUS 読み総数(msr_masked の分母) */
static uint64_t    s_data_blocked              = 0;   /* busy 中に FDC_Read を呼ばずブロックした DATA 読み数 */

/* カンマ区切りのトークン列に tok が含まれるか(未知のトークンは無視) */
static int p959_scope_has_token(const char *raw, const char *tok)
{
    if (raw == NULL || tok == NULL) return 0;
    size_t tlen = strlen(tok);
    const char *p = raw;
    while (*p) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == tlen && strncmp(p, tok, tlen) == 0) return 1;
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

static void fdlat_atexit_dump(void)
{
    mx68k_fdlat_dump("atexit");
}

/* DMA ch0 の差し替え Ready コールバック(enabled=1 時のみ登録される) */
static int32_t fdlat_is_ready(void)
{
    int32_t rdy  = FDC_IsDataReady();
    int     open = (s_gt >= s_release) ? 1 : 0;

    s_cb_calls++;
    if (s_arm_pending_eval) {
        /* アームごとの初回評価: アームからの経過 tick を記録 */
        uint64_t lag = s_gt - s_arm_gt;
        s_arm_pending_eval = 0;
        s_armed_dma++;
        if (lag > s_first_eval_lag_max) s_first_eval_lag_max = lag;
    }
    if (!rdy)       s_cb_rdy0++;
    else if (!open) s_cb_closed++;
    else            s_cb_open++;

    return (rdy && open) ? 1 : 0;
}

void mx68k_fdlat_init(int fd_fast_access)
{
    const char *raw = getenv(P940_ENV_NAME);
    long parsed = 0;
    int  valid  = 0;

    if (raw != NULL) {
        char *end = NULL;
        errno = 0;
        parsed = strtol(raw, &end, 10);
        if (errno == 0 && end != raw && *end == '\0' && parsed >= 1) valid = 1;
    }
#if defined(X68K_IOS)
    valid = 0;   /* macOS 限定実験: iOS では常に OFF */
#endif

    s_enabled     = valid;
    s_delay_ms    = valid ? (int)((parsed > P940_MAX_MS) ? P940_MAX_MS : parsed) : 0;
    s_delay_ticks = (uint64_t)s_delay_ms * P940_TICKS_PER_MS;
    s_last_fd_fast = fd_fast_access ? 1 : 0;

    /* 状態の 0 クリア */
    s_gt = 0; s_release = 0;
    s_dmainit_gen = 0; s_inst_gen = 0; s_last_dmainit_site = "(none)";
    s_site_calls = 0; s_pre_eval = 0;
    s_ocr_h[0] = s_ocr_h[1] = s_ocr_h[2] = s_ocr_h[3] = 0;
    s_dir[0] = s_dir[1] = 0;
    s_cb_calls = 0; s_cb_rdy0 = 0; s_cb_closed = 0; s_cb_open = 0;
    s_arm = 0; s_arm_seek = 0; s_arm_rw = 0; s_armed_dma = 0;
    s_arm_pending_eval = 0; s_arm_gt = 0; s_last_arm_f = -1; s_first_eval_lag_max = 0;
    s_sum_prev_f = g_mx68k_frame_num; s_sum_prev_gt = 0;

    /* P959: スコープ判定(DELAY_MS が無効なら irq_scope も強制 0) */
    s_scope_raw = getenv(P959_SCOPE_ENV_NAME);
    s_irq_scope = (s_enabled && p959_scope_has_token(s_scope_raw, "irq")) ? 1 : 0;
    s_irq_held = 0; s_irq_release = 0;
    s_irq_detect = 0; s_irq_held_n = 0; s_irq_merge_skip = 0; s_irq_overlap = 0;
    s_irq_reissue = 0; s_irq_reissue_masked = 0;
    s_irq_r1_hit = 0; s_data_read_total = 0;

    /* P961: "msr" は irq の保留窓に乗る設計のため、irq 無効時は無視して記録する */
    {
        int msr_req = p959_scope_has_token(s_scope_raw, "msr");
        s_msr_scope = (s_irq_scope && msr_req) ? 1 : 0;
        s_msr_requested_without_irq = (msr_req && !s_irq_scope) ? 1 : 0;
    }
    s_msr_masked = 0; s_status_read_total = 0; s_data_blocked = 0;

    /* 配線行(ON/OFF とも無条件に1回): 生値 env_raw/scope_raw と派生値を併記 */
    debug_log("[P940-FDLATENCY] wired env_raw=\"%s\" parsed=%ld enabled=%d delay_ms=%d "
              "delay_ticks=%llu fd_fast=%d platform=%s frame=%d scope_raw=\"%s\" irq_scope=%d "
              "msr_scope=%d msr_requested_without_irq=%d\n",
              raw ? raw : "(null)", parsed, s_enabled, s_delay_ms,
              (unsigned long long)s_delay_ticks, s_last_fd_fast, P940_PLATFORM,
              g_mx68k_frame_num, s_scope_raw ? s_scope_raw : "(null)", s_irq_scope,
              s_msr_scope, s_msr_requested_without_irq);

    if (s_enabled && !s_atexit_done) {
        s_atexit_done = 1;
        atexit(fdlat_atexit_dump);
    }
}

void mx68k_fdlat_note_dma_init(const char *site)
{
    if (!s_enabled) return;
    s_dmainit_gen++;
    s_last_dmainit_site = site ? site : "(null)";
}

void mx68k_fdlat_install(const char *site)
{
    if (!s_enabled) return;   /* OFF 時は DMA_SetReadyCB を呼ばない(絶対条件) */
    DMA_SetReadyCB(0, fdlat_is_ready);
    s_inst_gen++;             /* dmainit_gen には触れない */
    s_release = 0;
    debug_log("[P940-FDLATENCY] install site=%s inst_gen=%u dmainit_gen=%u "
              "dmainit_site=%s frame=%d\n",
              site ? site : "(null)", (unsigned)s_inst_gen, (unsigned)s_dmainit_gen,
              s_last_dmainit_site, g_mx68k_frame_num);
}

void mx68k_fdlat_arm(unsigned usec_req, int fd_fast_access)
{
    if (!s_enabled) return;
    s_release = s_gt + s_delay_ticks;
    s_arm_gt  = s_gt;
    s_arm_pending_eval = 1;
    s_arm++;
    if (usec_req == P940_USEC_RW) s_arm_rw++;   /* Seek は movetrack*200+150 で 300 にならない */
    else                          s_arm_seek++;
    s_last_arm_f   = g_mx68k_frame_num;
    s_last_fd_fast = fd_fast_access ? 1 : 0;

    if (s_arm <= P940_ARM_LOG_MAX) {
        debug_log("[P940-FDLATENCY] arm#%llu f=%d gt=%llu release=%llu usec_req=%u "
                  "ocr=0x%02x csr=0x%02x ccr=0x%02x mtc=0x%04x fd_fast=%d\n",
                  (unsigned long long)s_arm, g_mx68k_frame_num,
                  (unsigned long long)s_gt, (unsigned long long)s_release, usec_req,
                  (unsigned)DMA[0].OCR, (unsigned)DMA[0].CSR, (unsigned)DMA[0].CCR,
                  (unsigned)DMA[0].MTC, s_last_fd_fast);
    }
}

void mx68k_fdlat_tick_line(int32_t fc, int32_t vl, int32_t vt)
{
    if (!s_enabled) return;

    /* 600フレーム毎の集計行(フレーム先頭の走査線で判定) */
    if (vl == 0 && (g_mx68k_frame_num - s_sum_prev_f) >= P940_SUM_PERIOD) {
        mx68k_fdlat_dump("period");
    }

    if (vt > 0 && fc > 0) {
        /* dt = fc*(vl+1)/vt − fc*vl/vt(整数除算)。Σ dt = fc を厳密に満たす */
        int64_t a = ((int64_t)fc * (int64_t)(vl + 1)) / (int64_t)vt;
        int64_t b = ((int64_t)fc * (int64_t)vl) / (int64_t)vt;
        if (a > b) s_gt += (uint64_t)(a - b);
    }

    /* P959: 保留明けの再発行(FDC_SetInt と同じ IOC_IntStat bit2 条件) */
    if (s_irq_held && s_gt >= s_irq_release) {
        s_irq_held = 0;
        if (IOC_IntStat & 4) {
            IRQH_Int(1, (void*)FDC_Int);
            s_irq_reissue++;
            if (s_irq_reissue <= P959_IRQ_LOG_MAX) {
                debug_log("[P940-FDLATENCY] irq_reissue#%llu f=%d gt=%llu release=%llu "
                          "ioc_intstat=0x%02x\n",
                          (unsigned long long)s_irq_reissue, g_mx68k_frame_num,
                          (unsigned long long)s_gt, (unsigned long long)s_irq_release,
                          (unsigned)IOC_IntStat);
            }
        } else {
            s_irq_reissue_masked++;
        }
    }
}

uint8_t mx68k_fdlat_fdc_pre(void)
{
    if (!s_irq_scope) return 0;
    return (uint8_t)((IRQH_IRQ[1] ? 1 : 0) | (s_irq_held ? 2 : 0));
}

void mx68k_fdlat_fdc_post(uint8_t pre, int kind, int is_read)
{
    if (!s_irq_scope) return;

    /* R1 診断: 保留中に DATA が読まれたか(分母 data_read_total も併記) */
    if (kind == P959_KIND_DATA && is_read) {
        s_data_read_total++;
        if (pre & 2) {
            s_irq_r1_hit++;
            if (s_irq_r1_hit <= P959_R1_LOG_MAX) {
                debug_log("[P940-FDLATENCY] irq_r1_hit#%llu f=%d gt=%llu release=%llu kind=%d "
                          "data_read_total=%llu\n",
                          (unsigned long long)s_irq_r1_hit, g_mx68k_frame_num,
                          (unsigned long long)s_gt, (unsigned long long)s_irq_release, kind,
                          (unsigned long long)s_data_read_total);
            }
        }
    }

    /* 新規発生の検出: アクセス前 0 → アクセス後 1、かつハンドラが FDC_Int */
    if (IRQH_IRQ[1] && IRQH_CallBack[1] == (void*)FDC_Int) {
        if (pre & 1) {
            /* アクセス前から保留中(SASI 等の可能性): 触らない(R2)。
             * このアクセスで FDC が新たに割込みを上げたかは区別できないため、
             * 「レベル1保留中の FDC アクセス数」として数える */
            s_irq_merge_skip++;
            return;
        }
        s_irq_detect++;
        if (s_irq_held) {
            /* 保留中に次の FDC 割込み: 新しい方を素通しし、古い保留は破棄(二重発行防止) */
            s_irq_held = 0;
            s_irq_overlap++;
            return;
        }
        IRQH_IRQCallBack(1);
        s_irq_held    = 1;
        s_irq_release = s_gt + s_delay_ticks;
        s_irq_held_n++;
        if (s_irq_held_n <= P959_IRQ_LOG_MAX) {
            debug_log("[P940-FDLATENCY] irq_hold#%llu f=%d gt=%llu release=%llu kind=%d is_read=%d\n",
                      (unsigned long long)s_irq_held_n, g_mx68k_frame_num,
                      (unsigned long long)s_gt, (unsigned long long)s_irq_release,
                      kind, is_read);
        }
    }
}

/* P961: MSR busy 偽装。FDC 完了割込み保留中を「USB 往復中でレジスタに応答できない」
 * とみなすふろっぴU君固有の通信モデルの近似であり、実機 μPD72065 の挙動再現ではない
 * (実機は SEEK/RECALIBRATE 実行中も RQM=1 のまま)。 */
int mx68k_fdlat_busy(void)
{
    if (!s_msr_scope) return 0;
    return s_irq_held ? 1 : 0;
}

uint8_t mx68k_fdlat_filter_status(uint8_t msr)
{
    if (!s_irq_scope) return msr;         /* P959 以前の挙動を変えない */
    s_status_read_total++;
    if (!mx68k_fdlat_busy()) return msr;
    s_msr_masked++;
    return (uint8_t)((msr & ~0x80u) | 0x10u);   /* RQM=0, CB=1(実機模倣ではない) */
}

int mx68k_fdlat_should_block_data(void)
{
    if (!mx68k_fdlat_busy()) return 0;
    s_data_blocked++;
    return 1;
}

void mx68k_fdlat_observe_site(void)
{
    if (!s_enabled) return;
    /* DMA[0] は読取りのみ(書かない)。条件は Core:x68k/dmac.c:236 の前段と同一 */
    s_site_calls++;
    if ((DMA[0].CSR & 0x08) && !(DMA[0].CCR & 0x20) && !(DMA[0].CSR & 0x80) && DMA[0].MTC) {
        s_pre_eval++;
        s_ocr_h[DMA[0].OCR & 3]++;
        s_dir[(DMA[0].OCR >> 7) & 1]++;
    }
}

void mx68k_fdlat_reset(int load_rc)
{
    if (!s_enabled) return;
    /* ゲート状態はステートに含めない: ロード後は開放側へ倒す(rc の成否に関わらず) */
    s_release = 0;
    s_arm_pending_eval = 0;
    s_irq_held = 0;   /* P959: 保留中の割込みは破棄(復元後の Core 状態に合わせる) */
    debug_log("[P940-FDLATENCY] reset reason=load_state rc=%d gt=%llu frame=%d\n",
              load_rc, (unsigned long long)s_gt, g_mx68k_frame_num);
}

void mx68k_fdlat_dump(const char *reason)
{
    if (!s_enabled) return;
    int      f   = g_mx68k_frame_num;
    int      df  = f - s_sum_prev_f;
    uint64_t gpf = (df > 0) ? (s_gt - s_sum_prev_gt) / (uint64_t)df : 0;

    debug_log("[P940-FDLATENCY] sum reason=%s f=%d enabled=1 delay_ticks=%llu "
              "inst_gen=%u dmainit_gen=%u gt=%llu gt_per_frame=%llu df=%d "
              "site_calls=%llu pre_eval=%llu ocr_h0=%llu ocr_h1=%llu ocr_h2=%llu ocr_h3=%llu "
              "dir0=%llu dir1=%llu cb_calls=%llu cb_rdy0=%llu cb_closed=%llu cb_open=%llu "
              "arm=%llu arm_seek=%llu arm_rw=%llu armed_dma=%llu last_arm_f=%d "
              "first_eval_lag_max=%llu fd_fast=%d "
              "irq_scope=%d irq_detect=%llu irq_held=%llu irq_merge_skip=%llu irq_overlap=%llu "
              "irq_reissue=%llu irq_reissue_masked=%llu irq_r1_hit=%llu data_read_total=%llu "
              "irq_pending=%d msr_scope=%d msr_masked=%llu status_read_total=%llu "
              "data_blocked=%llu\n",
              reason ? reason : "(null)", f, (unsigned long long)s_delay_ticks,
              (unsigned)s_inst_gen, (unsigned)s_dmainit_gen,
              (unsigned long long)s_gt, (unsigned long long)gpf, df,
              (unsigned long long)s_site_calls, (unsigned long long)s_pre_eval,
              (unsigned long long)s_ocr_h[0], (unsigned long long)s_ocr_h[1],
              (unsigned long long)s_ocr_h[2], (unsigned long long)s_ocr_h[3],
              (unsigned long long)s_dir[0], (unsigned long long)s_dir[1],
              (unsigned long long)s_cb_calls, (unsigned long long)s_cb_rdy0,
              (unsigned long long)s_cb_closed, (unsigned long long)s_cb_open,
              (unsigned long long)s_arm, (unsigned long long)s_arm_seek,
              (unsigned long long)s_arm_rw, (unsigned long long)s_armed_dma,
              s_last_arm_f, (unsigned long long)s_first_eval_lag_max, s_last_fd_fast,
              s_irq_scope, (unsigned long long)s_irq_detect, (unsigned long long)s_irq_held_n,
              (unsigned long long)s_irq_merge_skip, (unsigned long long)s_irq_overlap,
              (unsigned long long)s_irq_reissue, (unsigned long long)s_irq_reissue_masked,
              (unsigned long long)s_irq_r1_hit, (unsigned long long)s_data_read_total,
              s_irq_held, s_msr_scope, (unsigned long long)s_msr_masked,
              (unsigned long long)s_status_read_total, (unsigned long long)s_data_blocked);

    s_sum_prev_f  = f;
    s_sum_prev_gt = s_gt;
}
