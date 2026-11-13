/* ===========================================================================
 *  P940: FDC/DMA 人為的遅延実験 [P940-FDLATENCY](実験的機能、既定 OFF)
 * ---------------------------------------------------------------------------
 *  将来の「ふろっぴU君」(USB 接続の実機 FDD)対応に向け、FD アクセスに
 *  数十〜数百 ms のレイテンシが乗ったとき IPL-ROM IOCS / Human68k が
 *  耐えるかを観測するための実験。DMA ch0 の Ready コールバックを
 *  DMA_SetReadyCB() で差し替え、FDD_Seek/Read/Write(usleep シム)ごとに
 *  ゲスト時間で delay_ticks の間だけ転送開始を保留する。
 *
 *  ★環境変数 MX68K_FLOPPYU_DELAY_MS(1..10000、10進整数)が未設定/無効なら
 *    DMA_SetReadyCB を一切呼ばず、エミュレーション状態は従来と完全に同一。
 *  ★macOS 限定: iOS には起動時環境変数を渡す通常手段が無いため常に OFF。
 *  ★P959: 環境変数 MX68K_FLOPPYU_DELAY_SCOPE(カンマ区切り)に "irq" を含めると、
 *    FDC 完了割込み(レベル1)も発生から delay_ticks の間 CPU へ届けず保留し、
 *    保留明けに再発行する(Seek/Recalibrate など DMA を伴わないコマンドも対象)。
 *  ★Swift には公開しない(EmulatorBridge.h は無変更)。Core は無改変。
 * =========================================================================== */
#ifndef MX68K_FD_LATENCY_EXPERIMENT_H
#define MX68K_FD_LATENCY_EXPERIMENT_H

#include <stdint.h>

/* mx68k_init() 中、DMA_Init() より前に1回。配線行を無条件に1行出す。 */
void mx68k_fdlat_init(int fd_fast_access);

/* DMA_Init() 呼出しの直後に必ず呼ぶ(install とは別関数・別呼出し)。
 * dmainit_gen を加算するだけで DMA_SetReadyCB は呼ばない。 */
void mx68k_fdlat_note_dma_init(const char *site);

/* note_dma_init の直後に呼ぶ。enabled=1 時のみ DMA_SetReadyCB(0, ...) で
 * 差し替え関数を登録し inst_gen を加算する。 */
void mx68k_fdlat_install(const char *site);

/* mx68k_fdd_usleep_shim() の先頭で呼ぶ。遅延を開始(アーム)する。 */
void mx68k_fdlat_arm(unsigned usec_req, int fd_fast_access);

/* 走査線ループの vl++ 直前で呼ぶ。ゲスト時間(名目10MHzチック)を加算する。 */
void mx68k_fdlat_tick_line(int32_t fc, int32_t vl, int32_t vt);

/* Bridge の DMA_Exec(0) 呼出し直前で呼ぶ。DMA[0] を読むだけ(書かない)。 */
void mx68k_fdlat_observe_site(void);

/* consume_pending_ops() 内 do_load_state() の直後で呼ぶ。ゲートを開放側へ。 */
void mx68k_fdlat_reset(int load_rc);

/* 集計行を明示的に1行出す(atexit からも呼ばれる)。 */
void mx68k_fdlat_dump(const char *reason);

/* P959: FDC 割込み保留+再発行(MX68K_FLOPPYU_DELAY_SCOPE に "irq" を含む時のみ有効)。
 * 0x4A(FDC)アクセスの直前に pre を呼び、Core 関数呼出し後に戻り値を post へ渡す。
 * pre の戻り値: bit0=アクセス前の IRQH_IRQ[1]、bit1=アクセス前の保留中フラグ
 * (irq_scope 無効時は常に 0)。kind は p939_kind_of() の分類値(1=DATA)。 */
uint8_t mx68k_fdlat_fdc_pre(void);
void    mx68k_fdlat_fdc_post(uint8_t pre, int kind, int is_read);

/* P961: MSR(RQM)busy 偽装(SCOPE に "msr" と "irq" の両方を含む時のみ有効)。
 * FDC 完了割込み保留中を「USB 往復中で応答できない」状態とみなす通信モデルの近似で、
 * 実機 μPD72065 の挙動再現ではない。
 * busy: 保留中なら 1。filter_status: STATUS 読みの値を加工(busy 中は RQM=0, CB=1)。
 * should_block_data: busy 中なら 1 を返し、呼出し側は FDC_Read を呼ばず 0xff を返す。 */
int     mx68k_fdlat_busy(void);
uint8_t mx68k_fdlat_filter_status(uint8_t msr);
int     mx68k_fdlat_should_block_data(void);

#endif /* MX68K_FD_LATENCY_EXPERIMENT_H */
