/* Bridge/midi_shadow.h — internal only. MIDI 送受信の累積カウンタ
 * (P693 MIDI Viewer 用)。Swift 向けの契約そのものではなく、値は
 * EmulatorBridge.h 側の mx68k_midi_get_* getter 経由で公開する。
 *
 * P693: MIDI ボード(CZ-6BM1 / YM3802)は P488/P490 で完全実装済みだが、
 * ランプもモニタも無く有効化後は完全に不可視だった。本ヘッダはその可視化の
 * ためのカウンタ群を宣言する。
 *
 * ★ヘッダ+別 TU 定義パターンの理由(memory
 *   feedback_instrumentation_layout_adjacency_corruption): 計装用グローバルを
 *   無関係なグローバル群の隣へ足すとメモリレイアウトが変わり実害が出た前例が
 *   あるため、Bridge/opm_shadow.h・mercury_opn_shadow.h と同じく宣言だけを
 *   ヘッダに置き、定義は専用 TU(ここでは Bridge/midi_coremidi.c、MIDI 送受信
 *   経路そのものを持つファイル)に閉じる。
 *
 * ★スレッドと _Atomic の必要性:
 *   - TX 側(g_midi_tx_*)は p633_midi_send_bytes() で加算される。呼び出し元は
 *     ゲスト CPU 実行、すなわちエミュレーションスレッド。
 *   - RX 側(g_midi_rx_*)は mid_In_callback() で加算される。こちらは CoreMIDI
 *     専用コールバックスレッド。
 *   読み手はいずれも CVDisplayLink スレッド(EmulatorEngine の
 *   fetchMonitorsAndPerfStats)なので、双方ともスレッドをまたぐ。よって
 *   MX68K_AudioBufferStatus(CoreAudio 実時間スレッド更新 → 表示側読み、
 *   Bridge/EmulatorBridge.h:771-779・EmulatorBridge.c:968-982)と同じ
 *   _Atomic + memory_order_relaxed の組合せを踏襲する。
 *
 * ★relaxed の含意: 複数フィールドの厳密な同時性は保証しない(messages と
 *   bytes と last が別々の瞬間の値になりうる)。モニタ表示専用であり、
 *   1 tick 分のずれは実害にならないため意図的にこの単純さを採る
 *   —— seqlock 相当の仕掛けは導入しない。
 *
 * ★本カウンタは既存の Rx_buff / RxW_point / RxR_point / MIDI_IntFlag /
 *   MIDI_IntVect(D-73 として Docs/09 に起票済みの、2 スレッド間無同期共有)を
 *   一切読み書きしない。既知の競合に新たな読み手を足さないための設計上の制約。
 */
#ifndef MIDI_SHADOW_H
#define MIDI_SHADOW_H

#include <stdint.h>
#include <stdatomic.h>

/* ---- TX(ゲスト → 実 MIDI 機器)---- */

/* p633_midi_send_bytes() が CoreMIDI へ引き渡した回数と総バイト数。
 * messages は「ゲストが何回送ろうとしたか」、bytes は「実際に何バイト
 * 渡したか」——両方を並べて表示することで、送出層で落ちている
 * (messages>0 かつ bytes==0)のか、外部機器側の問題(bytes>0 で無音)なのかを
 * 1 画面で切り分けられるようにするための分母/分子の対。 */
extern _Atomic(unsigned long long) g_midi_tx_messages;
extern _Atomic(unsigned long long) g_midi_tx_bytes;

/* 直近 1 メッセージのパック値: status | len<<8 | data1<<16 | data2<<24。
 * 1 回の atomic 読み書きで直近メッセージを表現するための新規エンコード
 * (複数 atomic に分けると seqlock 相当の同期が要るため、1 ワードに畳む)。
 *
 * ★パッキング契約(P693 Code Review 指摘により明文化):
 *   - len は 8bit しかないため min(実長, 0xff) へクランプした「表示用の
 *     目安値」であり、実長そのものではない。SysEx は最大
 *     P633_MIDI_MAX_MSG_BYTES(1024)まで届きうる。
 *   - data1 は実長 >= 2 のときだけ data[1] を読む。それ未満なら 0 固定。
 *   - data2 は実長 >= 3 のときだけ data[2] を読む。それ未満なら 0 固定。
 *   すなわち呼び出し元バッファの実長を超えて読み取らない(1 バイト
 *   メッセージ、例えば 0xf8 クロックバイトを安全に扱うための必須ガード)。 */
extern _Atomic(uint32_t) g_midi_tx_last;

/* ---- RX(実 MIDI 機器 → ゲスト)---- */

/* mid_In_callback() が受信したバイト数と、その元になったパケット数。
 * TX と同じく分母/分子の対として並べて表示する。 */
extern _Atomic(unsigned long long) g_midi_rx_messages;
extern _Atomic(unsigned long long) g_midi_rx_bytes;

/* 直近 1 メッセージのパック値。TX 側とまったく同じ形式・同じ契約。 */
extern _Atomic(uint32_t) g_midi_rx_last;

#endif /* MIDI_SHADOW_H */
