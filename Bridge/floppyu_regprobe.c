/* ===========================================================================
 *  P939: FDC レジスタアクセス観測プローブ [P939-FDCREG](診断専用)
 * ---------------------------------------------------------------------------
 *  Bridge/floppyu_regprobe_shim.h により、mem_wrap.c の MemReadTable/
 *  MemWriteTable に登録される FDC_Read/FDC_Write がここの観測関数へ
 *  差し替わる。観測関数は必ず元の FDC_Read/FDC_Write を呼び、その戻り値・
 *  副作用をそのまま返す(処理結果は一切変えない)。
 *
 *  動作:
 *   - 初回呼び出し時に1回だけ getenv("MX68K_FLOPPYU_PROBE") を判定して
 *     static 変数へキャッシュし、値に関わらず無条件に1回
 *     "[P939-FDCREG] probe wired env=<0|1>" を出力する(配線確認マーカー。
 *     0件時に「本当に無アクセス」と「配線されていない」を切り分けるため)。
 *   - env=0(既定): 以後ログ無し、元の関数を呼ぶだけ。
 *   - env=1: レジスタ種別(adr&0x07: 0x01=STATUS/0x03=DATA/0x05=DRIVE/
 *     0x07=SELECT/それ以外=OTHER)ごとに総数をカウントし、各種別の先頭16件を
 *     即時出力、末尾16件はリングバッファに保持してダンプ時に出力する。
 *     各サンプルは分類前の生 adr・Read 時の ret / Write 時の data・PC・
 *     今回 frame・前回 frame・差フレーム数(派生値とその算出元の両方)を含む。
 *   - ヒストグラム(総数)は、600 フレームごと(その間にアクセスがあった
 *     場合のみ)と、atexit/明示ダンプ関数で出力する。
 *
 *  ★観測対象はメモリマップ経由のアクセス(CPU 命令および DMAC 転送の両方)。
 *    両者は区別しない(PC はアクセス時点の CPU の PC を記録する参考値)。
 * =========================================================================== */

#include <stdint.h>
#include <stdlib.h>

#define FLOPPYU_REGPROBE_SHIM_NO_MACROS
#include "floppyu_regprobe_shim.h"

#include "fdc.h"              /* 元の FDC_Read/FDC_Write(マクロ無効化済み) */
#include "mx_cpu_iface.h"     /* mx_cpu_get_pc() */
#include "fd_latency_experiment.h"   /* P959: mx68k_fdlat_fdc_pre()/_post() */

extern void debug_log(const char* fmt, ...);   /* Bridge/EmulatorBridge.c で定義 */
extern int  g_mx68k_frame_num;                 /* フレームカウンタ(EmulatorBridge.c で定義) */

#define P939_HEAD_N        16
#define P939_TAIL_N        16
#define P939_KIND_N        5
#define P939_HIST_PERIOD   600   /* ヒストグラム定期出力の間隔(フレーム) */

/* 判定状態: -1 = 未判定 / 0 = OFF / 1 = ON */
static int s_p939_env = -1;

typedef struct {
    uint32_t adr;
    uint32_t val;     /* Read: ret / Write: data */
    uint32_t pc;
    int32_t  frame;
    int32_t  prev_frame;
    uint8_t  is_write;
} p939_sample_t;

typedef struct {
    uint64_t       count_r;
    uint64_t       count_w;
    int32_t        last_frame;      /* 直近アクセスのフレーム(-1 = 未アクセス) */
    p939_sample_t  tail[P939_TAIL_N];
    uint32_t       tail_pos;        /* 次に書く位置 */
} p939_kind_t;

static p939_kind_t s_p939_kind[P939_KIND_N];
static int32_t     s_p939_last_hist_frame = 0;
static uint64_t    s_p939_total_at_last_hist = 0;

static const char *const s_p939_kind_name[P939_KIND_N] = {
    "STATUS", "DATA", "DRIVE", "SELECT", "OTHER"
};

static inline int p939_kind_of(uint32_t adr)
{
    switch (adr & 0x07u) {
    case 0x01u: return 0;
    case 0x03u: return 1;
    case 0x05u: return 2;
    case 0x07u: return 3;
    default:    return 4;
    }
}

static void p939_print_sample(const char *tag, int kind, uint64_t seq, const p939_sample_t *s)
{
    debug_log("[P939-FDCREG] %s reg=%s seq=%llu %s adr=0x%06x %s=0x%02x PC=0x%08x "
              "frame=%d prev_frame=%d dframe=%d\n",
              tag, s_p939_kind_name[kind], (unsigned long long)seq,
              s->is_write ? "W" : "R",
              (unsigned)(s->adr & 0x00FFFFFFu),
              s->is_write ? "data" : "ret",
              (unsigned)(s->val & 0xFFu),
              (unsigned)s->pc,
              (int)s->frame, (int)s->prev_frame,
              (s->prev_frame < 0) ? -1 : (int)(s->frame - s->prev_frame));
}

static void p939_print_hist(const char *why)
{
    uint64_t total = 0;
    for (int k = 0; k < P939_KIND_N; k++) {
        total += s_p939_kind[k].count_r + s_p939_kind[k].count_w;
    }
    debug_log("[P939-FDCREG] HIST why=%s frame=%d total=%llu "
              "STATUS=R%llu/W%llu DATA=R%llu/W%llu DRIVE=R%llu/W%llu "
              "SELECT=R%llu/W%llu OTHER=R%llu/W%llu\n",
              why, (int)g_mx68k_frame_num, (unsigned long long)total,
              (unsigned long long)s_p939_kind[0].count_r, (unsigned long long)s_p939_kind[0].count_w,
              (unsigned long long)s_p939_kind[1].count_r, (unsigned long long)s_p939_kind[1].count_w,
              (unsigned long long)s_p939_kind[2].count_r, (unsigned long long)s_p939_kind[2].count_w,
              (unsigned long long)s_p939_kind[3].count_r, (unsigned long long)s_p939_kind[3].count_w,
              (unsigned long long)s_p939_kind[4].count_r, (unsigned long long)s_p939_kind[4].count_w);
    s_p939_total_at_last_hist = total;
}

static void p939_atexit_dump(void)
{
    mx68k_floppyu_regprobe_dump();
}

/* 初回呼び出し時に1回だけ環境変数を判定し、配線確認マーカーを無条件出力する。 */
static int p939_env_check(void)
{
    if (s_p939_env >= 0) return s_p939_env;
    const char *v = getenv("MX68K_FLOPPYU_PROBE");
    int on = (v != NULL && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0')) ? 1 : 0;
    for (int k = 0; k < P939_KIND_N; k++) s_p939_kind[k].last_frame = -1;
    s_p939_last_hist_frame = g_mx68k_frame_num;
    s_p939_env = on;
    debug_log("[P939-FDCREG] probe wired env=%d frame=%d\n", on, (int)g_mx68k_frame_num);
    if (on) atexit(p939_atexit_dump);
    return on;
}

static void p939_record(uint32_t adr, uint32_t val, int is_write)
{
    int kind = p939_kind_of(adr);
    p939_kind_t *k = &s_p939_kind[kind];
    uint64_t seq = k->count_r + k->count_w;   /* 0 始まりの種別内通し番号 */

    p939_sample_t s;
    s.adr        = adr;
    s.val        = val;
    s.pc         = mx_cpu_get_pc();
    s.frame      = g_mx68k_frame_num;
    s.prev_frame = k->last_frame;
    s.is_write   = (uint8_t)(is_write ? 1 : 0);

    if (is_write) k->count_w++; else k->count_r++;
    k->last_frame = s.frame;

    if (seq < P939_HEAD_N) {
        p939_print_sample("HEAD", kind, seq, &s);
    }
    k->tail[k->tail_pos % P939_TAIL_N] = s;
    k->tail_pos++;

    /* 定期ヒストグラム(その間にアクセスがあった場合のみ) */
    if ((int32_t)(s.frame - s_p939_last_hist_frame) >= P939_HIST_PERIOD) {
        s_p939_last_hist_frame = s.frame;
        uint64_t total = 0;
        for (int i = 0; i < P939_KIND_N; i++) total += s_p939_kind[i].count_r + s_p939_kind[i].count_w;
        if (total != s_p939_total_at_last_hist) p939_print_hist("periodic");
    }
}

/* ===========================================================================
 *  P942: FDC コマンド単位ポーリング列トレース [P942-FDCPOLL](診断専用)
 * ---------------------------------------------------------------------------
 *  環境変数 MX68K_FLOPPYU_POLLTRACE(未設定/"0"=OFF、既定)で有効化する。
 *  MX68K_FLOPPYU_PROBE(P939)とは独立。
 *
 *  DATA レジスタへの書き込みをセッション開始とみなす近似実装(Core 内部の
 *  fdc 構造体は static で不可視のため、コマンドバイトとパラメータバイトを
 *  区別できない)。セッションは次の DATA 書き込み、または開始から
 *  P942_SESSION_TIMEOUT_FRAMES 経過で終了する。タイムアウトはアクセス時に
 *  判定するため、end_frame はセッション内最終アクセスのフレームとする。
 *  セッション中の読み出し(STATUS/DATA/DRIVE/SELECT)を poll 行として記録し、
 *  詳細行は P942_POLL_MAX 件までで打ち切る(total_polls は打ち切り後も数える)。
 *  CPU/DMA の区別は不可のため src=unknown 固定。
 *  cmdseq < P942_DETAIL_MAX は詳細段階(seq_start/poll/seq_end)、
 *  P942_DETAIL_MAX <= cmdseq < P942_SUMMARY_MAX は要約段階
 *  (seq_start_summary/seq_end_summary のみ、poll 行なし)、それ以降は出力なし。
 *  呼び出し文脈は FDC_Read/FDC_Write 経由のエミュレーションスレッドのみを前提とする。
 * =========================================================================== */

#define P942_POLL_MAX                256
#define P942_DETAIL_MAX              20
#define P942_SUMMARY_MAX             4000
#define P942_SESSION_TIMEOUT_FRAMES  120

static int s_p942_env = -1;   /* -1 = 未判定 / 0 = OFF / 1 = ON */

typedef struct {
    int       active;
    uint32_t  cmdseq;
    uint32_t  trigger_adr;
    uint8_t   trigger_data;
    int32_t   start_frame;
    int32_t   last_frame;
    uint64_t  total_polls;
    uint64_t  status_polls;
    uint64_t  other_polls;
} p942_session_t;

static p942_session_t s_p942_sess;
static uint32_t       s_p942_total_sessions = 0;
static int            s_p942_limit_notified = 0;
/* 直前の STATUS 読出し値。bit4(CB)=1 ならコマンド実行中とみなし DATA 書込みを継続扱いにする */
static uint8_t        s_p942_last_status_val = 0x00;
static uint32_t       s_p942_cmd_hist[256];

static void p942_end_session(int32_t end_frame)
{
    p942_session_t *s = &s_p942_sess;
    if (!s->active) return;
    s->active = 0;
    if (s->cmdseq >= P942_SUMMARY_MAX) return;
    debug_log("[P942-FDCPOLL] %s cmdseq=%u trigger_adr=0x%06x trigger_data=0x%02x "
              "start_frame=%d end_frame=%d total_polls=%llu status_polls=%llu other_polls=%llu\n",
              (s->cmdseq < P942_DETAIL_MAX) ? "seq_end" : "seq_end_summary",
              (unsigned)s->cmdseq, (unsigned)(s->trigger_adr & 0x00FFFFFFu),
              (unsigned)s->trigger_data, (int)s->start_frame, (int)end_frame,
              (unsigned long long)s->total_polls, (unsigned long long)s->status_polls,
              (unsigned long long)s->other_polls);
}

static void p942_dump_cmd_hist(void)
{
    for (int code = 0; code < 256; code++) {
        if (s_p942_cmd_hist[code] == 0) continue;
        debug_log("[P942-FDCPOLL] cmd_hist code=0x%02x count=%u\n",
                  (unsigned)code, (unsigned)s_p942_cmd_hist[code]);
    }
}

static void p942_atexit_dump(void)
{
    p942_end_session(s_p942_sess.last_frame);
    p942_dump_cmd_hist();
}

static int p942_env_check(void)
{
    if (s_p942_env >= 0) return s_p942_env;
    const char *v = getenv("MX68K_FLOPPYU_POLLTRACE");
    int on = (v != NULL && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0')) ? 1 : 0;
    s_p942_env = on;
    debug_log("[P942-FDCPOLL] wired env=%d frame=%d\n", on, (int)g_mx68k_frame_num);
    if (on) atexit(p942_atexit_dump);
    return on;
}

static void p942_check_timeout(int32_t frame)
{
    if (s_p942_sess.active &&
        (int32_t)(frame - s_p942_sess.start_frame) >= P942_SESSION_TIMEOUT_FRAMES) {
        p942_end_session(s_p942_sess.last_frame);
    }
}

static void p942_on_write(uint32_t adr, uint8_t data)
{
    int32_t frame = g_mx68k_frame_num;
    p942_check_timeout(frame);
    if (p939_kind_of(adr) != 1) return;

    int cb_busy = (s_p942_last_status_val & 0x10) != 0;
    if (cb_busy && s_p942_sess.active) {
        return;   /* パラメータ/データバイトとみなし、既存セッションを継続 */
    }

    p942_end_session(frame);
    s_p942_cmd_hist[data]++;

    p942_session_t *s = &s_p942_sess;
    s->active       = 1;
    s->cmdseq       = s_p942_total_sessions++;
    s->trigger_adr  = adr;
    s->trigger_data = data;
    s->start_frame  = frame;
    s->last_frame   = frame;
    s->total_polls  = 0;
    s->status_polls = 0;
    s->other_polls  = 0;

    if (s->cmdseq < P942_DETAIL_MAX) {
        debug_log("[P942-FDCPOLL] seq_start cmdseq=%u trigger_adr=0x%06x trigger_data=0x%02x "
                  "prev_status=0x%02x start_frame=%d pc=0x%08x\n",
                  (unsigned)s->cmdseq, (unsigned)(adr & 0x00FFFFFFu), (unsigned)data,
                  (unsigned)s_p942_last_status_val, (int)frame, (unsigned)mx_cpu_get_pc());
    } else if (s->cmdseq < P942_SUMMARY_MAX) {
        debug_log("[P942-FDCPOLL] seq_start_summary cmdseq=%u trigger_adr=0x%06x trigger_data=0x%02x "
                  "prev_status=0x%02x start_frame=%d\n",
                  (unsigned)s->cmdseq, (unsigned)(adr & 0x00FFFFFFu), (unsigned)data,
                  (unsigned)s_p942_last_status_val, (int)frame);
    } else if (!s_p942_limit_notified) {
        s_p942_limit_notified = 1;
        debug_log("[P942-FDCPOLL] session_limit_reached total_sessions=%u\n",
                  (unsigned)s_p942_total_sessions);
    }
}

static void p942_on_read(uint32_t adr, uint8_t ret)
{
    int32_t frame = g_mx68k_frame_num;
    p942_check_timeout(frame);

    int kind = p939_kind_of(adr);
    if (kind == 0) {
        s_p942_last_status_val = ret;   /* セッション有無に関わらず常に更新 */
    }

    p942_session_t *s = &s_p942_sess;
    if (!s->active) return;
    if (kind > 3) return;   /* OTHER(偶数アドレス)は対象外 */

    uint64_t poll_seq = s->total_polls++;
    if (kind == 0) s->status_polls++; else s->other_polls++;
    s->last_frame = frame;

    if (s->cmdseq < P942_DETAIL_MAX && poll_seq < P942_POLL_MAX) {
        debug_log("[P942-FDCPOLL] poll cmdseq=%u poll_seq=%llu reg=%s val=0x%02x pc=0x%08x "
                  "frame=%d src=unknown\n",
                  (unsigned)s->cmdseq, (unsigned long long)poll_seq, s_p939_kind_name[kind],
                  (unsigned)ret, (unsigned)mx_cpu_get_pc(), (int)frame);
    }
}

uint8_t FASTCALL mx68k_floppyu_fdc_read_probe(uint32_t adr)
{
    uint8_t pre  = mx68k_fdlat_fdc_pre();   /* P959: irq_scope 無効時は 0 を返すだけ */
    int     kind = p939_kind_of(adr);
    uint8_t ret;
    if (kind == 1 && mx68k_fdlat_should_block_data()) {
        ret = 0xff;                         /* P961: FDC_Read を呼ばない(rdptr/rdnum 副作用回避) */
    } else {
        ret = FDC_Read(adr);
        if (kind == 0) ret = mx68k_fdlat_filter_status(ret);
    }
    mx68k_fdlat_fdc_post(pre, kind, 1);
    /* P939/P942 プローブには加工後の値(ゲストが実際に見た値)を渡す */
    if (p939_env_check()) {
        p939_record(adr, ret, 0);
    }
    if (p942_env_check()) {
        p942_on_read(adr, ret);
    }
    return ret;
}

void FASTCALL mx68k_floppyu_fdc_write_probe(uint32_t adr, uint8_t data)
{
    uint8_t pre = mx68k_fdlat_fdc_pre();   /* P959: irq_scope 無効時は 0 を返すだけ */
    if (p939_env_check()) {
        p939_record(adr, data, 1);
    }
    if (p942_env_check()) {
        p942_on_write(adr, data);
    }
    FDC_Write(adr, data);
    mx68k_fdlat_fdc_post(pre, p939_kind_of(adr), 0);
}

void mx68k_floppyu_regprobe_dump(void)
{
    if (s_p939_env != 1) return;
    p939_print_hist("dump");
    for (int kind = 0; kind < P939_KIND_N; kind++) {
        p939_kind_t *k = &s_p939_kind[kind];
        uint64_t total = k->count_r + k->count_w;
        uint32_t n = (k->tail_pos < P939_TAIL_N) ? k->tail_pos : P939_TAIL_N;
        uint32_t start = k->tail_pos - n;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t pos = start + i;
            uint64_t seq = total - n + i;
            p939_print_sample("TAIL", kind, seq, &k->tail[pos % P939_TAIL_N]);
        }
    }
}
