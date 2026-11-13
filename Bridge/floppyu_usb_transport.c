/* ===========================================================================
 *  P945: ふろっぴU君 USB通信基盤(termios層)[P945-FLOPPYUUSB]
 * ---------------------------------------------------------------------------
 *  構成(macOS):
 *   - 専用シリアル dispatch queue "com.mx68k.floppyu.usb"(s_queue)。状態遷移・
 *     IOKit 通知・走査・ハンドシェイク・teardown・shutdown は全てこの queue 上で行う。
 *   - port2 受信スレッド "mx68k.floppyu.rx"。生バイトを SPSC リングへ積むだけで、
 *     公開APIは一切呼ばない(切断検出時は s_queue へ teardown を async 投入するのみ)。
 *   - port1/port2 の送信APIは呼出し元スレッド上で同期実行(fd は mutex で保護)。
 *  全ての読み書きは O_NONBLOCK + poll() + タイムアウトで行い、無期限ブロックしない。
 *
 *  出力行(全て [P945-FLOPPYUUSB] 接頭辞):
 *   wired(無条件1回)/ scan / cand / hs / open / state / disc / sum(shutdown時)/
 *   shutdown_rejected / notify(通知登録失敗時)
 * =========================================================================== */

#include "floppyu_usb_transport.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void debug_log(const char* fmt, ...);   /* Bridge/EmulatorBridge.c で定義 */
extern int  g_mx68k_frame_num;                 /* フレームカウンタ(EmulatorBridge.c で定義) */

#define FUSB_ENV_NAME "MX68K_FLOPPYU_USB"
#define FUSB_TAG      "[P945-FLOPPYUUSB]"

/* フレーム番号はエミュレーションスレッドが書く。ログ用の読取りなので relaxed で読む。 */
static int fusb_frame(void)
{
    return __atomic_load_n(&g_mx68k_frame_num, __ATOMIC_RELAXED);
}

#if defined(X68K_IOS)
/* ===========================================================================
 *  iOS: 常に無効のスタブ(IOKit/termios ヘッダは include しない)
 * =========================================================================== */
void mx68k_floppyu_usb_init(int config_enabled)
{
    const char* raw = getenv(FUSB_ENV_NAME);
    debug_log(FUSB_TAG " wired env_raw=\"%s\" config=%d enabled=0 src=ios queue_live=0 platform=ios frame=%d\n",
              raw ? raw : "(null)", config_enabled ? 1 : 0, fusb_frame());
}
int         mx68k_floppyu_usb_is_enabled(void) { return 0; }
const char* mx68k_floppyu_usb_enable_src(void) { return "ios"; }
void     mx68k_floppyu_usb_shutdown(void) {}
int      mx68k_floppyu_usb_is_connected(void) { return 0; }
uint32_t mx68k_floppyu_usb_generation(void) { return 0; }
int mx68k_floppyu_usb_port1_exec(const uint8_t* tx, size_t txlen, uint8_t* rx, size_t rxlen,
                                 int timeout_ms, int flush_input)
{
    (void)tx; (void)txlen; (void)rx; (void)rxlen; (void)timeout_ms; (void)flush_input;
    return -1;
}
size_t mx68k_floppyu_usb_port2_read(uint8_t* buf, size_t cap) { (void)buf; (void)cap; return 0; }
int    mx68k_floppyu_usb_port2_rx_overflowed(void) { return 0; }
int    mx68k_floppyu_usb_port2_write(const uint8_t* buf, size_t len, int timeout_ms)
{
    (void)buf; (void)len; (void)timeout_ms;
    return -1;
}
/* P957: iOS には USB 存在検出が無い(-2 = 非対応プラットフォーム) */
int32_t mx68k_floppyu_usb_device_present(void) { return -2; }

#else  /* macOS */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/serial/ioss.h>

/* ---- 定数(記号表は .mx68k_cycles/P945_plan.md 参照) ---- */
#define FUSB_VID                      0x04D8   /* たんぼ氏差分 fdc_bridger.h:86 */
#define FUSB_PID                      0xE6B1
#define FUSB_HS_CMD                   0x00     /* fdc_bridger.h:88,90 */
#define FUSB_HS_RESP_PORT1            'f'
#define FUSB_HS_RESP_PORT2            'F'
#define FUSB_HS_TIMEOUT_MS            1000     /* fdc_bridger.h:699 「一秒」 */
#define FUSB_PORT1_DEFAULT_TIMEOUT_MS 1000
#define FUSB_RX_RING_SIZE             65536u   /* 2^16 */
#define FUSB_RX_RING_MASK             (FUSB_RX_RING_SIZE - 1u)
#define FUSB_RX_CHUNK                 4096
#define FUSB_IOSSDATALAT_US           1
#define FUSB_BAUD                     B115200
#define FUSB_MAX_CAND                 8
#define FUSB_PATH_MAX                 256

enum { FUSB_DISABLED = 0, FUSB_WAITING = 1, FUSB_CONNECTED = 2 };

typedef void (*fusb_hook_fn)(void);

/* ---- 有効化・状態 ---- */
static _Atomic int      s_enabled       = 0;
static _Atomic int      s_state         = FUSB_DISABLED;
static _Atomic int      s_shutdown_done = 0;
/* init(呼出し元)/shutdown 後のみ書換え。★非atomicのプレーンポインタであり、
 * fusb_post_teardown() は port1_exec/port2_write 経由で任意スレッドから読む。
 * P945時点ではそれらの呼出し元が無いため到達不能だが、次サイクルで実際に呼ばれる
 * ようになった際は shutdown の dispatch_release/NULL 化との競合を再検討すること
 * (P945_plan.md 残留リスク R-10)。 */
static dispatch_queue_t s_queue         = NULL;
static char             s_queue_key;             /* アドレスのみをキーとして使う */

/* ---- IOKit 通知(s_queue 上のみ) ---- */
static IONotificationPortRef s_notify_port = NULL;
static io_iterator_t         s_it_added    = IO_OBJECT_NULL;
static io_iterator_t         s_it_removed  = IO_OBJECT_NULL;

/* ---- ポート ---- */
static pthread_mutex_t s_port1_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_port2_wlock = PTHREAD_MUTEX_INITIALIZER;
static int             s_fd1 = -1;               /* s_port1_lock 下で読み書き */
static int             s_fd2 = -1;               /* 書込みは s_port2_wlock 下。受信スレッドは
                                                  * pthread_create/join の happens-before 内で読む */
static char            s_path1[FUSB_PATH_MAX];   /* s_queue 上のみ */
static char            s_path2[FUSB_PATH_MAX];
static uint32_t        s_cur_gen = 0;            /* s_queue 上のみ(現接続の世代) */

/* ---- port2 受信スレッド ---- */
static pthread_t              s_rx_handle;          /* s_queue 上のみ(join 用) */
static int                    s_rx_started = 0;     /* s_queue 上のみ */
static _Atomic(pthread_t)     s_rx_thread  = NULL;  /* NULL=受信スレッド無し(§7 順序規約) */
static int                    s_wake_pipe[2] = { -1, -1 };
static _Atomic int            s_rx_stop    = 0;
static _Atomic int            s_link_error = 0;
static _Atomic(fusb_hook_fn)  s_rx_hook    = NULL;  /* selftest 専用(通常は常に NULL) */

/* ---- SPSC リング(生産者=受信スレッド、消費者=呼出し元1スレッド) ---- */
static uint8_t            s_rx_ring[FUSB_RX_RING_SIZE];
static _Atomic(uint64_t)  s_rx_wpos;
static _Atomic(uint64_t)  s_rx_rpos;
static _Atomic(uint64_t)  s_rx_connect_wpos;
static _Atomic(uint32_t)  s_rx_gen;
static _Atomic(uint64_t)  s_rx_overflow;     /* 現接続の破棄バイト数 */
static _Atomic int        s_rx_overflowed;   /* sticky、接続確立時に0へ */
static _Atomic(uint64_t)  s_rx_conn_bytes;   /* 現接続の受信バイト数 */
static uint32_t           s_consumer_gen = 0; /* 消費者専用 */

/* ---- 集計(sum 行) ---- */
static _Atomic(uint64_t) s_cnt_scans, s_cnt_connects, s_cnt_disconnects;
static _Atomic(uint64_t) s_cnt_rx_bytes, s_cnt_rx_overflow;
static _Atomic(uint64_t) s_cnt_p1_tx, s_cnt_p1_rx, s_cnt_p1_timeouts, s_cnt_p1_rtt_max_us;
static _Atomic(uint64_t) s_cnt_p2_tx;

/* ---- 時刻 ---- */
static int64_t fusb_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int fusb_remaining_ms(int64_t deadline_us)
{
    int64_t rem = deadline_us - fusb_now_us();
    if (rem <= 0) return 0;
    return (int)((rem + 999) / 1000);
}

static void fusb_atomic_max(_Atomic(uint64_t)* p, uint64_t v)
{
    uint64_t cur = atomic_load_explicit(p, memory_order_relaxed);
    while (v > cur &&
           !atomic_compare_exchange_weak_explicit(p, &cur, v, memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

/* ---- 低水準I/O(戻り値 0=完了 / -2=タイムアウト / -3=I/Oエラー、*err に errno) ---- */
static int fusb_write_all(int fd, const uint8_t* p, size_t len, int64_t deadline_us,
                          size_t* done, int* err)
{
    size_t off = 0;
    *err = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN) { *err = errno; *done = off; return -3; }
        int rem = fusb_remaining_ms(deadline_us);
        if (rem <= 0) { *done = off; return -2; }
        struct pollfd pf = { fd, POLLOUT, 0 };
        int pr = poll(&pf, 1, rem);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) { *err = errno; *done = off; return -3; }
        if (pr > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            *err = EIO; *done = off; return -3;
        }
    }
    *done = off;
    return 0;
}

static int fusb_read_n(int fd, uint8_t* p, size_t len, int64_t deadline_us,
                       size_t* done, int* err)
{
    size_t off = 0;
    *err = 0;
    while (off < len) {
        int rem = fusb_remaining_ms(deadline_us);
        struct pollfd pf = { fd, POLLIN, 0 };
        int pr = poll(&pf, 1, rem);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) { *err = errno; *done = off; return -3; }
        if (pr == 0) { *done = off; return -2; }
        if (pf.revents & (POLLERR | POLLNVAL)) { *err = EIO; *done = off; return -3; }
        if (pf.revents & POLLIN) {
            ssize_t n = read(fd, p + off, len - off);
            if (n > 0) { off += (size_t)n; continue; }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            /* POLLIN 後の read()==0 はハングアップ扱い(空転防止) */
            *err = (n < 0) ? errno : 0; *done = off; return -3;
        }
        if (pf.revents & POLLHUP) { *err = EIO; *done = off; return -3; }
    }
    *done = off;
    return 0;
}

/* ---- port open / close(§3) ---- */
static int fusb_port_open(const char* path, int* busy)
{
    *busy = 0;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        int e = errno;
        if (e == EBUSY) *busy = 1;
        debug_log(FUSB_TAG " open path=%s fd=-1 errno=%d frame=%d\n", path, e, fusb_frame());
        return -1;
    }
    int excl_rc = ioctl(fd, TIOCEXCL);
    int excl_errno = excl_rc ? errno : 0;
    if (excl_rc != 0) {
        *busy = 1;
        debug_log(FUSB_TAG " open path=%s fd=%d excl_rc=%d excl_errno=%d reason=busy frame=%d\n",
                  path, fd, excl_rc, excl_errno, fusb_frame());
        close(fd);
        return -1;
    }

    struct termios t;
    int attr_rc = tcgetattr(fd, &t);
    int attr_errno = attr_rc ? errno : 0;
    int speed_rc = -1, speed_errno = 0;
    if (attr_rc == 0) {
        cfmakeraw(&t);
        t.c_cflag |= (CLOCAL | CREAD);
        t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
        t.c_cflag |= CS8;
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 0;
        speed_rc = cfsetspeed(&t, FUSB_BAUD);
        speed_errno = speed_rc ? errno : 0;
        attr_rc = tcsetattr(fd, TCSANOW, &t);
        attr_errno = attr_rc ? errno : 0;
    }

    int dtr_rc = ioctl(fd, TIOCSDTR);                       /* 非致命 */
    int dtr_errno = dtr_rc ? errno : 0;
    unsigned long lat = FUSB_IOSSDATALAT_US;
    int datalat_rc = ioctl(fd, IOSSDATALAT, &lat);          /* 非致命(pty では ENOTTY) */
    int datalat_errno = datalat_rc ? errno : 0;
    tcflush(fd, TCIOFLUSH);

    debug_log(FUSB_TAG " open path=%s fd=%d excl_rc=%d attr_rc=%d attr_errno=%d speed_rc=%d "
              "speed_errno=%d dtr_rc=%d dtr_errno=%d datalat_rc=%d datalat_errno=%d frame=%d\n",
              path, fd, excl_rc, attr_rc, attr_errno, speed_rc, speed_errno,
              dtr_rc, dtr_errno, datalat_rc, datalat_errno, fusb_frame());
    return fd;
}

static void fusb_port_close(int fd)
{
    if (fd < 0) return;
    (void)ioctl(fd, TIOCCDTR);
    (void)ioctl(fd, TIOCNXCL);
    close(fd);
}

/* ---- ハンドシェイク: 応答バイト(0..255)/ -1=タイムアウト / -2=エラー ---- */
static int fusb_handshake(int fd, const char* path)
{
    int64_t t0 = fusb_now_us();
    int64_t deadline = t0 + (int64_t)FUSB_HS_TIMEOUT_MS * 1000;
    uint8_t cmd = FUSB_HS_CMD, resp = 0;
    size_t done = 0;
    int err = 0;
    int rc = fusb_write_all(fd, &cmd, 1, deadline, &done, &err);
    if (rc == 0) rc = fusb_read_n(fd, &resp, 1, deadline, &done, &err);
    long elapsed_ms = (long)((fusb_now_us() - t0) / 1000);
    int result = (rc == 0) ? (int)resp : (rc == -2 ? -1 : -2);
    const char* role = (result == FUSB_HS_RESP_PORT1) ? "port1"
                     : (result == FUSB_HS_RESP_PORT2) ? "port2" : "none";
    if (result >= 0) {
        debug_log(FUSB_TAG " hs path=%s resp=0x%02x errno=%d elapsed_ms=%ld role=%s frame=%d\n",
                  path, result, err, elapsed_ms, role, fusb_frame());
    } else {
        debug_log(FUSB_TAG " hs path=%s resp=%s errno=%d elapsed_ms=%ld role=%s frame=%d\n",
                  path, result == -1 ? "timeout" : "err", err, elapsed_ms, role, fusb_frame());
    }
    tcflush(fd, TCIFLUSH);
    return result;
}

static void fusb_log_state(const char* state, const char* reason)
{
    debug_log(FUSB_TAG " state=%s reason=%s gen=%u frame=%d\n", state, reason,
              (unsigned)atomic_load_explicit(&s_rx_gen, memory_order_relaxed), fusb_frame());
}

/* ---- SPSC リング書込み(受信スレッドのみ) ---- */
static void fusb_ring_write(const uint8_t* p, size_t n)
{
    uint64_t w    = atomic_load_explicit(&s_rx_wpos, memory_order_relaxed);
    uint64_t r    = atomic_load_explicit(&s_rx_rpos, memory_order_acquire);
    uint64_t base = atomic_load_explicit(&s_rx_connect_wpos, memory_order_relaxed);
    /* 消費者がまだ世代リセットしていない場合でも、旧接続の未読分は空き容量に数える */
    if (r < base) r = base;
    uint64_t used = w - r;
    uint64_t cap  = FUSB_RX_RING_SIZE - 1u;   /* audio_ring と同じく 1 要素空ける */
    uint64_t room = (used >= cap) ? 0 : cap - used;
    size_t   k    = (n < room) ? n : (size_t)room;
    for (size_t i = 0; i < k; i++) {
        s_rx_ring[(w + i) & FUSB_RX_RING_MASK] = p[i];
    }
    atomic_store_explicit(&s_rx_wpos, w + k, memory_order_release);
    if (k < n) {
        atomic_fetch_add_explicit(&s_rx_overflow, (uint64_t)(n - k), memory_order_relaxed);
        atomic_fetch_add_explicit(&s_cnt_rx_overflow, (uint64_t)(n - k), memory_order_relaxed);
        atomic_store_explicit(&s_rx_overflowed, 1, memory_order_release);
    }
    atomic_fetch_add_explicit(&s_rx_conn_bytes, (uint64_t)n, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_cnt_rx_bytes, (uint64_t)n, memory_order_relaxed);
}

/* ---- teardown(§5、s_queue 上のみ。冪等) ---- */
static void fusb_teardown(const char* reason, uint32_t gen_req, int final_state)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != FUSB_CONNECTED) return;
    if (gen_req != 0 && gen_req != s_cur_gen) return;   /* 旧接続からの遅延要求 */

    /* 先に下ろす = 以後の API 呼出しは即失敗 */
    atomic_store_explicit(&s_state, final_state, memory_order_release);

    if (s_rx_started) {
        atomic_store_explicit(&s_rx_stop, 1, memory_order_release);
        if (s_wake_pipe[1] >= 0) {
            uint8_t b = 1;
            (void)write(s_wake_pipe[1], &b, 1);
        }
        pthread_join(s_rx_handle, NULL);
        s_rx_started = 0;
        atomic_store_explicit(&s_rx_thread, (pthread_t)NULL, memory_order_release);
    }

    pthread_mutex_lock(&s_port1_lock);
    fusb_port_close(s_fd1);
    s_fd1 = -1;
    pthread_mutex_unlock(&s_port1_lock);

    pthread_mutex_lock(&s_port2_wlock);
    fusb_port_close(s_fd2);
    s_fd2 = -1;
    pthread_mutex_unlock(&s_port2_wlock);

    if (s_wake_pipe[0] >= 0) close(s_wake_pipe[0]);
    if (s_wake_pipe[1] >= 0) close(s_wake_pipe[1]);
    s_wake_pipe[0] = s_wake_pipe[1] = -1;

    atomic_fetch_add_explicit(&s_cnt_disconnects, 1, memory_order_relaxed);
    debug_log(FUSB_TAG " disc reason=%s errno=%d gen=%u rx_bytes=%llu rx_overflow=%llu frame=%d\n",
              reason, atomic_load_explicit(&s_link_error, memory_order_relaxed),
              (unsigned)s_cur_gen,
              (unsigned long long)atomic_load_explicit(&s_rx_conn_bytes, memory_order_relaxed),
              (unsigned long long)atomic_load_explicit(&s_rx_overflow, memory_order_relaxed),
              fusb_frame());
}

typedef struct {
    const char* reason;
    uint32_t    gen;
} fusb_td_req;

/* dispatch_async_f 用: ctx は malloc した fusb_td_req(本関数で解放) */
static void fusb_teardown_async(void* ctx)
{
    fusb_td_req* req = (fusb_td_req*)ctx;
    fusb_teardown(req->reason, req->gen, FUSB_WAITING);
    free(req);
}

static void fusb_post_teardown(const char* reason, uint32_t gen)
{
    if (!s_queue) return;   /* shutdown 後(R-10、完全な同期保証ではない) */
    fusb_td_req* req = (fusb_td_req*)malloc(sizeof(*req));
    if (!req) return;
    req->reason = reason;
    req->gen    = gen;
    dispatch_async_f(s_queue, req, fusb_teardown_async);
}

/* ---- 受信スレッド(§4) ---- */
static void* fusb_rx_main(void* arg)
{
    /* §7 書込み1: ループ前に自スレッドで書く */
    atomic_store_explicit(&s_rx_thread, pthread_self(), memory_order_release);
    pthread_setname_np("mx68k.floppyu.rx");

    uint32_t gen  = (uint32_t)(uintptr_t)arg;
    int      fd   = s_fd2;
    int      wake = s_wake_pipe[0];
    uint8_t  buf[FUSB_RX_CHUNK];
    int      failed = 0;

    for (;;) {
        fusb_hook_fn hook = atomic_load_explicit(&s_rx_hook, memory_order_acquire);
        if (hook) hook();
        if (atomic_load_explicit(&s_rx_stop, memory_order_acquire)) break;

        struct pollfd pf[2] = { { fd, POLLIN, 0 }, { wake, POLLIN, 0 } };
        int pr = poll(pf, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            atomic_store_explicit(&s_link_error, errno, memory_order_relaxed);
            failed = 1;
            break;
        }
        if (pf[1].revents & POLLIN) {
            uint8_t d[16];
            (void)read(wake, d, sizeof(d));
            if (atomic_load_explicit(&s_rx_stop, memory_order_acquire)) break;
        }
        if (pf[0].revents & (POLLERR | POLLNVAL)) {
            atomic_store_explicit(&s_link_error, EIO, memory_order_relaxed);
            failed = 1;
            break;
        }
        if (pf[0].revents & POLLIN) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) { fusb_ring_write(buf, (size_t)n); continue; }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            atomic_store_explicit(&s_link_error, (n < 0) ? errno : 0, memory_order_relaxed);
            failed = 1;
            break;
        }
        if (pf[0].revents & POLLHUP) {
            atomic_store_explicit(&s_link_error, EIO, memory_order_relaxed);
            failed = 1;
            break;
        }
    }

    /* 自スレッドからは teardown しない(join できないため)。queue へ投入のみ。 */
    if (failed && !atomic_load_explicit(&s_rx_stop, memory_order_acquire)) {
        fusb_post_teardown("rx_error", gen);
    }
    return NULL;
}

/* ---- 接続確立(§2 手順4、s_queue 上のみ) ---- */
static int fusb_connect_paths(const char* const* paths, const int* ifnums, int n)
{
    int fds[FUSB_MAX_CAND];
    int any_busy = 0;
    if (n > FUSB_MAX_CAND) n = FUSB_MAX_CAND;
    for (int i = 0; i < n; i++) {
        int busy = 0;
        fds[i] = fusb_port_open(paths[i], &busy);
        if (busy) any_busy = 1;
    }

    int p1 = -1, p2 = -1;
    for (int i = 0; i < n && (p1 < 0 || p2 < 0); i++) {
        if (fds[i] < 0) continue;
        int resp = fusb_handshake(fds[i], paths[i]);
        if (resp == FUSB_HS_RESP_PORT1 && p1 < 0) p1 = i;
        else if (resp == FUSB_HS_RESP_PORT2 && p2 < 0) p2 = i;
    }

    if (p1 < 0 || p2 < 0) {
        for (int i = 0; i < n; i++) fusb_port_close(fds[i]);
        atomic_store_explicit(&s_state, FUSB_WAITING, memory_order_release);
        fusb_log_state("waiting", any_busy ? "busy" : "handshake_fail");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (i != p1 && i != p2) fusb_port_close(fds[i]);
    }

    if (pipe(s_wake_pipe) != 0) {
        s_wake_pipe[0] = s_wake_pipe[1] = -1;
        fusb_port_close(fds[p1]);
        fusb_port_close(fds[p2]);
        atomic_store_explicit(&s_state, FUSB_WAITING, memory_order_release);
        fusb_log_state("waiting", "pipe_fail");
        return -1;
    }
    (void)fcntl(s_wake_pipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(s_wake_pipe[1], F_SETFL, O_NONBLOCK);

    pthread_mutex_lock(&s_port1_lock);
    s_fd1 = fds[p1];
    pthread_mutex_unlock(&s_port1_lock);
    pthread_mutex_lock(&s_port2_wlock);
    s_fd2 = fds[p2];
    pthread_mutex_unlock(&s_port2_wlock);
    strlcpy(s_path1, paths[p1], sizeof(s_path1));
    strlcpy(s_path2, paths[p2], sizeof(s_path2));

    atomic_store_explicit(&s_rx_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&s_link_error, 0, memory_order_relaxed);
    atomic_store_explicit(&s_rx_overflow, 0, memory_order_relaxed);
    atomic_store_explicit(&s_rx_overflowed, 0, memory_order_relaxed);
    atomic_store_explicit(&s_rx_conn_bytes, 0, memory_order_relaxed);

    /* §4 順序規約: connect_wpos を書いてから gen を release で公開する */
    atomic_store_explicit(&s_rx_connect_wpos,
                          atomic_load_explicit(&s_rx_wpos, memory_order_acquire),
                          memory_order_relaxed);
    s_cur_gen = atomic_fetch_add_explicit(&s_rx_gen, 1, memory_order_release) + 1u;

    if (pthread_create(&s_rx_handle, NULL, fusb_rx_main, (void*)(uintptr_t)s_cur_gen) != 0) {
        pthread_mutex_lock(&s_port1_lock);
        fusb_port_close(s_fd1); s_fd1 = -1;
        pthread_mutex_unlock(&s_port1_lock);
        pthread_mutex_lock(&s_port2_wlock);
        fusb_port_close(s_fd2); s_fd2 = -1;
        pthread_mutex_unlock(&s_port2_wlock);
        close(s_wake_pipe[0]); close(s_wake_pipe[1]);
        s_wake_pipe[0] = s_wake_pipe[1] = -1;
        atomic_store_explicit(&s_state, FUSB_WAITING, memory_order_release);
        fusb_log_state("waiting", "thread_fail");
        return -1;
    }
    s_rx_started = 1;

    atomic_fetch_add_explicit(&s_cnt_connects, 1, memory_order_relaxed);
    atomic_store_explicit(&s_state, FUSB_CONNECTED, memory_order_release);
    debug_log(FUSB_TAG " state=connected reason=handshake_ok gen=%u port1=%s port1_ifnum=%d "
              "port2=%s port2_ifnum=%d frame=%d\n",
              (unsigned)s_cur_gen, s_path1, ifnums ? ifnums[p1] : -1,
              s_path2, ifnums ? ifnums[p2] : -1, fusb_frame());
    return 0;
}

/* ---- IOKit 走査(§2 手順1〜3、s_queue 上のみ) ---- */
static int fusb_cf_int(CFTypeRef v, int* out)
{
    if (!v) return 0;
    int ok = 0;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int32_t x = 0;
        if (CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &x)) { *out = (int)x; ok = 1; }
    }
    CFRelease(v);
    return ok;
}

static CFTypeRef fusb_search_parent(io_object_t svc, CFStringRef key)
{
    return IORegistryEntrySearchCFProperty(svc, kIOServicePlane, key, kCFAllocatorDefault,
                                           kIORegistryIterateRecursively |
                                           kIORegistryIterateParents);
}

static void fusb_scan(const char* reason)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) == FUSB_CONNECTED) return;
    atomic_fetch_add_explicit(&s_cnt_scans, 1, memory_order_relaxed);

    io_iterator_t it = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    IOServiceMatching(kIOSerialBSDServiceValue),
                                                    &it);
    int serial_total = 0, vid_seen = 0, cand = 0;
    char paths[FUSB_MAX_CAND][FUSB_PATH_MAX];
    int  ifnums[FUSB_MAX_CAND];

    if (kr == KERN_SUCCESS) {
        io_object_t svc;
        while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
            serial_total++;
            int vid = -1, pid = -1;
            int has_vid = fusb_cf_int(fusb_search_parent(svc, CFSTR("idVendor")), &vid);
            int has_pid = fusb_cf_int(fusb_search_parent(svc, CFSTR("idProduct")), &pid);
            if (has_vid) vid_seen++;
            if (has_vid && has_pid && vid == FUSB_VID && pid == FUSB_PID) {
                char path[FUSB_PATH_MAX] = { 0 };
                CFTypeRef cp = IORegistryEntryCreateCFProperty(svc, CFSTR(kIOCalloutDeviceKey),
                                                               kCFAllocatorDefault, 0);
                if (cp) {
                    if (CFGetTypeID(cp) == CFStringGetTypeID()) {
                        CFStringGetCString((CFStringRef)cp, path, sizeof(path),
                                           kCFStringEncodingUTF8);
                    }
                    CFRelease(cp);
                }
                int ifnum = -1;
                (void)fusb_cf_int(fusb_search_parent(svc, CFSTR("bInterfaceNumber")), &ifnum);
                debug_log(FUSB_TAG " cand path=%s vid=0x%04x pid=0x%04x ifnum=%d frame=%d\n",
                          path[0] ? path : "(none)", vid, pid, ifnum, fusb_frame());
                if (path[0] && cand < FUSB_MAX_CAND) {
                    strlcpy(paths[cand], path, FUSB_PATH_MAX);
                    ifnums[cand] = ifnum;
                    cand++;
                }
            }
            IOObjectRelease(svc);
        }
        IOObjectRelease(it);
    }

    debug_log(FUSB_TAG " scan reason=%s kr=%d serial_total=%d vid_seen=%d cand=%d frame=%d\n",
              reason, (int)kr, serial_total, vid_seen, cand, fusb_frame());

    if (cand < 2) {
        atomic_store_explicit(&s_state, FUSB_WAITING, memory_order_release);
        fusb_log_state("waiting", cand == 1 ? "partial" : "not_found");
        return;
    }
    const char* pp[FUSB_MAX_CAND];
    for (int i = 0; i < cand; i++) pp[i] = paths[i];
    (void)fusb_connect_paths(pp, ifnums, cand);
}

/* ---- IOKit 通知(§5、s_queue 上で呼ばれる) ---- */
static void fusb_drain(io_iterator_t it)
{
    io_object_t o;
    while ((o = IOIteratorNext(it)) != IO_OBJECT_NULL) IOObjectRelease(o);
}

static void fusb_cb_added(void* refcon, io_iterator_t it)
{
    (void)refcon;
    fusb_drain(it);   /* 回し切らないと再アームされない */
    if (atomic_load_explicit(&s_state, memory_order_acquire) == FUSB_WAITING) fusb_scan("notify");
}

static void fusb_cb_removed(void* refcon, io_iterator_t it)
{
    (void)refcon;
    fusb_drain(it);
    fusb_teardown("terminated", 0, FUSB_WAITING);
}

static void fusb_start_on_queue(void* ctx)
{
    (void)ctx;
    s_notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!s_notify_port) {
        atomic_store_explicit(&s_state, FUSB_DISABLED, memory_order_release);
        fusb_log_state("disabled", "notify_port_fail");
        return;
    }
    IONotificationPortSetDispatchQueue(s_notify_port, s_queue);

    /* 切断通知: IOUSBHostDevice かつ VID/PID 一致 */
    CFMutableDictionaryRef rm = IOServiceMatching("IOUSBHostDevice");
    if (rm) {
        int32_t vid = FUSB_VID, pid = FUSB_PID;
        CFNumberRef nv = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vid);
        CFNumberRef np = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pid);
        if (nv) { CFDictionarySetValue(rm, CFSTR("idVendor"), nv); CFRelease(nv); }
        if (np) { CFDictionarySetValue(rm, CFSTR("idProduct"), np); CFRelease(np); }
        kern_return_t kr = IOServiceAddMatchingNotification(s_notify_port, kIOTerminatedNotification,
                                                            rm, fusb_cb_removed, NULL,
                                                            &s_it_removed);  /* rm を消費 */
        if (kr == KERN_SUCCESS) fusb_drain(s_it_removed);
        else debug_log(FUSB_TAG " notify kind=terminated kr=0x%x frame=%d\n", (unsigned)kr, fusb_frame());
    }

    /* 接続通知: 全シリアル(登録直後の初回分を回し切る) */
    kern_return_t kr = IOServiceAddMatchingNotification(s_notify_port, kIOFirstMatchNotification,
                                                        IOServiceMatching(kIOSerialBSDServiceValue),
                                                        fusb_cb_added, NULL, &s_it_added);
    if (kr == KERN_SUCCESS) fusb_drain(s_it_added);
    else debug_log(FUSB_TAG " notify kind=first_match kr=0x%x frame=%d\n", (unsigned)kr, fusb_frame());

    fusb_scan("init");
}

/* ---- 公開API ---- */
static void fusb_create_queue(void)
{
    s_queue = dispatch_queue_create("com.mx68k.floppyu.usb", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(s_queue, &s_queue_key, &s_queue_key, NULL);
}

/* P1054: 有効条件の3値解決。環境変数 "1"=強制ON / "0"=強制OFF / 未設定・その他=設定「Use FloppyU」に従う。
 * *src には判定の由来("env1"/"env0"/"config"/"off")を返す */
static int fusb_resolve_enabled(int config_enabled, const char* raw, const char** src)
{
    if (raw != NULL && strcmp(raw, "1") == 0) { *src = "env1"; return 1; }
    if (raw != NULL && strcmp(raw, "0") == 0) { *src = "env0"; return 0; }
    if (config_enabled)                       { *src = "config"; return 1; }
    *src = "off";
    return 0;
}

/* 直近の init で解決した由来(ログ用。エミュレーションスレッドのみが書き、読む) */
static const char* s_enable_src = "off";

void mx68k_floppyu_usb_init(int config_enabled)
{
    const char* raw = getenv(FUSB_ENV_NAME);
    const char* src = "off";
    int enabled = fusb_resolve_enabled(config_enabled, raw, &src);
    /* 既に稼働中なら由来は最初に起動したときのものを保つ(通信層は停止しないため) */
    if (s_queue == NULL) s_enable_src = src;

    /* P1054: init(mx68k_init)とハードリセット(mx68k_reset_hard)の双方から呼ばれる。起動時は2行出るのが正常 */
    debug_log(FUSB_TAG " wired env_raw=\"%s\" config=%d enabled=%d src=%s queue_live=%d platform=macos frame=%d\n",
              raw ? raw : "(null)", config_enabled ? 1 : 0, enabled, src, s_queue != NULL ? 1 : 0, fusb_frame());
    if (!enabled) return;   /* 以降の処理(IOKit・スレッド・queue 生成)は一切行わない(稼働中なら止めもしない) */
    if (s_queue != NULL) return;   /* 再 init: 既に稼働中(冪等) */

    atomic_store_explicit(&s_shutdown_done, 0, memory_order_relaxed);
    atomic_store_explicit(&s_state, FUSB_WAITING, memory_order_release);
    atomic_store_explicit(&s_enabled, 1, memory_order_release);
    fusb_create_queue();
    /* 1秒級のハンドシェイクで呼出しスレッドをブロックしないよう async で投入 */
    dispatch_async_f(s_queue, NULL, fusb_start_on_queue);
}

/* ctx: on_queue フラグ(0/1) */
static void fusb_shutdown_on_queue(void* ctx)
{
    int on_queue = (int)(uintptr_t)ctx;
    fusb_teardown("shutdown", 0, FUSB_DISABLED);
    if (s_it_added != IO_OBJECT_NULL)   { IOObjectRelease(s_it_added);   s_it_added   = IO_OBJECT_NULL; }
    if (s_it_removed != IO_OBJECT_NULL) { IOObjectRelease(s_it_removed); s_it_removed = IO_OBJECT_NULL; }
    if (s_notify_port) { IONotificationPortDestroy(s_notify_port); s_notify_port = NULL; }
    atomic_store_explicit(&s_state, FUSB_DISABLED, memory_order_release);

    debug_log(FUSB_TAG " sum scans=%llu connects=%llu disconnects=%llu rx_bytes=%llu "
              "rx_overflow=%llu p1_tx_bytes=%llu p1_rx_bytes=%llu p1_timeouts=%llu "
              "p1_rtt_max_us=%llu p2_tx_bytes=%llu on_queue=%d frame=%d\n",
              (unsigned long long)atomic_load(&s_cnt_scans),
              (unsigned long long)atomic_load(&s_cnt_connects),
              (unsigned long long)atomic_load(&s_cnt_disconnects),
              (unsigned long long)atomic_load(&s_cnt_rx_bytes),
              (unsigned long long)atomic_load(&s_cnt_rx_overflow),
              (unsigned long long)atomic_load(&s_cnt_p1_tx),
              (unsigned long long)atomic_load(&s_cnt_p1_rx),
              (unsigned long long)atomic_load(&s_cnt_p1_timeouts),
              (unsigned long long)atomic_load(&s_cnt_p1_rtt_max_us),
              (unsigned long long)atomic_load(&s_cnt_p2_tx),
              on_queue, fusb_frame());
}

void mx68k_floppyu_usb_shutdown(void)
{
    if (!atomic_load_explicit(&s_enabled, memory_order_acquire)) return;

    /* 受信スレッド上からの呼出しは拒否(dispatch_sync ↔ pthread_join の相互待ち回避)。
     * 二重呼出しフラグより前に判定し、拒否時はフラグを立てない。 */
    pthread_t rt = atomic_load_explicit(&s_rx_thread, memory_order_acquire);
    if (rt != NULL && pthread_equal(pthread_self(), rt)) {
        debug_log(FUSB_TAG " shutdown_rejected reason=rx_thread gen=%u frame=%d\n",
                  (unsigned)atomic_load_explicit(&s_rx_gen, memory_order_relaxed), fusb_frame());
        return;
    }
    if (atomic_exchange_explicit(&s_shutdown_done, 1, memory_order_acq_rel)) return;
    if (s_queue == NULL) return;

    if (dispatch_get_specific(&s_queue_key) == &s_queue_key) {
        /* 既に s_queue 上: dispatch_sync すると自己デッドロックするので直接呼ぶ。
         * 実行中の queue は解放しない(リーク1個のほうがハングより安全側)。 */
        fusb_shutdown_on_queue((void*)(uintptr_t)1);
        return;
    }
    dispatch_sync_f(s_queue, (void*)(uintptr_t)0, fusb_shutdown_on_queue);
    dispatch_release(s_queue);
    s_queue = NULL;
}

int mx68k_floppyu_usb_is_connected(void)
{
    return atomic_load_explicit(&s_state, memory_order_acquire) == FUSB_CONNECTED ? 1 : 0;
}

int mx68k_floppyu_usb_is_enabled(void)
{
    return atomic_load_explicit(&s_enabled, memory_order_acquire) ? 1 : 0;
}

const char* mx68k_floppyu_usb_enable_src(void)
{
    return atomic_load_explicit(&s_enabled, memory_order_acquire) ? s_enable_src : "off";
}

uint32_t mx68k_floppyu_usb_generation(void)
{
    return atomic_load_explicit(&s_rx_gen, memory_order_acquire);
}

int mx68k_floppyu_usb_port1_exec(const uint8_t* tx, size_t txlen, uint8_t* rx, size_t rxlen,
                                 int timeout_ms, int flush_input)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != FUSB_CONNECTED) return -1;
    if (timeout_ms <= 0) timeout_ms = FUSB_PORT1_DEFAULT_TIMEOUT_MS;

    pthread_mutex_lock(&s_port1_lock);
    if (atomic_load_explicit(&s_state, memory_order_acquire) != FUSB_CONNECTED || s_fd1 < 0) {
        pthread_mutex_unlock(&s_port1_lock);
        return -1;
    }
    int fd = s_fd1;
    if (flush_input) tcflush(fd, TCIFLUSH);

    int64_t t0 = fusb_now_us();
    int64_t deadline = t0 + (int64_t)timeout_ms * 1000;
    size_t  sent = 0, got = 0;
    int     err = 0;
    int     rc = 0;
    if (tx && txlen) rc = fusb_write_all(fd, tx, txlen, deadline, &sent, &err);
    atomic_fetch_add_explicit(&s_cnt_p1_tx, (uint64_t)sent, memory_order_relaxed);
    if (rc == 0 && rx && rxlen) rc = fusb_read_n(fd, rx, rxlen, deadline, &got, &err);
    atomic_fetch_add_explicit(&s_cnt_p1_rx, (uint64_t)got, memory_order_relaxed);

    int ret;
    if (rc == 0) {
        fusb_atomic_max(&s_cnt_p1_rtt_max_us, (uint64_t)(fusb_now_us() - t0));
        ret = (int)got;
    } else if (rc == -2) {
        atomic_fetch_add_explicit(&s_cnt_p1_timeouts, 1, memory_order_relaxed);
        ret = -2;
    } else {
        atomic_store_explicit(&s_link_error, err, memory_order_relaxed);
        /* ロック保持中に投入する(shutdown による s_queue 解放より前であることを保証) */
        fusb_post_teardown("port1_error", atomic_load_explicit(&s_rx_gen, memory_order_relaxed));
        ret = -3;
    }
    pthread_mutex_unlock(&s_port1_lock);
    return ret;
}

size_t mx68k_floppyu_usb_port2_read(uint8_t* buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    if (atomic_load_explicit(&s_state, memory_order_acquire) != FUSB_CONNECTED) return 0;

    /* 世代が変わっていれば旧接続の未読分を捨てる(rpos の書き手は消費者のみ) */
    uint32_t g = atomic_load_explicit(&s_rx_gen, memory_order_acquire);
    if (g != s_consumer_gen) {
        uint64_t base = atomic_load_explicit(&s_rx_connect_wpos, memory_order_acquire);
        atomic_store_explicit(&s_rx_rpos, base, memory_order_release);
        s_consumer_gen = g;
    }

    uint64_t r = atomic_load_explicit(&s_rx_rpos, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&s_rx_wpos, memory_order_acquire);
    uint64_t avail = w - r;
    size_t   n = (avail < cap) ? (size_t)avail : cap;
    for (size_t i = 0; i < n; i++) {
        buf[i] = s_rx_ring[(r + i) & FUSB_RX_RING_MASK];
    }
    atomic_store_explicit(&s_rx_rpos, r + n, memory_order_release);
    return n;
}

int mx68k_floppyu_usb_port2_rx_overflowed(void)
{
    return atomic_load_explicit(&s_rx_overflowed, memory_order_acquire);
}

int mx68k_floppyu_usb_port2_write(const uint8_t* buf, size_t len, int timeout_ms)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != FUSB_CONNECTED) return -1;
    if (timeout_ms <= 0) timeout_ms = FUSB_PORT1_DEFAULT_TIMEOUT_MS;

    pthread_mutex_lock(&s_port2_wlock);
    if (atomic_load_explicit(&s_state, memory_order_acquire) != FUSB_CONNECTED || s_fd2 < 0) {
        pthread_mutex_unlock(&s_port2_wlock);
        return -1;
    }
    size_t sent = 0;
    int    err = 0;
    int    rc = 0;
    if (buf && len) {
        rc = fusb_write_all(s_fd2, buf, len, fusb_now_us() + (int64_t)timeout_ms * 1000,
                            &sent, &err);
    }
    atomic_fetch_add_explicit(&s_cnt_p2_tx, (uint64_t)sent, memory_order_relaxed);
    int ret = (rc == 0) ? (int)sent : rc;
    if (rc == -3) {
        atomic_store_explicit(&s_link_error, err, memory_order_relaxed);
        fusb_post_teardown("port2_error", atomic_load_explicit(&s_rx_gen, memory_order_relaxed));
    }
    pthread_mutex_unlock(&s_port2_wlock);
    return ret;
}

/* ===========================================================================
 *  P957: ポートを開かない軽量な存在検出 [P957-FLOPPYUPRESENT]
 * ---------------------------------------------------------------------------
 *  IOUSBHostDevice を全件列挙し、デバイスノード自身の idVendor/idProduct を照合する。
 *  環境変数・状態機械(s_state/s_queue)・ポートとは独立で、任意スレッドから呼べる。
 * =========================================================================== */

/* vid/pid に一致する IOUSBHostDevice 数。vid<0 または pid<0 なら照合しない(match=0)。
 * 戻り値: 0=成功 / -1=IOKit照会失敗。 */
static int fusb_count_usb(int vid, int pid, int* match_out, int* total_out, int* fail_out)
{
    int match = 0, total = 0, fail = 0;
    io_iterator_t it = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    IOServiceMatching("IOUSBHostDevice"), &it);
    if (kr != KERN_SUCCESS) {
        *match_out = 0; *total_out = 0; *fail_out = 0;
        return -1;
    }
    io_object_t svc;
    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        total++;
        int v = -1, p = -1;
        int has_v = fusb_cf_int(IORegistryEntryCreateCFProperty(svc, CFSTR("idVendor"),
                                                                kCFAllocatorDefault, 0), &v);
        int has_p = fusb_cf_int(IORegistryEntryCreateCFProperty(svc, CFSTR("idProduct"),
                                                                kCFAllocatorDefault, 0), &p);
        if (!has_v || !has_p) fail++;
        else if (vid >= 0 && pid >= 0 && v == vid && p == pid) match++;
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    *match_out = match; *total_out = total; *fail_out = fail;
    return 0;
}

static pthread_mutex_t s_present_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned        s_present_calls = 0;          /* s_present_lock 下 */
static int             s_present_logged = 0;
static int             s_present_last_rc = 0, s_present_last_match = 0, s_present_last_total = 0;

int32_t mx68k_floppyu_usb_device_present(void)
{
    int64_t t0 = fusb_now_us();
    int match = 0, total = 0, fail = 0;
    int rc = fusb_count_usb(FUSB_VID, FUSB_PID, &match, &total, &fail);
    long long elapsed_us = (long long)(fusb_now_us() - t0);

    /* 1Hz ポーリングでログを埋めないよう、初回と (rc,match,usb_total) の変化時のみ出力 */
    pthread_mutex_lock(&s_present_lock);
    unsigned calls = ++s_present_calls;
    if (!s_present_logged || rc != s_present_last_rc || match != s_present_last_match ||
        total != s_present_last_total) {
        s_present_logged = 1;
        s_present_last_rc = rc;
        s_present_last_match = match;
        s_present_last_total = total;
        debug_log("[P957-FLOPPYUPRESENT] rc=%d match=%d usb_total=%d vid_read_fail=%d "
                  "elapsed_us=%lld calls=%u frame=%d\n",
                  rc, match, total, fail, elapsed_us, calls, fusb_frame());
    }
    pthread_mutex_unlock(&s_present_lock);

    return rc < 0 ? rc : match;
}

/* ===========================================================================
 *  単体テスト専用(Scripts/floppyu_usb_selftest.sh からのみビルドされる)
 * =========================================================================== */
#ifdef MX68K_FLOPPYU_USB_SELFTEST
typedef struct {
    const char* const* paths;
    int                n;
    int                rc;
} fusb_st_conn;

static void fusb_st_connect_on_queue(void* ctx)
{
    fusb_st_conn* c = (fusb_st_conn*)ctx;
    if (atomic_load_explicit(&s_state, memory_order_acquire) == FUSB_CONNECTED) { c->rc = -1; return; }
    c->rc = fusb_connect_paths(c->paths, NULL, c->n);
}

int mx68k_floppyu_usb_selftest_connect_n(const char* const* paths, int n)
{
    if (s_queue == NULL) fusb_create_queue();
    atomic_store_explicit(&s_shutdown_done, 0, memory_order_relaxed);
    s_enable_src = "selftest";   /* P1054: 由来ログ用(init を経ない起動) */
    atomic_store_explicit(&s_enabled, 1, memory_order_release);
    if (atomic_load_explicit(&s_state, memory_order_acquire) == FUSB_DISABLED) {
        atomic_store_explicit(&s_state, FUSB_WAITING, memory_order_release);
    }
    fusb_st_conn c = { paths, n, -1 };
    dispatch_sync_f(s_queue, &c, fusb_st_connect_on_queue);
    return c.rc;
}

int mx68k_floppyu_usb_selftest_connect(const char* path_a, const char* path_b)
{
    const char* pp[2] = { path_a, path_b };
    return mx68k_floppyu_usb_selftest_connect_n(pp, 2);
}

static void fusb_st_disconnect_on_queue(void* ctx)
{
    fusb_teardown((const char*)ctx, 0, FUSB_WAITING);
}

void mx68k_floppyu_usb_selftest_disconnect(const char* reason)
{
    if (s_queue == NULL) return;
    dispatch_sync_f(s_queue, (void*)(reason ? reason : "selftest"), fusb_st_disconnect_on_queue);
}

static void fusb_st_shutdown_trampoline(void* ctx)
{
    (void)ctx;
    mx68k_floppyu_usb_shutdown();
}

void mx68k_floppyu_usb_selftest_shutdown_on_queue(void)
{
    if (s_queue == NULL) return;
    dispatch_sync_f(s_queue, NULL, fusb_st_shutdown_trampoline);
}

void mx68k_floppyu_usb_selftest_set_rx_hook(void (*hook)(void))
{
    atomic_store_explicit(&s_rx_hook, hook, memory_order_release);
}

void mx68k_floppyu_usb_selftest_rx_stats(uint64_t* rx_bytes, uint64_t* rx_overflow)
{
    if (rx_bytes)    *rx_bytes    = atomic_load_explicit(&s_rx_conn_bytes, memory_order_acquire);
    if (rx_overflow) *rx_overflow = atomic_load_explicit(&s_rx_overflow, memory_order_acquire);
}

int32_t mx68k_floppyu_usb_selftest_count_vidpid(int vid, int pid, int* usb_total_out)
{
    int match = 0, total = 0, fail = 0;
    int rc = fusb_count_usb(vid, pid, &match, &total, &fail);
    if (usb_total_out) *usb_total_out = total;
    return rc < 0 ? rc : match;
}
#endif /* MX68K_FLOPPYU_USB_SELFTEST */

#endif /* X68K_IOS */
