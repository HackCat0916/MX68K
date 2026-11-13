/* ===========================================================================
 *  P1004: ふろっぴU君 最小垂直スライス(使い捨て診断)[P1004-SLICE]
 * ---------------------------------------------------------------------------
 *  環境変数 MX68K_FLOPPYU_SLICE_TEST="1" かつ MX68K_FLOPPYU_USB="1" のときだけ、
 *  起動時に専用スレッドで1回、実機ドライブ0のトラック0/サイド0/セクタ1(N=3)を
 *  READ DATA で読み、送受信の生値とデータ全体を debug.log へ記録する。
 *  送信列は固定(READ系のみ、WRITE/FORMAT系コマンドは一切含まない)。
 *  どちらかの環境変数が "1" でなければ、ログ0行・スレッド0・USBアクセス0。
 *  iOS ではスタブ(何もしない)。
 *  ★Swift には公開しない(EmulatorBridge.h・Bridging Header には含めない)。
 *  送信バイト列・期待値の根拠は .mx68k_cycles/P1004_plan.md 記号表。
 * =========================================================================== */
#ifndef MX68K_FLOPPYU_VSLICE_H
#define MX68K_FLOPPYU_VSLICE_H

/* run_once() の戻り値 */
#define MX68K_VSLICE_RC_OK             0    /* フレーム受信・リザルト正常 */
#define MX68K_VSLICE_RC_RESULT_NG      1    /* フレームは揃ったがリザルト/長さが異常 */
/* -1〜-8 = そのステップ番号で失敗 */
#define MX68K_VSLICE_RC_ABORTED      (-98)
#define MX68K_VSLICE_RC_NOT_CONNECTED (-99)
#define MX68K_VSLICE_RC_UNSUPPORTED  (-97)  /* iOS */

/* mx68k_init() から1回。環境変数判定→専用スレッド起動。
 * floppyu_enabled は呼出し時点の振り分け器設定(干渉が無いことの確認用にログへ出すだけ)。 */
void mx68k_floppyu_vslice_start(int floppyu_enabled);
/* mx68k_shutdown() から(通信層の shutdown より前に)。中断要求→join。冪等。 */
void mx68k_floppyu_vslice_shutdown(void);
/* 同期実行本体(呼出しスレッドをブロックする)。単体テストからも直接呼ぶ。 */
int  mx68k_floppyu_vslice_run_once(int connect_wait_ms);
/* P1009: スライスが起動済み(port2 の SPSC 消費者を使っている)なら1。iOS は常に0。
 * 振り分け器はこれが1の間は実機中継をしない(port2 の消費者を1つに保つため)。 */
int  mx68k_floppyu_vslice_claimed(void);

/* EmulatorBridge.c で定義する内部用関数: debug_log() を強制ONにし、以後
 * mx68k_set_debug_log_enabled(0) を無視させる(Release ビルドでもログを残すため)。 */
void mx68k_debug_log_force_on(void);

#endif /* MX68K_FLOPPYU_VSLICE_H */
