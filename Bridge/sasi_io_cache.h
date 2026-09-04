/* ===========================================================================
 *  P502 (D-9): SASI ホストファイル I/O のディスクリプタキャッシュ層
 * ---------------------------------------------------------------------------
 *  Core/px68k/x68k/sasi.c の SASI_Seek()/SASI_Flush() は 256B セクタごとに
 *  open → seek → read/write → close の 4 syscall を発行する(px68k 本家/
 *  px68k-libretro と構造同一。MX 固有の劣化ではない)。ホスト実測では
 *  open+lseek+read+close = 15.82µs/sector に対し lseek+read のみ = 0.46µs/
 *  sector で 34.4 倍の差があり、DMAC ch1 の転送がエミュレートサイクルを
 *  消費しないホスト側ループである以上、この差はそのままフレーム時間を蝕む。
 *
 *  Core は編集しない(CLAUDE.md「Core File Modification Policy」)。代わりに
 *  本ヘッダを sasi.c 専用の COMPILER_FLAGS(-include)で翻訳単位の先頭へ
 *  強制挿入し、File_Open/Seek/Read/Write/Close をマクロで Bridge 側の
 *  キャッシュ実装へ差し替える。既存の Bridge/px68k_compat.h と同じ機構で
 *  あり、新規パターンではない。
 *
 *  ★このヘッダは sasi.c 以外の翻訳単位には force-include されない
 *    (project.pbxproj の "sasi.c in Sources" の COMPILER_FLAGS のみ)。
 *    他の Core ファイルの File_* 呼び出しは従来どおり dosio.c を使う。
 * =========================================================================== */

#ifndef MX68K_SASI_IO_CACHE_H
#define MX68K_SASI_IO_CACHE_H

#include "win32api/dosio.h"   /* FILEH(= HANDLE = void*)と File_* の元宣言 */

#ifdef __cplusplus
extern "C" {
#endif

/* dosio.c の File_* と戻り値の型・意味を完全互換に保つこと。
 *  - sasi_io_open()  : 失敗時 NULL(sasi.c の `if (!fp)` 判定と同じ)
 *  - sasi_io_seek()  : 新しい絶対オフセットを返す(SetFilePointer 互換)。
 *                      OS の lseek() は発行せず、エントリ内 current_offset を
 *                      更新するだけ。値そのものは次の read/write まで保持する。
 *  - sasi_io_read()  : 実転送バイト数(失敗時 0)。current_offset を進める。
 *  - sasi_io_write() : 実転送バイト数(失敗時 0)。current_offset を進める。
 *  - sasi_io_close() : no-op(常に 0)。実 close() は LRU eviction と
 *                      sasi_io_cache_invalidate_all() でのみ発生する。 */
FILEH    sasi_io_open(char* filename);
uint32_t sasi_io_seek(FILEH handle, uint32_t pointer, uint16_t mode);
uint32_t sasi_io_read(FILEH handle, void *data, uint32_t length);
uint32_t sasi_io_write(FILEH handle, void *data, uint32_t length);
int16_t  sasi_io_close(FILEH handle);

/* 全エントリの fd を実際に close() しテーブルをクリアする(公開関数)。
 * イメージ差し替え(mx68k_hdd_insert/_eject)・ハードリセットの際に
 * Bridge/EmulatorBridge.c から呼ぶ。★必ずエミュレーションスレッド上で
 * 呼ぶこと(フレーム境界消費 or 起動/ステートロードの単一スレッド区間)。 */
void sasi_io_cache_invalidate_all(void);

#ifdef __cplusplus
}
#endif

/* --- File_* → キャッシュ層への差し替え -----------------------------------
 * sasi_io_cache.c 自身は生の open/pread/pwrite/close を使うため、
 * SASI_IO_CACHE_NO_MACROS を定義してこのブロックを無効化する。 */
#ifndef SASI_IO_CACHE_NO_MACROS
#define File_Open   sasi_io_open
#define File_Seek   sasi_io_seek
#define File_Read   sasi_io_read
#define File_Write  sasi_io_write
#define File_Close  sasi_io_close
#endif

#endif /* MX68K_SASI_IO_CACHE_H */
