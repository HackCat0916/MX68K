/* ===========================================================================
 *  P1004: ふろっぴU君 最小垂直スライス(使い捨て診断)[P1004-SLICE]
 * ---------------------------------------------------------------------------
 *  シーケンス(.mx68k_cycles/P1004_plan.md「変更内容」節1):
 *   S0 前提状態の記録・接続待ち / S1 初期化 / S2 IOC設定 / S3 SPECIFY /
 *   S4 モータON+1000ms待ち / S5 SEEK 0+'C'待ち / S6 SENSE INTERRUPT STATUS /
 *   S7 READ DATA / S8 port2 'R'フレーム受信 / S9 モータOFF / S10 サマリ
 *  port1 はすべて mx68k_floppyu_usb_port1_exec(flush_input=1)。port2 の消費者は
 *  このスライスを実行するスレッドただ1つ(通信層の SPSC 規約)。
 *  自己反証可能性のため、各 port1_exec の送信列・戻り値・受信・所要時間を生値で、
 *  port2 の受信はステップ毎に総数+先頭64Bを、派生判定より先に出力する。
 * =========================================================================== */

#include "floppyu_vslice.h"

#include <stdlib.h>
#include <string.h>

extern void debug_log(const char* fmt, ...);   /* Bridge/EmulatorBridge.c で定義 */

#define VS_TAG       "[P1004-SLICE]"
#define VS_ENV_SLICE "MX68K_FLOPPYU_SLICE_TEST"
#define VS_ENV_USB   "MX68K_FLOPPYU_USB"

static int vs_env_is_1(const char* name)
{
    const char* v = getenv(name);
    return v != NULL && strcmp(v, "1") == 0;
}

#if defined(X68K_IOS)
/* ===========================================================================
 *  iOS: スタブ(USB通信層自体が iOS では常に無効)
 * =========================================================================== */
void mx68k_floppyu_vslice_start(int floppyu_enabled)
{
    (void)floppyu_enabled;
    if (!vs_env_is_1(VS_ENV_SLICE)) return;
    debug_log(VS_TAG " skip reason=ios\n");
}
void mx68k_floppyu_vslice_shutdown(void) {}
int  mx68k_floppyu_vslice_run_once(int connect_wait_ms)
{
    (void)connect_wait_ms;
    return MX68K_VSLICE_RC_UNSUPPORTED;
}
int  mx68k_floppyu_vslice_claimed(void) { return 0; }

#else  /* macOS */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "floppyu_usb_transport.h"

/* ---- 固定送信列(記号表。書込み=reg|0x80,val / 読出し=reg) ---- */
static const uint8_t VS_INIT[]      = { 0x80, 0xFE, 0x40 };                    /* XM6t fdc_bridger.h:92-97 */
static const uint8_t VS_IOC[]       = { 0x93, 0x60, 0x91, 0x06 };              /* INT.VECTOR=0x60, INT.MASK=0x06 */
static const uint8_t VS_SPECIFY[]   = { 0x83, 0x03, 0x83, 0xD0, 0x83, 0x10 };  /* SPECIFY 03 D0 10 */
static const uint8_t VS_MOTOR_ON[]  = { 0x87, 0x80 };                          /* ACCESS DRIVE SELECT=0x80 */
static const uint8_t VS_SEEK0[]     = { 0x83, 0x0F, 0x83, 0x00, 0x83, 0x00 };  /* SEEK HU=0 NCN=0 */
static const uint8_t VS_SENSEINT[]  = { 0x83, 0x08, 0x03 };                    /* SENSE INT + ST0読出し */
static const uint8_t VS_RD_STATUS[] = { 0x01 };                                /* STATUS読出し */
static const uint8_t VS_RD_DATA[]   = { 0x03 };                                /* PCN読出し */
static const uint8_t VS_READ[]      = { 0x83, 0x46, 0x83, 0x00, 0x83, 0x00,    /* READ DATA MFM HU=0 C=0 */
                                        0x83, 0x00, 0x83, 0x01, 0x83, 0x03,    /* H=0 R=1 N=3 */
                                        0x83, 0x01, 0x83, 0x35, 0x83, 0xFF };  /* EOT=1 GPL=0x35 DTL=0xFF */
static const uint8_t VS_MOTOR_OFF[] = { 0x87, 0x00 };

static const uint8_t VS_EXPECT_RESULT[7] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x03 };

#define VS_FRAME_MARK          0x52   /* 'R' */
#define VS_SEEK_DONE_MARK      0x43   /* 'C' */
#define VS_EXPECT_FRAME_LEN    1031   /* 1024 データ + 7 リザルト */
#define VS_RESULT_LEN          7
#define VS_INIT_DRAIN_MS       100
#define VS_STEP_DRAIN_MS       20
#define VS_MOTOR_WAIT_MS       1000
#define VS_SEEK_WAIT_MS        3000
#define VS_FRAME_WAIT_MS       3000
#define VS_EXEC_TIMEOUT_MS     1000
#define VS_CONNECT_POLL_MS     50
#define VS_THREAD_CONNECT_MS   15000
#define VS_HEX_SHOW_MAX        64

static _Atomic int s_abort           = 0;
static _Atomic int s_started         = 0;
static _Atomic int s_joinable        = 0;
static _Atomic int s_floppyu_enabled = -1;   /* -1 = start() を経由しない直接呼出し */
static pthread_t   s_thread;

static int64_t vs_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int vs_aborted(void) { return atomic_load(&s_abort) != 0; }

/* bytes を連続hex(区切り無し)で out へ。max バイトを超える分は "..." を付ける */
static void vs_hex(char* out, size_t outsz, const uint8_t* p, size_t n, size_t max)
{
    size_t show = n < max ? n : max;
    size_t o = 0;
    out[0] = '\0';
    for (size_t i = 0; i < show && o + 3 < outsz; i++) {
        o += (size_t)snprintf(out + o, outsz - o, "%02X", p[i]);
    }
    if (show < n && o + 4 < outsz) snprintf(out + o, outsz - o, "...");
}

/* port1 同期実行 + 生値1行(ログタグ指定版)。戻り値は port1_exec そのまま。
 * expect>=0 のとき(受信1Bの比較)、生値の後ろに expect/ok を並べる。 */
static int vs_exec_tag(const char* tag, const char* step, const uint8_t* tx, size_t txlen,
                       uint8_t* rx, size_t rxlen, int expect)
{
    char txh[2 * 32 + 4], rxh[2 * 32 + 4];
    int64_t t0 = vs_now_us();
    int r = mx68k_floppyu_usb_port1_exec(tx, txlen, rx, rxlen, VS_EXEC_TIMEOUT_MS, 1);
    int64_t us = vs_now_us() - t0;
    vs_hex(txh, sizeof(txh), tx, txlen, 32);
    size_t got = (r > 0) ? (size_t)r : 0;   /* -2(タイムアウト)の部分受信数はAPIから得られない */
    vs_hex(rxh, sizeof(rxh), rx, got, 32);
    if (expect >= 0) {
        int ok = (r == 1 && rx[0] == (uint8_t)expect);
        debug_log("%s %s tx=%s ret=%d rx=%s us=%lld expect=%02X ok=%d\n",
                  tag, step, txh, r, rxh, (long long)us, expect, ok);
    } else {
        debug_log("%s %s tx=%s ret=%d rx=%s us=%lld\n",
                  tag, step, txh, r, rxh, (long long)us);
    }
    return r;
}

static int vs_exec(const char* step, const uint8_t* tx, size_t txlen,
                   uint8_t* rx, size_t rxlen, int expect)
{
    return vs_exec_tag(VS_TAG, step, tx, txlen, rx, rxlen, expect);
}

/* ---- P1023: MSR観測プローブ [P1023-MSRCHK](観測専用、既存S1-S9の判定には合流させない) ---- */
#define MSRCHK_TAG       "[P1023-MSRCHK]"
#define MSR_DIO          0x40   /* µPD765 MSR bit6 DIO(1=FDC→ホスト)。一次情報源: Core/px68k/x68k/fdc.c:473 */
#define MSRCHK_DRAIN_MAX 8

/* MSRを1回読み、生値とDIO判定を同じ行に出力する。戻り値: MSR値(0-255)、読出し失敗時は-1 */
static int vs_msrchk_read(const char* step, int iter)
{
    uint8_t rx[1] = { 0 };
    char sname[32];
    snprintf(sname, sizeof(sname), "%s msr#%d", step, iter);
    int r = vs_exec_tag(MSRCHK_TAG, sname, VS_RD_STATUS, sizeof(VS_RD_STATUS), rx, 1, -1);
    if (r != 1) {
        debug_log(MSRCHK_TAG " %s msr#%d read_failed ret=%d\n", step, iter, r);
        return -1;
    }
    debug_log(MSRCHK_TAG " %s msr#%d msr=%02X dio=%d\n", step, iter, rx[0], (rx[0] & MSR_DIO) ? 1 : 0);
    return rx[0];
}

/* S1chk: DIO=1の間、データレジスタを読み捨ててMSRを読み直す(最大MSRCHK_DRAIN_MAX回)。
 * 読出し失敗・上限到達・中断はいずれもログに残すのみで、呼出し元のフローは変えない。 */
static void vs_msrchk_s1(void)
{
    int drained = 0;
    int msr = vs_msrchk_read("S1chk", 0);
    while (msr >= 0 && (msr & MSR_DIO)) {
        if (drained >= MSRCHK_DRAIN_MAX) {
            debug_log(MSRCHK_TAG " S1chk drain_limit_reached drained=%d last_msr=%02X\n", drained, msr);
            break;
        }
        if (vs_aborted()) {
            debug_log(MSRCHK_TAG " S1chk aborted drained=%d last_msr=%02X\n", drained, msr);
            break;
        }
        uint8_t rx[1] = { 0 };
        char sname[32];
        snprintf(sname, sizeof(sname), "S1chk data#%d", drained);
        int r = vs_exec_tag(MSRCHK_TAG, sname, VS_RD_DATA, sizeof(VS_RD_DATA), rx, 1, -1);
        drained++;
        if (r != 1) {
            debug_log(MSRCHK_TAG " S1chk data#%d read_failed ret=%d\n", drained - 1, r);
            break;
        }
        debug_log(MSRCHK_TAG " S1chk data#%d discarded=%02X\n", drained - 1, rx[0]);
        msr = vs_msrchk_read("S1chk", drained);
    }
    /* final_msr=-1 は読出し失敗 */
    debug_log(MSRCHK_TAG " S1chk end drained=%d final_msr=%d\n", drained, msr);
}

/* port2 を ms ミリ秒読み続け、総数と先頭64Bを記録する(分母)。
 * stop_on>=0 なら、そのバイトを受信した時点で終了(戻り値1)。 */
static int vs_drain(const char* step, int ms, int stop_on)
{
    uint8_t keep[VS_HEX_SHOW_MAX];
    size_t kept = 0, total = 0;
    int found = 0;
    int64_t t0 = vs_now_us();
    int64_t dl = t0 + (int64_t)ms * 1000;
    for (;;) {
        uint8_t buf[256];
        size_t n = mx68k_floppyu_usb_port2_read(buf, sizeof(buf));
        for (size_t i = 0; i < n; i++) {
            if (kept < sizeof(keep)) keep[kept++] = buf[i];
            if (stop_on >= 0 && buf[i] == (uint8_t)stop_on) found = 1;
        }
        total += n;
        if (found || vs_aborted() || vs_now_us() >= dl) break;
        if (n == 0) usleep(2000);
    }
    char h[2 * VS_HEX_SHOW_MAX + 8];
    vs_hex(h, sizeof(h), keep, total, VS_HEX_SHOW_MAX);
    if (stop_on >= 0) {
        debug_log(VS_TAG " %s port2 n=%zu bytes=%s wait_for=%02X found=%d us=%lld\n",
                  step, total, h, stop_on, found, (long long)(vs_now_us() - t0));
    } else {
        debug_log(VS_TAG " %s port2 n=%zu bytes=%s\n", step, total, h);
    }
    return found;
}

static void vs_dump_data(const uint8_t* p, size_t n)
{
    for (size_t off = 0; off < n; off += 16) {
        char line[16 * 3 + 4];
        size_t o = 0;
        for (size_t i = off; i < n && i < off + 16; i++) {
            o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%02X", o ? " " : "", p[i]);
        }
        debug_log(VS_TAG " data %03zx: %s\n", off, line);
    }
    uint32_t h = 2166136261u;   /* FNV-1a 32bit */
    int all_same = (n > 0);
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
        if (p[i] != p[0]) all_same = 0;
    }
    debug_log(VS_TAG " data n=%zu fnv1a=0x%08x all_same=%d\n", n, h, all_same);
}

/* S8: 'R' + 長さ2B LE + 本体。'R'以前の他バイトは記録して読み飛ばす(XM6t Run() と同じ)。
 * 戻り値: 1=フレーム完全受信 / 0=タイムアウト or 中断(受信分は全て記録済み)。
 * body_out には本体(malloc)、len_out には長さ(未受信は-1)、got_out には受信数。 */
static int vs_recv_frame(uint8_t** body_out, int* len_out, size_t* got_out)
{
    uint8_t other[VS_HEX_SHOW_MAX];
    size_t other_n = 0, other_total = 0;
    int seen_r = 0, len = -1;
    uint8_t lenb[2];
    int lenb_n = 0;
    uint8_t* body = NULL;
    size_t got = 0, after = 0;
    int complete = 0;
    int64_t t0 = vs_now_us();
    int64_t dl = t0 + (int64_t)VS_FRAME_WAIT_MS * 1000;

    while (!complete && !vs_aborted() && vs_now_us() < dl) {
        uint8_t buf[512];
        size_t n = mx68k_floppyu_usb_port2_read(buf, sizeof(buf));
        if (n == 0) { usleep(2000); continue; }
        for (size_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (complete) { after++; continue; }
            if (!seen_r) {
                if (b == VS_FRAME_MARK) { seen_r = 1; continue; }
                if (other_n < sizeof(other)) other[other_n++] = b;
                other_total++;
            } else if (lenb_n < 2) {
                lenb[lenb_n++] = b;
                if (lenb_n == 2) {
                    len = lenb[0] | (lenb[1] << 8);
                    body = (uint8_t*)malloc(len > 0 ? (size_t)len : 1);
                    if (len == 0) complete = 1;
                }
            } else {
                if (body) body[got] = b;
                got++;
                if ((int)got == len) complete = 1;
            }
        }
    }
    int64_t us = vs_now_us() - t0;
    char h[2 * VS_HEX_SHOW_MAX + 8];
    vs_hex(h, sizeof(h), other, other_total, VS_HEX_SHOW_MAX);
    debug_log(VS_TAG " S8 frame R=%d len=%d got=%zu complete=%d after=%zu other_before=%zu other=%s us=%lld aborted=%d\n",
              seen_r, len, got, complete, after, other_total, h, (long long)us, vs_aborted());
    if (!complete && !vs_aborted()) {
        debug_log(VS_TAG " S8 timeout wait_ms=%d\n", VS_FRAME_WAIT_MS);
    }
    *body_out = body;
    *len_out = len;
    *got_out = got;
    return complete;
}

static const char* vs_build(void)
{
#ifdef DEBUG
    return "debug";
#else
    return "release";
#endif
}

int mx68k_floppyu_vslice_run_once(int connect_wait_ms)
{
    int64_t t_begin = vs_now_us();
    const char* es = getenv(VS_ENV_SLICE);
    const char* eu = getenv(VS_ENV_USB);
    int present = mx68k_floppyu_usb_device_present();
    debug_log(VS_TAG " begin env_slice=%s env_usb=%s present=%d floppyu_enabled=%d build=%s"
              " gen=%u connected=%d overflowed=%d\n",
              es ? es : "(null)", eu ? eu : "(null)", present, atomic_load(&s_floppyu_enabled),
              vs_build(), mx68k_floppyu_usb_generation(), mx68k_floppyu_usb_is_connected(),
              mx68k_floppyu_usb_port2_rx_overflowed());

    /* S0: 接続待ち */
    int waited = 0;
    while (!mx68k_floppyu_usb_is_connected() && waited < connect_wait_ms && !vs_aborted()) {
        usleep(VS_CONNECT_POLL_MS * 1000);
        waited += VS_CONNECT_POLL_MS;
    }
    int connected = mx68k_floppyu_usb_is_connected();
    uint32_t gen0 = mx68k_floppyu_usb_generation();
    debug_log(VS_TAG " connect wait_ms=%d connected=%d gen=%u overflowed=%d\n",
              waited, connected, gen0, mx68k_floppyu_usb_port2_rx_overflowed());

    int rc;
    const char* result;
    int s1_ok = 0;
    uint8_t rx[4];

    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }
    if (!connected)   { rc = MX68K_VSLICE_RC_NOT_CONNECTED; result = "not_connected"; goto done; }

    /* S1 初期化(意味不明の 80 FE 40、XM6t が接続直後に毎回送る) */
    if (vs_exec("S1", VS_INIT, sizeof(VS_INIT), NULL, 0, -1) < 0) { rc = -1; result = "fail_s1"; goto done; }
    s1_ok = 1;
    vs_drain("S1", VS_INIT_DRAIN_MS, -1);
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S1chk(P1023、観測専用): 初期化直後のMSR確認+DIO=1なら残留結果の読み捨て */
    vs_msrchk_s1();
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S2 IOC設定 */
    if (vs_exec("S2", VS_IOC, sizeof(VS_IOC), NULL, 0, -1) < 0) { rc = -2; result = "fail_s2"; goto done; }
    vs_drain("S2", VS_STEP_DRAIN_MS, -1);
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S3 SPECIFY */
    if (vs_exec("S3", VS_SPECIFY, sizeof(VS_SPECIFY), NULL, 0, -1) < 0) { rc = -3; result = "fail_s3"; goto done; }
    vs_drain("S3", VS_STEP_DRAIN_MS, -1);
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S4 モータON + スピンアップ待ち(待っている間の port2 も記録) */
    if (vs_exec("S4", VS_MOTOR_ON, sizeof(VS_MOTOR_ON), NULL, 0, -1) < 0) { rc = -4; result = "fail_s4"; goto done; }
    vs_drain("S4", VS_MOTOR_WAIT_MS, -1);
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S4chk(P1023、観測専用): SEEK送信直前のMSRを1回読むのみ */
    vs_msrchk_read("S4chk", 0);
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S5 SEEK 0 + 'C' 待ち('C' が来なくても続行し、S6 で実状態を読む) */
    if (vs_exec("S5", VS_SEEK0, sizeof(VS_SEEK0), NULL, 0, -1) < 0) { rc = -5; result = "fail_s5"; goto done; }
    vs_drain("S5", VS_SEEK_WAIT_MS, VS_SEEK_DONE_MARK);
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S6 SENSE INTERRUPT STATUS(期待: ST0=0x20 / STATUS=0xD0 / PCN=0x00) */
    if (vs_exec("S6", VS_SENSEINT, sizeof(VS_SENSEINT), rx, 1, 0x20) < 0) { rc = -6; result = "fail_s6"; goto done; }
    if (vs_exec("S6", VS_RD_STATUS, sizeof(VS_RD_STATUS), rx, 1, 0xD0) < 0) { rc = -6; result = "fail_s6"; goto done; }
    if (vs_exec("S6", VS_RD_DATA, sizeof(VS_RD_DATA), rx, 1, 0x00) < 0) { rc = -6; result = "fail_s6"; goto done; }
    vs_drain("S6", VS_STEP_DRAIN_MS, -1);
    if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; goto done; }

    /* S7 READ DATA(18B) */
    if (vs_exec("S7", VS_READ, sizeof(VS_READ), NULL, 0, -1) < 0) { rc = -7; result = "fail_s7"; goto done; }

    /* S8 'R' フレーム受信 */
    {
        uint8_t* body = NULL;
        int len = -1;
        size_t got = 0;
        int complete = vs_recv_frame(&body, &len, &got);
        if (!complete) {
            if (body && got > 0) vs_dump_data(body, got);   /* 受信分を全て記録 */
            free(body);
            if (vs_aborted()) { rc = MX68K_VSLICE_RC_ABORTED; result = "aborted"; }
            else              { rc = -8; result = "timeout_s8"; }
            goto done;
        }
        if (len >= VS_RESULT_LEN) {
            size_t dn = (size_t)len - VS_RESULT_LEN;
            const uint8_t* res = body + dn;
            vs_dump_data(body, dn);
            char eh[2 * VS_RESULT_LEN + 4];
            vs_hex(eh, sizeof(eh), VS_EXPECT_RESULT, VS_RESULT_LEN, VS_RESULT_LEN);
            int match = (memcmp(res, VS_EXPECT_RESULT, VS_RESULT_LEN) == 0);
            debug_log(VS_TAG " result %02X %02X %02X %02X %02X %02X %02X expect=%s match=%d\n",
                      res[0], res[1], res[2], res[3], res[4], res[5], res[6], eh, match);
            int st_ok = ((res[0] & 0xC0) == 0) && res[1] == 0 && res[2] == 0;
            if (st_ok && len == VS_EXPECT_FRAME_LEN) { rc = MX68K_VSLICE_RC_OK; result = "ok"; }
            else { rc = MX68K_VSLICE_RC_RESULT_NG; result = "result_ng"; }
            debug_log(VS_TAG " judge st_ok=%d len=%d expect_len=%d\n", st_ok, len, VS_EXPECT_FRAME_LEN);
        } else {
            vs_dump_data(body, got);
            rc = MX68K_VSLICE_RC_RESULT_NG;
            result = "result_ng";
            debug_log(VS_TAG " judge st_ok=0 len=%d expect_len=%d (too_short)\n", len, VS_EXPECT_FRAME_LEN);
        }
        free(body);
    }

done:
    /* S9 モータOFF(S1 が通っていれば中断・失敗時も必ず試みる) */
    if (s1_ok) {
        vs_exec("S9", VS_MOTOR_OFF, sizeof(VS_MOTOR_OFF), NULL, 0, -1);
    }
    /* S10 サマリ */
    debug_log(VS_TAG " end rc=%d result=%s gen=%u gen_begin=%u connected=%d overflowed=%d total_ms=%lld\n",
              rc, result, mx68k_floppyu_usb_generation(), gen0, mx68k_floppyu_usb_is_connected(),
              mx68k_floppyu_usb_port2_rx_overflowed(), (long long)((vs_now_us() - t_begin) / 1000));
    return rc;
}

static void* vs_thread_main(void* arg)
{
    (void)arg;
    pthread_setname_np("mx68k.floppyu.vslice");
    mx68k_floppyu_vslice_run_once(VS_THREAD_CONNECT_MS);
    return NULL;
}

void mx68k_floppyu_vslice_start(int floppyu_enabled)
{
    if (!vs_env_is_1(VS_ENV_SLICE)) return;   /* OFF 時はログ0行 */
    if (atomic_load(&s_started)) {
        debug_log(VS_TAG " skip reason=already_started\n");
        return;
    }
    if (!vs_env_is_1(VS_ENV_USB)) {
        /* 通信層を勝手に有効化しない(両方の環境変数を要求する) */
        debug_log(VS_TAG " skip reason=usb_env_off\n");
        return;
    }
    if (atomic_exchange(&s_started, 1)) {
        debug_log(VS_TAG " skip reason=already_started\n");
        return;
    }
    mx68k_debug_log_force_on();
    atomic_store(&s_floppyu_enabled, floppyu_enabled ? 1 : 0);
    int e = pthread_create(&s_thread, NULL, vs_thread_main, NULL);
    if (e != 0) {
        debug_log(VS_TAG " skip reason=pthread_create_failed err=%d\n", e);
        return;
    }
    atomic_store(&s_joinable, 1);
}

int mx68k_floppyu_vslice_claimed(void)
{
    /* s_started は一度立つと下ろさない(スライスは起動ごとに1回限り)ので、shutdown 後も1のまま */
    return atomic_load(&s_started) ? 1 : 0;
}

void mx68k_floppyu_vslice_shutdown(void)
{
    if (!atomic_load(&s_joinable)) return;          /* 未起動(OFF 時)はログ0行 */
    atomic_store(&s_abort, 1);
    if (!atomic_exchange(&s_joinable, 0)) return;
    int64_t t0 = vs_now_us();
    pthread_join(s_thread, NULL);
    debug_log(VS_TAG " shutdown joined=1 wait_ms=%lld\n", (long long)((vs_now_us() - t0) / 1000));
}

#endif /* X68K_IOS */
