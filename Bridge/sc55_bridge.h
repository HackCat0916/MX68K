/*
 * Bridge/sc55_bridge.h — 内蔵 MIDI 音源(SC-55 / Nuked-SC55)の Bridge 内部向け宣言(P826)
 *
 * Swift から呼ぶ公開 API(mx68k_sc55_* / mx68k_set_midi_output_destination)は
 * EmulatorBridge.h 側で宣言する。ここに置くのは Bridge の C ファイル同士でだけ使う関数に限る。
 */
#ifndef MX68K_SC55_BRIDGE_H
#define MX68K_SC55_BRIDGE_H

#include <stdint.h>

/* p633_midi_send_bytes() から呼ぶ。★エミュレーションスレッド専用(入力リングの唯一の書き手)。
 * 入力リングへ積むだけで、Nuked-SC55 コア(X68SC55_Send)はここでは呼ばない。
 * iOS ではスタブ(バイトを静かに破棄する)。 */
void sc55_bridge_send_bytes(const uint8_t *data, uint32_t len);

#endif /* MX68K_SC55_BRIDGE_H */
