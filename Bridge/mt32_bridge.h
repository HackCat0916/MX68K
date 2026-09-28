/*
 * Bridge/mt32_bridge.h — 内蔵 MIDI 音源(MT-32 / mt32emu)の Bridge 内部向け宣言(P825)
 *
 * Swift から呼ぶ公開 API(mx68k_request_mt32_reconfigure / mx68k_mt32_render_mix /
 * mx68k_set_midi_output_destination)は EmulatorBridge.h 側で宣言する。ここに置くのは
 * Bridge の C ファイル同士でだけ使う関数に限る。
 */
#ifndef MX68K_MT32_BRIDGE_H
#define MX68K_MT32_BRIDGE_H

#include <stdint.h>

/* mx68k_run_frame() / mx68k_pump_pending() のフレーム境界(consume_pending_ops)から
 * 呼ぶ。★エミュレーションスレッド専用。pending 要求が無ければ atomic load 1 回で戻る。 */
void mx68k_mt32_apply_pending_reconfigure(void);

/* p633_midi_send_bytes() から呼ぶ。★エミュレーションスレッド専用(ロック不要)。 */
void mt32_bridge_send_bytes(const uint8_t *data, uint32_t len);

/* mx68k_shutdown() から呼ぶ。context が残っていれば close + free する。 */
void mx68k_mt32_shutdown(void);

#endif /* MX68K_MT32_BRIDGE_H */
