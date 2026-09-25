/* Bridge/opm_shadow.h — 内部専用。m68000_bridge.c(書き手、CPU 書き込みフック経由)
 * と EmulatorBridge.c(読み手/書き手、ステートのセーブ/ロード経由)で共有する。
 * Swift 向けの契約には含まれない。
 *
 * P479 (D-41 症状1): OPM (YM2151) チップには、レジスタファイルを読み戻す Core 側の
 * API が無い(opm.h:88 の GetReg は宣言のみで定義されておらず、class OPM は
 * 生のレジスタ配列を保持しない)ため、ステートのスナップショットはチップ自体から
 * 音色パラメータを取り込めない。そこで Bridge が OPM バスポートへ届くゲストの
 * 書き込みをすべてシャドウし、そのシャドウをステートファイル内へ保存して、
 * ロード時にチップへ再生(リプレイ)する。
 *
 * 既知の制限: DMA 駆動の書き込みは CPU 書き込みフックを通らないため、
 * シャドウされない。OPM を DMA 転送先とする X68000 の音楽ドライバは存在しない
 * (DMAC のデバイス割り当てに OPM の DREQ 相当が無い、dmac.c:398-403)ため、
 * これは理論上の抜けにすぎない。 */
#ifndef OPM_SHADOW_H
#define OPM_SHADOW_H

#include <stdint.h>

/* 各 OPM レジスタ番号へ最後に書き込まれた値(添字 = レジスタ番号)と、
 * レジスタごとの「一度でも書き込まれたか」フラグ。ロード時はゲストが実際に
 * 設定したレジスタだけをリプレイする。 */
extern uint8_t g_opm_shadow[256];
extern uint8_t g_opm_written[256];

/* レジスタ 0x19 は 1 つのレジスタ番号に独立した 2 つの変数を持ち、bit7 で
 * 選択される (opm.cpp:219-221): bit7=1 -> PMD, bit7=0 -> AMD。単純な 256 要素の
 * シャドウでは最後に書かれた方しか残らないため、両者それぞれに専用の格納先 +
 * 書き込み済みフラグを持たせる。 */
extern uint8_t g_opm_reg19_pmd, g_opm_reg19_amd;
extern uint8_t g_opm_reg19_pmd_written, g_opm_reg19_amd_written;

/* レジスタ 0x08 (KEYON) はデータバイト自体にチャンネル番号 (bits0-2) を、
 * bits3-6 にオペレータごとのゲートマスクを持つため、単純な 256 要素のシャドウでは
 * 最後のチャンネルのマスクしか残らない(レジスタ番号は常に 0x08 で、チャンネル
 * ごとには分かれない)。チャンネルごとの専用配列が必要となる。
 * ビット配置は Core/px68k/fmgen/fmgen.cpp:793-799 と照合済み
 * (Channel4::KeyControl): bit0=op0, bit1=op1, bit2=op2, bit3=op3。 */
extern uint8_t g_opm_keyon[8];

/* OPM バスは 2 段ラッチ(アドレス書き込み→データ書き込み)であり、これは
 * Core 内部の CurReg の Bridge 側ミラーである。 */
extern uint8_t g_opm_curreg;

/* シャドウ全体をクリアする。実チップを(再)初期化するすべての箇所から呼ばれ、
 * シャドウとチップの同期を保つ。定義は m68000_bridge.c。 */
void p479_opm_shadow_reset(void);

#endif /* OPM_SHADOW_H */
