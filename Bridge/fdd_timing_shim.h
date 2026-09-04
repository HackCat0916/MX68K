/* ===========================================================================
 *  P557: FDD 実時間ウェイト(usleep)のユーザー切替式ショートカット層
 * ---------------------------------------------------------------------------
 *  Core/px68k/x68k/fdd.c は、シーク(fdd.c:222 = movetrack*200+150µs)・
 *  読込(fdd.c:268 = 300µs)・書込(fdd.c:300 = 300µs)で **ホストの実時間**を
 *  usleep() で消費する。これはエミュレートサイクルではなくエミュレーション
 *  スレッドそのものをブロックするため、FD アクセス中の体感速度を直接支配する。
 *  参照実装 3 種(MPX68K HEAD 42fc340 / px68k 本家 / px68k-libretro)は
 *  いずれもこの種の実時間遅延を持たない(3/3 一致)。
 *
 *  一方 XM6 は実機 FDC レジスタ値(SRT/HUT/HLT)+回転角度に基づく物理モデルを
 *  持ちつつ、Config::floppy_speed(「フロッピーディスク高速化」ON/OFF 設定、
 *  config.h:104 / FDC::ApplyCfg fdc.cpp:328-356 / FDD::ApplyCfg fdd.cpp:441-469)
 *  で 128 hus(= 64µs)固定へ短縮する fast mode を持つ。本サイクルの
 *  ON/OFF トグルは、この XM6 の設計をそのまま precedent としている。
 *  ★ON 時も「遅延ゼロ」にはせず 64µs を残すのは意図的な選択(記号表参照)。
 *
 *  Core は編集しない(CLAUDE.md「Core File Modification Policy」)。代わりに
 *  本ヘッダを fdd.c 専用の COMPILER_FLAGS(-include)で翻訳単位の先頭へ強制
 *  挿入し、usleep をマクロで Bridge 側の切替関数へ差し替える。既存の
 *  Bridge/sasi_io_cache.h(P502 / D-9)と同じ機構であり、新規パターンではない。
 *
 *  ★このヘッダは fdd.c 以外の翻訳単位には force-include されない
 *    (project.pbxproj の "fdd.c in Sources" の COMPILER_FLAGS のみ)。
 *    Core 内で usleep を呼ぶのは fdd.c の上記 3 箇所だけであることを
 *    Core/px68k/x68k/*.c 全体の grep で確認済み(fdc.c は呼んでいない)。
 * =========================================================================== */

#ifndef MX68K_FDD_TIMING_SHIM_H
#define MX68K_FDD_TIMING_SHIM_H

#include <unistd.h>   /* usleep() / useconds_t の元宣言。★マクロ定義より前に置くこと */

#ifdef __cplusplus
extern "C" {
#endif

/* usleep() と戻り値の型・意味を完全互換に保つこと(fdd.c は戻り値を見ていないが、
 * マクロ置換である以上シグネチャ互換であるべき)。
 *  - 高速化 OFF(既定): microseconds をそのまま素の usleep() へ渡す
 *    = P557 以前と完全に同一の挙動。
 *  - 高速化 ON: 引数の値に関わらず常に 64µs(XM6 fast mode 相当)。 */
int mx68k_fdd_usleep_shim(useconds_t microseconds);

#ifdef __cplusplus
}
#endif

/* --- usleep → 切替層への差し替え ------------------------------------------
 * mx68k_fdd_usleep_shim() の実装ファイル自身は素の usleep() を呼ぶ必要がある。
 * ガードなしでこのヘッダを include すると、実装関数の中の usleep() 呼び出しが
 * 自分自身へマクロ置換され、g_fd_fast_access の値に関係なく無条件・無限の
 * 自己再帰(即スタックオーバーフロー)になる。実装ファイルは include の前に
 * FDD_TIMING_SHIM_NO_MACROS を定義すること(sasi_io_cache.h:59 /
 * sasi_io_cache.c:40 と同じ約束事)。 */
#ifndef FDD_TIMING_SHIM_NO_MACROS
#define usleep mx68k_fdd_usleep_shim
#endif

#endif /* MX68K_FDD_TIMING_SHIM_H */
