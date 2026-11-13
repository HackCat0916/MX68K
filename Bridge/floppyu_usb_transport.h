/* ===========================================================================
 *  P945: ふろっぴU君 USB通信基盤(termios層)[P945-FLOPPYUUSB]
 * ---------------------------------------------------------------------------
 *  TNB製作所「ふろっぴU君」(実機5インチFDDをUSBでホストへ接続するユニット)の
 *  2本のUSBシリアル(CDC ACM)を、IOKitで検出しtermiosで直接扱う通信層。
 *   - port1: 同期レジスタアクセス用(0x00 送信に 'f' で応答する側)
 *   - port2: 非同期通知・データ用(0x00 送信に 'F' で応答する側)。受信は専用
 *            スレッドがSPSCリングへ積む。
 *  判別はインタフェース番号ではなく 0x00 ハンドシェイク応答で行う
 *  (参照: たんぼ氏差分 fdc_bridger.h:86-91)。
 *
 *  ★P1054: 有効条件は3値。環境変数 MX68K_FLOPPYU_USB が "1"=強制ON / "0"=強制OFF /
 *    未設定・その他=設定「Use FloppyU」(config_enabled)に従う(既定 OFF)。
 *    無効時は wired 行1行の出力以外、IOKit・スレッド・dispatch queue を一切生成しない。
 *    init はアプリ起動時と各ハードリセットで呼ばれ、稼働中なら即 return する(冪等)。
 *    稼働中に無効へ変えても通信層は停止しない(停止は mx68k_floppyu_usb_shutdown のみ)。
 *  ★iOS では常に無効(全APIが「未接続」を返すスタブ、IOKit 非リンク)。
 *  ★Swift には公開しない(EmulatorBridge.h・Bridging Header には含めない)。
 *    唯一の例外: P957 の mx68k_floppyu_usb_device_present() は存在検出のみで
 *    状態機械に触れないため EmulatorBridge.h から公開する。
 *    本サイクル(P945)時点では init/shutdown 以外の呼出し元は無い。
 * =========================================================================== */
#ifndef MX68K_FLOPPYU_USB_TRANSPORT_H
#define MX68K_FLOPPYU_USB_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

void   mx68k_floppyu_usb_init(int config_enabled);
int    mx68k_floppyu_usb_is_enabled(void);        /* P1054: 1=通信層を起動済み(以後プロセス終了まで1) */
const char* mx68k_floppyu_usb_enable_src(void);   /* P1054: 起動の由来 "env1"/"config"/"off"(ログ用) */
void   mx68k_floppyu_usb_shutdown(void);          /* 冪等 */
int    mx68k_floppyu_usb_is_connected(void);      /* 1=CONNECTED */
uint32_t mx68k_floppyu_usb_generation(void);      /* 接続ごとに+1、切断検出に使う */

/* port1: 同期。txを全て送り、rxlenバイト受信するまで待つ(rx==NULL/rxlen==0なら送信のみ)。
 * flush_input!=0 なら送信前に入力をフラッシュ(参照実装 Purge 相当)。
 * 戻り値: 受信バイト数(>=0)、-1=未接続、-2=タイムアウト(部分受信分はrxに入る)、-3=I/Oエラー(切断処理を投入済み)。
 * 呼出しスレッドを最大 timeout_ms ブロックする——フレームループから呼ぶ設計は次サイクルで決める。 */
int    mx68k_floppyu_usb_port1_exec(const uint8_t* tx, size_t txlen,
                                    uint8_t* rx, size_t rxlen,
                                    int timeout_ms, int flush_input);

/* port2: 受信は非ブロッキング(リングから取り出すだけ)。戻り値=取り出したバイト数(未接続は0)。
 * 消費者は1スレッドのみ(SPSC)。 */
size_t mx68k_floppyu_usb_port2_read(uint8_t* buf, size_t cap);
int    mx68k_floppyu_usb_port2_rx_overflowed(void);   /* sticky、次の接続で0に戻る */
/* port2: 送信は同期(参照実装の 'W' 応答 = 0x4D+データ返送用)。戻り値は送信バイト数、
 * それ以外は port1_exec と同じ規約(-1/-2/-3)。 */
int    mx68k_floppyu_usb_port2_write(const uint8_t* buf, size_t len, int timeout_ms);

/* P957: ポートを開かない軽量な存在検出(環境変数・状態機械とは独立、任意スレッド可)。
 * 戻り値: >=0 = VID/PID一致のIOUSBHostDevice数 / -1 = IOKit照会失敗 / -2 = 非対応(iOS)。
 * 初回と結果変化時のみ [P957-FLOPPYUPRESENT] 行を出力する。 */
int32_t mx68k_floppyu_usb_device_present(void);

#ifdef MX68K_FLOPPYU_USB_SELFTEST
/* 単体テスト専用: IOKit走査を介さず、指定パスでハンドシェイク〜接続確立を行う。0=接続成功 / -1=失敗 */
int    mx68k_floppyu_usb_selftest_connect(const char* path_a, const char* path_b);
/* 単体テスト専用: 3候補以上の構成(片側無応答等)を試すための n パス版 */
int    mx68k_floppyu_usb_selftest_connect_n(const char* const* paths, int n);
void   mx68k_floppyu_usb_selftest_disconnect(const char* reason);
/* 単体テスト専用: s_queue上からshutdownを呼ぶ */
void   mx68k_floppyu_usb_selftest_shutdown_on_queue(void);
/* 単体テスト専用: 受信スレッドがループ1周ごとに呼ぶフックを登録(NULLで解除) */
void   mx68k_floppyu_usb_selftest_set_rx_hook(void (*hook)(void));
/* 単体テスト専用: 現接続の受信バイト数・溢れ破棄バイト数 */
void   mx68k_floppyu_usb_selftest_rx_stats(uint64_t* rx_bytes, uint64_t* rx_overflow);
/* 単体テスト専用: 任意の VID/PID で一致数を数える(本体と同じ照合ロジック)。
 * vid/pid<0 なら照合しない(戻り値0)。*usb_total_out に全 IOUSBHostDevice 数。-1=IOKit照会失敗。 */
int32_t mx68k_floppyu_usb_selftest_count_vidpid(int vid, int pid, int* usb_total_out);
#endif

#endif /* MX68K_FLOPPYU_USB_TRANSPORT_H */
