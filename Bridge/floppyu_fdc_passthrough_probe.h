/* ===========================================================================
 *  P949: ふろっぴU君対応検討向け FDC テーブル差替え配線の素通し診断プローブ
 *        [P949-FDCPT](診断専用、既定 OFF)
 * ---------------------------------------------------------------------------
 *  案C(FDC レジスタ層の Bridge 製フロントエンド)の本実装に先立ち、
 *  MemReadTable[0x4A]/MemWriteTable[0x4A] の実行時差替えと
 *  DMA_SetReadyCB(0, …) の差替えを「素通し」で配線しても、現行の
 *  Core FDC/DMAC 駆動フローが阻害されないことを実行時に確認する。
 *
 *  ★P958: config.json の設定値(mx68k_set_floppyu_config 経由)で有効、既定 OFF。
 *    OFF ではテーブルも ReadyCB も差し替えない(前回差し替えていれば保存済み元値へ戻す)。
 *    本ヘッダの関数自体は Swift へ公開しない(公開 API は EmulatorBridge.h の setter のみ)。Core は無改変。
 * =========================================================================== */
#ifndef MX68K_FLOPPYU_FDC_PASSTHROUGH_PROBE_H
#define MX68K_FLOPPYU_FDC_PASSTHROUGH_PROBE_H

#include <stdint.h>

/* DMA_Init() の直後ごと(init / hardreset)に呼ぶ。設定値から毎回再計算し、有効時は
 * テーブル 0x4A と DMA ch0 の ReadyCB を振り分け器へ差し替え、無効時は元値へ戻す(冪等)。
 * assigned_base は 0(ドライブ0/1)または 2(ドライブ2/3、ext_fdd_wired 時のみ有効)。 */
void mx68k_floppyu_fdcpt_install(const char *site, int enabled, int32_t assigned_base, int ext_fdd_wired);

/* フレーム末に1回呼ぶ。有効時のみ 300 フレームごとに集計行を出す。 */
void mx68k_floppyu_fdcpt_tick_frame(void);

/* P951: DMA_Exec(0) 呼出し直後ごとに呼ぶ。有効時のみ IOC_IntStat/MTC を読取り集計する。 */
void mx68k_floppyu_fdcpt_observe_dma0_post(void);

/* P1054: 設定「Allow writing to FloppyU disks」の値を渡す。install 直前に呼ぶ(反映は install のモード解析、
 * 環境変数 MX68K_FLOPPYU_WRITE が設定されていればそちらが優先)。 */
void mx68k_floppyu_fdcpt_set_write_config(int enabled);

/* P1067: 設定「Allow FORMAT on FloppyU disks」の値を渡す。install 直前に呼ぶ(反映は install、normal かつ設定由来のときだけ有効)。 */
void mx68k_floppyu_fdcpt_set_format_config(int enabled);

/* P1089: 設定「書込みモード」(0=安全優先 / 1=高速)の値を渡す。install 直前に呼ぶ(反映は install、normal かつ設定由来のときだけ有効)。 */
void mx68k_floppyu_fdcpt_set_write_fast_config(int fast);

/* P1054: mx68k_shutdown から呼ぶ。開いている [P1054-E5P] 観測窓を閉じ、累積要約を出す(冪等)。 */
void mx68k_floppyu_fdcpt_shutdown(void);

/* P1054: 書込み状態の取得(設定画面用、読取りのみ)。EmulatorBridge.h でも Swift へ公開する。 */
void mx68k_floppyu_get_write_status(int32_t* mode, int32_t* latched, int32_t* env_override);

/* P1067: FORMAT 状態の取得(設定画面用、読取りのみ)。EmulatorBridge.h でも Swift へ公開する。 */
void mx68k_floppyu_get_format_status(int32_t* on, int32_t* halted, int32_t* tracks_ok, int32_t* src);

/* P1089: 書込みモード「高速」の状態取得(設定画面用、読取りのみ)。EmulatorBridge.h でも Swift へ公開する。 */
void mx68k_floppyu_get_write_fast(int32_t* fast, int32_t* src, int32_t* dirty, int32_t* cached_unverified,
                                  int32_t* v_mismatch_cached, int32_t* discarded);

#ifdef MX68K_FLOPPYU_USB_SELFTEST
/* P1054: 単体テスト専用。観測関数(e5p_*)の呼出し前後で振り分け器の状態が同一なら1 */
int mx68k_floppyu_fdcpt_selftest_e5p_invariance(void);
/* P1055: 単体テスト専用。判別保留の状態(st: 0=I 1=H1 2=H2)・ユニット別エポック・基準値・'D'/'d' 受信数・FDD割込み発行待ち */
void mx68k_floppyu_fdcpt_selftest_nm_attr(int* st, uint32_t ue[4], int base_ok[4], uint8_t base_val[4], uint8_t* oc_mask,
                                         uint32_t* D_events, uint32_t* dl_events, int* fi_pending);
/* P1064: 単体テスト専用。fmt1 の次の1回の FORMAT 送信で、装置へ送らずに送信関数の戻り値を r(-1/-2/-3)として扱う(0=解除) */
void mx68k_floppyu_fdcpt_selftest_fmt1_force_send_ret(int r);
/* P1067: 単体テスト専用。normal FORMAT の次の1回の送信で、装置へ送らずに送信関数の戻り値を r として扱う(0=解除) */
void mx68k_floppyu_fdcpt_selftest_nmf_force_send_ret(int r);
/* P1067: 単体テスト専用。世代を変えずに次の tick で再ブリングアップさせる */
void mx68k_floppyu_fdcpt_selftest_force_rebringup(void);
/* P1067: 単体テスト専用。normal FORMAT の走行・停止・検証予約・PCN 記録の生値 */
void mx68k_floppyu_fdcpt_selftest_nmf_state(int* run_active, uint32_t* run_pos, int* halt, int* partial, int* verify_pending,
                                           int pcn_valid[4], uint8_t pcn[4]);
/* P1089: 単体テスト専用。高速モードの装置キャッシュ推定状態と port1 往復上限 */
void mx68k_floppyu_fdcpt_selftest_fast_state(int* nm_fast, int* dirty, int* flush_req, int* flush_src, uint32_t* est,
                                            uint32_t* pend_n, int* pend_ovf, uint8_t* last_us, int* timeout_ms);
/* P1089: 単体テスト専用。B-1 の確定式を経ずに高速モードの内部フラグを立てる/落とす */
void mx68k_floppyu_fdcpt_selftest_force_nm_fast(int v);
/* P1089: 単体テスト専用。normal の照合表1エントリ(out = written, pending, cached, uncertain) */
void mx68k_floppyu_fdcpt_selftest_nm_sec(int us, uint8_t c, uint8_t h, uint8_t r, int out[4]);
/* P1089: 単体テスト専用。振り分け器のコマンドバッファの写し */
void mx68k_floppyu_fdcpt_selftest_cmd(uint8_t out[10], uint32_t* n);
/* P1089: 単体テスト専用。normal の書込みラッチを立てる */
void mx68k_floppyu_fdcpt_selftest_latch(void);
#endif

#endif /* MX68K_FLOPPYU_FDC_PASSTHROUGH_PROBE_H */
