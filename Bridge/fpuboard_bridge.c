/* P901: X68000 世代 FPU ボード CZ-6BP1(MC68881)の CIR デバイス。P900 の検証プローブ
 * (fpuboard_probe.c、削除済み)を本実装へ格上げしたもの。
 *
 * 一次情報源(定数の根拠は .mx68k_cycles/P901_plan.md の記号表):
 *   CIR 配置              Inside X68000 書籍p.112 図8、Outside X68000 書籍p.87-88 図4
 *   応答プリミティブ        Inside X68000 書籍p.116-120 図9-15
 *   命令クラス別の応答遷移  Inside X68000 書籍p.120-127 図16-21
 *   セーブ/リストア        Inside X68000 書籍p.127-128 §6-7・図22
 *
 * 応答は2段のシフトキュー(resp_lo/resp_hi)で返す。読出しで resp_lo を返し、resp_hi≠0 なら繰り上げる。
 * 最終段は次の状態遷移まで同じ値を返し続ける(P900 の実機 hands-on で SI.R の表示を確認した方式)。
 *
 * 初版で扱わないもの(Docs/01 3-1 行の既知の制限): FMOVEM FPn(110/111)、例外プリミティブ
 * ($5C0B 等。FLOAT3.X は $8900 以外を転送プリミティブとみなすため生成しない)、FPCR の例外許可、
 * スーパーバイザ限定アクセス、演算時間、ボード2/CZ-6BP2。 */
#include <stdlib.h>
#include <string.h>

#include "fpuboard_bridge.h"
#include "EmulatorBridge.h"                      /* debug_log */
#include "../ThirdParty/Musashi/m68kfpu_cir.h"

/* CIR オフセット(Inside 図8) */
#define CIR_RESPONSE    0x00u
#define CIR_CONTROL     0x02u
#define CIR_SAVE        0x04u
#define CIR_RESTORE     0x06u
#define CIR_OPWORD      0x08u
#define CIR_COMMAND     0x0Au
#define CIR_RESERVED    0x0Cu
#define CIR_CONDITION   0x0Eu
#define CIR_OPERAND_HI  0x10u
#define CIR_OPERAND_LO  0x12u
#define CIR_REGSEL      0x14u
#define CIR_INSTADDR_HI 0x18u
#define CIR_INSTADDR_LO 0x1Au
#define CIR_OPADDR_HI   0x1Cu
#define CIR_OPADDR_LO   0x1Eu

/* 応答 CIR の値(Inside 図10-12) */
#define RESP_IDLE       0x0802u   /* ヌル、アイドル(PF=1) */
#define RESP_BUSY       0x0900u   /* ヌル、内部処理中(IA=1) */
#define RESP_CA         0x8900u   /* ヌル、再読出し要求(come-again) */
#define RESP_COND_T     0x0800u   /* コンディション成立(Inside 書籍p.127 本文) */
#define RESP_COND_F     0x0801u   /* コンディション不成立 */
#define PRIM_DYNK_BASE  0x8C00u   /* 動的 K ファクタ用 Dn 転送要求(+n) */

/* セーブ CIR(Inside 書籍p.127 §6-7) */
#define SAVE_NULL       0x0018u

#define XBUF_MAX        45u       /* BUSY フレーム 180 バイト */
#define LOG_EVENT_CAP   64u
#define LOG_SUMMARY_CAP 64u
#define LOG_TRACE_CAP   20000u

enum {
    ST_IDLE = 0, ST_IN_XFER, ST_OUT_XFER, ST_CTL_IN, ST_CTL_OUT, ST_DYNK_WAIT, ST_RESTORE_XFER, ST_SAVE_XFER
};
static const char *const k_stage_name[] = {
    "IDLE", "IN_XFER", "OUT_XFER", "CTL_IN", "CTL_OUT", "DYNK_WAIT", "RESTORE_XFER", "SAVE_XFER"
};

/* ボード1枚分の状態。将来のボード2用に base を持つ(初版は1個だけ) */
typedef struct {
    uint32_t base;
    uint16_t resp_lo, resp_hi;
    uint32_t stage;
    uint16_t cmd;
    uint32_t xbuf[XBUF_MAX];
    uint32_t xidx, xn;          /* 転送位置 / 要素数 */
    uint32_t esize;             /* 要素サイズ 1/2/4 バイト */
    uint16_t latch_hi, latch_lo;
    uint16_t have_latch;        /* 1 = ロングの上位ワードを処理済み(下位ワード待ち) */
    uint16_t restore_fmt;       /* リストア CIR の読み戻し値(直近に書いたフォーマットワード) */
    struct mx68k_fpu_ctx ctx;
} fpuboard_t;

typedef struct {
    unsigned cmds, gen, in, out, ctl, fmovecr, cond, save, restore, abort_n;
    unsigned unimpl, proto, packed_in, packed_out, acc_r, acc_w, buserr_exempt;
    uint16_t last_cmd;
} fpuboard_stats_t;

static fpuboard_t       s_board;
static fpuboard_stats_t s_st;            /* プロセス累積(診断値、ステートセーブしない) */
static int              s_wired;
static int              s_env_read, s_env_forced, s_env_trace;
static unsigned         s_log_event_n, s_log_summary_n, s_log_trace_n;

int fpuboard_env_forced(void)
{
    if (!s_env_read) {
        const char *e = getenv("MX68K_FPUBOARD");
        const char *t = getenv("MX68K_FPUBOARD_TRACE");
        s_env_read = 1;
        s_env_forced = (e != NULL && strcmp(e, "1") == 0);
        s_env_trace  = (t != NULL && strcmp(t, "1") == 0);
    }
    return s_env_forced;
}

/* ---------------------------------------------------------------- ログ */

static void log_summary(const char *why)
{
    if (s_log_summary_n >= LOG_SUMMARY_CAP) return;
    s_log_summary_n++;
    debug_log("[P901-FPUBOARD] summary why=%s cmds=%u gen=%u in=%u out=%u ctl=%u fmovecr=%u cond=%u "
              "save=%u restore=%u abort=%u unimpl=%u proto=%u packed_in=%u packed_out=%u "
              "acc_r=%u acc_w=%u buserr_exempt=%u last_cmd=0x%04X\n",
              why, s_st.cmds, s_st.gen, s_st.in, s_st.out, s_st.ctl, s_st.fmovecr, s_st.cond,
              s_st.save, s_st.restore, s_st.abort_n, s_st.unimpl, s_st.proto, s_st.packed_in,
              s_st.packed_out, s_st.acc_r, s_st.acc_w, s_st.buserr_exempt, (unsigned)s_st.last_cmd);
}

/* 未実装コマンド・プロトコル違反・不正フォーマットの個別イベント(生の値をそのまま出す) */
static void log_event(const char *kind, unsigned off, uint32_t val)
{
    if (s_log_event_n >= LOG_EVENT_CAP) return;
    s_log_event_n++;
    debug_log("[P901-FPUBOARD] event=%s off=0x%02X val=0x%04X cmd=0x%04X stage=%s resp_lo=0x%04X "
              "resp_hi=0x%04X xfer_idx=%u xfer_n=%u esize=%u n=%u\n",
              kind, off, (unsigned)(val & 0xFFFFu), (unsigned)s_board.cmd, k_stage_name[s_board.stage],
              (unsigned)s_board.resp_lo, (unsigned)s_board.resp_hi, s_board.xidx, s_board.xn,
              s_board.esize, s_log_event_n);
}

static void log_trace(unsigned off, char dir, int size, uint32_t val)
{
    if (!s_env_trace) return;
    if (s_log_trace_n >= LOG_TRACE_CAP) {
        if (s_log_trace_n == LOG_TRACE_CAP) {
            s_log_trace_n++;
            debug_log("[P901-FPUBOARD-TRACE] further lines suppressed (>%u)\n", LOG_TRACE_CAP);
        }
        return;
    }
    s_log_trace_n++;
    debug_log("[P901-FPUBOARD-TRACE] off=0x%02X dir=%c size=%d val=0x%04X stage=%s resp_lo=0x%04X "
              "resp_hi=0x%04X xfer_idx=%u xfer_n=%u\n",
              off, dir, size, (unsigned)(val & 0xFFFFu), k_stage_name[s_board.stage],
              (unsigned)s_board.resp_lo, (unsigned)s_board.resp_hi, s_board.xidx, s_board.xn);
}

/* ---------------------------------------------------------------- 状態遷移 */

static void resp_set(uint16_t lo, uint16_t hi)
{
    s_board.resp_lo = lo;
    s_board.resp_hi = hi;
}

static void xfer_begin(uint32_t stage, uint32_t n, uint32_t esize)
{
    s_board.stage = stage;
    s_board.xidx = 0;
    s_board.xn = n;
    s_board.esize = esize;
    s_board.have_latch = 0;
}

static void go_idle(void)
{
    s_board.stage = ST_IDLE;
    s_board.xidx = s_board.xn = 0;
    s_board.esize = 4;
    s_board.have_latch = 0;
}

/* 転送途中に別の命令が来たときなど。現在の命令を破棄する */
static void discard_if_busy(const char *why, unsigned off, uint32_t val)
{
    if (s_board.stage == ST_IDLE) return;
    s_st.proto++;
    log_event(why, off, val);
    go_idle();
}

static void board_reset_state(void)
{
    s_board.base = FPUBOARD_CIR_BASE;
    resp_set(RESP_IDLE, 0);
    s_board.cmd = 0;
    memset(s_board.xbuf, 0, sizeof s_board.xbuf);
    go_idle();
    s_board.latch_hi = s_board.latch_lo = 0;
    s_board.restore_fmt = 0;
    mx68k_fpucir_reset(&s_board.ctx);
}

void fpuboard_init(int wired, int enabled, int cpu_model, const char *src)
{
    (void)fpuboard_env_forced();
    s_wired = wired ? 1 : 0;
    board_reset_state();
    /* 「CIR のログが0件」を「未配線」と区別するため、配線状態の分母を毎回1行出す */
    debug_log("[P901-FPUBOARD] reset enabled=%d wired=%d cpu_model=%d env=%d trace=%d src=%s\n",
              enabled ? 1 : 0, s_wired, cpu_model, s_env_forced, s_env_trace, src ? src : "?");
    log_summary(src ? src : "reset");
}

int fpuboard_claims_addr(uint32_t addr)
{
    uint32_t a = addr & 0x00FFFFFFu;
    if (!s_wired) return 0;
    return (a >= FPUBOARD_CIR_BASE && a <= FPUBOARD_CIR_LAST) ? 1 : 0;
}

int fpuboard_buserr_exempt(uint32_t addr)
{
    if (!fpuboard_claims_addr(addr)) return 0;
    s_st.buserr_exempt++;
    return 1;
}

/* 書式番号(コマンドの bit12-10)→ 転送の要素数・要素サイズ・転送プリミティブ。
 * 0=L 1=S 2=X 3=P(静的K) 4=W 5=D 6=B(7 は呼出し側で別扱い) */
static void fmt_xfer(int fmt, int dir_out, uint32_t *n, uint32_t *esize, uint16_t *prim)
{
    static const uint8_t len[7] = { 4, 4, 12, 12, 2, 8, 1 };
    uint32_t bytes = len[fmt];
    *esize = (bytes >= 4) ? 4u : bytes;
    *n = (bytes >= 4) ? bytes / 4u : 1u;
    /* Inside 図11: 入力 = %1001 01ea len、出力 = %1011 0xea len。D/X/P は ea 欄が 110 / 010 */
    if (bytes <= 4)
        *prim = (uint16_t)((dir_out ? 0xB100u : 0x9500u) | bytes);
    else
        *prim = (uint16_t)((dir_out ? 0xB200u : 0x9600u) | bytes);
}

static void do_command(uint16_t cmd)
{
    int opclass = (cmd >> 13) & 7;
    int fmt = (cmd >> 10) & 7;
    uint32_t n, esize;
    uint16_t prim;

    discard_if_busy("cmd_during_xfer", CIR_COMMAND, cmd);
    s_board.cmd = cmd;
    s_st.cmds++;
    s_st.last_cmd = cmd;

    switch (opclass) {
    case 0:   /* レジスタ間(Inside 図16: $0900→$0802) */
        s_st.gen++;
        if (mx68k_fpucir_gen(&s_board.ctx, cmd, NULL) < 0) {
            s_st.unimpl++;
            log_event("unimpl", CIR_COMMAND, cmd);
            resp_set(RESP_IDLE, 0);
        } else {
            resp_set(RESP_BUSY, RESP_IDLE);
        }
        break;
    case 2:   /* 外部→FPn(rm=1)。fmt=7 は FMOVECR */
        if (fmt == 7) {
            s_st.fmovecr++;
            if (mx68k_fpucir_gen(&s_board.ctx, cmd, NULL) < 0) {
                s_st.unimpl++;
                log_event("unimpl", CIR_COMMAND, cmd);
                resp_set(RESP_IDLE, 0);
            } else {
                resp_set(RESP_BUSY, RESP_IDLE);
            }
            break;
        }
        s_st.in++;
        if (!mx68k_fpucir_cmd_valid(cmd)) {
            /* 実機は命令前例外 $5C0B(Inside 図14)だが、初版は例外プリミティブを生成しない */
            s_st.unimpl++;
            log_event("unimpl", CIR_COMMAND, cmd);
            resp_set(RESP_IDLE, 0);
            break;
        }
        fmt_xfer(fmt, 0, &n, &esize, &prim);
        xfer_begin(ST_IN_XFER, n, esize);
        resp_set(prim, RESP_CA);   /* Inside 図17: 転送プリミティブ→$8900 */
        break;
    case 3:   /* FPn→外部 */
        s_st.out++;
        if (fmt == 7) {   /* 動的 K: 先に Dn を受け取る(Inside 図18下段) */
            xfer_begin(ST_DYNK_WAIT, 1, 4);
            resp_set((uint16_t)(PRIM_DYNK_BASE | ((cmd >> 4) & 7)), 0);
            break;
        }
        (void)mx68k_fpucir_out(&s_board.ctx, cmd, 0, s_board.xbuf);
        if (fmt == 3) s_st.packed_out++;
        fmt_xfer(fmt, 1, &n, &esize, &prim);
        xfer_begin(ST_OUT_XFER, n, esize);
        resp_set(RESP_CA, prim);   /* Inside 図18: $8900→転送プリミティブ(P900 で実証) */
        break;
    case 4:   /* 外部→制御レジスタ(Inside 図19: プリミティブ→$8900→$0802) */
    case 5: { /* 制御レジスタ→外部(Inside 図20) */
        int regsel = (cmd >> 10) & 7;
        int cnt = ((regsel >> 2) & 1) + ((regsel >> 1) & 1) + (regsel & 1);
        static const uint16_t prim_in[4]  = { 0, 0x9704u, 0x9608u, 0x960Cu };
        static const uint16_t prim_out[4] = { 0, 0xB304u, 0xB208u, 0xB20Cu };
        s_st.ctl++;
        if (cnt == 0) {
            s_st.unimpl++;
            log_event("unimpl", CIR_COMMAND, cmd);
            resp_set(RESP_IDLE, 0);
            break;
        }
        if (opclass == 4) {
            xfer_begin(ST_CTL_IN, (uint32_t)cnt, 4);
            resp_set(prim_in[cnt], RESP_CA);
        } else {
            (void)mx68k_fpucir_ctl_read(&s_board.ctx, regsel, s_board.xbuf);
            xfer_begin(ST_CTL_OUT, (uint32_t)cnt, 4);
            resp_set(prim_out[cnt], RESP_CA);
        }
        break;
    }
    default:  /* 001=未定義、110/111=FMOVEM FPn(初版未実装) */
        s_st.unimpl++;
        log_event("unimpl", CIR_COMMAND, cmd);
        resp_set(RESP_IDLE, 0);
        break;
    }

    /* 要約行: 総コマンド数が2の累乗に達したとき */
    if ((s_st.cmds & (s_st.cmds - 1u)) == 0u) log_summary("pow2");
}

/* 転送要素が全部そろった/全部読まれたときの後処理 */
static void xfer_complete(void)
{
    switch (s_board.stage) {
    case ST_IN_XFER:
        if (mx68k_fpucir_gen(&s_board.ctx, s_board.cmd, s_board.xbuf) < 0) {
            s_st.unimpl++;
            log_event("unimpl", CIR_COMMAND, s_board.cmd);
            resp_set(RESP_IDLE, 0);
        } else {
            resp_set(RESP_BUSY, RESP_IDLE);   /* Inside 図17: $0900→$0802 */
        }
        if (((s_board.cmd >> 10) & 7) == 3) s_st.packed_in++;
        go_idle();
        break;
    case ST_CTL_IN:
        mx68k_fpucir_ctl_write(&s_board.ctx, (s_board.cmd >> 10) & 7, s_board.xbuf);
        resp_set(RESP_IDLE, 0);
        go_idle();
        break;
    case ST_DYNK_WAIT: {
        int k = (int)s_board.xbuf[0];
        (void)mx68k_fpucir_out(&s_board.ctx, s_board.cmd, k, s_board.xbuf);
        s_st.packed_out++;
        xfer_begin(ST_OUT_XFER, 3, 4);
        resp_set(RESP_CA, 0xB20Cu);
        break;
    }
    case ST_RESTORE_XFER:
        s_board.ctx.just_reset = 0;   /* IDLE/BUSY フレームの内容は捨てる(030 経路の FRESTORE と同じ扱い) */
        go_idle();
        break;
    case ST_OUT_XFER:
    case ST_CTL_OUT:
        resp_set(RESP_IDLE, 0);
        go_idle();
        break;
    case ST_SAVE_XFER:
    default:
        go_idle();
        break;
    }
}

static int stage_is_input(void)
{
    return s_board.stage == ST_IN_XFER || s_board.stage == ST_CTL_IN ||
           s_board.stage == ST_DYNK_WAIT || s_board.stage == ST_RESTORE_XFER;
}

static int stage_is_output(void)
{
    return s_board.stage == ST_OUT_XFER || s_board.stage == ST_CTL_OUT || s_board.stage == ST_SAVE_XFER;
}

static void operand_put(uint32_t v)
{
    s_board.xbuf[s_board.xidx++] = v;
    if (s_board.xidx >= s_board.xn) xfer_complete();
}

/* セーブ CIR の読出し。返すフォーマットワードは NULL=$0018(Inside 書籍p.127)、
 * IDLE=フレーム先頭ロングの上位16bit(版数<<8 | $18) */
static uint16_t do_save(void)
{
    uint32_t words[7];
    int n;
    if (s_board.stage != ST_SAVE_XFER) discard_if_busy("save_during_xfer", CIR_SAVE, 0);
    s_st.save++;
    n = mx68k_fpucir_save_frame(&s_board.ctx, words);
    if (n <= 1) {
        go_idle();   /* NULL: 転送するデータは無い */
        return SAVE_NULL;
    }
    memcpy(s_board.xbuf, &words[1], (size_t)(n - 1) * sizeof(uint32_t));
    xfer_begin(ST_SAVE_XFER, (uint32_t)(n - 1), 4);
    return (uint16_t)(words[0] >> 16);
}

static void do_restore(uint16_t w)
{
    discard_if_busy("restore_during_xfer", CIR_RESTORE, w);
    s_st.restore++;
    s_board.restore_fmt = w;
    if ((w & 0xFF00u) == 0) {
        /* NULL(版数0): FPU リセット(FP0-7=NaN、FPCR/FPSR/FPIAR=0)。FLOAT3.X のボード検出とエラー経路で多用 */
        mx68k_fpucir_reset(&s_board.ctx);
        resp_set(RESP_IDLE, 0);
        go_idle();
    } else if ((w & 0xFFu) == 0x18u) {
        xfer_begin(ST_RESTORE_XFER, 6, 4);   /* IDLE: 24 バイト */
    } else if ((w & 0xFFu) == 0xB4u) {
        xfer_begin(ST_RESTORE_XFER, 45, 4);  /* BUSY: 180 バイト */
    } else {
        s_st.proto++;
        log_event("bad_restore_format", CIR_RESTORE, w);
    }
}

/* 副作用無しでワード値を覗く(バイト読出し用) */
static uint16_t peek_word(unsigned off)
{
    switch (off) {
    case CIR_RESPONSE: return s_board.resp_lo;
    case CIR_SAVE: {   /* バイト読出しではセーブを開始しない(フォーマットワードだけ見せる) */
        uint32_t words[7];
        return (mx68k_fpucir_save_frame(&s_board.ctx, words) <= 1) ? (uint16_t)SAVE_NULL
                                                                   : (uint16_t)(words[0] >> 16);
    }
    case CIR_RESTORE:  return s_board.restore_fmt;
    case CIR_REGSEL:   return 0;
    case CIR_OPERAND_HI:
    case CIR_OPERAND_LO: return 0;
    default:           return 0xFFFFu;
    }
}

uint32_t fpuboard_read(uint32_t addr, int size)
{
    unsigned off = (unsigned)((addr & 0x00FFFFFFu) - s_board.base);
    uint32_t v = 0;

    s_st.acc_r++;
    if (size != 2) {
        /* B 書式のオペランドは $10 へのバイト1回で1要素(R12)。それ以外のバイト読出しは状態を進めない */
        if (off == CIR_OPERAND_HI && stage_is_output() && s_board.esize == 1 && s_board.xidx < s_board.xn) {
            v = s_board.xbuf[s_board.xidx] & 0xFFu;
            s_board.xidx++;
            if (s_board.xidx >= s_board.xn) xfer_complete();
        } else {
            uint16_t w = peek_word(off & ~1u);
            v = (off & 1u) ? (w & 0xFFu) : (uint32_t)(w >> 8);
        }
        log_trace(off, 'R', 1, v);
        return v;
    }

    switch (off) {
    case CIR_RESPONSE:
        v = s_board.resp_lo;
        if (s_board.resp_hi != 0) {
            s_board.resp_lo = s_board.resp_hi;
            s_board.resp_hi = 0;
        }
        break;
    case CIR_SAVE:
        v = do_save();
        break;
    case CIR_RESTORE:
        v = s_board.restore_fmt;
        break;
    case CIR_REGSEL:
        v = 0;   /* FMOVEM 未実装 */
        break;
    case CIR_OPERAND_HI:
        if (stage_is_output() && s_board.xidx < s_board.xn) {
            uint32_t e = s_board.xbuf[s_board.xidx];
            if (s_board.esize == 4) {
                v = e >> 16;
                s_board.latch_lo = (uint16_t)e;
                s_board.have_latch = 1;
            } else {
                v = (s_board.esize == 2) ? (e & 0xFFFFu) : ((e & 0xFFu) << 8);
                s_board.xidx++;
                if (s_board.xidx >= s_board.xn) xfer_complete();
            }
        } else {
            s_st.proto++;
            log_event("operand_read_idle", off, 0);
        }
        break;
    case CIR_OPERAND_LO:
        if (stage_is_output() && s_board.esize == 4 && s_board.have_latch) {
            v = s_board.latch_lo;
            s_board.have_latch = 0;
            s_board.xidx++;
            if (s_board.xidx >= s_board.xn) xfer_complete();
        } else {
            s_st.proto++;
            log_event("operand_read_lo_unpaired", off, 0);
        }
        break;
    default:   /* 書込み専用($02/$0A/$0E)・68881 不使用($08/$0C/$18-$1E) */
        v = 0xFFFFu;
        break;
    }
    log_trace(off, 'R', 2, v);
    return v;
}

void fpuboard_write(uint32_t addr, uint32_t val, int size)
{
    unsigned off = (unsigned)((addr & 0x00FFFFFFu) - s_board.base);
    uint16_t w = (uint16_t)(val & 0xFFFFu);

    s_st.acc_w++;
    if (size != 2) {
        if (off == CIR_OPERAND_HI && stage_is_input() && s_board.esize == 1 && s_board.xidx < s_board.xn)
            operand_put(val & 0xFFu);
        /* それ以外のバイト書込みは無視する */
        log_trace(off, 'W', 1, val & 0xFFu);
        return;
    }

    switch (off) {
    case CIR_CONTROL:   /* アボート: 実行中断・アイドルへ(FPU レジスタは保持) */
        s_st.abort_n++;
        go_idle();
        resp_set(RESP_IDLE, 0);
        break;
    case CIR_RESTORE:
        do_restore(w);
        break;
    case CIR_COMMAND:
        do_command(w);
        break;
    case CIR_CONDITION: {
        int r;
        discard_if_busy("cond_during_xfer", off, w);
        s_st.cond++;
        r = mx68k_fpucir_condition(&s_board.ctx, w & 0x3F);
        if (r < 0) {
            s_st.proto++;
            log_event("bad_condition", off, w);
            r = 0;
        }
        resp_set(r ? RESP_COND_T : RESP_COND_F, 0);
        break;
    }
    case CIR_OPERAND_HI:
        if (stage_is_input() && s_board.xidx < s_board.xn) {
            if (s_board.esize == 4) {
                s_board.latch_hi = w;
                s_board.have_latch = 1;
            } else {
                operand_put((s_board.esize == 2) ? w : (uint32_t)(w >> 8));
            }
        } else {
            s_st.proto++;
            log_event("operand_write_idle", off, w);
        }
        break;
    case CIR_OPERAND_LO:
        if (stage_is_input() && s_board.esize == 4 && s_board.have_latch) {
            s_board.have_latch = 0;
            operand_put(((uint32_t)s_board.latch_hi << 16) | w);
        } else {
            s_st.proto++;
            log_event("operand_write_lo_unpaired", off, w);
        }
        break;
    default:   /* $04(読出し専用)・$08/$0C/$14/$18-$1E: 書込み無視(Inside §4-5/§4-10/§4-11) */
        break;
    }
    log_trace(off, 'W', 2, w);
}

/* ---------------------------------------------------------------- ステートセーブ */

#define FB_FIELD(v)      do { if (buf) { if (save) memcpy(buf + off, &(v), sizeof(v)); \
                                         else       memcpy(&(v), buf + off, sizeof(v)); } \
                              off += (uint32_t)sizeof(v); } while (0)
#define FB_FIELDN(p, n)  do { if (buf) { if (save) memcpy(buf + off, (p), (n)); \
                                         else       memcpy((p), buf + off, (n)); } \
                              off += (uint32_t)(n); } while (0)

uint32_t fpuboard_state_block(uint8_t *buf, int save)
{
    uint32_t off = 0;
    FB_FIELDN(s_board.ctx.fp_hi, sizeof s_board.ctx.fp_hi);
    FB_FIELDN(s_board.ctx.fp_lo, sizeof s_board.ctx.fp_lo);
    FB_FIELD(s_board.ctx.fpcr);
    FB_FIELD(s_board.ctx.fpsr);
    FB_FIELD(s_board.ctx.fpiar);
    FB_FIELD(s_board.ctx.just_reset);
    FB_FIELD(s_board.resp_lo);
    FB_FIELD(s_board.resp_hi);
    FB_FIELD(s_board.stage);
    FB_FIELD(s_board.cmd);
    FB_FIELDN(s_board.xbuf, sizeof s_board.xbuf);
    FB_FIELD(s_board.xidx);
    FB_FIELD(s_board.xn);
    FB_FIELD(s_board.esize);
    FB_FIELD(s_board.latch_hi);
    FB_FIELD(s_board.latch_lo);
    FB_FIELD(s_board.have_latch);
    FB_FIELD(s_board.restore_fmt);
    if (buf && !save) {
        /* 壊れた値で配列外を指さないよう正規化する(ブロック長は検証済みだが中身は検証していない) */
        if (s_board.stage > ST_SAVE_XFER) s_board.stage = ST_IDLE;
        if (s_board.xn > XBUF_MAX) s_board.xn = XBUF_MAX;
        if (s_board.xidx > s_board.xn) s_board.xidx = s_board.xn;
        if (s_board.esize != 1 && s_board.esize != 2 && s_board.esize != 4) s_board.esize = 4;
    }
    return off;
}
