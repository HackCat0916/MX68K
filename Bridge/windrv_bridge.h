#ifndef MX68K_WINDRV_BRIDGE_H
#define MX68K_WINDRV_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* P642: Windrv = Mac のフォルダを X68000 ゲストから 1 台のドライブとして見せる
 * ホスト共有機能。ゲスト側ドライバは標準ブートディスク同梱の `\SYS\WindrvXM.SYS`
 * (XM6 用の実バイナリ、無改造でそのまま利用)。MX は「ホスト側」だけを新規実装する。
 *
 * 読み取り系(P642/P643、常に有効):
 *   $40 InitDrive / $41 CheckDir / $47 Files / $48 NFiles /
 *   $4A Open / $4B Close / $4C Read / $50 GetCapacity / $52 GetDPB
 * および無害な成功応答スタブ $51/$55/$56/$57/$58。
 *
 * 書込み系(P647、g_windrv_write_wired が立っているときのみ動作):
 *   $49 Create / $4D Write / $45 Delete / $44 Rename /
 *   $42 MakeDir / $43 RemoveDir
 * 書込み許可は共有有効トグルとは **独立した第 2 のトグル**(既定 OFF)で
 * 制御する。OFF のときは従来どおり FS_CANTWRITE を返す。
 *
 * 依然として未実装:
 *   $46 Attribute(設定)/ $4E Seek / $4F TimeStamp /
 *   $53/$54 DiskRead/DiskWrite(セクタ raw アクセス)
 * (理由は P647 Fix Plan §スコープ外)。
 *
 * プロトコルは純粋なメモリマップド I/O(Core 改変不要):
 *   $E9F000 read  → 識別文字 'Y'(装着時)。ドライバ側のプローブ用。
 *   $E9F000 write → コマンド同期実行(WINDRV 互換モード経路)。
 *   $E9F001 write → data=0x00 で実行開始 / 0xFF でハンドル解放(WindrvXM 非同期経路)。
 *   $E9F001 read  → 非同期実行のステータス(0=実行中, 1=完了, 0xFF=不正ハンドル)。
 * 参照: XM6 `vm/windrv.cpp:711-860`(ReadByte/ReadOnly/WriteByte/
 *       ExecuteAsynchronous/StatusAsynchronous/ReleaseAsynchronous)。
 *
 * ★注意: $E9E000-$E9FFFF は P419(D-26)で「純正 FPU ボード未装着 = 無条件
 *   バスエラー」として扱われている。Windrv 装着時のみ $E9F000/$E9F001 の 2 番地を
 *   そのバスエラーから除外する(`windrv_claims_addr()` が唯一の判定点)。
 *   未装着時(既定)の挙動は P642 以前と完全に同一。 */

/* Windrv MMIO 窓。XM6 vm/windrv.cpp:581-591 の memdev.first/last と同一範囲だが、
 * 実際に応答するのは下の 2 番地のみ。 */
#define WINDRV_MMIO_FIRST   0x00E9E000u
#define WINDRV_MMIO_LAST    0x00E9FFFFu
#define WINDRV_PORT_CMD     0x00E9F000u   /* 識別文字 read / 同期実行 write */
#define WINDRV_PORT_STATUS  0x00E9F001u   /* 非同期ステータス read / 実行・解放 write */

/* --- 設定値 / 配線確定値(Mercury / MIDI と同型)-------------------------
 * 実体の定義は Bridge/EmulatorBridge.c(Fix Plan §3)。設定値は即時反映、
 * 配線確定値 g_windrv_installed は init / ハードリセットでのみラッチする。 */
extern bool g_windrv_enabled;        /* 設定値: 設定画面のトグル */
extern char g_windrv_host_path[];    /* 設定値: 共有する Mac フォルダ(空 = 未選択) */
extern int  g_windrv_installed;      /* 配線確定値: 0 = 未装着(既定) */
/* P647: 書込み許可。g_windrv_enabled とは独立した第 2 のトグルで、
 * 書込み系 6 コマンドはすべて g_windrv_write_wired を先頭ゲートに持つ。 */
extern bool g_windrv_write_enabled;  /* 設定値: 書込み許可トグル(既定 false) */
extern int  g_windrv_write_wired;    /* 配線確定値: 0 = 書込み不可(既定) */

/* g_windrv_host_path のバイト長。PATH_MAX(macOS = 1024)相当。 */
#define WINDRV_HOST_PATH_MAX 1024

/* init / mx68k_reset_hard の「設定値 → 配線確定」ブロックから呼ぶ。
 * g_windrv_enabled と g_windrv_host_path を読み、マウントルートを realpath() で
 * 正規化して g_windrv_installed を確定する(正規化に失敗したら未装着へ倒す)。
 * 併せてファイルハンドルテーブル / 検索コンテキストを全解放する。 */
void windrv_init(void);

/* $E9F000 / $E9F001 への書込みフック。addr は 24bit マスク済みを渡すこと。
 * size は 1(byte)/ 2(word)。装着時のみ呼ぶこと。 */
void windrv_mmio_write(uint32_t addr, uint32_t val, int size);

/* $E9F000 / $E9F001 からの読出しフック。装着時のみ呼ぶこと。 */
uint32_t windrv_mmio_read(uint32_t addr, int size);

/* 装着済みかつ addr が Windrv の応答する 2 番地のいずれかなら 1。
 * P419 のバスエラー合成を抑止するゲートと、read/write フックの発火条件を
 * この 1 関数に集約する(判定ロジックの書き写しは事故源)。 */
int windrv_claims_addr(uint32_t addr);

#endif /* MX68K_WINDRV_BRIDGE_H */
