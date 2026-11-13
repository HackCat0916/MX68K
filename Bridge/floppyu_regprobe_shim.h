/* ===========================================================================
 *  P939: FDC レジスタアクセス観測プローブ [P939-FDCREG] の差し替え層
 * ---------------------------------------------------------------------------
 *  ふろっぴU君(実機 5inch FDD を USB 経由でホストへ接続するユニット)対応の
 *  検討材料として、ゲスト(IOCS/FDC ドライバ)が FDC レジスタ
 *  ($E94001/3/5/7)へ実際にどの頻度・どのパターンでアクセスしているかを
 *  観測する。診断専用であり、エミュレーション挙動は変更しない。
 *
 *  Core は編集しない(CLAUDE.md「Core File Modification Policy」)。代わりに
 *  本ヘッダを mem_wrap.c 専用の COMPILER_FLAGS(-include)で翻訳単位の先頭へ
 *  強制挿入し、MemReadTable/MemWriteTable に登録される FDC_Read/FDC_Write を
 *  マクロで Bridge 側の観測関数へ差し替える。既存の Bridge/sasi_io_cache.h
 *  (P502 / D-9)・Bridge/fdd_timing_shim.h(P557)と同じ機構であり、新規
 *  パターンではない。
 *
 *  ★このヘッダは mem_wrap.c 以外の翻訳単位には force-include されない
 *    (project.pbxproj の "mem_wrap.c in Sources" の COMPILER_FLAGS のみ)。
 *    fdc.c 自身の定義や、Bridge 内の診断コードが直接呼ぶ FDC_Read は
 *    従来どおり素の実装を指す(=観測対象はメモリマップ経由のアクセスのみ)。
 *
 *  ★環境変数 MX68K_FLOPPYU_PROBE が未設定(既定)なら、観測関数は元の
 *    FDC_Read/FDC_Write を呼ぶだけで、ログは初回の配線確認マーカー1行のみ。
 * =========================================================================== */

#ifndef MX68K_FLOPPYU_REGPROBE_SHIM_H
#define MX68K_FLOPPYU_REGPROBE_SHIM_H

#include <stdint.h>
#include "common.h"   /* FASTCALL の元定義(Core/px68k/common.h、fdc.h と同じ参照先) */

#ifdef __cplusplus
extern "C" {
#endif

/* fdc.h の FDC_Read/FDC_Write とシグネチャ(FASTCALL 修飾含む)を完全互換に
 * 保つこと。マクロ置換後の fdc.h の宣言がこのプロトタイプと一致する必要がある。 */
uint8_t FASTCALL mx68k_floppyu_fdc_read_probe(uint32_t adr);
void    FASTCALL mx68k_floppyu_fdc_write_probe(uint32_t adr, uint8_t data);

/* レジスタ種別ごとの総カウント(ヒストグラム)と末尾サンプルを出力する。
 * 環境変数 OFF 時は何もしない。atexit からも自動で呼ばれる。 */
void mx68k_floppyu_regprobe_dump(void);

#ifdef __cplusplus
}
#endif

/* --- FDC_Read/FDC_Write → 観測関数への差し替え ----------------------------
 * floppyu_regprobe.c 自身は元の FDC_Read/FDC_Write を呼ぶため、
 * FLOPPYU_REGPROBE_SHIM_NO_MACROS を定義してこのブロックを無効化する
 * (定義を怠ると観測関数が自分自身を呼び続ける無限再帰になる)。 */
#ifndef FLOPPYU_REGPROBE_SHIM_NO_MACROS
#define FDC_Read   mx68k_floppyu_fdc_read_probe
#define FDC_Write  mx68k_floppyu_fdc_write_probe
#endif

#endif /* MX68K_FLOPPYU_REGPROBE_SHIM_H */
