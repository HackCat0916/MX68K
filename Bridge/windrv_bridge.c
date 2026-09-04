/* P642: Windrv ホスト側実装(Mac フォルダのゲスト共有)。
 * P647: 書込み対応($49 Create / $4D Write / $45 Delete / $44 Rename /
 *       $42 MakeDir / $43 RemoveDir)を追加。書込みは共有有効トグルとは
 *       **独立した第 2 のトグル**(既定 OFF)で制御し、OFF のときは
 *       P642/P643 と完全に同一の読み取り専用挙動へ倒れる。
 *
 * ゲスト側ドライバは標準ブートディスク同梱の `\SYS\WindrvXM.SYS`(XM6 用の実
 * バイナリ、無改造)。本ファイルは XM6 の `vm/windrv.cpp` が担っていた
 * 「A5 コマンドブロック ⇔ ホストファイルシステム」のマーシャリング層を、
 * macOS/POSIX 向けに新規実装したもの。
 *
 * ★参照実装との関係: `xm6/vm/windrv.cpp` はコマンドディスパッチ層のみを含み、
 *   実際のホスト側ファイルシステム実装(`FileSys` 純粋仮想の具象サブクラス)は
 *   公開されていない(`.mx68k_cycles/windrv_protocol.md` 冒頭)。したがって
 *   「A5 ブロックのレイアウト・構造体オフセット・エラーコード体系」は
 *   windrv.cpp からの [static code reading] で完全に裏取りできているが、
 *   「パス変換・ワイルドカード照合・大文字小文字ポリシー」は MX 独自設計である。
 *
 * ★スレッドモデル: 本ファイルの全コードは `trace_Memory_WriteB/WriteW`
 *   (= CPU コアのメモリ書込みコールバック、CVDisplayLink スレッド)から
 *   同期的に呼ばれる。XM6 は別スレッドへ委譲するが、MX は 1 コマンドを
 *   その場で完結させる —— ホストのファイル I/O 中はエミュレーションが
 *   停止する。MVP としてはこれを許容する(1 コマンドあたりの I/O は
 *   ディレクトリ 1 走査 or 数十 KB の read が上限)。
 *
 * ★セキュリティ: 利用者が設定画面で明示的に選んだ 1 フォルダのみを公開する。
 *   パストラバーサル対策は「既存の対象」については
 *   `windrv_resolve_and_validate_path()`、「これから作る対象」については
 *   `windrv_resolve_new_path()` に一本化してある(個別コマンドで検証ロジックを
 *   複製しないこと)。書込み系はさらに `g_windrv_write_wired` を各ハンドラの
 *   最初の実行文でゲートする。 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <iconv.h>
#include <unistd.h>      /* P647: unlink, rmdir, close, ftruncate, fileno */
#include <fcntl.h>       /* P647: open, O_RDWR/O_CREAT/O_EXCL/O_TRUNC/O_NOFOLLOW */
/* P647: renameatx_np / RENAME_EXCL(macOS 10.12+ の macOS 固有ヘッダ)。
 * MX は arm64 macOS 専用ビルドなので条件コンパイルはしない。
 * ★POSIX 標準の rename(2) を使わない理由は windrv_cmd_rename() のコメント参照
 *   —— rename(2) は宛先を黙って上書きする。 */
#include <sys/stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "windrv_bridge.h"
#include "EmulatorBridge.h"          /* debug_log */
#include "../Core/c68k/c68k.h"       /* C68K / C68k_Get_AReg / C68k_Set_DReg */

/* Core のメモリアクセス。sasi_bridge.c / sram_ext_bridge.c と同じ
 * 「ヘッダを引かず extern 再宣言」パターン。`Memory_ReadB`/`Memory_WriteB` は
 * Core/px68k/x68k/x68kmemory.h:6,10 でこの 2 関数への #define。
 * 実体は Core/px68k/x68k/mem_wrap.c:424 / :199、シグネチャは厳密一致。
 * ★CPU コールバック側(m68000_bridge.c の trace_Memory_ReadB/WriteB)ではなく
 *   Core 本体を直接呼ぶ —— 診断プローブ群の再入を避けるため。 */
extern uint32_t cpu_readmem24(uint32_t adr);
extern void     cpu_writemem24(uint32_t adr, uint32_t data);

/* ------------------------------------------------------------------------
 * Human68k / Windrv プロトコル定数
 *
 * 出所はすべて XM6 `vm/windrv.cpp` / `vm/windrv.h` の直読み
 * (`.mx68k_cycles/windrv_protocol.md` に記号表化済み)。
 * ------------------------------------------------------------------------ */

/* コマンドブロック(A5)のオフセット。windrv.cpp:328-366 (Execute/ExecuteCompatible)
 * および各コマンド実装。 */
#define CMD_OFF_UNIT      1    /* (1B) ユニット番号 */
#define CMD_OFF_CMD       2    /* (1B) コマンド番号 */
#define CMD_OFF_ERRL      3    /* (1B) 致命的エラー下位 */
#define CMD_OFF_ERRH      4    /* (1B) 致命的エラー上位 */
#define CMD_OFF_ATTR     13    /* (1B) 属性 / $40 では対応ドライブ数の書き戻し先 */
#define CMD_OFF_ADDR1    14    /* (4B) NAMESTS / バッファ / capacity / dpb アドレス */
#define CMD_OFF_ADDR2    18    /* (4B) files_t アドレス・サイズ、および結果格納先 */
#define CMD_OFF_ADDR3    22    /* (4B) FCB アドレス / $40 ではベースドライブ番号(1B) */
#define CMD_OFF_DATE     18    /* $4F: FAT形式日付(W)。CMD_OFF_ADDR2 と同一オフセット
                                * ——windrv_set_result() がここを結果格納にも使うため、
                                * ハンドラ冒頭で必ず先に読むこと(実装規約2)。 */
#define CMD_OFF_TIME     20    /* $4F: FAT形式時刻(W) */

/* NAMESTS 構造体(windrv.cpp:2566-2598 GetNameSts / windrv.h:107-119)。 */
#define NAMESTS_OFF_WILDCARD  0    /* (1B) ワイルドカード文字数, 0xFF = ファイル指定なし */
#define NAMESTS_OFF_DRIVE     1    /* (1B) ドライブ番号 (A=0) */
#define NAMESTS_OFF_PATH      2    /* (65B) パス */
#define NAMESTS_OFF_NAME     67    /* (8B)  ファイル名先頭 8 文字 (0x20 padding) */
#define NAMESTS_OFF_EXT      75    /* (3B)  拡張子 (0x20 padding) */
#define NAMESTS_OFF_ADD      78    /* (10B) ファイル名残り 10 文字 (0x00 padding) */
#define NAMESTS_PATH_LEN     65
#define NAMESTS_NAME_LEN      8
#define NAMESTS_EXT_LEN       3
#define NAMESTS_ADD_LEN      10

/* ★NAMESTS.path のディレクトリ区切りは標準表記の '¥'(0x5C)ではなく TAB(0x09)。
 * windrv.cpp:1415 の明示コメントによる WINDRV.SYS 独自ワイヤフォーマット。
 * ここを 0x5C と誤ると全てのサブディレクトリ指定が失敗する(既知の罠)。 */
#define NAMESTS_PATH_SEP   0x09

/* files_t 構造体(windrv.cpp:2596-2680 GetFiles/SetFiles / windrv.h:122-135)。 */
#define FILES_OFF_FATR     0    /* (1B) 検索属性        読込専用 */
#define FILES_OFF_DRIVE    1    /* (1B) ドライブ番号    読込専用 */
#define FILES_OFF_SECTOR   2    /* (4B) 検索コンテキスト(不透明ハンドル) */
#define FILES_OFF_OFFSET   8    /* (2B) 検索位置 */
#define FILES_OFF_ATTR    21    /* (1B) 結果: ファイル属性 */
#define FILES_OFF_TIME    22    /* (2B) 結果: 最終変更時刻 */
#define FILES_OFF_DATE    24    /* (2B) 結果: 最終変更月日 */
#define FILES_OFF_SIZE    26    /* (4B) 結果: ファイルサイズ */
#define FILES_OFF_FULL    30    /* (23B) 結果: フルファイル名 */
#define FILES_FULL_LEN    23

/* FCB 構造体(windrv.cpp:2686-2812 GetFcb/SetFcb / windrv.h:169-184)。
 * ★fileptr(+6)は Read 完了後にホスト側から書き戻す —— SetFcb は fileptr
 *   (windrv.cpp:2788)と mode(:2789)を無条件に書き、続けて attr/time/date/size
 *   (:2806-2811)も書く。書き戻さないとゲスト側の読み込み位置が前進せず、
 *   TYPE 等が同一箇所を無限に読み直す(P643)。 */
#define FCB_OFF_FILEPTR    6    /* (4B) ファイルポインタ(読込・Read完了後に書戻し必須、windrv.cpp:2788参照) */
#define FCB_OFF_MODE      14    /* (2B) オープンモード(読込のみ・MVP では無視) */
#define FCB_OFF_ATTR      47    /* (1B) 書込 */
#define FCB_OFF_TIME      58    /* (2B) 書込 */
#define FCB_OFF_DATE      60    /* (2B) 書込 */
#define FCB_OFF_SIZE      64    /* (4B) 書込 */

/* capacity_t (windrv.h:156-162) / dpb_t (windrv.h:169-184)。 */
#define CAP_OFF_FREE       0
#define CAP_OFF_CLUSTERS   2
#define CAP_OFF_SECTORS    4
#define CAP_OFF_BYTES      6

#define DPB_OFF_SECTOR_SIZE   0
#define DPB_OFF_CLUSTER_SIZE  2
#define DPB_OFF_SHIFT         3
#define DPB_OFF_FAT_SECTOR    4
#define DPB_OFF_FAT_MAX       6
#define DPB_OFF_FAT_SIZE      7
#define DPB_OFF_FILE_MAX      8
#define DPB_OFF_DATA_SECTOR  10
#define DPB_OFF_CLUSTER_MAX  12
#define DPB_OFF_ROOT_SECTOR  14
#define DPB_OFF_MEDIA        20

/* エラーコード(windrv.h:34-72)。 */
#define FS_INVALIDFUNC          0xFFFFFFFFu
#define FS_FILENOTFND           0xFFFFFFFEu
#define FS_DIRNOTFND            0xFFFFFFFDu
#define FS_OVEROPENED           0xFFFFFFFCu
#define FS_CANTACCESS           0xFFFFFFFBu
#define FS_NOTOPENED            0xFFFFFFFAu
#define FS_INVALIDPATH          0xFFFFFFF3u
#define FS_INVALIDPRM           0xFFFFFFF2u
#define FS_LASTFILE             0xFFFFFFEEu
#define FS_CANTWRITE            0xFFFFFFEDu
/* P647: 書込み系で使う追加コード(windrv.h:53-56,67)。 */
#define FS_DIRALREADY           0xFFFFFFECu   /* windrv.h:53 指定のディレクトリは既に登録されている */
#define FS_CANTDELETE           0xFFFFFFEBu   /* windrv.h:54 削除できない */
#define FS_CANTRENAME           0xFFFFFFEAu   /* windrv.h:55 改名できない */
#define FS_DISKFULL             0xFFFFFFE9u   /* windrv.h:56 ディスクが一杯でファイルが作れない */
#define FS_FILEEXIST            0xFFFFFFB0u   /* windrv.h:67 ファイルが存在する */
/* P650 変更0: $53 DiskRead の非対応応答に使う。 */
#define FS_NOTIOCTRL            0xFFFFFFEFu   /* XM6h:windrv.h:50 IOCTRLできないデバイス */
#define FS_FATAL_INVALIDUNIT    0xFFFFFFA0u
#define FS_FATAL_INVALIDCOMMAND 0xFFFFFFA1u
#define FS_FATAL_WRITEPROTECT   0xFFFFFFA2u
#define FS_FATAL_MEDIAOFFLINE   0xFFFFFFA3u

/* ファイル属性ビット(windrv.h:87-95)。 */
#define AT_READONLY   0x01
#define AT_HIDDEN     0x02
#define AT_SYSTEM     0x04
#define AT_VOLUME     0x08
#define AT_DIRECTORY  0x10
#define AT_ARCHIVE    0x20

/* ------------------------------------------------------------------------
 * MVP の実装上限
 * ------------------------------------------------------------------------ */

/* ★Code Review 指摘4への対応: 同時オープン数の上限。満杯時は fopen() を
 * 呼ばずに FS_OVEROPENED を返す(ホスト側 fd を一切消費しない)。 */
#define WINDRV_MAX_FILES    32

/* $47/$48 の検索コンテキスト。ゲストが検索ループを途中で放棄しても
 * DIR* が漏れないよう、固定数 + 最古のものを閉じて再利用する。 */
#define WINDRV_MAX_SEARCH    8

/* 非同期実行ハンドルの同時生存数($E9F001 経路)。 */
#define WINDRV_MAX_HANDLES   8

/* ★Code Review 指摘3への対応: ホスト側パス結合バッファ。
 * マウントルート(最大 PATH_MAX)+ NAMESTS 由来の相対パス(path 65B を
 * TAB 分割したセグメント群 + name8 + '.' + ext3 + add10)を安全マージン込みで
 * 収める固定長。結合は必ず snprintf() で行い、切り詰めが起きたら
 * FS_INVALIDPATH で失敗させる(不完全パスでファイル操作を続行しない)。 */
#define WINDRV_PATH_BUF     (WINDRV_HOST_PATH_MAX * 2)

/* $4C Read の 1 回あたりの転送チャンク。巨大なスタックバッファを確保せず、
 * このサイズで fread → Memory_WriteB を繰り返す。 */
#define WINDRV_READ_CHUNK   4096

/* P647: $4D Write の転送チャンク。Read と同じ理由(巨大なスタック確保をしない)。 */
#define WINDRV_WRITE_CHUNK  WINDRV_READ_CHUNK

/* P647: $4D 1 回あたりの書込みサイズ上限。
 * ★導出: 書込元はゲストの 24bit アドレス空間 = 16MB、X68000 の最大実装 RAM は
 *   12MB(Docs/01 ハードウェア仕様)。1 回の $4D が正当に 16MB を超えることは
 *   物理的に有り得ない。参照実装(XM6 の非公開 FileSys 側)に対応物が無いため
 *   「値が一致しているから安全」という論法は使っておらず、MX 側の物理的制約から
 *   独立に導出した値である。超過は FS_INVALIDPRM。 */
#define WINDRV_WRITE_MAX    (16 * 1024 * 1024)

/* P647: 構築するホストファイル名の上限バイト数。
 * ★Human68k 側の最大は base18 + '.' + ext3 = 22 SJIS バイト。UTF-8 展開は
 *   最悪 3 倍 ≒ 66 バイトなので NAME_MAX(macOS = 255)は上限として十分。
 *   超過時に snprintf が黙って切り詰めると「要求されたのと別のファイル」を
 *   作ることになるため、切り詰め検出時は必ず FS_INVALIDPATH で失敗させる。 */
#define WINDRV_HOST_NAME_MAX NAME_MAX

/* Human68k のファイル名は 18 + '.' + 3。full[23] に NUL 込みで収まる。 */
#define H68_BASE_LEN    (NAMESTS_NAME_LEN + NAMESTS_ADD_LEN)   /* 18 */

/* ------------------------------------------------------------------------
 * 状態
 * ------------------------------------------------------------------------ */

/* Human68k の 8+10+3 ファイル名表現。★P650 で「Human68k ファイル名表現」節から
 * ここへ前倒しした —— WindrvSearch が値メンバ `ent` として持つため。 */
typedef struct {
    uint8_t name[NAMESTS_NAME_LEN];   /* 0x20 padding */
    uint8_t ext[NAMESTS_EXT_LEN];     /* 0x20 padding */
    uint8_t add[NAMESTS_ADD_LEN];     /* 0x00 padding */
    char    full[FILES_FULL_LEN];     /* "BASE.EXT" + NUL(Shift-JIS) */
} WindrvH68Name;

/* realpath() 済みのマウントルート。windrv_init() / $40 InitDrive で確定する。
 * 空文字列 = 未確定(この状態では一切のパス解決を許可しない)。 */
static char s_root[WINDRV_HOST_PATH_MAX];
/* s_root に必ず末尾 '/' を付けたもの。前方一致判定に使う
 * (`/mnt/foo` に対し `/mnt/foobar` が誤って許可される境界バグの回避)。 */
static char s_root_slash[WINDRV_HOST_PATH_MAX + 1];

typedef struct {
    uint32_t fcb;      /* ゲスト FCB のアドレス = ハンドルキー。0 = 未使用 */
    FILE*    fp;
    /* P647: 監査ログ用のホスト絶対パス。$4D Write は FCB キーでしか対象を
     * 知らないため、Open / Create の時点で保存しておかないと
     * 「どのファイルが書き換わったか」がログに出ず、監査として意味を成さない。
     * windrv_file_close() でクリアする。 */
    char     path[WINDRV_PATH_BUF];
} WindrvFile;
static WindrvFile s_files[WINDRV_MAX_FILES];

typedef struct {
    uint32_t files_addr;   /* ゲスト files_t のアドレス。0 = 未使用 */
    DIR*     dir;
    uint32_t serial;       /* LRU 判定用 */
    uint8_t  fatr;         /* 検索属性 */
    uint8_t  wildcard;     /* NAMESTS.wildcard の複写 */
    uint8_t  name[NAMESTS_NAME_LEN];
    uint8_t  ext[NAMESTS_EXT_LEN];
    uint8_t  add[NAMESTS_ADD_LEN];
    char     dirpath[WINDRV_PATH_BUF];
    /* P650 変更4: 直近に返した検索エントリの控え。XM6 は CHostFiles が dirent を
     * 保持し GetEntry() で取り出す(XM6fs:4451)が、MX は検索結果をゲストの
     * files_t へ直接書いておりホスト側に控えが無い。files_t から読み戻す案は
     * full[23](結合済み文字列)からの name/ext/add 再分割が必要で誤りの余地が
     * 大きいため、XM6 と同じく検索側で控える方式を採る。 */
    int           have_entry;   /* 0 = 未取得(= $53 は FS_NOTIOCTRL) */
    WindrvH68Name ent;          /* name[8](0x20詰) / ext[3](0x20詰) / add[10](0x00詰) */
    uint8_t       ent_attr;
    uint16_t      ent_time, ent_date;
    uint32_t      ent_size;
} WindrvSearch;
static WindrvSearch s_search[WINDRV_MAX_SEARCH];
static uint32_t     s_search_serial = 0;

static uint32_t s_handles[WINDRV_MAX_HANDLES];
static uint32_t s_handle_next = 1;

/* コマンド実行中フラグ。ホスト I/O 中にゲストが同じポートを叩く事態
 * (通常起きないが、DMA 等の別経路からの再入)への保険。 */
static int s_busy = 0;

/* 現在のコマンド文脈(dispatch が設定し、各コマンド実装が読む)。 */
static uint32_t s_a5      = 0;
static uint8_t  s_unit    = 0;
static uint8_t  s_command = 0;
/* $40 InitDrive で確定する対応ユニット数。MX は 1 フォルダ = 1 ドライブ。 */
static uint32_t s_unit_max = 1;

/* 動作確認用の軽量ログ(コマンド番号と結果のみ)。Windrv 未装着なら 1 行も出ない。 */
static uint32_t s_log_count = 0;
#define WINDRV_LOG_CAP 200

/* P647: 書込み系操作の監査ログ用カウンタ。
 * ★s_log_count と共有しない理由: 読み取り系コマンドが上限 200 を容易に食い潰し、
 *   「読み取りが多いセッションでは書込み監査が 1 行も残らない」という
 *   自己反証可能性の破れ(0 行の解釈が複数通りに割れる)を生むため。 */
static uint32_t s_wlog_count = 0;
#define WINDRV_WLOG_CAP 200

/* P649: エラー応答専用ログ用カウンタ。
 * ★s_log_count と共有しない理由: 通常コマンドログが上限 200 を使い切った後は
 *   エラー応答が 1 行も残らず、「拒否が起きていない」のか「見えていないだけ」
 *   なのかを区別できなくなるため。 */
static uint32_t s_errlog_count = 0;
#define WINDRV_ERRLOG_CAP 200

/* P650: $47/$48 系の「分母つき」ログ用カウンタ($47/$53 の成否によらない全呼出し)。
 * ★s_log_count / s_errlog_count と共有しない理由: エラー専用ログには致命的な
 *   非対称があり、修正が効くとエラー行自体が消えるため「効いた理由」が観測できない
 *   (0 件の解釈が「仮説が正しかった」と「そもそも呼ばれていない」に割れる)。
 *   独立カウンタで全呼出しを記録することで 0 件の解釈が一意になる。 */
static uint32_t s_fslog_count = 0;
#define WINDRV_FSLOG_CAP 120

/* P652: $46 Attribute(取得方向)のパス解決失敗時に NAMESTS の生内容を記録する
 * 診断プローブ用カウンタ。既存カウンタと共有しない理由は s_fslog_count と同じ
 * ——0 件の解釈を一意にするため、専用のカウンタ/上限を持たせる。 */
static uint32_t s_attrlog_count = 0;
#define WINDRV_ATTRLOG_CAP 60

/* P656: 18 バイト超のベース名を切り詰めた場合のみ記録する診断プローブ用カウンタ。
 * ★既存カウンタと共有しない理由: 切り詰めは希少事象であり、他のログが上限を
 *   使い切ると 1 行も残らず「切り詰めが起きていない」のか「見えていないだけ」
 *   なのかを区別できなくなるため(0 行の解釈を一意にする)。 */
static uint32_t s_trunclog_count = 0;
#define WINDRV_TRUNCLOG_CAP 60

/* ------------------------------------------------------------------------
 * ゲストメモリアクセス(ビッグエンディアン)
 * ------------------------------------------------------------------------ */

static inline uint32_t wd_rb(uint32_t a) { return cpu_readmem24(a & 0x00FFFFFFu) & 0xFFu; }
static inline void     wd_wb(uint32_t a, uint32_t v) { cpu_writemem24(a & 0x00FFFFFFu, v & 0xFFu); }

static uint32_t wd_rw(uint32_t a) { return (wd_rb(a) << 8) | wd_rb(a + 1); }
static void     wd_ww(uint32_t a, uint32_t v) { wd_wb(a, v >> 8); wd_wb(a + 1, v); }

static uint32_t wd_rl(uint32_t a) {
    return (wd_rb(a) << 24) | (wd_rb(a + 1) << 16) | (wd_rb(a + 2) << 8) | wd_rb(a + 3);
}
static void wd_wl(uint32_t a, uint32_t v) {
    wd_wb(a, v >> 24); wd_wb(a + 1, v >> 16); wd_wb(a + 2, v >> 8); wd_wb(a + 3, v);
}
/* windrv.cpp:2482 GetAddr — 24bit アドレスとして読む(最上位バイトは必ず 0)。 */
static uint32_t wd_raddr(uint32_t a) { return wd_rl(a) & 0x00FFFFFFu; }

/* windrv.cpp:2273-2295 SetResult と同一。致命的 4 種のみ a5+3/a5+4 へ振り分け、
 * それ以外は a5+18 へ DOS エラーコードとして格納する。 */
static void windrv_set_result(uint32_t result) {
    uint32_t fatal = 0;
    switch (result) {
    case FS_FATAL_INVALIDUNIT:    fatal = 0x5001; break;
    case FS_FATAL_INVALIDCOMMAND: fatal = 0x5003; break;
    case FS_FATAL_WRITEPROTECT:   fatal = 0x700D; break;
    case FS_FATAL_MEDIAOFFLINE:   fatal = 0x7002; break;
    default: break;
    }
    if (fatal) {
        result = FS_INVALIDFUNC;
        wd_wb(s_a5 + CMD_OFF_ERRL, fatal & 0xFFu);
        wd_wb(s_a5 + CMD_OFF_ERRH, (fatal >> 8) & 0xFFu);
        /* リトライ可能ケース(bit13 立ち)では a5+18 を書き換えない
         * —— windrv.cpp:2288 の明示コメントどおり。 */
        if ((fatal & 0x2000u) == 0u) {
            wd_wl(s_a5 + CMD_OFF_ADDR2, result);
        }
    } else {
        wd_wl(s_a5 + CMD_OFF_ADDR2, result);
    }
}

/* ------------------------------------------------------------------------
 * 文字コード変換(Shift-JIS/CP932 ⇔ UTF-8)
 *
 * ゲスト側のファイル名は Human68k 標準の Shift-JIS。macOS 側は UTF-8。
 * iconv(3) を使う。★macOS の HFS+/APFS は UTF-8 NFD で保持することがあるが、
 * ここでは readdir() が返したバイト列をそのまま CP932 へ変換する
 * —— NFD の濁点分離は CP932 へ変換できず、その名前は列挙対象から外れる
 * (MVP の既知の制約。ASCII 名は影響を受けない)。
 * ------------------------------------------------------------------------ */

static int windrv_iconv(const char* from, const char* to,
                        const char* src, size_t srclen,
                        char* dst, size_t dstsize) {
    iconv_t cd;
    char*   inbuf;
    char*   outbuf;
    size_t  inleft, outleft, r;

    if (dstsize == 0) return -1;
    cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return -1;

    inbuf   = (char*)src;
    inleft  = srclen;
    outbuf  = dst;
    outleft = dstsize - 1;
    r = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    iconv_close(cd);
    if (r == (size_t)-1 || inleft != 0) return -1;   /* 変換不能 or バッファ不足 */
    *outbuf = '\0';
    return (int)(dstsize - 1 - outleft);
}

static int windrv_utf8_to_sjis(const char* src, char* dst, size_t dstsize) {
    return windrv_iconv("UTF-8", "CP932", src, strlen(src), dst, dstsize);
}
static int windrv_sjis_to_utf8(const char* src, size_t srclen, char* dst, size_t dstsize) {
    return windrv_iconv("CP932", "UTF-8", src, srclen, dst, dstsize);
}

/* ------------------------------------------------------------------------
 * Human68k ファイル名表現
 *
 * ★P650: 型定義 `WindrvH68Name` は「状態」節の直前へ移動した
 *   —— WindrvSearch が直近エントリの控え(`ent`)として値メンバに持つため、
 *   WindrvSearch より前で完全型になっている必要がある。
 * ------------------------------------------------------------------------ */

/* Human68k で使えない文字。DOS 予約文字 + 制御文字。 */
static int windrv_char_forbidden(unsigned char c) {
    if (c < 0x20) return 1;
    switch (c) {
    case '/': case '\\': case ':': case '*': case '?':
    case '"': case '<':  case '>': case '|':
        return 1;
    default:
        return 0;
    }
}

/* CP932 の 2 バイト文字の先頭バイト判定。windrv_host_to_h68() の大文字化ループと
 * 同一の判定式を共有する(書き写しをしない)。 */
static int windrv_sjis_lead(unsigned char c) {
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

/* XM6fs:1177-1196 SeparateExt 相当。戻り値 = Human68k 拡張子の '.' の位置(無ければ len)。 */
static size_t windrv_separate_ext(const char* s, size_t len) {
    const char* dot = strrchr(s, '.');
    size_t pos = dot ? (size_t)(dot - s) : len;
    long   n;
    /* XM6fs:1188 の特例: 長さ 20〜22 かつ s[18]=='.' かつ末尾 '.' */
    if (len >= 20 && len <= 22 && s[18] == '.' && s[len - 1] == '.') pos = 18;
    n = (long)len - (long)pos - 1;
    if (pos == 0 || n < 1 || n > 3) pos = len;   /* XM6fs:1193 */
    return pos;
}

/* XM6fs:857-1055 の nCount 省略版(衝突時サフィックスなし、P650_design_inv.md §C-3 で
 * 意図的に除外——MX は列挙 / 名前解決とも無キャッシュ設計のため、XM6 のカウンタ方式は
 * 構造的に持ち込めない)。base/ext は切り詰め後(いずれも '.' を含まない)。
 * 戻り値 0 = 表現不能(呼出し側は従来どおり列挙から除外)。 */
static int windrv_truncate_h68(const char* s, size_t len,
                               char* base, size_t* pbaselen, char* ext, size_t* pextlen) {
    const size_t nMax = H68_BASE_LEN;   /* 18、既存定数を再利用 */
    size_t pExt = windrv_separate_ext(s, len);
    size_t pCut = 0, pSecond, pStop, nExt, i;
    long   stop;

    /* XM6fs:987-997 —— ★符号付きで計算すること。size_t で先にクランプすると
       SJIS 補正の stop++ が効いてしまい 1 バイトずれる(P650 調査で確認済みの落とし穴)。 */
    stop = (long)pExt - (long)nMax;
    if (pExt > 0) {
        pCut = 1;
        if (windrv_sjis_lead((unsigned char)s[0])) { pCut = 2; stop++; }
    }
    if (stop < 0) stop = 0;
    pStop = (size_t)stop;

    /* XM6fs:1000-1004 ベース名終端 = pCut 以降で最初の '.'、無ければ末尾、nMax で頭打ち */
    { const char* d = (const char*)memchr(s + pCut, '.', len - pCut);
      pCut = d ? (size_t)(d - s) : len;
      if (pCut > nMax) pCut = nMax; }

    /* XM6fs:1007-1010 pSecond = (pStop, pExt) 区間で最も左の '.'(=残す末尾チャンク先頭) */
    pSecond = pExt;
    for (i = pExt; i > pStop + 1; i--) if (s[i - 1] == '.') pSecond = i - 1;
    nExt = pExt - pSecond;

    /* XM6fs:1014 ベース名をさらに短縮 */
    if (pCut + nExt > nMax) pCut = (nMax > nExt) ? (nMax - nExt) : 0;

    /* XM6fs:1016-1025 SJIS 2 バイト文字の途中で切らない */
    for (i = 0; i < pCut; i++)
        if (windrv_sjis_lead((unsigned char)s[i])) { i++; if (i >= pCut) { pCut--; break; } }

    /* XM6fs:1027-1035 結合 */
    memcpy(base, s, pCut);
    memcpy(base + pCut, s + pSecond, nExt);
    *pbaselen = pCut + nExt;
    *pextlen  = (pExt < len) ? (len - pExt - 1) : 0;
    if (*pextlen) memcpy(ext, s + pExt + 1, *pextlen);

    /* XM6fs:1038-1054 妥当性判定 */
    if (*pbaselen == 0) return 0;
    if (base[*pbaselen - 1] == ' ') return 0;
    return 1;
}

/* ホストのファイル名(UTF-8)を Human68k の 8+10+3 表現へ変換する。
 * 表現できない名前(長すぎる・CP932 に無い文字・禁止文字)は 0 を返し、
 * 呼出し側はそのエントリを列挙対象から外す(MVP の既知の制約)。 */
static int windrv_host_to_h68(const char* host_utf8, WindrvH68Name* out) {
    char sjis[512];
    char base[H68_BASE_LEN + 1];
    char ext[NAMESTS_EXT_LEN + 1];
    size_t blen, elen, i, n;

    if (windrv_utf8_to_sjis(host_utf8, sjis, sizeof(sjis)) < 0) return 0;

    n = strlen(sjis);
    if (n == 0) return 0;
    for (i = 0; i < n; i++) {
        if (windrv_char_forbidden((unsigned char)sjis[i])) return 0;
    }

    /* ★P656: 18+3 の枠を超える場合は XM6 同様に切り詰める(従来は列挙から除外)。 */
    if (!windrv_truncate_h68(sjis, n, base, &blen, ext, &elen)) return 0;
    base[blen] = '\0';
    ext[elen] = '\0';

    /* P656: 切り詰めが実際に発生した場合のみ記録する自己反証可能性プローブ。
     * 元のベース名(拡張子除く)が 18 バイトを超えていた = 切り詰めが起きた。 */
    {
        size_t orig_pExt = windrv_separate_ext(sjis, n);
        if (orig_pExt > H68_BASE_LEN && s_trunclog_count < WINDRV_TRUNCLOG_CAP) {
            s_trunclog_count++;
            debug_log("[P656-WINDRV-TRUNC] #%u host=\"%s\" -> base=\"%.*s\" ext=\"%.*s\"\n",
                      s_trunclog_count, host_utf8, (int)blen, base, (int)elen, ext);
        }
    }

    /* 大文字化(ASCII のみ。CP932 の 2 バイト文字は触らない)。
     * ★2 バイト文字の 2 バイト目が ASCII 小文字域(0x61-0x7A)に入りうるため、
     *   先頭バイトが 0x81-0x9F / 0xE0-0xFC のときは 2 バイトまとめて読み飛ばす。 */
    for (i = 0; i < blen; ) {
        unsigned char c = (unsigned char)base[i];
        if (windrv_sjis_lead(c)) { i += 2; continue; }
        base[i] = (char)toupper(c);
        i++;
    }
    for (i = 0; i < elen; ) {
        unsigned char c = (unsigned char)ext[i];
        if (windrv_sjis_lead(c)) { i += 2; continue; }
        ext[i] = (char)toupper(c);
        i++;
    }

    memset(out->name, 0x20, sizeof(out->name));
    memset(out->ext,  0x20, sizeof(out->ext));
    memset(out->add,  0x00, sizeof(out->add));
    for (i = 0; i < blen && i < NAMESTS_NAME_LEN; i++) out->name[i] = (uint8_t)base[i];
    for (i = NAMESTS_NAME_LEN; i < blen; i++) out->add[i - NAMESTS_NAME_LEN] = (uint8_t)base[i];
    for (i = 0; i < elen; i++) out->ext[i] = (uint8_t)ext[i];

    /* full[23] は `dir` がそのまま表示する文字列。 */
    memset(out->full, 0, sizeof(out->full));
    if (elen) snprintf(out->full, sizeof(out->full), "%s.%s", base, ext);
    else      snprintf(out->full, sizeof(out->full), "%s", base);
    return 1;
}

/* P647: Shift-JIS バイト列に Human68k の禁止文字 / 制御文字 / 埋め込み NUL が
 * 含まれていないかを走査する(windrv_h68_to_host() の第 1 層検証)。
 * 1 = 問題なし / 0 = 拒否。
 * ★2 バイト文字の第 2 バイトを誤検出しないこと —— CP932 の第 2 バイトは
 *   0x5C('\')や 0x7C('|')を取りうる(例:「表」= 0x95 0x5C)。先頭バイトが
 *   0x81-0x9F / 0xE0-0xFC のときは 2 バイトまとめて読み飛ばす。判定式は既存
 *   windrv_host_to_h68() の大文字化ループと同一のもの(書き写しではなく同じ式)。
 * ★末尾が中途半端なリードバイトで終わる入力はここを素通りするが、その場合は
 *   続く iconv() が不完全なシーケンスとして変換に失敗し、そこで拒否される。 */
static int windrv_sjis_scan_ok(const char* s, size_t len) {
    size_t i;
    for (i = 0; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) { i += 2; continue; }
        /* windrv_char_forbidden() は c < 0x20 も拒否するので、埋め込み NUL や
         * 制御文字もこの 1 判定で落ちる。 */
        if (windrv_char_forbidden(c)) return 0;
        i++;
    }
    return 1;
}

/* P647: NAMESTS の name[8]/ext[3]/add[10] から**ホスト側の**ファイル名(UTF-8)を
 * 構築する。windrv_host_to_h68() の逆方向。
 *
 * ★本関数は Create / Rename(新名)/ MakeDir が使う唯一の入口であり、本機能で
 *   最大の新規攻撃面である。「まだ存在しないファイル名」をゲスト由来のバイト列
 *   から組み立てるため、既存の windrv_find_entry()(readdir で**実在する**
 *   エントリを探す)は転用できず、ここが新規の検証点になる。
 *
 * ★設計方針: 曖昧・不正な入力は **拒否**する(0 を返す)。エスケープや置換で
 *   「それらしい別の名前」を作ってはならない —— 利用者が意図したのと違う
 *   パスへ書き込む事故になる。
 *
 * ★大文字小文字(MX 独自ポリシー、参照実装に対応物なし): ゲストが送ってきた
 *   バイトをそのまま使い、MX 側では大文字化も小文字化もしない。
 *   windrv_host_to_h68() はホスト名を大文字化してゲストへ見せるので、
 *   ゲストが TEST.TXT を作ればホストにも TEST.TXT ができ、往復で一致する。
 *
 * 戻り値: 1 = 成功(out に UTF-8 のファイル名 1 要素、区切り文字を含まない)
 *         0 = 拒否(呼出し側は FS_INVALIDPATH を返すこと) */
static int windrv_h68_to_host(uint32_t ns_addr, char* out, size_t outsz) {
    uint8_t name[NAMESTS_NAME_LEN];
    uint8_t ext[NAMESTS_EXT_LEN];
    uint8_t add[NAMESTS_ADD_LEN];
    char    base[H68_BASE_LEN + 1];
    char    extbuf[NAMESTS_EXT_LEN + 1];
    char    sjis[H68_BASE_LEN + NAMESTS_EXT_LEN + 2];   /* base18 + '.' + ext3 + NUL */
    char    utf8[WINDRV_HOST_NAME_MAX + 1];
    size_t  blen1, blen2, blen, elen, i, ulen;
    int     n;

    if (outsz == 0) return 0;

    for (i = 0; i < NAMESTS_NAME_LEN; i++) name[i] = (uint8_t)wd_rb(ns_addr + NAMESTS_OFF_NAME + i);
    for (i = 0; i < NAMESTS_EXT_LEN;  i++) ext[i]  = (uint8_t)wd_rb(ns_addr + NAMESTS_OFF_EXT  + i);
    for (i = 0; i < NAMESTS_ADD_LEN;  i++) add[i]  = (uint8_t)wd_rb(ns_addr + NAMESTS_OFF_ADD  + i);

    /* --- base 部の組み立て(name は 0x20 padding、add は 0x00 padding)---
     * ★CP932 の 2 バイト目は 0x40-0x7E / 0x80-0xFC の範囲であり 0x20 / 0x00 を
     *   取らないため、末尾からの padding 除去が 2 バイト文字を割ることはない。 */
    blen1 = NAMESTS_NAME_LEN;
    while (blen1 > 0 && name[blen1 - 1] == 0x20) blen1--;
    blen2 = NAMESTS_ADD_LEN;
    while (blen2 > 0 && (add[blen2 - 1] == 0x00 || add[blen2 - 1] == 0x20)) blen2--;

    /* ★不整合の拒否: 9 文字目以降(add)があるなら、Human68k の規約上 name は
     *   8 文字で埋まっているはず。そうでない入力は破損しているか作為的なので、
     *   それらしい名前へ直さず拒否する。 */
    if (blen2 > 0 && blen1 < NAMESTS_NAME_LEN) return 0;

    blen = blen1 + blen2;
    if (blen == 0 || blen > H68_BASE_LEN) return 0;   /* 拡張子だけの名前は作らない */
    memcpy(base, name, blen1);
    memcpy(base + blen1, add, blen2);
    base[blen] = '\0';

    elen = NAMESTS_EXT_LEN;
    while (elen > 0 && ext[elen - 1] == 0x20) elen--;
    memcpy(extbuf, ext, elen);
    extbuf[elen] = '\0';

    /* --- 第 1 層検証(Shift-JIS バイト列に対して)--- */
    if (!windrv_sjis_scan_ok(base, blen))   return 0;
    if (!windrv_sjis_scan_ok(extbuf, elen)) return 0;

    if (elen) n = snprintf(sjis, sizeof(sjis), "%s.%s", base, extbuf);
    else      n = snprintf(sjis, sizeof(sjis), "%s", base);
    if (n < 0 || (size_t)n >= sizeof(sjis)) return 0;

    if (windrv_sjis_to_utf8(sjis, (size_t)n, utf8, sizeof(utf8)) < 0) return 0;

    /* --- 第 2 層検証(変換後の UTF-8 = 実際に POSIX へ渡るバイト列に対して)---
     * ★これが本質的な防御であり、第 1 層は縦深防御の外側にすぎない。
     *   UTF-8 の構造上、非 ASCII コードポイントのエンコード結果は先頭バイト・
     *   継続バイトとも必ず 0x80 以上になる。したがって iconv() が変換に成功した
     *   時点で、その出力に ASCII の '/' や '.' が「多バイト文字の一部として」
     *   紛れ込む経路は存在しない —— ここで見えている ASCII バイトは入力側でも
     *   ASCII バイトだったものだけである。よって第 1 層のリードバイト判定が
     *   仮に誤っても、実際に POSIX へ渡る文字列側のこの再走査で必ず捕捉される。 */
    ulen = strlen(utf8);
    if (ulen == 0) return 0;
    /* ★パス区切りの密輸を最終的に止めるのはここ。 */
    if (strchr(utf8, '/') != NULL) return 0;
    if (strcmp(utf8, ".") == 0 || strcmp(utf8, "..") == 0) return 0;
    /* ★先頭 '.' の拒否: 列挙側(windrv_find_entry / windrv_search_next)が
     *   d_name[0]=='.' を無条件にスキップするため、ドット始まりのファイルを
     *   作れてしまうと「作成できるが dir に出ず、ゲストが二度と列挙・削除
     *   できない名前空間」へ書き込めることになる(一貫性の破れ)。加えて
     *   ホスト側の設定ファイル(.DS_Store 等)と衝突しうる。 */
    if (utf8[0] == '.') return 0;
    if (ulen >= WINDRV_HOST_NAME_MAX) return 0;

    /* ★切り詰めは「要求されたのと別のファイルを作る」ことになるので必ず失敗させる。 */
    n = snprintf(out, outsz, "%s", utf8);
    if (n < 0 || (size_t)n >= outsz) return 0;
    return 1;
}

/* NAMESTS のパターンとホスト由来の Human68k 名を照合する。
 * '?' は任意 1 文字にマッチ(Human68k は '*' を OS 側で '?' 列へ展開する)。
 *
 * ★MVP の設計判断: `wildcard != 0`(ワイルドカード検索)のときは
 *   add[10](ファイル名 9-18 文字目)を照合対象から外す。
 *   `dir *.*` のときゲストが送る add は 0x00 埋めであり、これを厳密照合すると
 *   長いファイル名が一切列挙されなくなるため。`wildcard == 0`(完全名指定、
 *   `type FOO.TXT` 等)では add まで含めて厳密に照合する。 */
static int windrv_name_match(const WindrvSearch* q, const WindrvH68Name* h) {
    int i;
    for (i = 0; i < NAMESTS_NAME_LEN; i++) {
        if (q->name[i] == '?') continue;
        if (q->name[i] != h->name[i]) return 0;
    }
    for (i = 0; i < NAMESTS_EXT_LEN; i++) {
        if (q->ext[i] == '?') continue;
        if (q->ext[i] != h->ext[i]) return 0;
    }
    if (q->wildcard == 0) {
        for (i = 0; i < NAMESTS_ADD_LEN; i++) {
            if (q->add[i] == '?') continue;
            if (q->add[i] != h->add[i]) return 0;
        }
    }
    return 1;
}

/* stat の mtime を Human68k(= MS-DOS FAT)の time/date 形式へ。 */
static void windrv_time_date(const struct stat* st, uint16_t* out_time, uint16_t* out_date) {
    struct tm tmv;
    time_t t = st->st_mtime;
    localtime_r(&t, &tmv);
    *out_time = (uint16_t)(((tmv.tm_hour & 0x1F) << 11) |
                           ((tmv.tm_min  & 0x3F) << 5)  |
                           ((tmv.tm_sec / 2) & 0x1F));
    /* FAT の基準年は 1980。それ以前は 1980 にクランプする。 */
    {
        int year = tmv.tm_year + 1900;
        if (year < 1980) year = 1980;
        if (year > 2107) year = 2107;
        *out_date = (uint16_t)((((year - 1980) & 0x7F) << 9) |
                               (((tmv.tm_mon + 1) & 0x0F) << 5) |
                               (tmv.tm_mday & 0x1F));
    }
}

/* P647: 書込み許可の配線状態と、ホスト側の実際の書込み可否の両方を見る。
 *
 * ★読み取り専用マウント時(既定)は従来どおり通常ファイルへ常に AT_READONLY を
 *   立てる —— ゲスト側ツールが書込みを試みる前に自分で判断できるようにするため。
 * ★書込み許可時は、ホスト側が実際に書けないファイル(オーナー書込みビットが
 *   落ちている)だけ AT_READONLY にする。ここを常時 AT_READONLY のままにすると、
 *   ゲストの DOS ツールが「読取専用ファイル」と判断して削除・上書きを
 *   自ら拒否してしまい、新コマンドを足しても書込みが実用にならない。
 *
 * この判定は dir 表示 / $47/$48 の検索結果 / $4A Open の FCB 書き戻しの
 * 3 経路すべてが通る唯一の場所である(判定の書き写しをしないこと)。 */
static uint8_t windrv_attr_of(const struct stat* st) {
    if (S_ISDIR(st->st_mode)) return AT_DIRECTORY;
    if (!g_windrv_write_wired) return (uint8_t)(AT_ARCHIVE | AT_READONLY);
    return (uint8_t)(AT_ARCHIVE | ((st->st_mode & S_IWUSR) ? 0 : AT_READONLY));
}

/* ------------------------------------------------------------------------
 * ★パス解決と検証(Code Review 指摘1・指摘3 への対応)
 *
 * NAMESTS からホストパスを得る経路はこの 2 関数に一本化してある。
 * $41 CheckDir / $47 Files / $4A Open のいずれもここを通る —— 個別コマンドで
 * 検証ロジックを複製しないこと(片方だけ直し忘れる典型的な事故源)。
 * ------------------------------------------------------------------------ */

/* 解決済み候補パスがマウントルート配下かを判定する。
 * realpath() 済みであることが前提(シンボリックリンクは実体へ展開済みなので、
 * リンク経由の脱出もこの前方一致で捕捉できる)。 */
static int windrv_path_inside_root(const char* resolved) {
    size_t rootlen;
    if (s_root[0] == '\0') return 0;
    /* ルート自身を指す場合も許可する。 */
    if (strcmp(resolved, s_root) == 0) return 1;
    rootlen = strlen(s_root_slash);
    return (strncmp(resolved, s_root_slash, rootlen) == 0) ? 1 : 0;
}

/* NAMESTS.path(TAB 区切り)をマウントルート配下のホストディレクトリへ解決する。
 * 成功なら 0 を、失敗なら FS_* を返す。out には realpath() 済みの絶対パスが入る。 */
static uint32_t windrv_resolve_dir(uint32_t ns_addr, char* out, size_t outsz) {
    uint8_t path[NAMESTS_PATH_LEN];
    char    joined[WINDRV_PATH_BUF];
    char    resolved[WINDRV_PATH_BUF];
    size_t  i;
    int     n;

    if (s_root[0] == '\0') return FS_FATAL_MEDIAOFFLINE;

    for (i = 0; i < NAMESTS_PATH_LEN; i++) {
        path[i] = (uint8_t)wd_rb(ns_addr + NAMESTS_OFF_PATH + i);
    }

    n = snprintf(joined, sizeof(joined), "%s", s_root);
    if (n < 0 || (size_t)n >= sizeof(joined)) return FS_INVALIDPATH;

    /* path は TAB 区切り。先頭も TAB(ルート表現)。空セグメントは読み飛ばす。 */
    i = 0;
    while (i < NAMESTS_PATH_LEN && path[i] != 0x00) {
        size_t start, seglen;
        char   seg_sjis[NAMESTS_PATH_LEN + 1];
        char   seg_utf8[NAMESTS_PATH_LEN * 4 + 1];
        size_t k;

        if (path[i] == NAMESTS_PATH_SEP) { i++; continue; }
        start = i;
        while (i < NAMESTS_PATH_LEN && path[i] != NAMESTS_PATH_SEP && path[i] != 0x00) i++;
        seglen = i - start;
        if (seglen == 0) continue;

        memcpy(seg_sjis, &path[start], seglen);
        seg_sjis[seglen] = '\0';

        /* ★パストラバーサル対策(第1層): セグメント内に区切り文字や
         *   `..` / `.` が現れたら、realpath() を待たずにここで拒否する。
         *   realpath() + 前方一致(第2層)だけでも脱出は防げるが、
         *   明らかに不正な要求はホストの stat すら発生させない。 */
        if (strcmp(seg_sjis, "..") == 0 || strcmp(seg_sjis, ".") == 0) return FS_INVALIDPATH;
        for (k = 0; k < seglen; k++) {
            if (seg_sjis[k] == '/' || seg_sjis[k] == '\\') return FS_INVALIDPATH;
        }

        if (windrv_sjis_to_utf8(seg_sjis, seglen, seg_utf8, sizeof(seg_utf8)) < 0) {
            return FS_INVALIDPATH;
        }
        {
            char tmp[WINDRV_PATH_BUF];
            n = snprintf(tmp, sizeof(tmp), "%s/%s", joined, seg_utf8);
            if (n < 0 || (size_t)n >= sizeof(tmp)) return FS_INVALIDPATH;   /* 切り詰め = 失敗 */
            memcpy(joined, tmp, (size_t)n + 1);
        }
    }

    if (realpath(joined, resolved) == NULL) return FS_DIRNOTFND;
    if (!windrv_path_inside_root(resolved)) return FS_INVALIDPATH;
    n = snprintf(out, outsz, "%s", resolved);
    if (n < 0 || (size_t)n >= outsz) return FS_INVALIDPATH;
    return 0;
}

/* ディレクトリ内から NAMESTS の name/ext/add に一致するホスト実ファイル名を探す。
 * 大文字小文字を区別せず、長いファイル名も Human68k 表現へ写像して照合する
 * (ホスト側の実名は out_host へそのまま返す)。 */
static uint32_t windrv_find_entry(const char* dirpath, uint32_t ns_addr,
                                  char* out_host, size_t outsz) {
    WindrvSearch q;
    DIR* d;
    struct dirent* de;
    int found = 0;
    int i;

    memset(&q, 0, sizeof(q));
    q.wildcard = 0;   /* 完全名照合(add まで見る) */
    for (i = 0; i < NAMESTS_NAME_LEN; i++) q.name[i] = (uint8_t)wd_rb(ns_addr + NAMESTS_OFF_NAME + i);
    for (i = 0; i < NAMESTS_EXT_LEN;  i++) q.ext[i]  = (uint8_t)wd_rb(ns_addr + NAMESTS_OFF_EXT  + i);
    for (i = 0; i < NAMESTS_ADD_LEN;  i++) q.add[i]  = (uint8_t)wd_rb(ns_addr + NAMESTS_OFF_ADD  + i);

    d = opendir(dirpath);
    if (!d) return FS_DIRNOTFND;
    while ((de = readdir(d)) != NULL) {
        WindrvH68Name h;
        if (de->d_name[0] == '.') continue;   /* '.' '..' と隠しファイルは公開しない */
        if (!windrv_host_to_h68(de->d_name, &h)) continue;
        if (!windrv_name_match(&q, &h)) continue;
        if ((size_t)snprintf(out_host, outsz, "%s", de->d_name) >= outsz) continue;
        found = 1;
        break;
    }
    closedir(d);
    return found ? 0u : FS_FILENOTFND;
}

/* ★NAMESTS → 検証済みホストパス。MVP 対象の全コマンドが使う唯一の入口。
 * want_file = 0 ならディレクトリのみ、1 ならファイル名まで解決する。 */
static uint32_t windrv_resolve_and_validate_path(uint32_t ns_addr, int want_file,
                                                 char* out, size_t outsz) {
    char dirpath[WINDRV_PATH_BUF];
    char hostname[NAME_MAX + 1];
    char joined[WINDRV_PATH_BUF];
    char resolved[WINDRV_PATH_BUF];
    uint32_t err;
    int n;

    err = windrv_resolve_dir(ns_addr, dirpath, sizeof(dirpath));
    if (err) return err;

    if (!want_file) {
        n = snprintf(out, outsz, "%s", dirpath);
        return (n < 0 || (size_t)n >= outsz) ? FS_INVALIDPATH : 0u;
    }

    err = windrv_find_entry(dirpath, ns_addr, hostname, sizeof(hostname));
    if (err) return err;

    n = snprintf(joined, sizeof(joined), "%s/%s", dirpath, hostname);
    if (n < 0 || (size_t)n >= sizeof(joined)) return FS_INVALIDPATH;

    /* ★第2層: 結合後の実体を realpath() で解決し、改めてルート配下かを判定する
     *   (dirpath 直下のシンボリックリンクがルート外を指す場合をここで捕捉)。 */
    if (realpath(joined, resolved) == NULL) return FS_FILENOTFND;
    if (!windrv_path_inside_root(resolved)) return FS_INVALIDPATH;

    n = snprintf(out, outsz, "%s", resolved);
    return (n < 0 || (size_t)n >= outsz) ? FS_INVALIDPATH : 0u;
}

/* ★P647: NAMESTS →「これから作る」対象のホスト絶対パス。
 * Create / MakeDir / Rename(新名)の唯一の入口。成功なら 0、失敗なら FS_*。
 *
 * ★既存の windrv_resolve_and_validate_path(want_file=1) との違い:
 *   あちらは windrv_find_entry() が readdir で**実在するエントリ**を探すため、
 *   まだ存在しない名前には使えない。こちらは親ディレクトリだけを既存の
 *   windrv_resolve_dir()(2 層のトラバーサル防御つき、対象の実在を要求しない)
 *   で解決し、そこへ windrv_h68_to_host() の結果を結合する。
 *
 * ★「親は検証済みだからファイル名の検証は省略してよい」は誤り —— ファイル名側に
 *   トラバーサル片(`..`, `/`)が混ざれば、検証済みの親からいくらでも脱出できる。 */
static uint32_t windrv_resolve_new_path(uint32_t ns_addr, char* out, size_t outsz) {
    char dirpath[WINDRV_PATH_BUF];
    char name[WINDRV_HOST_NAME_MAX + 1];
    char joined[WINDRV_PATH_BUF];
    char resolved[WINDRV_PATH_BUF];
    uint32_t err;
    int n;

    /* この時点で dirpath は realpath() 済み・ルート配下確定。 */
    err = windrv_resolve_dir(ns_addr, dirpath, sizeof(dirpath));
    if (err) return err;

    if (!windrv_h68_to_host(ns_addr, name, sizeof(name))) return FS_INVALIDPATH;

    /* ★結合直前の独立再確認。windrv_h68_to_host() 側で拒否済みの条件だが、
     *   将来あちらが変更されたときの安全網として、結合の直前でも不変条件を
     *   確認する(検証ロジックの一本化とは別に、縦深防御として妥当)。 */
    if (name[0] == '\0') return FS_INVALIDPATH;
    if (strchr(name, '/') != NULL) return FS_INVALIDPATH;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return FS_INVALIDPATH;

    n = snprintf(joined, sizeof(joined), "%s/%s", dirpath, name);
    if (n < 0 || (size_t)n >= sizeof(joined)) return FS_INVALIDPATH;

    /* ★既に何かが存在する場合の追加検証: realpath() で実体を解決し、
     *   「ルート外を指すシンボリックリンクが既に置かれている場所へ作成する」
     *   経路を塞ぐ。 */
    if (realpath(joined, resolved) != NULL) {
        if (!windrv_path_inside_root(resolved)) return FS_INVALIDPATH;
    } else if (errno != ENOENT) {
        /* ENOENT 以外(権限不足・ループ等)は曖昧な状態なので書込みへ進まない。 */
        return FS_INVALIDPATH;
    }

    /* ★out へ返すのは resolved ではなく joined(= 親の直下 1 要素)である。
     *   resolved を返すとシンボリックリンクが実体へ展開されてしまい、
     *   呼出し側の O_NOFOLLOW / mkdir の非追従性が「リンク自体を拒否する」
     *   という本来の意味を失う。
     *   joined を使って安全なのは、親が realpath 済みかつルート配下であることが
     *   確定しており、結合したファイル名側に区切り文字が一切含まれないことを
     *   2 回検証済みだから —— joined は定義により親の直下 1 要素であり、
     *   ルート配下から出られない。 */
    n = snprintf(out, outsz, "%s", joined);
    return (n < 0 || (size_t)n >= outsz) ? FS_INVALIDPATH : 0u;
}

/* P647: 書込み系操作の監査ログ。成功・失敗・ゲート拒否の 3 種すべてを同じ
 * プレフィクスで記録する —— 成功だけを記録する設計だと「0 行」の解釈が
 * (a) ゲストが書込みを要求しなかった /(b) 書込みトグル OFF で門前払い /
 * (c) 要求はあったが全部失敗 /(d) Windrv 未装着 の 4 通りに割れてしまう。
 * 各行に we=(g_windrv_write_wired の生値)と inst=(g_windrv_installed の生値)を
 * 必ず含めることで、0 行の解釈を「ゲストが書込み系コマンドを一度も送っていない」
 * の 1 通りに確定させる。
 * ★パスは相対部分のみを出し、ルート絶対パスは撒かない(プライバシー配慮)。
 *   rootlen を併記するので読み手は絶対パスの構造を復元できる。 */
static void windrv_wlog(const char* op, const char* abspath, uint32_t result) {
    const char* rel = "";
    size_t rootlen = strlen(s_root);

    if (abspath) {
        rel = abspath;
        if (rootlen > 0 && strncmp(abspath, s_root, rootlen) == 0) {
            rel = abspath + rootlen;
            if (*rel == '/') rel++;
        }
    }
    if (s_wlog_count < WINDRV_WLOG_CAP) {
        s_wlog_count++;
        debug_log("[P647-WINDRV-W] op=%s cmd=0x%02x we=%d inst=%d rootlen=%zu rel=\"%s\" result=0x%08x\n",
                  op, (unsigned)s_command, g_windrv_write_wired, g_windrv_installed,
                  rootlen, rel, (unsigned)result);
        if (s_wlog_count == WINDRV_WLOG_CAP) {
            debug_log("[P647-WINDRV-W] further write audit lines suppressed (>%d)\n",
                      WINDRV_WLOG_CAP);
        }
    }
}

/* ------------------------------------------------------------------------
 * ファイルハンドルテーブル / 検索コンテキスト
 * ------------------------------------------------------------------------ */

static WindrvFile* windrv_file_find(uint32_t fcb) {
    int i;
    if (fcb == 0) return NULL;
    for (i = 0; i < WINDRV_MAX_FILES; i++) {
        if (s_files[i].fcb == fcb) return &s_files[i];
    }
    return NULL;
}

static WindrvFile* windrv_file_alloc(void) {
    int i;
    for (i = 0; i < WINDRV_MAX_FILES; i++) {
        if (s_files[i].fcb == 0) return &s_files[i];
    }
    return NULL;   /* 満杯 —— 呼出し側は fopen() を呼ばずに FS_OVEROPENED を返すこと */
}

static void windrv_file_close(WindrvFile* f) {
    if (!f) return;
    if (f->fp) fclose(f->fp);
    f->fp  = NULL;
    f->fcb = 0;
    f->path[0] = '\0';   /* P647: 監査ログ用パスも一緒にクリアする */
}

static void windrv_files_close_all(void) {
    int i;
    for (i = 0; i < WINDRV_MAX_FILES; i++) windrv_file_close(&s_files[i]);
}

static WindrvSearch* windrv_search_find(uint32_t files_addr) {
    int i;
    if (files_addr == 0) return NULL;
    for (i = 0; i < WINDRV_MAX_SEARCH; i++) {
        if (s_search[i].files_addr == files_addr) return &s_search[i];
    }
    return NULL;
}

static void windrv_search_free(WindrvSearch* s) {
    if (!s) return;
    if (s->dir) closedir(s->dir);
    s->dir = NULL;
    s->files_addr = 0;
    /* P650 変更4: 解放済みスロットの残留エントリを $53 が拾わないよう明示クリア。 */
    s->have_entry = 0;
}

/* 未使用スロット優先、無ければ最も古いものを閉じて再利用する
 * (ゲストが検索ループを途中で放棄しても DIR* が漏れない)。 */
static WindrvSearch* windrv_search_alloc(uint32_t files_addr) {
    int i, oldest = 0;
    WindrvSearch* s = windrv_search_find(files_addr);
    if (s) { windrv_search_free(s); }
    else {
        for (i = 0; i < WINDRV_MAX_SEARCH; i++) {
            if (s_search[i].files_addr == 0) { s = &s_search[i]; break; }
            if (s_search[i].serial < s_search[oldest].serial) oldest = i;
        }
        if (!s) { s = &s_search[oldest]; windrv_search_free(s); }
    }
    memset(s, 0, sizeof(*s));
    s->files_addr = files_addr;
    s->serial     = ++s_search_serial;
    return s;
}

static void windrv_search_free_all(void) {
    int i;
    for (i = 0; i < WINDRV_MAX_SEARCH; i++) windrv_search_free(&s_search[i]);
}

/* 検索結果 1 件をゲストの files_t へ書き戻す(windrv.cpp:2634-2680 SetFiles 準拠)。 */
static void windrv_write_files_entry(uint32_t files_addr, const WindrvH68Name* h,
                                     const struct stat* st) {
    uint16_t tm, dt;
    int i;
    windrv_time_date(st, &tm, &dt);
    wd_wb(files_addr + FILES_OFF_ATTR, windrv_attr_of(st));
    wd_ww(files_addr + FILES_OFF_TIME, tm);
    wd_ww(files_addr + FILES_OFF_DATE, dt);
    wd_wl(files_addr + FILES_OFF_SIZE, S_ISDIR(st->st_mode) ? 0u : (uint32_t)st->st_size);
    for (i = 0; i < FILES_FULL_LEN; i++) {
        wd_wb(files_addr + FILES_OFF_FULL + i, (uint8_t)h->full[i]);
    }
}

/* 検索コンテキストから次の一致エントリを返す。0 = 見つかった / FS_LASTFILE = 終端。 */
static uint32_t windrv_search_next(WindrvSearch* s) {
    struct dirent* de;
    if (!s->dir) return FS_LASTFILE;
    while ((de = readdir(s->dir)) != NULL) {
        WindrvH68Name h;
        char full[WINDRV_PATH_BUF];
        struct stat st;

        if (de->d_name[0] == '.') continue;
        if (!windrv_host_to_h68(de->d_name, &h)) continue;
        if (!windrv_name_match(s, &h)) continue;
        if ((size_t)snprintf(full, sizeof(full), "%s/%s", s->dirpath, de->d_name) >= sizeof(full)) continue;
        if (stat(full, &st) != 0) continue;
        /* 検索属性フィルタ: ディレクトリは fatr の AT_DIRECTORY 要求時のみ返す。
         * 通常ファイルは Human68k の慣例どおり常に返す。 */
        if (S_ISDIR(st.st_mode) && !(s->fatr & AT_DIRECTORY)) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;

        windrv_write_files_entry(s->files_addr, &h, &st);
        {   /* P650 変更4: XM6 の CHostFiles::GetEntry() 相当の控え。 */
            uint16_t tm, dt;
            windrv_time_date(&st, &tm, &dt);
            s->ent       = h;
            s->ent_attr  = windrv_attr_of(&st);
            s->ent_time  = tm;
            s->ent_date  = dt;
            s->ent_size  = S_ISDIR(st.st_mode) ? 0u : (uint32_t)st.st_size;
            s->have_entry = 1;
        }
        return 0;
    }
    return FS_LASTFILE;
}

/* ------------------------------------------------------------------------
 * MVP コマンド実装
 * ------------------------------------------------------------------------ */

/* $40 InitDrive — windrv.cpp:1093-1131。 */
static void windrv_cmd_init_drive(void) {
    uint32_t base = wd_rb(s_a5 + CMD_OFF_ADDR3);
    char resolved[WINDRV_HOST_PATH_MAX];

    /* ★Code Review 指摘1・手順1: マウントルートをここで realpath() 正規化する
     * (windrv_init() でも実施済みだが、設定変更後にゲストが再初期化する経路も
     *  同じ検証を通す。以後の全パス解決はこの s_root 起点で行われる)。 */
    s_root[0] = '\0';
    s_root_slash[0] = '\0';
    if (g_windrv_host_path[0] != '\0' &&
        realpath(g_windrv_host_path, resolved) != NULL) {
        size_t l = strlen(resolved);
        if (l > 0 && l < sizeof(s_root)) {
            memcpy(s_root, resolved, l + 1);
            snprintf(s_root_slash, sizeof(s_root_slash), "%s%s",
                     s_root, (s_root[l - 1] == '/') ? "" : "/");
        }
    }

    if (s_root[0] == '\0') {
        s_unit_max = 0;
        wd_wb(s_a5 + CMD_OFF_ATTR, 0);
        windrv_set_result(FS_FATAL_MEDIAOFFLINE);
        return;
    }

    /* MX は 1 フォルダ = 1 ドライブ。ベースドライブ以降に空きが無ければ 0 台。 */
    s_unit_max = (base < 26) ? 1u : 0u;
    wd_wb(s_a5 + CMD_OFF_ATTR, s_unit_max);
    windrv_set_result(0);
}

/* $41 CheckDir — windrv.cpp:1147-1180(GetNameStsPath = パスのみ使用)。 */
static void windrv_cmd_check_dir(void) {
    uint32_t ns = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    char dirpath[WINDRV_PATH_BUF];
    struct stat st;
    uint32_t err = windrv_resolve_and_validate_path(ns, /*want_file=*/0,
                                                    dirpath, sizeof(dirpath));
    if (err) { windrv_set_result(err); return; }
    if (stat(dirpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        windrv_set_result(FS_DIRNOTFND);
        return;
    }
    windrv_set_result(0);
}

/* P650 変更1(B): $47 の AT_VOLUME 検索 —— XM6fs:mfc_host.cpp:4776-4805 FilesVolume 相当。
 * ★ホスト(macOS)には Win32 GetVolumeInformation 相当の概念が無いため、
 *   XM6 の「ホストの実ボリューム名を動的取得」を固定文字列で代替する。
 * ★定義位置: 本ファイルには static 関数の前方宣言が一切なく、すべて
 *   「使用箇所より前で定義」の規約のため windrv_cmd_files() より前に置く。 */
static void windrv_cmd_files_volume(uint32_t nfiles) {
    static const char label[] = "macOS";
    int i;

    /* XM6fs:4793 szVolume[0]=='\0' → FALSE → FS_FILENOTFND 相当。
     * MX では共有フォルダ未設定を同じ扱いにする。 */
    if (s_root[0] == '\0') { windrv_set_result(FS_FILENOTFND); return; }

    wd_wb(nfiles + FILES_OFF_ATTR, AT_VOLUME);   /* XM6fs:4795 */
    wd_ww(nfiles + FILES_OFF_TIME, 0);           /* XM6fs:4796 */
    wd_ww(nfiles + FILES_OFF_DATE, 0);           /* XM6fs:4797 */
    wd_wl(nfiles + FILES_OFF_SIZE, 0);           /* XM6fs:4798 */
    /* ★windrv_host_to_h68() を通してはならない —— MX 独自の大文字化により
     *   "macOS" → "MACOS" に化ける。固定 ASCII ≤18 バイトなので直書きで XM6 と同結果。
     * ★FILES_OFF_SECTOR / FILES_OFF_OFFSET は触らない —— XM6 の FilesVolume は
     *   両者を設定しない。後続 $48 は呼出し側の明示解放により
     *   windrv_search_find() が NULL → FS_LASTFILE で自然に終端する。 */
    for (i = 0; i < FILES_FULL_LEN; i++)         /* XM6fs:4803 strcpy 相当・残りは 0 埋め */
        wd_wb(nfiles + FILES_OFF_FULL + i,
              (i < (int)sizeof(label) - 1) ? (uint8_t)label[i] : 0u);
    windrv_set_result(0);
}

/* $47 Files(findfirst)— windrv.cpp:1418-1452。 */
static void windrv_cmd_files(void) {
    uint32_t nfiles = wd_raddr(s_a5 + CMD_OFF_ADDR2);
    uint32_t ns     = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    char dirpath[WINDRV_PATH_BUF];
    WindrvSearch* s;
    uint32_t err;
    int i;

    if (nfiles == 0) { windrv_set_result(FS_INVALIDPRM); return; }

    /* P650 変更1: XM6fs:3691-3695 と同順 —— 同一キーの古い検索文脈を先に解放する。
     * windrv_search_alloc() も内部で同一キー解放を行うが(:961-962)、
     * その時点で当該スロットは既に解放済みなので二重解放にはならない。 */
    { WindrvSearch* old = windrv_search_find(nfiles); if (old) windrv_search_free(old); }

    /* XM6fs:3698 —— ★ビット and ではなく完全一致。`&` にすると
     *   fatr=0x28 等の通常検索がラベル検索に化け、ファイル一覧が1件も出なくなる。
     * ★パス解決より前に置く(XM6 はこの経路でパスを一切見ない)。 */
    if ((uint8_t)wd_rb(nfiles + FILES_OFF_FATR) == AT_VOLUME) {
        windrv_cmd_files_volume(nfiles);
        return;                       /* バッファ確保もパス解決もしない(XM6fs:3699-3702) */
    }

    err = windrv_resolve_and_validate_path(ns, /*want_file=*/0, dirpath, sizeof(dirpath));
    if (err) { windrv_set_result(err); return; }

    s = windrv_search_alloc(nfiles);
    s->fatr     = (uint8_t)wd_rb(nfiles + FILES_OFF_FATR);
    s->wildcard = (uint8_t)wd_rb(ns + NAMESTS_OFF_WILDCARD);
    for (i = 0; i < NAMESTS_NAME_LEN; i++) s->name[i] = (uint8_t)wd_rb(ns + NAMESTS_OFF_NAME + i);
    for (i = 0; i < NAMESTS_EXT_LEN;  i++) s->ext[i]  = (uint8_t)wd_rb(ns + NAMESTS_OFF_EXT  + i);
    for (i = 0; i < NAMESTS_ADD_LEN;  i++) s->add[i]  = (uint8_t)wd_rb(ns + NAMESTS_OFF_ADD  + i);
    snprintf(s->dirpath, sizeof(s->dirpath), "%s", dirpath);

    s->dir = opendir(dirpath);
    if (!s->dir) { windrv_search_free(s); windrv_set_result(FS_DIRNOTFND); return; }

    err = windrv_search_next(s);
    if (err) {
        /* windrv.cpp:1398-1403 の規約: 検索失敗時は offset に -1 を書いて
         * 次回検索を必ず失敗させる。 */
        wd_ww(nfiles + FILES_OFF_OFFSET, 0xFFFFu);
        windrv_search_free(s);
        windrv_set_result(FS_FILENOTFND);
        return;
    }

    /* sector は「検索コンテキストの不透明ハンドル」。XM6 は DOS _FILES の
     * 先頭アドレスで代用しており(windrv.h:126)、MX も同じ値を書く。 */
    wd_wl(nfiles + FILES_OFF_SECTOR, nfiles);
    wd_ww(nfiles + FILES_OFF_OFFSET, 0);
    /* 非ワイルドカード検索は 1 件で完結するため offset に -1 を書いて $48 を
     * 終端させるが、★P650 変更2: 検索文脈自体は解放しない —— XM6fs:3747-3751 が
     * 「仮想セクタのエミュレーションで使う可能性があるため、すぐには開放しない」
     * と明記しており、直後の $53 DiskRead がこのハンドルを引く。 */
    if (s->wildcard == 0) {
        wd_ww(nfiles + FILES_OFF_OFFSET, 0xFFFFu);
    }
    windrv_set_result(0);
}

/* $48 NFiles(findnext)— windrv.cpp:1468-1502。 */
static void windrv_cmd_nfiles(void) {
    uint32_t nfiles = wd_raddr(s_a5 + CMD_OFF_ADDR2);
    WindrvSearch* s;
    uint32_t err;

    if (nfiles == 0) { windrv_set_result(FS_INVALIDPRM); return; }
    if (wd_rw(nfiles + FILES_OFF_OFFSET) == 0xFFFFu) { windrv_set_result(FS_LASTFILE); return; }

    s = windrv_search_find(nfiles);
    if (!s) { windrv_set_result(FS_LASTFILE); return; }

    err = windrv_search_next(s);
    if (err) {
        wd_ww(nfiles + FILES_OFF_OFFSET, 0xFFFFu);
        windrv_search_free(s);
        windrv_set_result(FS_LASTFILE);
        return;
    }
    /* ★P650 変更3: offset のインクリメントは廃止した —— XM6 の NFiles は offset を
     * 一切触らない(XM6fs:3782-3789 は attr/date/time/size/full のみ更新)。
     * 番兵判定(:1180 の == 0xFFFF)は終端時の明示書込み(:1187)で成立し続けるため、
     * $48 の終端動作は変わらない。 */
    windrv_set_result(0);
}

/* $4A Open — windrv.cpp:1576。
 * ★P642 の読み取り専用強制を P647 で条件化した。
 *   書込み許可が未配線(既定)なら FCB.mode の値に関わらず**常に "rb"** で開く
 *   —— P642/P643 と完全に同一のコードパスを通る。書込み許可が配線済みで、かつ
 *   ゲストが書込みモード(OP_WRITE=1 / OP_READWRITE=2、windrv.h:95-97)を
 *   要求したときだけ読み書き可能なストリームを開く。 */
static void windrv_cmd_open(void) {
    uint32_t ns   = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    uint32_t nfcb = wd_raddr(s_a5 + CMD_OFF_ADDR3);
    char path[WINDRV_PATH_BUF];
    struct stat st;
    WindrvFile* f;
    uint32_t err;
    uint16_t tm, dt;

    if (nfcb == 0) { windrv_set_result(FS_INVALIDPRM); return; }

    /* 同じ FCB での再オープンは、旧ストリームを閉じてから差し替える。 */
    f = windrv_file_find(nfcb);
    if (f) windrv_file_close(f);

    /* ★Code Review 指摘4への対応: 満杯なら fopen() を呼ばずに即 FS_OVEROPENED。
     *   ホスト側ファイルディスクリプタを 1 つも消費しない。 */
    f = windrv_file_alloc();
    if (!f) { windrv_set_result(FS_OVEROPENED); return; }

    err = windrv_resolve_and_validate_path(ns, /*want_file=*/1, path, sizeof(path));
    if (err) { windrv_set_result(err); return; }

    if (stat(path, &st) != 0) { windrv_set_result(FS_FILENOTFND); return; }
    if (S_ISDIR(st.st_mode))  { windrv_set_result(FS_CANTACCESS); return; }
    if (!S_ISREG(st.st_mode)) { windrv_set_result(FS_CANTACCESS); return; }

    /* ★P647: 書込み許可が配線済みで、かつゲストが書込みモードを要求している
     *   ときだけ読み書き可能なストリームを開く。
     *
     * ★存在しないファイルを Open で暗黙作成することは**しない**。Human68k / DOS の
     *   意味論では新規ファイルの入口は $49 Create(_NEWFILE / _CREATE)であり、
     *   _OPEN は既存ファイル専用で、存在しなければエラーを返す。参照実装も
     *   Create と Open を別コマンドとして分離している(windrv.cpp:1516 / :1576)。
     *   暗黙作成を足すと「mode の値だけでホストにファイルが生える」という
     *   Create を経由しない経路が増え、監査面でも不利になる。
     *
     * ★書込み許可が OFF のときに mode!=OP_READ で開かれた場合の挙動は
     *   P642/P643 から**変更しない**(read-only で開いて成功を返し、実際の
     *   書込みは $4D 側が FS_CANTWRITE で拒否する)。ここで FS_CANTWRITE を
     *   返すよう変えると、hands-on 確認済みの読み取り専用動作を退行させる恐れがある。 */
    {
        uint32_t mode = wd_rw(nfcb + FCB_OFF_MODE);   /* windrv.h:95-97 OP_READ=0/OP_WRITE=1/OP_READWRITE=2 */
        int want_write = (g_windrv_write_wired && mode != 0);

        if (mode != 0 && s_log_count < WINDRV_LOG_CAP) {
            debug_log("[P642-WINDRV] open: guest requested mode=%u — %s\n",
                      (unsigned)mode,
                      want_write ? "opened read/write (P647)" : "forced read-only");
        }

        if (want_write) {
            /* ★O_NOFOLLOW: 最終要素がシンボリックリンクなら ELOOP で失敗させ、
             *   リンク判定と open を原子的に行う(TOCTOU の窓を作らない)。 */
            int fd = open(path, O_RDWR | O_NOFOLLOW);
            if (fd < 0) {
                uint32_t e;
                switch (errno) {
                case EACCES: case EPERM: case EROFS: e = FS_CANTWRITE;  break;
                case ENOENT:                         e = FS_FILENOTFND; break;
                default:                             e = FS_CANTACCESS; break;   /* ELOOP を含む */
                }
                windrv_wlog("open_rw", path, e);
                windrv_set_result(e);
                return;
            }
            f->fp = fdopen(fd, "r+b");
            if (!f->fp) {
                close(fd);   /* ★fd を漏らさない */
                windrv_wlog("open_rw", path, FS_CANTACCESS);
                windrv_set_result(FS_CANTACCESS);
                return;
            }
            windrv_wlog("open_rw", path, 0);
        } else {
            f->fp = fopen(path, "rb");   /* 既存経路(P642/P643)を一切変更しない */
            if (!f->fp) { windrv_set_result(FS_FILENOTFND); return; }
        }
    }
    f->fcb = nfcb;
    /* P647: $4D Write は FCB キーでしか対象を知らないため、監査ログ用に
     * ホスト絶対パスをここで保存しておく。 */
    snprintf(f->path, sizeof(f->path), "%s", path);

    /* SetFcb(windrv.cpp:2812-2818)が書くのは attr/time/date/size の 4 つのみ。
     * fileptr(+6)はゲストが保持するため書き戻さない。
     * ★P647: attr はハードコードせず windrv_attr_of() を通す —— 書込み許可時に
     *   AT_READONLY を立てたままだとゲスト側 DOS が上書き・削除を自ら拒否する。
     *   同じ判定を 2 箇所へ書き写さない(既存ファイルの規律)。 */
    windrv_time_date(&st, &tm, &dt);
    wd_wb(nfcb + FCB_OFF_ATTR, windrv_attr_of(&st));
    wd_ww(nfcb + FCB_OFF_TIME, tm);
    wd_ww(nfcb + FCB_OFF_DATE, dt);
    wd_wl(nfcb + FCB_OFF_SIZE, (uint32_t)st.st_size);
    windrv_set_result(0);
}

/* $4B Close — windrv.cpp:1625-1655。 */
static void windrv_cmd_close(void) {
    uint32_t nfcb = wd_raddr(s_a5 + CMD_OFF_ADDR3);
    WindrvFile* f = windrv_file_find(nfcb);
    /* 未オープンの FCB へのクローズはエラーにしない(ゲスト側の後始末で
     * 起こりうる無害な呼出し。エラーを返すと正常終了処理が失敗扱いになる)。 */
    if (f) windrv_file_close(f);
    windrv_set_result(0);
}

/* $4C Read — windrv.cpp:1666-1708。
 * ★Code Review 指摘2への対応(サイズの符号処理):
 *   a5+18 のサイズは **符号付き 32bit** として読む。負数は「ファイル全体」の意味
 *   (windrv.cpp:1690 のコメント)であり、これを符号なしへキャストして
 *   巨大サイズとして扱うと範囲外読み取りになる —— その回避が本対応の主眼。
 *   負数のときは fstat() で得た実サイズ(- 現在位置)を使う。 */
static void windrv_cmd_read(void) {
    uint32_t nfcb    = wd_raddr(s_a5 + CMD_OFF_ADDR3);
    uint32_t dst     = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    int32_t  req     = (int32_t)wd_rl(s_a5 + CMD_OFF_ADDR2);
    WindrvFile* f    = windrv_file_find(nfcb);
    uint32_t fileptr;
    struct stat st;
    int64_t  remain;
    uint32_t total = 0;
    static uint8_t buf[WINDRV_READ_CHUNK];   /* ★巨大なスタック確保をしない */

    if (!f || !f->fp) { windrv_set_result(FS_NOTOPENED); return; }

    if (fstat(fileno(f->fp), &st) != 0) { windrv_set_result(FS_NOTOPENED); return; }

    /* ファイルポインタはゲスト側 FCB が保持する(SetFcb は +6 を書かない)。
     * 毎回そこへ seek してから読むので、ゲスト側の位置管理と必ず一致する。 */
    fileptr = wd_rl(nfcb + FCB_OFF_FILEPTR);
    if ((int64_t)fileptr > (int64_t)st.st_size) fileptr = (uint32_t)st.st_size;
    if (fseeko(f->fp, (off_t)fileptr, SEEK_SET) != 0) { windrv_set_result(FS_NOTOPENED); return; }

    if (req < 0) {
        /* 負数 = ファイル全体。現在位置からの残りを読む。 */
        remain = (int64_t)st.st_size - (int64_t)fileptr;
    } else {
        remain = (int64_t)req;
        if (remain > (int64_t)st.st_size - (int64_t)fileptr) {
            remain = (int64_t)st.st_size - (int64_t)fileptr;
        }
    }
    if (remain < 0) remain = 0;

    /* チャンク分割で読み、1 バイトずつゲストメモリへ書き戻す。 */
    while (remain > 0) {
        size_t want = (remain > (int64_t)sizeof(buf)) ? sizeof(buf) : (size_t)remain;
        size_t got  = fread(buf, 1, want, f->fp);
        size_t k;
        if (got == 0) break;
        for (k = 0; k < got; k++) wd_wb(dst + total + (uint32_t)k, buf[k]);
        total  += (uint32_t)got;
        remain -= (int64_t)got;
        if (got < want) break;
    }

    wd_wl(nfcb + FCB_OFF_FILEPTR, fileptr + total);

    windrv_set_result(total);
}

/* ------------------------------------------------------------------------
 * P647: 書込み系コマンド
 *
 * ★全コマンド共通の実装規約(順序を守ること):
 *   1. `g_windrv_write_wired` のゲートを**各ハンドラの最初の実行文**に置く。
 *      このゲートは共有トグルとは独立で、g_windrv_write_wired は windrv_init()
 *      で `g_windrv_installed && g_windrv_write_enabled` としてのみ立つ
 *      —— 書込みトグルだけを ON にしても共有が無効なら何も起きない。
 *   2. `a5+18`(CMD_OFF_ADDR2)は入力パラメータであると同時に
 *      **windrv_set_result() の結果格納先**でもある。a5+18 から読む値は
 *      必ず全部読み終えてから windrv_set_result() を呼ぶこと
 *      (該当: $49 Create の force / $4D Write のサイズ / $44 Rename の新 NAMESTS)。
 *   3. ファイルシステムを変更する syscall は必ず「チェックと操作が単一
 *      システムコール内で完結する」フラグ組合せを使う(check-then-act 二段構えを
 *      作らない)。共有フォルダは利用者の Finder / 同期エージェント /
 *      バックアップツールが同時に触りうる真に多プロセスな場所であり、
 *      「ゲストが単一スレッドだから TOCTOU は無視してよい」は成立しない。
 *
 * ★残余リスク(意図的に受容): 親ディレクトリは realpath() 済みだが、その後
 *   実操作を行うまでの間に親ディレクトリ自体がすり替わる窓は残る。これを消すには
 *   openat による 1 要素ずつのディレクトリ FD 追跡が必要で、windrv_resolve_dir()
 *   の構造を全面的に書き換えることになる。費用対効果が見合わないため受け入れる。
 * ------------------------------------------------------------------------ */

/* $49 Create — windrv.cpp:1516。
 * in: a5+13 (1B) 属性(:1536) / a5+14 (1L) NAMESTS(:1528) /
 *     a5+18 (1L) モード(0=_NEWFILE 上書き不可, 非0=_CREATE 上書き可、:1539) /
 *     a5+22 (1L) FCB(:1531)。結果 >=0 で SetFcb(:1551-1553)。 */
static void windrv_cmd_create(void) {
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    /* ★実装規約 2: a5+18(force)を含む in パラメータを先に全部読む。 */
    uint8_t  attr_req = (uint8_t)wd_rb(s_a5 + CMD_OFF_ATTR);
    uint32_t ns       = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    uint32_t force    = wd_rl(s_a5 + CMD_OFF_ADDR2);
    uint32_t nfcb     = wd_raddr(s_a5 + CMD_OFF_ADDR3);
    char path[WINDRV_PATH_BUF];
    WindrvFile* f;
    struct stat st;
    uint16_t tm, dt;
    uint32_t err;
    int fd, flags;

    /* ★ゲスト要求の属性は採用しない(理由は下の FCB 書き戻しコメント)。
     *   プロトコル上の位置を記録するために読むだけ。 */
    (void)attr_req;

    if (nfcb == 0) { windrv_set_result(FS_INVALIDPRM); return; }

    /* 同じ FCB での再作成は、旧ストリームを閉じてから差し替える($4A Open と同型)。 */
    f = windrv_file_find(nfcb);
    if (f) windrv_file_close(f);

    /* ★ハンドル満杯なら、ホスト側 fd を一切消費する前に判定する
     *   (既存 $4A Open と同一規律)。 */
    f = windrv_file_alloc();
    if (!f) { windrv_set_result(FS_OVEROPENED); return; }

    err = windrv_resolve_new_path(ns, path, sizeof(path));
    if (err) { windrv_wlog("create", NULL, err); windrv_set_result(err); return; }

    /* ★原子的な作成(TOCTOU 除去): stat で存在確認してから fopen する二段構えは
     *   使わない。存在確認・シンボリックリンク拒否・作成を open(2) のフラグで
     *   単一システムコール内に閉じ込める。
     *     - force==0 かつ既存      → O_EXCL が EEXIST(チェックと作成の間に
     *                                 他プロセスが割り込む隙間そのものが無い)
     *     - 最終要素がリンク       → O_NOFOLLOW が ELOOP(realpath 検証とは
     *                                 独立にルート外への迂回を阻止する)
     *     - 対象がディレクトリ     → O_RDWR が EISDIR
     *
     * ★force はゲストの要求どおり尊重し、MX 側で追加の上書き拒否は設けない。
     *   理由: (a) 書込みトグルの ON 自体が利用者の認可境界であり、そこで一度
     *   承認された以上、個別操作ごとに二重の拒否を挟むのは筋が悪い。(b) 上書き
     *   拒否は自明に迂回可能($45 Delete → $49 Create(force=0))であり、
     *   安全性を生まずに正当なゲストソフト(コンパイラ・アーカイバ・エディタは
     *   いずれも _CREATE 意味論に依存する)だけを壊す。実効的な防御は上記の
     *   EISDIR / ELOOP と、全書込み操作の監査ログ([P647-WINDRV-W])である。
     *
     * ★別 FCB で開いたままのファイルへの上書き Create(force=1)は、POSIX の
     *   truncate-while-open 意味論(他のディスクリプタから見えているデータが
     *   その場で切り詰められる)にそのまま従う —— 追加のガードは**意図的に
     *   設けない**。Fix Plan §S-3 の informed consent の受容範囲であり、
     *   閉じるべき穴ではない(影響は利用者が許可した共有フォルダ内に限定される)。 */
    flags = O_RDWR | O_CREAT | O_NOFOLLOW | (force ? O_TRUNC : O_EXCL);
    fd = open(path, flags, 0644);
    if (fd < 0) {
        uint32_t e;
        switch (errno) {
        case EEXIST:                         e = FS_FILEEXIST;  break;
        case ENOSPC: case EDQUOT:            e = FS_DISKFULL;   break;
        case EACCES: case EPERM: case EROFS: e = FS_CANTWRITE;  break;
        default:                             e = FS_CANTACCESS; break;   /* ELOOP / EISDIR を含む */
        }
        windrv_wlog("create", path, e);
        windrv_set_result(e);
        return;
    }
    f->fp = fdopen(fd, "r+b");
    if (!f->fp) {
        close(fd);   /* ★fd を漏らさない */
        windrv_wlog("create", path, FS_CANTACCESS);
        windrv_set_result(FS_CANTACCESS);
        return;
    }
    f->fcb = nfcb;
    snprintf(f->path, sizeof(f->path), "%s", path);

    /* ★FCB 書き戻し(windrv.cpp:2788-2818 SetFcb のフィールド集合に厳密に従う)。
     *   fileptr(+6)は SetFcb が**無条件に**書くフィールド(windrv.cpp:2788)で
     *   あり、Create 直後のファイルポインタは 0。
     *   **これを書き忘れると P643 と同型の不具合になる** —— P643 は $4C Read で
     *   まったく同じ書き戻しが漏れて type が無限ループした。 */
    wd_wl(nfcb + FCB_OFF_FILEPTR, 0);
    wd_wl(nfcb + FCB_OFF_SIZE,    0);
    /* ★attr はゲスト要求値を採用せず AT_ARCHIVE 固定。
     *   (a) POSIX には DOS 属性(HIDDEN/SYSTEM)を保持する場所が無く、嘘の値を
     *       書き戻すと以後の dir 表示が実体と食い違う。
     *   (b) AT_READONLY を立てない —— 作った直後のファイルが読取専用に見えると、
     *       続く $4D Write をゲスト側 DOS が自ら拒否しうる。 */
    wd_wb(nfcb + FCB_OFF_ATTR, AT_ARCHIVE);
    if (fstat(fileno(f->fp), &st) == 0) {
        windrv_time_date(&st, &tm, &dt);
        wd_ww(nfcb + FCB_OFF_TIME, tm);
        wd_ww(nfcb + FCB_OFF_DATE, dt);
    }
    /* ★mode(+14)は書き換えない —— ゲストが設定した値をそのまま尊重する。
     *   XM6 の SetFcb(windrv.cpp:2789)は書くが、それは GetFcb で読んだ同じ値を
     *   書き戻しているだけなので、書かないことと等価。 */

    windrv_wlog("create", path, 0);
    windrv_set_result(0);
}

/* $4D Write — windrv.cpp:1718。既存 windrv_cmd_read() と対称形。
 * in: a5+14 (1L) 書込元バッファ(:1733) / a5+18 (1L) サイズ(:1736) /
 *     a5+22 (1L) FCB(:1728)。結果 >=0 で SetFcb(:1751-1753)。 */
static void windrv_cmd_write(void) {
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    /* ★実装規約 2: a5+18(サイズ)を含む in パラメータを先に全部読む。 */
    uint32_t src  = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    int32_t  req  = (int32_t)wd_rl(s_a5 + CMD_OFF_ADDR2);
    uint32_t nfcb = wd_raddr(s_a5 + CMD_OFF_ADDR3);
    WindrvFile* f = windrv_file_find(nfcb);
    uint32_t fileptr;
    uint32_t total = 0;
    uint32_t fail  = 0;
    struct stat st;
    uint16_t tm, dt;
    static uint8_t wbuf[WINDRV_WRITE_CHUNK];   /* ★巨大なスタック確保をしない */

    if (!f || !f->fp) { windrv_set_result(FS_NOTOPENED); return; }

    fileptr = wd_rl(nfcb + FCB_OFF_FILEPTR);

    /* ★サイズの解釈は MX 独自設計 —— 参照実装の FileSys 側実装は非公開であり、
     *   ここは [reference-implementation behaviour] で裏取りできていない。 */
    if (req < 0) {
        /* XM6 のコメントは Read/Write 共通の文言として「サイズ 負の数なら
         * ファイルサイズを指定したのと同じ」と書く(windrv.cpp:1714)が、
         * **書込みにおいて「ファイルサイズ分を書く」は元バッファ長が定まらず
         * 意味を成さない**。曖昧な値のまま巨大な書込みへ進むより明示的に拒否する。
         * (Read 側の負数処理は従来どおり一切変更しない。) */
        windrv_wlog("write", f->path, FS_INVALIDPRM);
        windrv_set_result(FS_INVALIDPRM);
        return;
    }
    if (req > WINDRV_WRITE_MAX) {
        windrv_wlog("write", f->path, FS_INVALIDPRM);
        windrv_set_result(FS_INVALIDPRM);
        return;
    }

    if (req == 0) {
        /* ★サイズ 0 = 現在のファイルポインタ位置で切り詰める。
         *   根拠種別は [domain convention](MS-DOS INT 21h AH=40h の CX=0 と同じ
         *   慣行)であり **[reference-implementation behaviour] ではない**
         *   —— XM6 の FileSys 側実装は非公開で裏取りできていない未検証の解釈で、
         *   hands-on 確認で初めて検証される項目である。
         * ★ftruncate は fd を直接操作するため、先に fflush して stdio の
         *   バッファを吐き出しておく —— さもないと未書出しのバッファが後から
         *   切り詰め位置より後ろへ書き戻され、切り詰めが取り消される。 */
        fflush(f->fp);
        if (ftruncate(fileno(f->fp), (off_t)fileptr) != 0) {
            uint32_t e = (errno == ENOSPC || errno == EDQUOT) ? FS_DISKFULL : FS_CANTWRITE;
            windrv_wlog("truncate", f->path, e);
            windrv_set_result(e);
            return;
        }
    } else {
        int64_t remain;

        /* ★穴あき書込みの許容: fileptr が現ファイルサイズを超えていてもクランプ
         *   しない(Read 側はクランプするが、書込みでは POSIX の sparse file
         *   意味論をそのまま採る = 間はゼロ埋めされる)。Human68k 側の
         *   _SEEK 後書込みと整合する。 */
        if (fseeko(f->fp, (off_t)fileptr, SEEK_SET) != 0) {
            windrv_set_result(FS_NOTOPENED);
            return;
        }

        remain = (int64_t)req;
        while (remain > 0) {
            size_t want = (remain > (int64_t)sizeof(wbuf)) ? sizeof(wbuf) : (size_t)remain;
            size_t k, put;
            for (k = 0; k < want; k++) wbuf[k] = (uint8_t)wd_rb(src + total + (uint32_t)k);
            errno = 0;
            put = fwrite(wbuf, 1, want, f->fp);
            total  += (uint32_t)put;
            remain -= (int64_t)put;
            if (put < want) {
                fail = (errno == ENOSPC || errno == EDQUOT) ? FS_DISKFULL : FS_CANTWRITE;
                break;
            }
        }

        /* ★部分成功の扱い: XM6 は nResult >= 0 で SetFcb を呼ぶ設計なので、
         *   書けたバイト数 total を結果として返し FCB もその分だけ前進させる
         *   —— DOS の _WRITE 意味論(短い書込みはエラーではなく実書込み数の返却)と
         *   一致する。1 バイトも書けなかったときのみエラーコードを返す。 */
        if (total == 0 && fail) {
            windrv_wlog("write", f->path, fail);
            windrv_set_result(fail);
            return;
        }
    }

    /* ゲストが $56 Flush を呼ばなくてもホスト側 Finder に即座に見えるようにする。
     * 1 コマンド完結型の同期実行なのでコストは許容する。
     * ★下の fstat が実サイズを見るためにも、flush が先でなければならない。 */
    fflush(f->fp);

    /* ★FCB 書き戻し(windrv.cpp:2788-2818)。fileptr(+6)は SetFcb が**無条件に**
     *   書くフィールド —— **書き忘れると P643 と同型の不具合(書込み位置が
     *   前進せず同一オフセットへ上書きし続ける)になる**。 */
    wd_wl(nfcb + FCB_OFF_FILEPTR, fileptr + total);
    wd_wb(nfcb + FCB_OFF_ATTR, AT_ARCHIVE);
    /* ★size は max(旧size, fileptr+total) を自前で計算せず、書込み後の fstat で
     *   **実測**する(派生値より生値を採る)。切り詰め時もこれで正しくなる。 */
    if (fstat(fileno(f->fp), &st) == 0) {
        wd_wl(nfcb + FCB_OFF_SIZE, (uint32_t)st.st_size);
        windrv_time_date(&st, &tm, &dt);
        wd_ww(nfcb + FCB_OFF_TIME, tm);
        wd_ww(nfcb + FCB_OFF_DATE, dt);
    }

    windrv_wlog((req == 0) ? "truncate" : "write", f->path, total);
    windrv_set_result(total);
}

/* $45 Delete — windrv.cpp:1306。in: a5+14 (1L) NAMESTS(:1318)。 */
static void windrv_cmd_delete(void) {
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    uint32_t ns = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    char path[WINDRV_PATH_BUF];
    struct stat st;
    uint32_t err;

    /* 対象は実在するはずなので、既存の readdir 照合経路をそのまま使う
     * (path は realpath() 済み・ルート配下確定)。 */
    err = windrv_resolve_and_validate_path(ns, /*want_file=*/1, path, sizeof(path));
    if (err) { windrv_wlog("delete", NULL, err); windrv_set_result(err); return; }

    if (lstat(path, &st) != 0) {
        windrv_wlog("delete", path, FS_FILENOTFND);
        windrv_set_result(FS_FILENOTFND);
        return;
    }
    /* ディレクトリ削除は $43 RemoveDir の責務。unlink はそもそもディレクトリに
     * 失敗するが、専用のエラーコードを返したほうがゲスト側の挙動が素直になる。
     * これによりファイル削除経路からディレクトリへ回り込むこともできない。 */
    if (S_ISDIR(st.st_mode)) {
        windrv_wlog("delete", path, FS_CANTACCESS);
        windrv_set_result(FS_CANTACCESS);
        return;
    }
    /* マウントルート自身の保護(上の S_ISDIR で弾かれるはずだが独立に確認する)。 */
    if (strcmp(path, s_root) == 0) {
        windrv_wlog("delete", path, FS_CANTACCESS);
        windrv_set_result(FS_CANTACCESS);
        return;
    }

    /* ★unlink(2) は最終要素のシンボリックリンクを辿らない(POSIX 保証)ため、
     *   仮にリンクが対象になってもリンク自体が消えるだけでリンク先は無傷
     *   —— 追加防御は不要。
     * ★別 FCB で開いたままのファイルへの Delete は、POSIX の unlink-while-open
     *   意味論(クローズまで実体が残り、そちら側の読み書きは無警告に成功し続ける)に
     *   そのまま従う —— 追加のガードは**意図的に設けない**。Fix Plan §S-3 の
     *   informed consent の受容範囲であり、閉じるべき穴ではない。 */
    if (unlink(path) != 0) {
        uint32_t e;
        switch (errno) {
        case EACCES: case EPERM: case EROFS: e = FS_CANTDELETE; break;
        case ENOENT:                         e = FS_FILENOTFND; break;
        default:                             e = FS_CANTACCESS; break;
        }
        windrv_wlog("delete", path, e);
        windrv_set_result(e);
        return;
    }
    windrv_wlog("delete", path, 0);
    windrv_set_result(0);
}

/* $44 Rename — windrv.cpp:1265。
 * in: a5+14 (1L) 旧 NAMESTS(:1277) / a5+18 (1L) 新 NAMESTS(:1279)。 */
static void windrv_cmd_rename(void) {
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    /* ★実装規約 2: a5+18(新 NAMESTS アドレス)は結果格納先でもある
     *   —— 先に両方読み終えてから windrv_set_result() を呼ぶ。 */
    uint32_t ns_old = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    uint32_t ns_new = wd_raddr(s_a5 + CMD_OFF_ADDR2);
    char oldpath[WINDRV_PATH_BUF];
    char newpath[WINDRV_PATH_BUF];
    uint32_t err;

    err = windrv_resolve_and_validate_path(ns_old, /*want_file=*/1, oldpath, sizeof(oldpath));
    if (err) { windrv_wlog("rename", NULL, err); windrv_set_result(err); return; }

    /* ★新名側は「まだ存在しない名前」なので必ず新ヘルパを使う
     *   (windrv_find_entry() は readdir で実在エントリを探すため転用できない)。
     *   新 NAMESTS は自前の path フィールドを持つため、ディレクトリを跨ぐ移動も
     *   自然に扱える(両側ともルート配下確定)。 */
    err = windrv_resolve_new_path(ns_new, newpath, sizeof(newpath));
    if (err) { windrv_wlog("rename", NULL, err); windrv_set_result(err); return; }

    /* マウントルート自身の改名拒否。 */
    if (strcmp(oldpath, s_root) == 0) {
        windrv_wlog("rename", oldpath, FS_CANTACCESS);
        windrv_set_result(FS_CANTACCESS);
        return;
    }
    if (strcmp(oldpath, newpath) == 0) {   /* 同名への改名は何もしない */
        windrv_wlog("rename", newpath, 0);
        windrv_set_result(0);
        return;
    }

    /* ★素の rename(2) を使ってはならない —— POSIX の rename は宛先を**黙って
     *   上書き**する。Human68k の _RENAME は既存名との衝突をエラーにする意味論で
     *   あり、素の rename では利用者のファイルを予告なく破壊する。
     *   RENAME_EXCL(sys/stdio.h:37 = 0x00000004)により「宛先の存在確認」と
     *   「改名」が単一システムコール内で完結し、TOCTOU も同時に消える。
     *   renameatx_np は macOS 10.12+(sys/stdio.h:53)、MX の下限は 13.0。 */
    if (renameatx_np(AT_FDCWD, oldpath, AT_FDCWD, newpath, RENAME_EXCL) != 0) {
        uint32_t e;
        switch (errno) {
        case EEXIST:                         e = FS_FILEEXIST;  break;
        case EXDEV:                          e = FS_CANTRENAME; break;   /* 同一マウント内のはずだが防御的に */
        case EACCES: case EPERM: case EROFS: e = FS_CANTRENAME; break;
        case ENOENT:                         e = FS_FILENOTFND; break;
        default:                             e = FS_CANTACCESS; break;
        }
        windrv_wlog("rename", newpath, e);
        windrv_set_result(e);
        return;
    }
    windrv_wlog("rename", newpath, 0);
    windrv_set_result(0);
}

/* $42 MakeDir — windrv.cpp:1186。in: a5+14 (1L) NAMESTS(:1198)。
 * ★XM6 は GetNameSts(**完全形**)を使い、GetNameStsPath(パスのみ)ではない
 *   —— 新しいディレクトリ名は name/ext/add から取る。 */
static void windrv_cmd_makedir(void) {
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    uint32_t ns = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    char path[WINDRV_PATH_BUF];
    uint32_t err;

    err = windrv_resolve_new_path(ns, path, sizeof(path));
    if (err) { windrv_wlog("mkdir", NULL, err); windrv_set_result(err); return; }

    /* ★mkdir(2) は最終要素が既存(シンボリックリンクを含む)なら常に EEXIST で
     *   失敗し、リンクを辿って別の場所に作ることはない。 */
    if (mkdir(path, 0755) != 0) {
        uint32_t e;
        switch (errno) {
        case EEXIST:                         e = FS_DIRALREADY; break;
        case ENOSPC: case EDQUOT:            e = FS_DISKFULL;   break;
        case EACCES: case EPERM: case EROFS: e = FS_CANTWRITE;  break;
        default:                             e = FS_CANTACCESS; break;
        }
        windrv_wlog("mkdir", path, e);
        windrv_set_result(e);
        return;
    }
    windrv_wlog("mkdir", path, 0);
    windrv_set_result(0);
}

/* $43 RemoveDir — windrv.cpp:1225。in: a5+14 (1L) NAMESTS(完全形、:1237)。 */
static void windrv_cmd_removedir(void) {
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    uint32_t ns = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    char path[WINDRV_PATH_BUF];
    struct stat st;
    uint32_t err;

    /* ★windrv_find_entry() は windrv_host_to_h68() による名前照合のみで
     *   S_ISDIR フィルタを持たないため、ディレクトリも正しく見つかる。 */
    err = windrv_resolve_and_validate_path(ns, /*want_file=*/1, path, sizeof(path));
    if (err) { windrv_wlog("rmdir", NULL, err); windrv_set_result(err); return; }

    /* ★マウントルート自身の削除を明示的に拒否する。rmdir は空でないディレクトリに
     *   失敗するので実害は出にくいが、空のフォルダを共有していた場合に共有ルート
     *   そのものが消えるのは受け入れられない。 */
    if (strcmp(path, s_root) == 0) {
        windrv_wlog("rmdir", path, FS_CANTACCESS);
        windrv_set_result(FS_CANTACCESS);
        return;
    }
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        windrv_wlog("rmdir", path, FS_CANTACCESS);
        windrv_set_result(FS_CANTACCESS);
        return;
    }

    /* ★rmdir(2) は空でないディレクトリを削除しない —— **再帰削除は一切
     *   実装しない**(1 コマンドで多数のファイルを失う経路を作らない)。
     *   したがって 1 コマンドの最大破壊量は「空ディレクトリ 1 個」である。 */
    if (rmdir(path) != 0) {
        uint32_t e;
        switch (errno) {
        case ENOTEMPTY: case EEXIST:         e = FS_CANTDELETE; break;
        case ENOENT:                         e = FS_DIRNOTFND;  break;
        case EACCES: case EPERM: case EROFS: e = FS_CANTDELETE; break;
        default:                             e = FS_CANTACCESS; break;
        }
        windrv_wlog("rmdir", path, e);
        windrv_set_result(e);
        return;
    }
    windrv_wlog("rmdir", path, 0);
    windrv_set_result(0);
}

/* $50 GetCapacity — windrv.cpp:1888-1930 + SetCapacity(:2813-2828)。 */
static void windrv_cmd_get_capacity(void) {
    uint32_t cap = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    struct statvfs vfs;
    /* Human68k の 1 クラスタ = 32 セクタ x 512B = 16KB(下の DPB と一致させる)。 */
    const uint32_t bytes_per_sector  = 512;
    const uint32_t sectors_per_clust = 32;
    const uint64_t bytes_per_clust   = (uint64_t)bytes_per_sector * sectors_per_clust;
    uint64_t total_c = 0, free_c = 0;

    if (cap == 0) { windrv_set_result(FS_INVALIDPRM); return; }
    if (s_root[0] == '\0') { windrv_set_result(FS_FATAL_MEDIAOFFLINE); return; }

    if (statvfs(s_root, &vfs) == 0) {
        uint64_t frsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        total_c = ((uint64_t)vfs.f_blocks * frsize) / bytes_per_clust;
        free_c  = ((uint64_t)vfs.f_bavail * frsize) / bytes_per_clust;
    }
    /* capacity_t の各フィールドは WORD。Human68k から見える上限で頭打ちにする。 */
    if (total_c > 0xFFFFu) total_c = 0xFFFFu;
    if (free_c  > total_c) free_c  = total_c;

    /* ★P647: 読み取り専用マウント時(既定)は従来どおり空き 0 を報告する
     *   —— ゲスト側ツールが「書けるはず」と判断して書込みを試みる余地を減らす。
     *   書込み許可時のみ statvfs で得た実際の空きクラスタ数を報告する。空きを 0 の
     *   ままにすると Human68k が「ディスク満杯」と判断してファイル作成自体を
     *   試みず、新コマンドを足しても書込みが実用にならない。
     *   ★free_c は上で計算済み・total_c に頭打ち済み(生値をそのまま使う)。 */
    wd_ww(cap + CAP_OFF_FREE,     g_windrv_write_wired ? (uint32_t)free_c : 0u);
    wd_ww(cap + CAP_OFF_CLUSTERS, (uint32_t)total_c);
    wd_ww(cap + CAP_OFF_SECTORS,  sectors_per_clust);
    wd_ww(cap + CAP_OFF_BYTES,    bytes_per_sector);
    windrv_set_result(0);
}

/* $52 GetDPB — windrv.cpp:2014-2049 + SetDpb(:2830-2860)。
 * 仮想ファイルシステムなので実 FAT は存在しない。ゲストが構造として矛盾を
 * 感じない範囲の一貫した値を返す(GetCapacity と同じジオメトリ)。 */
static void windrv_cmd_get_dpb(void) {
    uint32_t dpb = wd_raddr(s_a5 + CMD_OFF_ADDR1);
    if (dpb == 0) { windrv_set_result(FS_INVALIDPRM); return; }
    wd_ww(dpb + DPB_OFF_SECTOR_SIZE,   512);
    wd_wb(dpb + DPB_OFF_CLUSTER_SIZE,  32 - 1);   /* 1 クラスタ当りのセクタ数 - 1 */
    wd_wb(dpb + DPB_OFF_SHIFT,         5);        /* 32 = 1 << 5 */
    wd_ww(dpb + DPB_OFF_FAT_SECTOR,    1);
    wd_wb(dpb + DPB_OFF_FAT_MAX,       1);
    wd_wb(dpb + DPB_OFF_FAT_SIZE,      1);
    wd_ww(dpb + DPB_OFF_FILE_MAX,      512);
    wd_ww(dpb + DPB_OFF_DATA_SECTOR,   34);
    wd_ww(dpb + DPB_OFF_CLUSTER_MAX,   0xFFFFu);
    wd_ww(dpb + DPB_OFF_ROOT_SECTOR,   2);
    wd_wb(dpb + DPB_OFF_MEDIA,         0xF8);     /* ハードディスク相当 */
    windrv_set_result(0);
}

/* $51 CtrlDrive — windrv.cpp:1938。in: a5+13(1B) 状態(0=状態検査/1=イジェクト、
 * windrv.cpp:1935)。成功時 out: a5+13(1B) 状態バイト(windrv.cpp:1965)。
 * ★MX固有の状態バイト値([static spec reading]、datacrystal X68k/FS wiki、
 *   一次資料未確認——hands-on確認で最終検証する): bit1=メディア挿入済み、
 *   bit3=書込禁止。Windrvは常にメディア「挿入済み」(共有フォルダは
 *   装着中は常時存在)・常に「準備完了」(実ハードウェアの遅延が無い)。 */
static void windrv_cmd_ctrl_drive(void) {
    uint32_t mode = wd_rb(s_a5 + CMD_OFF_ATTR);

    if (s_log_count < WINDRV_LOG_CAP) {
        debug_log("[P642-WINDRV] ctrl_drive: mode=%u\n", (unsigned)mode);
    }

    if (mode == 0) {
        /* 状態検査: メディア挿入済み(bit1)+ 書込許可状態に応じてbit3。 */
        uint8_t status = 0x02u | (g_windrv_write_wired ? 0u : 0x08u);
        wd_wb(s_a5 + CMD_OFF_ATTR, status);
        windrv_set_result(0);
    } else {
        /* ★イジェクト要求: 実体を持たない仮想マウントのため、実機の
         *   「イジェクト成功」を偽装せず明示的に拒否する(MX独自設計、
         *   参照実装に対応物なし)。 */
        windrv_set_result(FS_CANTWRITE);
    }
}

/* P650 変更5: $53 DiskRead(第1分岐のみ)— XM6:windrv.cpp:2062-2092 /
 * XM6fs:mfc_host.cpp:4423-4484。
 * 第2分岐(疑似セクタでのファイル実体読出し)は本サイクルのスコープ外
 * (lzdsys 専用機能で、$52 GetDPB のジオメトリ再設計が連鎖する)。 */
static void windrv_cmd_disk_read(void) {
    uint32_t naddr   = wd_raddr(s_a5 + CMD_OFF_ADDR1);   /* XM6:2072 GetAddr(a5+14) */
    uint32_t nsize   = wd_rl  (s_a5 + CMD_OFF_ADDR2);    /* XM6:2073 GetLong(a5+18) */
    uint32_t nsector = wd_raddr(s_a5 + CMD_OFF_ADDR3);   /* XM6:2074 GetLong(a5+22) */
    WindrvSearch* s;
    uint8_t d[0x20];
    int i;

    if (nsize != 1u) { windrv_set_result(FS_NOTIOCTRL); return; }   /* XM6fs:4434 */

    /* $47 が files_t+2 へ書いたハンドル(= files_t 先頭アドレス)で検索文脈を引く。
     * XM6fs:4448 m_cFiles.Search(nSector) と同一キー空間。 */
    s = windrv_search_find(nsector);
    if (!s || !s->have_entry) {
        /* 第2分岐を実装しないため、XM6fs:4514 の末尾値と同じ FS_NOTIOCTRL を返す。 */
        windrv_set_result(FS_NOTIOCTRL);
        return;
    }

    /* ★ここから先はリトルエンディアン(XM6fs:4451 の明示警告)。
     *   dirent_t は FAT 互換のオンディスク形式であり、files_t/DPB のような
     *   ビッグエンディアンのゲスト構造体とは別扱い。wd_ww/wd_wl は使わない。 */
    memset(d, 0, sizeof(d));
    memcpy(d + 0,  s->ent.name, 8);      /* +0  name[8]  (0x20 詰め済み) */
    memcpy(d + 8,  s->ent.ext,  3);      /* +8  ext[3]   (0x20 詰め済み) */
    d[11] = s->ent_attr;                 /* +11 attr */
    memcpy(d + 12, s->ent.add, 10);      /* +12 add[10]  (0x00 詰め済み) */
    d[22] = (uint8_t) (s->ent_time      ); d[23] = (uint8_t)(s->ent_time >>  8);  /* +22 time.W LE */
    d[24] = (uint8_t) (s->ent_date      ); d[25] = (uint8_t)(s->ent_date >>  8);  /* +24 date.W LE */
    d[26] = 0; d[27] = 0;                /* +26 cluster.W = 0(第2分岐未実装) */
    d[28] = (uint8_t) (s->ent_size      ); d[29] = (uint8_t)(s->ent_size >>  8);  /* +28 size.L LE */
    d[30] = (uint8_t) (s->ent_size >> 16); d[31] = (uint8_t)(s->ent_size >> 24);

    for (i = 0; i < 0x20;  i++) wd_wb(naddr + (uint32_t)i, d[i]);        /* XM6fs:4466 */
    for (i = 0x20; i < 0x200; i++) wd_wb(naddr + (uint32_t)i, 0xFFu);    /* XM6fs:4468 */
    windrv_set_result(0);
}

/* $46 Attribute(取得/設定両方向)— XM6:windrv.cpp:1332-1376 / XM6fs:mfc_host.cpp:3626-3676。
 * ★P655: 存在確認(両方向共通)を先に行い、その後 attr==0xFF で分岐する
 *   (XM6fs:3631-3638 と同じ順序 —— 従来は設定方向を存在確認より前で即 FS_CANTWRITE
 *   していたが、参照実装は方向によらず先に f.Find() する)。 */
static void windrv_cmd_attribute(void) {
    uint32_t ns   = wd_raddr(s_a5 + CMD_OFF_ADDR1);   /* XM6:1344 GetAddr(a5+14) */
    uint32_t attr = wd_rb(s_a5 + CMD_OFF_ATTR);       /* a5+13、XM6:1349 GetByte(a5+13)。
                                                       * 既存定数 CMD_OFF_ATTR(=13、:80行目)を
                                                       * 再利用する —— $51 CtrlDrive の
                                                       * windrv_cmd_ctrl_drive() も同じ定数で
                                                       * a5+13 を読んでいる(用途はコマンドにより
                                                       * 異なる: $46=属性/$FF取得, $51=mode)。 */
    char path[WINDRV_PATH_BUF];
    struct stat st;
    uint32_t err;

    err = windrv_resolve_and_validate_path(ns, /*want_file=*/1, path, sizeof(path));
    if (err) {
        /* P652: パス解決失敗時のみ、NAMESTSの生内容を記録する自己反証可能性プローブ。
         * name/ext/add を印字可能文字へ変換(非印字は '.')し、結合済み文字列も
         * 同じ行に併記する——「MXが何を要求として受け取ったか」を後から検証できる。 */
        if (s_attrlog_count < WINDRV_ATTRLOG_CAP) {
            uint8_t raw[NAMESTS_NAME_LEN + NAMESTS_EXT_LEN + NAMESTS_ADD_LEN];
            char printable[sizeof(raw) + 1];
            int i;
            for (i = 0; i < NAMESTS_NAME_LEN; i++) raw[i] = (uint8_t)wd_rb(ns + NAMESTS_OFF_NAME + i);
            for (i = 0; i < NAMESTS_EXT_LEN;  i++) raw[NAMESTS_NAME_LEN + i] = (uint8_t)wd_rb(ns + NAMESTS_OFF_EXT + i);
            for (i = 0; i < NAMESTS_ADD_LEN;  i++) raw[NAMESTS_NAME_LEN + NAMESTS_EXT_LEN + i] = (uint8_t)wd_rb(ns + NAMESTS_OFF_ADD + i);
            for (i = 0; i < (int)sizeof(raw); i++)
                printable[i] = (raw[i] >= 0x20 && raw[i] < 0x7Fu) ? (char)raw[i] : '.';
            printable[sizeof(raw)] = '\0';
            s_attrlog_count++;
            debug_log("[P652-WINDRV-ATTR] #%u ns=0x%06x err=0x%08x name=\"%.8s\" ext=\"%.3s\" "
                      "add=\"%.10s\" wildcard=%u pc=0x%06x\n",
                      s_attrlog_count, ns, err, printable, printable + 8, printable + 11,
                      (unsigned)wd_rb(ns + NAMESTS_OFF_WILDCARD),
                      (unsigned)(C68k_Get_PC(&C68K) & 0x00FFFFFFu));
        }
        windrv_set_result(err);   /* XM6fs:3638 Find()失敗 → FS_FILENOTFND相当 */
        return;
    }

    if (stat(path, &st) != 0) { windrv_set_result(FS_FILENOTFND); return; }

    if (attr == 0xFFu) {
        /* 取得方向(XM6fs:3641-3643) —— 結果値そのものが属性バイト
         * (専用バッファへの書込みではない)。 */
        windrv_set_result(windrv_attr_of(&st));
        return;
    }

    /* ★P655: 設定方向。 */
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    /* ★Code Review指摘: ディレクトリへの chmod は適用しない。
     * windrv_attr_of()(:673)は S_ISDIR を最優先判定し常に AT_DIRECTORY のみを
     * 返す(READONLY状態はゲストへ一切見えない)ため、設定方向だけがディレクトリの
     * 実権限を変更するのは同一ファイル内の既存設計と非対称。加えて Windows の
     * FILE_ATTRIBUTE_READONLY はフォルダに立てても実害が無い(XM6の前提)のに対し、
     * POSIX で S_IWUSR をディレクトリから外すと配下のファイル作成/削除が実際に
     * ブロックされる実害がある——参照実装(XM6)には無い退行を持ち込むことになる。 */
    if (!S_ISDIR(st.st_mode)) {
        mode_t new_mode = st.st_mode;
        /* windrv_attr_of()(:672-676)の逆写像。AT_SYSTEM/AT_HIDDENはホスト側に
         * 対応物が無く読み取り側でも元々公開していないため、ここでも無視する
         * (XM6fs:3663-3664相当のビットは黙って受理・無視)。 */
        if (attr & AT_READONLY) new_mode &= ~(mode_t)S_IWUSR;
        else                    new_mode |= S_IWUSR;
        if (chmod(path, new_mode & 07777) != 0) {
            /* XM6fs:3672 SetFileAttributes失敗 → FS_FILENOTFND(参照実装どおり、
             * 独自のエラーコードへ変えない)。 */
            windrv_wlog("attribute", path, FS_FILENOTFND);
            windrv_set_result(FS_FILENOTFND);
            return;
        }
    }

    if (stat(path, &st) != 0) { windrv_set_result(FS_FILENOTFND); return; }  /* XM6fs:3675再取得 */
    windrv_wlog("attribute", path, 0);
    windrv_set_result(windrv_attr_of(&st));
}

/* $4F TimeStamp — XM6:windrv.cpp:1823-1859 / XM6fs:mfc_host.cpp:4226-4292。
 * a5+18=DATE.W, a5+20=TIME.W(共にFAT形式), a5+22=FCBアドレス(CMD_OFF_ADDR3)。
 * date==0 && time==0 → 取得方向、それ以外 → 設定方向。
 * ★実装規約2厳守: windrv_set_result() は a5+18(CMD_OFF_ADDR2 と同一オフセット)を
 *   結果格納に使うため、DATE/TIME/FCB は関数冒頭で必ず先に全部読むこと。 */
static void windrv_cmd_timestamp(void) {
    uint32_t nfcb  = wd_raddr(s_a5 + CMD_OFF_ADDR3);
    uint16_t date  = (uint16_t)wd_rw(s_a5 + CMD_OFF_DATE);
    uint16_t timev = (uint16_t)wd_rw(s_a5 + CMD_OFF_TIME);
    WindrvFile* f;

    if (date == 0 && timev == 0) {
        /* 取得方向(XM6fs:4234-4236) — 読み取り操作なので書込みトグルのゲート対象外。
         * FCB の日付/時刻キャッシュ(Open/Create 時に書込み済み)をそのまま返す。 */
        uint16_t fcb_date = (uint16_t)wd_rw(nfcb + FCB_OFF_DATE);
        uint16_t fcb_time = (uint16_t)wd_rw(nfcb + FCB_OFF_TIME);
        windrv_set_result(((uint32_t)fcb_date << 16) | fcb_time);
        return;
    }

    /* 設定方向。既存の書込み系ハンドラ($42/$43/$44/$45/$49/$4D)全てが、ゲート拒否時にも
     * windrv_wlog("gate", NULL, FS_CANTWRITE) を呼ぶ既存慣行に揃える
     * (★Code Review 指摘1: これが無いとプラン自身が問題視した「行が無い」の
     *   二義性を $4F のゲート拒否ケースだけ再導入してしまう)。 */
    if (!g_windrv_write_wired) {
        windrv_wlog("gate", NULL, FS_CANTWRITE);
        windrv_set_result(FS_CANTWRITE);
        return;
    }

    f = windrv_file_find(nfcb);
    if (!f) { windrv_set_result(FS_NOTOPENED); return; }   /* XM6fs:4244相当。$4D Write の
                                                            * !f||!f->fp ケース(:1532)も
                                                            * 無ログのため既存慣行と整合。 */

    {
        struct tm tmv;
        struct timespec ts[2];
        time_t t;

        /* FAT形式 → struct tm。windrv_time_date()(:639-654)の逆変換。 */
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year  = (int)((date >> 9) & 0x7Fu) + 1980 - 1900;
        tmv.tm_mon   = (int)((date >> 5) & 0x0Fu) - 1;
        tmv.tm_mday  = (int)(date & 0x1Fu);
        tmv.tm_hour  = (int)((timev >> 11) & 0x1Fu);
        tmv.tm_min   = (int)((timev >> 5) & 0x3Fu);
        tmv.tm_sec   = (int)((timev & 0x1Fu) * 2u);
        tmv.tm_isdst = -1;   /* windrv_time_date() の localtime_r() と対称 */

        t = mktime(&tmv);
        if (t == (time_t)-1) {
            /* 不正な日付(月0等)。XM6 の SetFileTime 失敗相当。 */
            windrv_wlog("timestamp", f->path, FS_CANTWRITE);
            windrv_set_result(FS_CANTWRITE);
            return;
        }

        ts[0].tv_sec = t; ts[0].tv_nsec = 0;   /* atime */
        ts[1].tv_sec = t; ts[1].tv_nsec = 0;   /* mtime */
        if (futimens(fileno(f->fp), ts) != 0) {
            windrv_wlog("timestamp", f->path, FS_CANTWRITE);
            windrv_set_result(FS_CANTWRITE);
            return;
        }
        /* ★Code Review 指摘2で訂正: XM6:1848-1856 SetFcb(nFcb,&fcb) は、直前 :1835 の
         * GetFcb() で読んだ「変更前」の値をそのまま書き戻す実質 no-op であり、
         * mfc_host.cpp:4244-4290(設定方向本体)も pFcb->date/time への代入を一切
         * 行っていない(SetFileTime() はホスト側オブジェクトのみ操作)。
         * XM6 は新しい日付/時刻を FCB キャッシュへ反映しない——MX もここでは
         * FCB を変更せず、参照実装の実挙動に厳密に一致させる。 */
        windrv_wlog("timestamp", f->path, 0);
        windrv_set_result(0);
    }
}

/* ------------------------------------------------------------------------
 * コマンドディスパッチ
 * ------------------------------------------------------------------------ */

static void windrv_dispatch(void) {
    switch (s_command & 0x7Fu) {
    case 0x41: windrv_cmd_check_dir();    return;
    case 0x47: windrv_cmd_files();        return;
    case 0x48: windrv_cmd_nfiles();       return;
    case 0x4A: windrv_cmd_open();         return;
    case 0x4B: windrv_cmd_close();        return;
    case 0x4C: windrv_cmd_read();         return;
    case 0x50: windrv_cmd_get_capacity(); return;
    case 0x52: windrv_cmd_get_dpb();      return;

    case 0x51: windrv_cmd_ctrl_drive();   return;

    /* ★P648で$51を分離。残りは無害な成功応答スタブのまま。ゲスト側ドライバが
     * これらを呼んでエラー扱いされると他コマンドの前提が崩れうるため、
     * 実処理は行わずに成功だけ返す。
     *   $55 IoControl / $56 Flush / $57 CheckMedia / $58 Lock */
    case 0x55: case 0x56: case 0x57: case 0x58:
        windrv_set_result(0);
        return;

    /* ★P647: 書込み系。各ハンドラの先頭が g_windrv_write_wired のゲートであり、
     * 書込み許可が未配線(既定)なら従来のスタブと**同じ** FS_CANTWRITE を返す。 */
    case 0x42: windrv_cmd_makedir();   return;
    case 0x43: windrv_cmd_removedir(); return;
    case 0x44: windrv_cmd_rename();    return;
    case 0x45: windrv_cmd_delete();    return;
    case 0x49: windrv_cmd_create();    return;
    case 0x4D: windrv_cmd_write();     return;

    /* ★P655: $46 は取得/設定両方向を実装(windrv_cmd_attribute() 内で判別)。
     *   $54 DiskWrite は引き続きスコープ外。 */
    case 0x46: windrv_cmd_attribute(); return;

    /* ★P654: $4F は取得/設定両方向を実装(windrv_cmd_timestamp() 内で判別)。
     *   $54 DiskWrite はスコープ外。 */
    case 0x4F: windrv_cmd_timestamp(); return;

    /* ★P647 スコープ外(理由は P647 Fix Plan §スコープ外)。書込みトグルの状態に
     * 関わらず FS_CANTWRITE を返す。
     *   $54 DiskWrite */
    case 0x54:
        windrv_set_result(FS_CANTWRITE);
        return;

    /* P650: $53 DiskRead の第1分岐(疑似ディレクトリエントリ)を実装。
     * 第2分岐(疑似セクタでのファイル実体読出し)は未実装で FS_NOTIOCTRL。 */
    case 0x53: windrv_cmd_disk_read(); return;

    /* $4E Seek は MVP 範囲外(Fix Plan が明示的にスコープ外)。 */
    default:
        windrv_set_result(FS_INVALIDFUNC);
        return;
    }
}

/* windrv.cpp:352-366 ExecuteCompatible と同一手順。
 * 戻り値は D0.L 相当 = GetByte(a5+3) | (GetByte(a5+4) << 8)。 */
static uint32_t windrv_execute(uint32_t a5) {
    uint32_t ret;
    /* P650 変更6: ディスパッチ前スナップショット(下記の代入箇所のコメント参照)。 */
    uint32_t pre_a1 = 0, pre_a2 = 0, pre_a3 = 0, pre_fatr = 0x100u;

    if (s_busy) return 0;    /* 再入防止(通常は起こらない) */
    s_busy = 1;

    s_a5 = a5 & 0x00FFFFFFu;
    wd_wb(s_a5 + CMD_OFF_ERRL, 0);
    wd_wb(s_a5 + CMD_OFF_ERRH, 0);
    s_unit    = (uint8_t)wd_rb(s_a5 + CMD_OFF_UNIT);
    s_command = (uint8_t)wd_rb(s_a5 + CMD_OFF_CMD);

    /* P650 変更6: 診断ログ用の生値スナップショット。
     * ★a5+18(CMD_OFF_ADDR2)は入力パラメータと結果格納先を兼ねる(:355,:358)ため、
     *   ログ用の生値は必ずディスパッチ「前」に控える —— 後で読むと入力値が結果で
     *   上書きされており、誤った referent を記録してしまう。 */
    pre_a1 = wd_rl(s_a5 + CMD_OFF_ADDR1);
    pre_a2 = wd_rl(s_a5 + CMD_OFF_ADDR2);
    pre_a3 = wd_rl(s_a5 + CMD_OFF_ADDR3);
    /* pre_fatr の 0x100 = 未採取(1バイトに収まらない番兵)。「採取できなかった」と
     * 「fatr=0xFF だった」を絶対に混同しないための表現。 */
    if (s_command == 0x47 && (pre_a2 & 0x00FFFFFFu) != 0u)
        pre_fatr = wd_rb((pre_a2 & 0x00FFFFFFu) + FILES_OFF_FATR);

    if (s_command == 0x40) {
        /* $40 のみユニット番号チェックの前に処理する(windrv.cpp:984-987)。 */
        windrv_cmd_init_drive();
    } else if ((uint32_t)s_unit >= s_unit_max) {
        windrv_set_result(FS_FATAL_INVALIDUNIT);
    } else {
        windrv_dispatch();
    }

    ret = wd_rb(s_a5 + CMD_OFF_ERRL) | (wd_rb(s_a5 + CMD_OFF_ERRH) << 8);

    if (s_log_count < WINDRV_LOG_CAP) {
        s_log_count++;
        debug_log("[P642-WINDRV] cmd=0x%02x unit=%u a5=0x%06x result=0x%08x fatal=0x%04x\n",
                  (unsigned)s_command, (unsigned)s_unit, (unsigned)s_a5,
                  (unsigned)wd_rl(s_a5 + CMD_OFF_ADDR2), (unsigned)ret);
        if (s_log_count == WINDRV_LOG_CAP) {
            debug_log("[P642-WINDRV] further command lines suppressed (>%d)\n", WINDRV_LOG_CAP);
        }
    }

    /* P649: エラー応答専用ログ(s_log_count の200行上限に影響されない
     * 独立カウンタ)。「コピー後にTFが発行するコマンドがMXに拒否されて
     * いるか」を検証するための自己反証可能性プローブ——通常コマンドログが
     * 打切られた後でも、エラー応答だけは確実に記録される。
     * ★FS_FATAL_WRITEPROTECT/MEDIAOFFLINE は ADDR2 を書き換えないため、
     *   res 単体の判定だけでは検出漏れが起きる。ret(ERRL/ERRH の結合値、
     *   致命時は必ず非0)との OR で判定する。 */
    {
        uint32_t res = wd_rl(s_a5 + CMD_OFF_ADDR2);
        if ((res >= 0xFFFFFF00u || ret != 0u) && s_errlog_count < WINDRV_ERRLOG_CAP) {
            s_errlog_count++;
            /* P650 変更6: ディスパッチ前スナップショット fatr/a1/a2/a3 を全エラー行へ
             * 追記(コマンド別の分岐は設けず 1 書式・全コマンド共通。各フィールドの
             * 意味は下の [P650-WINDRV-FS] のコメントで表引きする)。 */
            debug_log("[P649-WINDRV-ERR] #%u cmd=0x%02x unit=%u result=0x%08x pc=0x%06x "
                      "fatr=0x%03x a1=0x%08x a2=0x%08x a3=0x%08x\n",
                      s_errlog_count, (unsigned)s_command, (unsigned)s_unit,
                      (unsigned)res, (unsigned)(C68k_Get_PC(&C68K) & 0x00FFFFFFu),
                      (unsigned)pre_fatr, (unsigned)pre_a1, (unsigned)pre_a2, (unsigned)pre_a3);
        }

        /* P650 変更6: $47/$53 の**成否によらない全呼出し**を記録する分母つきログ。
         * エラー専用ログは「修正が効くと行が消える」非対称を持つため、これが無いと
         * 0 件の解釈が「仮説が正しかった」と「そもそも呼ばれていない」に割れる。 */
        if ((s_command == 0x47 || s_command == 0x53) && s_fslog_count < WINDRV_FSLOG_CAP) {
            s_fslog_count++;
            /* フィールドの意味: $47 → a2=files_t アドレス / fatr=検索属性
             *                   $53 → a1=nAddress, a2=nSize, a3=nSector(検索ハンドル)
             * hit は派生値だが、その生値 a3 を同じ行に併記してある。 */
            debug_log("[P650-WINDRV-FS] #%u cmd=0x%02x unit=%u fatr=0x%03x "
                      "a1=0x%08x a2=0x%08x a3=0x%08x hit=%d result=0x%08x pc=0x%06x\n",
                      s_fslog_count, (unsigned)s_command, (unsigned)s_unit, (unsigned)pre_fatr,
                      (unsigned)pre_a1, (unsigned)pre_a2, (unsigned)pre_a3,
                      (s_command == 0x53) ? (windrv_search_find(pre_a3 & 0x00FFFFFFu) ? 1 : 0) : -1,
                      (unsigned)res, (unsigned)(C68k_Get_PC(&C68K) & 0x00FFFFFFu));
            if (s_fslog_count == WINDRV_FSLOG_CAP)
                debug_log("[P650-WINDRV-FS] further lines suppressed (>%d)\n", WINDRV_FSLOG_CAP);
        }
    }

    s_busy = 0;
    return ret;
}

/* ------------------------------------------------------------------------
 * 非同期ハンドル($E9F001 経路)
 *
 * MX はコマンドをその場で同期完結させるため、ハンドルは「完了済みの実行結果を
 * 1 回だけ引き取るための引換券」に過ぎない。ゲスト側から見た手順
 * (write 0x00 → D0.L にハンドル → read でステータス → write 0xFF で解放)は
 * XM6(windrv.cpp:895-968)と同一。
 * ------------------------------------------------------------------------ */

static int windrv_handle_alloc(uint32_t* out) {
    int i;
    for (i = 0; i < WINDRV_MAX_HANDLES; i++) {
        if (s_handles[i] == 0) {
            uint32_t h = s_handle_next++;
            if (s_handle_next == 0 || s_handle_next == 0xFFFFFFFFu) s_handle_next = 1;
            s_handles[i] = h;
            *out = h;
            return 1;
        }
    }
    return 0;
}

static int windrv_handle_live(uint32_t h) {
    int i;
    if (h == 0) return 0;
    for (i = 0; i < WINDRV_MAX_HANDLES; i++) if (s_handles[i] == h) return 1;
    return 0;
}

static void windrv_handle_free(uint32_t h) {
    int i;
    for (i = 0; i < WINDRV_MAX_HANDLES; i++) if (s_handles[i] == h) s_handles[i] = 0;
}

/* ------------------------------------------------------------------------
 * 公開 API
 * ------------------------------------------------------------------------ */

int windrv_claims_addr(uint32_t addr) {
    uint32_t a = addr & 0x00FFFFFFu;
    if (!g_windrv_installed) return 0;
    return (a == WINDRV_PORT_CMD || a == WINDRV_PORT_STATUS) ? 1 : 0;
}

void windrv_init(void) {
    char resolved[WINDRV_HOST_PATH_MAX];
    int  root_ok = 0;

    windrv_files_close_all();
    windrv_search_free_all();
    memset(s_handles, 0, sizeof(s_handles));
    s_handle_next   = 1;
    s_search_serial = 0;
    s_busy          = 0;
    s_unit_max      = 1;
    s_log_count     = 0;
    s_wlog_count    = 0;
    s_errlog_count  = 0;
    s_root[0]       = '\0';
    s_root_slash[0] = '\0';

    if (g_windrv_enabled && g_windrv_host_path[0] != '\0') {
        if (realpath(g_windrv_host_path, resolved) != NULL) {
            struct stat st;
            size_t l = strlen(resolved);
            if (l > 0 && l < sizeof(s_root) && stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                memcpy(s_root, resolved, l + 1);
                snprintf(s_root_slash, sizeof(s_root_slash), "%s%s",
                         s_root, (s_root[l - 1] == '/') ? "" : "/");
                root_ok = 1;
            }
        }
    }

    /* ★配線確定: 「設定で有効」かつ「フォルダが実在するディレクトリとして
     * 正規化できた」ときのみ装着。どちらか欠ければ未装着へ倒す —— 未装着なら
     * $E9E000-$E9FFFF は P419 の従来どおりのバスエラーのままで、既存挙動は不変。 */
    g_windrv_installed = root_ok ? 1 : 0;

    /* ★P647: 書込み許可は「共有が装着済み」であることに従属する —— 未装着なら
     * 書込み許可も必ず 0(単独で立つことがない)。既定は 0 = 書込み不可。 */
    g_windrv_write_wired = (g_windrv_installed && g_windrv_write_enabled) ? 1 : 0;

    debug_log("[P642-WINDRV] init: enabled=%d path=\"%s\" root_ok=%d installed=%d "
              "write_enabled=%d write_wired=%d\n",
              (int)g_windrv_enabled, g_windrv_host_path, root_ok, g_windrv_installed,
              (int)g_windrv_write_enabled, g_windrv_write_wired);
}

uint32_t windrv_mmio_read(uint32_t addr, int size) {
    uint32_t a = addr & 0x00FFFFFFu;
    uint32_t v;

    if (!g_windrv_installed) return 0xFFu;

    if (a == WINDRV_PORT_CMD) {
        /* windrv.cpp:754-772 ReadOnly: ENABLE/DUAL は 'Y'(WindrvXM)。 */
        v = (uint32_t)'Y';
        if (size == 2) {
            /* word read は $E9F000(上位)+ $E9F001(下位)の合成
             * —— Core mem_wrap.c rm16_main() の既定合成順序に合わせる。 */
            uint32_t handle = C68k_Get_DReg(&C68K, 0);
            uint32_t st = windrv_handle_live(handle) ? 1u : 0xFFu;
            return (v << 8) | st;
        }
        return v;
    }

    if (a == WINDRV_PORT_STATUS) {
        /* windrv.cpp:926-946 StatusAsynchronous: D0.L のハンドルを見て
         * 0 = 実行中 / 1 = 完了 / 0xFF = 不正ハンドル。MX は同期実行なので
         * 生存ハンドルは常に「完了済み」。 */
        uint32_t handle = C68k_Get_DReg(&C68K, 0);
        return windrv_handle_live(handle) ? 1u : 0xFFu;
    }
    return 0xFFu;
}

void windrv_mmio_write(uint32_t addr, uint32_t val, int size) {
    uint32_t a = addr & 0x00FFFFFFu;

    if (!g_windrv_installed) return;

    /* word 書込みは上位バイトが addr、下位バイトが addr+1 に対応する
     * (Core mem_wrap.c wm16_main() の分解順序)。各ポートへ振り分ける。 */
    if (size == 2) {
        windrv_mmio_write(a,     (val >> 8) & 0xFFu, 1);
        windrv_mmio_write(a + 1, val & 0xFFu,        1);
        return;
    }

    if (a == WINDRV_PORT_CMD) {
        /* WINDRV 互換(同期)経路。windrv.cpp:817-824 では ENABLE 単独モードだと
         * 何もせず return するが(DUAL のみ Execute)、MX は DUAL 相当として
         * 同期実行する —— 互換ドライバもそのまま動く上位互換。
         * 結果は D0.L(= a5+3/+4 の fatal コード)へ返す。 */
        uint32_t a5  = C68k_Get_AReg(&C68K, 5);
        uint32_t ret = windrv_execute(a5);
        C68k_Set_DReg(&C68K, 0, ret);
        return;
    }

    if (a == WINDRV_PORT_STATUS) {
        /* windrv.cpp:826-832: data 0x00 = 実行開始、0xFF = ハンドル解放。
         * それ以外の値は何もしない(拡張用、バスエラーにもしない)。 */
        uint32_t v = val & 0xFFu;
        if (v == 0x00u) {
            uint32_t a5 = C68k_Get_AReg(&C68K, 5);
            uint32_t handle = 0;
            (void)windrv_execute(a5);
            /* windrv.cpp:906-915: 空きスレッドが無ければ D0.L = -1。 */
            if (!windrv_handle_alloc(&handle)) handle = 0xFFFFFFFFu;
            C68k_Set_DReg(&C68K, 0, handle);
        } else if (v == 0xFFu) {
            windrv_handle_free(C68k_Get_DReg(&C68K, 0));
        }
        return;
    }
}
