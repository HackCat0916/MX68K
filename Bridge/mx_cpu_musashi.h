/* mx_cpu_musashi.h — mx_cpu_iface.h のMusashiバックエンド(P867)
 * mx_cpu_iface.h の各関数と1対1に対応する mx_cpu_musashi_* を宣言する。
 * ★P868: MX68K_CPU_CORE=musashi のとき m68000_bridge.c の mx_cpu_* から呼ばれる(既定はc68k)。
 * ★Musashiの型・定数はこのヘッダに出さない(<stdint.h> と mx_cpu_iface.h のみに依存)。 */
#ifndef MX_CPU_MUSASHI_H
#define MX_CPU_MUSASHI_H
#include <stdint.h>
#include <stddef.h>
#include "mx_cpu_iface.h"

uint32_t mx_cpu_musashi_get_dreg(int n);
uint32_t mx_cpu_musashi_get_areg(int n);
uint32_t mx_cpu_musashi_get_pc(void);
uint32_t mx_cpu_musashi_get_sr(void);
uint32_t mx_cpu_musashi_get_usp(void);
uint32_t mx_cpu_musashi_get_ssp(void);

void mx_cpu_musashi_set_dreg(int n, uint32_t v);
void mx_cpu_musashi_set_areg(int n, uint32_t v);
void mx_cpu_musashi_set_sr(uint32_t v);
void mx_cpu_musashi_set_usp(uint32_t v);
void mx_cpu_musashi_set_pc(uint32_t v);
void mx_cpu_musashi_set_ssp(uint32_t v);
void mx_cpu_musashi_jump_raw32(uint32_t pc32);   /* P868: 32bitのまま(mx_cpu_jump_raw32) */

void mx_cpu_musashi_init(const mx_cpu_bus *bus);
void mx_cpu_musashi_map_fetch(uint32_t lo, uint32_t hi, const void *host);
void mx_cpu_musashi_reset(void);

int32_t mx_cpu_musashi_execute(int32_t cycles);
void    mx_cpu_musashi_set_irq(int32_t level);
void    mx_cpu_musashi_end_timeslice(void);
void    mx_cpu_musashi_add_cycles(int32_t cycles);
int32_t mx_cpu_musashi_cycles_done(void);

int32_t mx_cpu_musashi_get_irq_line(void);
int     mx_cpu_musashi_is_halted(void);

uint32_t mx_cpu_musashi_state_get_run_status(void);
int32_t  mx_cpu_musashi_state_get_irq_line(void);
void     mx_cpu_musashi_state_restore_run(uint32_t run_status, int32_t irq_line);

/* P869: 実験用CPUモデル(環境変数 MX68K_MUSASHI_MODEL)と一時プローブ[P869-*]。
 * ログは関数ポインタで受け取る(selftest は debug_log を持たないため直接参照しない)。 */
typedef void (*mx_cpu_musashi_logf)(const char *fmt, ...);
/* P869: MX68K_MUSASHI_MODEL を読みCPU型等を設定する。mx_cpu_musashi_init の直後・最初のリセット前に1回だけ呼ぶ */
void mx_cpu_musashi_set_model_from_env(mx_cpu_musashi_logf logf);
/* P872: 本番経路のCPU型設定(0=68000型、1=EC030、2=MC68040[P910]、3=MC68060相当[P916、Musashi 040型+060のMOVEC応答])。環境変数を読まず、P869プローブも有効にしない */
void mx_cpu_musashi_set_model(int model);
/* P872: 現在の型の分類。0x00=68000型、0x01=EC030(本番構成)、0x02=EC030+ハイメモリ有効(P887)、
 * 0x03=MC68040(24bit・HAS_PMMU=0)、0x04=MC68040+ハイメモリ有効(P910)、
 * 0x05=MC68060相当(24bit)、0x06=MC68060相当+ハイメモリ有効(P916)、0xFF=それ以外 */
uint32_t mx_cpu_musashi_get_model_id(void);
/* P887/P889: ハイメモリのバッファと配置(base/bytes)。NULL=無効(アドレスマスク24bit、base/bytesは0を渡す)、
 * 非NULL=有効(32bit)。TS-6BE16相当=$01000000/16MB、060turbo相当=$10000000/可変。
 * set_model の後、CPU非実行のリセット区間からだけ呼ぶ */
void mx_cpu_musashi_set_highmem(uint8_t *buf, uint32_t base, uint32_t bytes);
/* P887: Musashi の CPU_ADDRESS_MASK の現在値(ログ用の読み戻し) */
uint32_t mx_cpu_musashi_get_address_mask(void);
/* P887: [P887-HIMEM-SELFTEST] の1行を out へ書く(テスト専用、環境変数 MX68K_HIMEM_SELFTEST=1 のときだけ呼ぶ)。戻り値はhimem */
int mx_cpu_musashi_highmem_selftest(char *out, size_t n);
/* P869: 一時プローブの定期集計行。プローブ有効モデル以外では何もしない */
void mx_cpu_musashi_p869_tick(int frame);

/* P897: FPU(68881/68882、ThirdParty/Musashi/m68kfpu.c)。環境変数 MX68K_MUSASHI_FPU=1 かつEC030のときだけ
 * set_model / set_model_from_env が装着する(MX68K_MUSASHI_FPU_MODEL=68881|68882、既定68882)。
 * 直接指定(selftest用): set_model の後・reset の前に呼ぶ */
void mx_cpu_musashi_set_fpu_config(int present, int model);
int  mx_cpu_musashi_get_fpu_present(void);
int  mx_cpu_musashi_get_fpu_model(void);
/* MX68K_MUSASHI_FPU の生値(未設定・空なら NULL) */
const char *mx_cpu_musashi_fpu_env_raw(void);
/* P897: [P897-FPU] の定期行(FPU装着時のみ出す、観測カウンタは累計) */
void mx_cpu_musashi_p897_tick(int frame, mx_cpu_musashi_logf logf);
/* P910: [P910-040EXEC] の定期行(040型のときだけ出す、カウンタはプロセス内の累計) */
void mx_cpu_musashi_p910_tick(int frame, mx_cpu_musashi_logf logf);

#endif /* MX_CPU_MUSASHI_H */
