/* ===========================================================================
 *  P502 (D-9): SASI ホストファイル I/O のディスクリプタキャッシュ層(実装)
 * ---------------------------------------------------------------------------
 *  設計は Bridge/sasi_io_cache.h の冒頭コメント参照。要点のみ再掲:
 *
 *   - path をキーとする固定 MX68K_SASI_UNIT_COUNT(=8, Bridge/EmulatorBridge.h:95)
 *     エントリの fd テーブル。上限 8 を超えて fd を確保しない(満杯時は最古
 *     エントリを実際に close() してから差し替える LRU eviction)。
 *   - sasi.c は File_Seek と File_Read/File_Write を別呼び出しとして発行する
 *     (sasi.c:100→106 / :128→134)ため、Seek で指定されたオフセットは次の
 *     Read/Write まで保持する必要がある。sasi_io_seek() はエントリ内
 *     current_offset を書き換えるだけで OS の lseek() は呼ばず、
 *     sasi_io_read()/sasi_io_write() がその値を pread()/pwrite() の offset
 *     引数へ渡して転送後に進める。これにより
 *       (a) OS のファイル位置ポインタに依存しない(複数ユニットが同一プロセス
 *           内で互いの位置を壊さない)
 *       (b) lseek システムコール自体が発行されない
 *     の 2 点を両立する。
 *   - sasi_io_close() は no-op。実 close() は LRU eviction と
 *     sasi_io_cache_invalidate_all() でのみ発生する。
 *
 *  書込みの外部可視性: pwrite はページキャッシュへ即時反映されるため、
 *  close() を待たない点を除き従来(close 時点で確定)と実質同等。どちらも
 *  fsync は行っておらず、この挙動は本サイクルで変更していない。
 *
 *  スレッド: 本ファイルの関数はすべてエミュレーションスレッド上でのみ
 *  呼ばれる(sasi.c の I/O 経路、および EmulatorBridge.c のフレーム境界
 *  消費ブロック / 起動・ステートロードの単一スレッド区間)。よってロックは
 *  持たない。
 * =========================================================================== */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#define SASI_IO_CACHE_NO_MACROS   /* 本ファイル自身は生の open/pread/pwrite/close を使う */
#include "sasi_io_cache.h"

#include "EmulatorBridge.h"   /* MX68K_SASI_UNIT_COUNT / debug_log() */
#include "prop.h"             /* Config.HDImage[] — [P502-SASIIO] の unit 導出用 */

/* Config.HDImage[] の 1 要素と同じ長さを持たせる。MAX_PATH(=MAXPATHLEN=1024)
 * では Core が受理するパスを切り詰めてしまい、キャッシュが毎回ミスして
 * 「効かない」だけの静かな性能劣化になる(データ破損はしない)。 */
#define SASI_IO_PATH_MAX  4096
_Static_assert(sizeof(Config.HDImage[0]) == SASI_IO_PATH_MAX,
               "SASI_IO_PATH_MAX must match sizeof(Config.HDImage[0])");

typedef struct {
    char     path[SASI_IO_PATH_MAX];
    int      fd;              /* -1 = 未使用スロット */
    off_t    current_offset;  /* File_Seek が設定し Read/Write が消費・前進させる */
    uint64_t lru;             /* 最終使用時刻(単調増加カウンタ、0 = 未使用) */
} sasi_io_entry;

static sasi_io_entry s_tab[MX68K_SASI_UNIT_COUNT];
static int           s_tab_ready = 0;
static uint64_t      s_lru_clock = 0;

extern int g_mx68k_frame_num;

/* ---------------------------------------------------------------------------
 *  [P502-SASIIO] プローブ
 *
 *  自己反証可能性(Fix Plan「自己反証可能性」節): 「open が 0 件」という出力は
 *  「そもそも SASI I/O が起きていない」とも読めてしまうため、必ず分母である
 *  セクタ I/O 総数(sectors = rd + wr)を同じ行に併記し、opens / hits をその
 *  内訳として生値のまま出す(比率は出さない)。
 *
 *    キャッシュ無効(=従来実装)相当 … opens == sectors
 *    キャッシュ有効               … opens ≈ 使用ユニット数まで低下、hits が伸びる
 *
 *  opens が下がらなければ「毎セクタ open が支配的原因」という仮説は反証される。
 *
 *  1 行 = 1 バースト。バースト = 同一エントリに対する、オフセットが連続する
 *  一連のセクタ転送(SASI の 1 コマンドが読み書きする連続ブロック列に相当。
 *  ライト時の Flush(sector N) → Seek(sector N+1) の交互呼び出しもオフセットは
 *  連続するので 1 バーストにまとまる)。連続性が切れた時・パスが変わった時・
 *  一定フレーム以上あいた時・無効化時にフラッシュする。
 * ------------------------------------------------------------------------- */
static const sasi_io_entry* s_burst_ent   = NULL;  /* NULL = バースト非進行中。同一性比較にのみ使う */
static off_t    s_burst_next_off = 0;
static int      s_burst_unit     = -1;
static int      s_burst_frame    = 0;
static int      s_burst_rd       = 0;
static int      s_burst_wr       = 0;
static int      s_burst_opens    = 0;
static int      s_burst_hits     = 0;
static long long s_burst_ns      = 0;

/* 直前の sasi_io_open() が実 open だったか(1)/ヒットだったか(0)を、対応する
 * read/write がバーストを確定させるまで一時保持する。 */
static int       s_pend_open = 0;
static int       s_pend_hit  = 0;
static long long s_pend_ns   = 0;

static long long p502_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

/* path から MX 論理ユニット番号を導出する。sasi.c が使うキーそのもの
 * (Config.HDImage[SASI_Device*2+SASI_Unit])と同じ表を引くので取り違えない。
 * 見つからなければ -1。 */
static int p502_unit_of_path(const char* path)
{
    for (int i = 0; i < 16; i++) {
        if (Config.HDImage[i][0] && strcmp(Config.HDImage[i], path) == 0)
            return i / 2;   /* 論理 unit n -> Config.HDImage[n*2](LUN0 スロット) */
    }
    return -1;
}

static void p502_burst_flush(void)
{
    if (!s_burst_ent) return;
    int sectors = s_burst_rd + s_burst_wr;
    char op = (s_burst_wr == 0) ? 'R' : ((s_burst_rd == 0) ? 'W' : 'M');
    debug_log("[P502-SASIIO] unit=%d op=%c sectors=%d rd=%d wr=%d opens=%d hits=%d us=%lld frame=%d\n",
              s_burst_unit, op, sectors, s_burst_rd, s_burst_wr,
              s_burst_opens, s_burst_hits, s_burst_ns / 1000, s_burst_frame);
    s_burst_ent   = NULL;
    s_burst_rd    = 0;
    s_burst_wr    = 0;
    s_burst_opens = 0;
    s_burst_hits  = 0;
    s_burst_ns    = 0;
}

/* read/write の入口で呼ぶ。連続性が切れていれば前バーストをフラッシュし、
 * 新しいバーストを開始する。そのうえで直前の open の生値を取り込む。 */
static void p502_burst_note(const sasi_io_entry* e, off_t start)
{
    if (s_burst_ent != e || start != s_burst_next_off) {
        p502_burst_flush();
        s_burst_ent   = e;
        s_burst_unit  = p502_unit_of_path(e->path);
        s_burst_frame = g_mx68k_frame_num;
    }
    s_burst_opens += s_pend_open;
    s_burst_hits  += s_pend_hit;
    s_burst_ns    += s_pend_ns;
    s_pend_open = 0;
    s_pend_hit  = 0;
    s_pend_ns   = 0;
}

/* ------------------------------------------------------------------------- */

static void p502_tab_init(void)
{
    if (s_tab_ready) return;
    for (int i = 0; i < MX68K_SASI_UNIT_COUNT; i++) {
        s_tab[i].fd = -1;
        s_tab[i].path[0] = '\0';
        s_tab[i].current_offset = 0;
        s_tab[i].lru = 0;
    }
    s_tab_ready = 1;
}

static void p502_entry_close(sasi_io_entry* e)
{
    if (e->fd >= 0) {
        close(e->fd);
        e->fd = -1;
    }
    e->path[0] = '\0';
    e->current_offset = 0;
    e->lru = 0;
}

FILEH sasi_io_open(char* filename)
{
    p502_tab_init();

    if (!filename || filename[0] == '\0')
        return (FILEH)NULL;

    /* バーストがフレームをまたいで放置されるのを防ぐ(アイドル明けの最初の
     * アクセスで前回分を吐き出す)。 */
    if (s_burst_ent && (g_mx68k_frame_num - s_burst_frame) > 2)
        p502_burst_flush();

    /* --- ヒット判定 --------------------------------------------------- */
    for (int i = 0; i < MX68K_SASI_UNIT_COUNT; i++) {
        if (s_tab[i].fd >= 0 && strcmp(s_tab[i].path, filename) == 0) {
            s_tab[i].lru = ++s_lru_clock;
            s_pend_open = 0;
            s_pend_hit  = 1;
            s_pend_ns   = 0;
            return (FILEH)&s_tab[i];
        }
    }

    if (strlen(filename) >= SASI_IO_PATH_MAX)
        return (FILEH)NULL;

    /* --- 空きスロット、無ければ LRU eviction ---------------------------- */
    sasi_io_entry* victim = NULL;
    for (int i = 0; i < MX68K_SASI_UNIT_COUNT; i++) {
        if (s_tab[i].fd < 0) { victim = &s_tab[i]; break; }
    }
    if (!victim) {
        victim = &s_tab[0];
        for (int i = 1; i < MX68K_SASI_UNIT_COUNT; i++) {
            if (s_tab[i].lru < victim->lru) victim = &s_tab[i];
        }
        /* 追い出す実体を参照しているバーストがあれば先に確定させる。 */
        if (s_burst_ent == victim) p502_burst_flush();
        p502_entry_close(victim);
    }

    /* --- 実 open(dosio.c File_Open の O_RDWR → O_RDONLY フォールバックを踏襲) */
    long long t0 = p502_now_ns();
    int fd = open(filename, O_RDWR);
    if (fd < 0)
        fd = open(filename, O_RDONLY);
    long long dt = p502_now_ns() - t0;

    if (fd < 0) {
        s_pend_open = 0;
        s_pend_hit  = 0;
        s_pend_ns   = 0;
        return (FILEH)NULL;   /* dosio.c は (FILEH)FALSE を返す = NULL 相当 */
    }

    victim->fd = fd;
    strncpy(victim->path, filename, SASI_IO_PATH_MAX - 1);
    victim->path[SASI_IO_PATH_MAX - 1] = '\0';
    victim->current_offset = 0;
    victim->lru = ++s_lru_clock;

    s_pend_open = 1;
    s_pend_hit  = 0;
    s_pend_ns   = dt;
    return (FILEH)victim;
}

uint32_t sasi_io_seek(FILEH handle, uint32_t pointer, uint16_t mode)
{
    sasi_io_entry* e = (sasi_io_entry*)handle;
    if (!e || e->fd < 0) return (uint32_t)-1;

    off_t newpos;
    switch (mode) {
    case FSEEK_SET:
    default:
        newpos = (off_t)pointer;
        break;
    case FSEEK_CUR:
        newpos = e->current_offset + (off_t)(int32_t)pointer;
        break;
    case FSEEK_END: {
        /* sasi.c は FSEEK_SET しか使わないが、File_Seek と意味を合わせておく。 */
        off_t end = lseek(e->fd, 0, SEEK_END);
        if (end < 0) return (uint32_t)-1;
        newpos = end + (off_t)(int32_t)pointer;
        break;
    }
    }
    if (newpos < 0) return (uint32_t)-1;

    /* OS の lseek() は発行しない — 値だけを保持し、次の read/write が
     * pread/pwrite の offset 引数として消費する。 */
    e->current_offset = newpos;
    e->lru = ++s_lru_clock;
    return (uint32_t)newpos;
}

uint32_t sasi_io_read(FILEH handle, void *data, uint32_t length)
{
    sasi_io_entry* e = (sasi_io_entry*)handle;
    if (!e || e->fd < 0) return 0;

    p502_burst_note(e, e->current_offset);

    long long t0 = p502_now_ns();
    ssize_t n = pread(e->fd, data, (size_t)length, e->current_offset);
    s_burst_ns += p502_now_ns() - t0;

    if (n <= 0) return 0;   /* dosio.c: ReadFile が *lp<=0 で FALSE → File_Read は 0 */
    e->current_offset += n;
    e->lru = ++s_lru_clock;
    s_burst_rd++;
    s_burst_next_off = e->current_offset;
    return (uint32_t)n;
}

uint32_t sasi_io_write(FILEH handle, void *data, uint32_t length)
{
    sasi_io_entry* e = (sasi_io_entry*)handle;
    if (!e || e->fd < 0) return 0;

    p502_burst_note(e, e->current_offset);

    long long t0 = p502_now_ns();
    ssize_t n = pwrite(e->fd, data, (size_t)length, e->current_offset);
    s_burst_ns += p502_now_ns() - t0;

    if (n <= 0) return 0;   /* dosio.c: WriteFile が *lp<=0 で FALSE → File_Write は 0 */
    e->current_offset += n;
    e->lru = ++s_lru_clock;
    s_burst_wr++;
    s_burst_next_off = e->current_offset;
    return (uint32_t)n;
}

int16_t sasi_io_close(FILEH handle)
{
    (void)handle;
    /* no-op: fd はテーブルに保持し続ける。実 close() は LRU eviction か
     * sasi_io_cache_invalidate_all() でのみ発生する。 */
    if (s_pend_open || s_pend_hit) {
        /* 転送が起きないまま閉じられた(Seek 失敗等)。生値を落とさず、
         * 進行中バーストがあればそこへ計上する。 */
        if (s_burst_ent) {
            s_burst_opens += s_pend_open;
            s_burst_hits  += s_pend_hit;
            s_burst_ns    += s_pend_ns;
        }
        s_pend_open = 0;
        s_pend_hit  = 0;
        s_pend_ns   = 0;
    }
    return 0;
}

void sasi_io_cache_invalidate_all(void)
{
    p502_tab_init();
    p502_burst_flush();
    s_burst_next_off = 0;
    s_pend_open = 0;
    s_pend_hit  = 0;
    s_pend_ns   = 0;
    for (int i = 0; i < MX68K_SASI_UNIT_COUNT; i++)
        p502_entry_close(&s_tab[i]);
}
