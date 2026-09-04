/* Bridge/mercury_opn_shadow.h — internal only, shared between m68000_bridge.c
 * (writer, via the CPU-write hook) and EmulatorBridge.c (reader, via
 * mx68k_get_mercury_opn_status). Not part of the Swift-facing contract.
 *
 * P491: the Mercury Unit's FM part (YMF288 / OPN3-L) cannot be read back from
 * the chip. Y288::GetReg (opna.cpp:2180-2189) returns the PSG file for
 * addr<0x10, a mode byte for addr==0xff, and 0 for everything else — so the FM
 * (0x30-0xB6, 0x130-0x1B6) and rhythm (0x10-0x1D) registers are unreadable, and
 * the SSG path that IS readable would corrupt the guest's address latch
 * (M288_Read port 0 writes CurReg, fmg_wrap.cpp:207-208). The Bridge therefore
 * shadows every guest write that reaches the OPN bus ports and derives the
 * sound monitor's readout from that shadow — same design as the P479 OPM
 * shadow, for the same reason.
 *
 * Known limitation (same as P479): DMA-driven writes do not pass through the
 * CPU write hook and are therefore not shadowed.
 *
 * Known limitation (Core/upstream, not MX-specific): Mcry_Write computes the
 * chip port as (adr>>1)&3, which is always 0-3, so M288_Write never selects the
 * second YMF288 (ymf288b, reached only by port>3, fmg_wrap.cpp:301-305). The
 * guest can only ever reach one chip = 6 FM channels + 3 SSG channels. Upstream
 * px68k x68k/mercury.c:210-217 is identical, so this shadow deliberately
 * mirrors the same single-chip reach. */
#ifndef MERCURY_OPN_SHADOW_H
#define MERCURY_OPN_SHADOW_H

#include <stdint.h>

/* Register file is 512 entries because the OPN bus is banked: bank 0 covers
 * 0x000-0x0FF and bank 1 covers 0x100-0x1FF (YMF288::WriteIO adds 0x100 for
 * bank 1, fmg_wrap.cpp:205-209). g_mcry_opn_written is the per-register
 * "has ever been programmed" flag. */
#define MCRY_OPN_REG_COUNT 512
extern uint8_t g_mcry_opn_shadow[MCRY_OPN_REG_COUNT];
extern uint8_t g_mcry_opn_written[MCRY_OPN_REG_COUNT];

/* The OPN bus is a two-stage latch (address write, then data write), one latch
 * per bank; this is the Bridge-side mirror of the Core-internal CurReg[2]. */
extern uint8_t g_mcry_opn_curreg[2];

/* Register 0x28 (FM KEYON) has exactly the structural problem OPM register 0x08
 * has (see opm_shadow.h:34-40): the register number is always 0x28 and the
 * channel number is encoded in the data byte, so a plain shadow keeps only the
 * last channel written. Channel index formula is opna.cpp:527-533,
 * c = (data&3) + (data&4 ? 3 : 0), valid only while (data&3) < 3. */
extern uint8_t g_mcry_opn_keyon[6];

/* Register 0x10 (rhythm KEYON/DUMP) is cumulative, not a plain last-value
 * register: bit7=0 sets bits (|= data&0x3f), bit7=1 clears them (&= ~data)
 * (opna.cpp:2074-2089). Reading the raw shadow would show 0 for most frames. */
extern uint8_t g_mcry_rhythmkey;

/* Total number of guest writes accepted by the shadow. This is the denominator
 * for the monitor: it distinguishes "programmed, currently silent" from "no
 * software has ever touched the Mercury OPN". */
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

/* Clear the whole shadow. Called wherever the real chip is (re)initialised, so
 * shadow and chip stay in sync. Defined in m68000_bridge.c. */
void p491_mercury_opn_shadow_reset(void);

#endif /* MERCURY_OPN_SHADOW_H */
