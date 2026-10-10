/* Bridge/midi_note_shadow.h — 内部専用。MIDI Keyboard Viewer(P929)用の
 * 「ゲストが送信したNote On/Offに基づく、現在ON状態のノート」のシャドウ。
 * Swift 向けの契約そのものではなく、値は EmulatorBridge.h 側の
 * mx68k_get_midi_note_status() 経由で公開する。
 *
 * ★ヘッダ+別 TU 定義パターン(memory feedback_instrumentation_layout_adjacency_corruption):
 *   midi_shadow.h と同じく宣言だけをここに置き、定義は MIDI 送出経路そのものを持つ
 *   Bridge/midi_coremidi.c に閉じる。
 *
 * ★スレッド: 書き手はエミュレーションスレッドのみ(p633_midi_send_bytes() と
 *   mx68k_reset_hard())、読み手は CVDisplayLink スレッド(EmulatorEngine)。
 *   モニタ用途で厳密な同時性は不要なため memory_order_relaxed(P693 と同じ契約)。
 *
 * ★意味論: 値は「ゲストが送ったNote On/Off」の結果であり、音源(MT-32/SC-55/外部機器)が
 *   実際に発音中のボイスとは一致しない(リリース中の残響・ボイス数上限による発音打切り・
 *   CC64 ホールドは反映されない)。 */
#ifndef MIDI_NOTE_SHADOW_H
#define MIDI_NOTE_SHADOW_H

#include <stdint.h>
#include <stdatomic.h>

/* [ch 0-15][note 0-127] = 直近 Note On のベロシティ(1-127)、0 = OFF。 */
extern _Atomic(uint8_t) g_midi_note_vel[16][128];

/* 全 16ch × 128 鍵を OFF にする(リセット系メッセージ・mx68k_reset_hard() 用)。 */
void midi_note_shadow_clear_all(void);

#endif /* MIDI_NOTE_SHADOW_H */
