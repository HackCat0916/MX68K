/* mx_cpu_iface.h — CPUコア抽象層(P860、第1段)
 * Bridge内のステートセーブ/ロードとモニタ機能が使う、CPUレジスタの読み書き口。
 * 現在の実装はc68kの関数をそのまま呼ぶだけ(m68000_bridge.c末尾の「mx_cpu_* 実装」節)。
 * 本番経路(実行・割込み・メモリ・バスエラー)はまだここを通らない。
 * ★CPUコア固有の型・定数はこのヘッダに一切出さない(<stdint.h>のみに依存)。
 *   将来の別コア(030用等)へ差し替える際の継ぎ目とするため。
 * ★Swiftには公開しない(EmulatorBridge.hからはincludeしない)。 */
#ifndef MX_CPU_IFACE_H
#define MX_CPU_IFACE_H
#include <stdint.h>

/* --- レジスタ読み出し(モニタ・ステートセーブ) --- */
uint32_t mx_cpu_get_dreg(int n);   /* n = 0..7 */
uint32_t mx_cpu_get_areg(int n);   /* n = 0..7。A7は現在有効なSP */
uint32_t mx_cpu_get_pc(void);
uint32_t mx_cpu_get_sr(void);
uint32_t mx_cpu_get_usp(void);
uint32_t mx_cpu_get_ssp(void);     /* 68000のスーパーバイザSP(c68kではMSP) */

/* --- レジスタ書き込み(ステートロード専用) --- */
void mx_cpu_set_dreg(int n, uint32_t v);
void mx_cpu_set_areg(int n, uint32_t v);
void mx_cpu_set_sr(uint32_t v);
void mx_cpu_set_usp(uint32_t v);   /* 結果はSRのSビットに依存する。SR→A7→USPの順で呼ぶこと */
void mx_cpu_set_pc(uint32_t v);    /* 24bitにマスクしてから設定(m68000_set_regと同じ) */

/* --- 実行状態(モニタ) --- */
int32_t mx_cpu_get_irq_line(void); /* 保留中の割込みレベル(生値) */
int     mx_cpu_is_halted(void);    /* HALT または STOP待ちなら1 */

/* --- 実行状態の生値(ステート専用、コアごとに中身が違う不透明な値) --- */
uint32_t mx_cpu_state_get_run_status(void);
int32_t  mx_cpu_state_get_irq_line(void);
/* 生値をそのまま書き戻す。割込み要求(コアのIRQ設定関数)ではない:
 * 実行中断・HALT解除などの副作用は起こさない。 */
void     mx_cpu_state_restore_run(uint32_t run_status, int32_t irq_line);

#endif /* MX_CPU_IFACE_H */
