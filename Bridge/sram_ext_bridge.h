#ifndef MX68K_SRAM_EXT_BRIDGE_H
#define MX68K_SRAM_EXT_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* P493: 内蔵 SRAM 64KB 化 Stage 1(Core 無改変)。
 * 低位 16KB($ED0000-$ED3FFF)は Core 既存の SRAM[0x4000] をそのまま使い、
 * 上位 48KB($ED4000-$EDFFFF)だけを Bridge 所有バッファで提供する。 */

/* MemReadTable/MemWriteTable の index 0x6A-0x6F を差し替える(enabled=false なら
 * Core 既定の SRAM_Read/SRAM_Write へ復元する)。設定値の有効/無効ラッチも兼ねる。
 * init / ハードリセットの「設定値→配線確定」ブロックから呼ぶこと。 */
void sram_ext_install_table(bool enabled);

bool sram_ext_is_enabled(void);

/* 64KB 有効時のみ $dir_path/sram_ext.dat を読み込む(プロセス内で一度だけ)。
 * 無効時・ファイル不在時はバッファを触らない(BSS の 0 のまま)。 */
void sram_ext_load(const char* dir_path);

/* 64KB 有効時のみ $dir_path/sram_ext.dat へ保存する。無効時はファイルに一切触れない。 */
void sram_ext_save(const char* dir_path);

/* P494-①: 上位 48KB($ED4000-$EDFFFF)をゼロクリアする。64KB 設定の有効/無効に
 * 関わらず常にクリアする(「Clear SRAM」= 全 SRAM を消す、という直感的な期待に
 * 合わせる。無効時は元々読み出されないので副作用は無い)。
 * 低位 16KB(Core 所有 SRAM[])のクリアは呼出し側 mx68k_sram_clear() の責務。 */
void sram_ext_clear(void);

/* P494-②: 命令フェッチ専用シャドウ。c68k の Fetch[] は $ED0000-$EDFFFF の 64KB 全域を
 * 単一エントリで扱い範囲チェックを行わないため、低位 16KB(Core 所有 SRAM[])と
 * 上位 48KB(Bridge 所有バッファ)に分かれた実体を「連続した 64KB のホストバッファ」
 * として見せる必要がある。データ read/write 経路(MemReadTable/MemWriteTable)には
 * 一切影響しない。 */

/* SRAM[] / 上位 48KB バッファの現在の生バイト列でシャドウ全体を再構築する。
 * ^1 スワップは一切自前で解決/付与しない(両実体が既に FETCH_WORD/FETCH_LONG の
 * ネイティブ u16 読みで正しい 68k 値になる格納規約を持っているため、生の memcpy
 * 連結が唯一正しい)。 */
void sram_ext_fetch_shadow_rebuild(void);

/* 実書込み完了直後に無条件で呼ぶ。アドレスが $ED0000-$EDFFFF かつ 64KB 有効時のみ
 * 再構築する。書込み許可ゲート等の判定はここで再実装しない(実体側が処理した
 * 「結果」を丸ごと写すだけ)。 */
void sram_ext_fetch_shadow_note(uint32_t addr_raw);

/* Fetch[] の $ED0000-$EDFFFF エントリを配線する。
 * enabled=true  → シャドウを再構築して Fetch[] をシャドウへ向ける。
 * enabled=false → P494 以前と完全に同一($ED0000-$ED3FFF, Core 所有 SRAM[])へ復元。 */
void sram_ext_install_fetch(bool enabled);

/* P495: セーブステート連携。
 *
 * sram_ext_snapshot64() — 低位 16KB(Core 所有 SRAM[])+ 上位 48KB を連結した
 * 64KB($ED0000-$EDFFFF)スナップショットの先頭を返す。呼出し時点で必ず最新化
 * されるため、64KB 設定の有効/無効に関わらずセーブステートへ「常に物理最大」を
 * 書ける(D-11/P472 と同型のパターン)。返り値は Bridge 所有の静的バッファで、
 * 次回の SRAM 書込み/再構築まで有効。
 *
 * sram_ext_restore_upper48() — 64KB 形式のステートに含まれる上位 48KB
 * (呼出し元が blk_sram + 0x4000 を渡す)を書き戻し、Fetch シャドウを追随させる。
 * 低位 16KB の復元(memcpy(SRAM, ...))を先に済ませてから呼ぶこと。 */
const uint8_t* sram_ext_snapshot64(void);
void sram_ext_restore_upper48(const uint8_t* src48);

/* P601: 読み取り専用デバッグビューア(P600 mx68k_read_memory_bytes)からの
 * 単バイト読み出し用。MemReadTable の生きたエントリと同一実装だが、
 * Fetch シャドウ(g_sram_fetch_shadow)には一切触れない——64KB モード有効時に
 * UI スレッドから頻繁に呼ばれても、CPU コアのライブ命令フェッチ元バッファへは
 * 何の影響も与えない。g_sram_ext[] 単体への torn read の可能性は、RAM 等
 * 他領域の直読みで既に許容されているのと同じトレードオフ(単バイト、
 * 範囲外アクセス無し)。 */
uint8_t sram_ext_read(uint32_t addr);

#endif /* MX68K_SRAM_EXT_BRIDGE_H */
