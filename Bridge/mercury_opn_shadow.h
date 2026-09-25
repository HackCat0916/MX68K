/* Bridge/mercury_opn_shadow.h — 内部専用。m68000_bridge.c(書き手、CPU 書き込み
 * フック経由)と EmulatorBridge.c(読み手、mx68k_get_mercury_opn_status 経由)で
 * 共有する。Swift 向けの契約には含まれない。
 *
 * P491: Mercury Unit の FM 部 (YMF288 / OPN3-L) はチップから読み戻せない。
 * Y288::GetReg (opna.cpp:2180-2189) は addr<0x10 で PSG レジスタ群、addr==0xff で
 * モードバイト、それ以外はすべて 0 を返す — つまり FM (0x30-0xB6, 0x130-0x1B6)
 * とリズム (0x10-0x1D) のレジスタは読めず、読める SSG 経路はゲストのアドレス
 * ラッチを不正な値にしてしまう (M288_Read のポート 0 が CurReg へ書き込む、
 * fmg_wrap.cpp:207-208)。そこで Bridge は OPN バスポートへ届くゲストの
 * 書き込みをすべてシャドウし、サウンドモニタの表示はそのシャドウから導出する —
 * P479 の OPM シャドウと同じ設計であり、理由も同じである。
 *
 * 既知の制限 (P479 と同じ): DMA 駆動の書き込みは CPU 書き込みフックを
 * 通らないため、シャドウされない。
 *
 * 既知の制限 (Core/上流由来、MX 固有ではない): Mcry_Write はチップのポートを
 * (adr>>1)&3 で算出し、これは常に 0-3 なので、M288_Write が 2 個目の YMF288
 * (ymf288b、port>3 でのみ到達、fmg_wrap.cpp:301-305) を選択することはない。
 * ゲストが到達できるのは常に 1 チップ = FM 6ch + SSG 3ch のみ。上流の
 * px68k x68k/mercury.c:210-217 も同一なので、
 * 本シャドウは意図的に同じ
 * 1 チップのみの到達範囲を再現している。 */
#ifndef MERCURY_OPN_SHADOW_H
#define MERCURY_OPN_SHADOW_H

#include <stdint.h>

/* OPN バスはバンク切り替え式のため、レジスタファイルは 512 要素とする: バンク 0 が
 * 0x000-0x0FF、バンク 1 が 0x100-0x1FF を担う (YMF288::WriteIO がバンク 1 に
 * 0x100 を加算する、fmg_wrap.cpp:205-209)。g_mcry_opn_written はレジスタごとの
 * 「一度でも設定されたか」フラグ。 */
#define MCRY_OPN_REG_COUNT 512
extern uint8_t g_mcry_opn_shadow[MCRY_OPN_REG_COUNT];
extern uint8_t g_mcry_opn_written[MCRY_OPN_REG_COUNT];

/* OPN バスはバンクごとに 1 つずつの 2 段ラッチ(アドレス書き込み→データ書き込み)
 * であり、これは Core 内部の CurReg[2] の Bridge 側ミラーである。 */
extern uint8_t g_mcry_opn_curreg[2];

/* レジスタ 0x28 (FM KEYON) は OPM レジスタ 0x08 と全く同じ構造上の問題を抱える
 * (opm_shadow.h:34-40 参照): レジスタ番号は常に 0x28 で、チャンネル番号は
 * データバイトに埋め込まれるため、単純なシャドウでは最後に書かれたチャンネルしか
 * 残らない。チャンネル番号の算出式は opna.cpp:527-533 の
 * c = (data&3) + (data&4 ? 3 : 0) で、(data&3) < 3 の間のみ有効。 */
extern uint8_t g_mcry_opn_keyon[6];

/* レジスタ 0x10 (リズム KEYON/DUMP) は単純な最終値レジスタではなく累積型である:
 * bit7=0 でビットをセット (|= data&0x3f)、bit7=1 でクリア (&= ~data)
 * (opna.cpp:2074-2089)。生のシャドウを読むと大半のフレームで 0 になってしまう。 */
extern uint8_t g_mcry_rhythmkey;

/* シャドウが受け付けたゲスト書き込みの総数。モニタ用の分母であり、「設定済みで
 * 現在は無音」と「Mercury OPN にどのソフトも一度も触れていない」を区別する。
 */
extern uint32_t g_mcry_opn_write_count;

/* P636: Mercury の PCM 出力サンプル値(Mcry_OutDataL/R)が毎スキャンラインの
 * ポーリングで前回値から変化した累積回数。OPN レジスタ書込みの
 * g_mcry_opn_write_count とは別カウンタ(混同しないこと) — こちらは FM 側を
 * 一切通らない PCM 経路だけを見て、サウンドモニタの「Mercury PCM」セクションで
 * 「PCM が動いているが無音」と「そもそも PCM 活動が一度も無い」を区別する
 * 分母として使う。書き手は m68000_bridge.c の p636_mercury_pcm_poll_samples()、
 * 読み手は EmulatorBridge.c の mx68k_get_mercury_pcm_status()。
 *
 * ★P635 の書込みフック方式(g_mcry_pcm_write_count)を置き換えたもの。旧方式は
 * CPU 命令駆動の書込みしかフックできず、PCM8PP のように DMAC ch2 経由で
 * Mercury へ転送するソフト(P492 の逆アセンブル調査で確認済み)を構造的に
 * 検出できなかった。Core 側が保持する最新サンプル値を直接ポーリングする本方式は
 * CPU 書込み・DMAC 書込みのいずれの経路でも活動を検出できる。 */
extern uint32_t g_mcry_pcm_sample_change_count;

/* シャドウ全体をクリアする。実チップを(再)初期化するすべての箇所から呼ばれ、
 * シャドウとチップの同期を保つ。定義は m68000_bridge.c。 */
void p491_mercury_opn_shadow_reset(void);

#endif /* MERCURY_OPN_SHADOW_H */
