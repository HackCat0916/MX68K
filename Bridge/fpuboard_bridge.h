#ifndef MX68K_FPUBOARD_BRIDGE_H
#define MX68K_FPUBOARD_BRIDGE_H

#include <stdint.h>

/* P901: X68000 世代 FPU ボード CZ-6BP1(MC68881)の CIR デバイス。
 *
 * 68000 には コプロセッサインタフェースが無いため、FLOAT3.X・SI.R 等のゲストソフトは CIR を
 * 通常の I/O ポートとして読み書きする(Inside X68000 書籍p.112)。本モジュールはその CIR 窓
 * (ボード1 = JP1 標準設定の $E9E000-$E9E01F のみ、Outside X68000 書籍p.87-88 図4)に応答し、
 * 演算は ThirdParty/Musashi/m68kfpu.c の CIR 入口(m68kfpu_cir.h)、FMOVE.P は Bridge/fpu_packed.c で行う。
 *
 * 配線されていない(既定)とき fpuboard_claims_addr() / fpuboard_buserr_exempt() は常に 0 を返し、
 * $E9E000 窓は従来どおり P419 の無条件バスエラーのまま。ボード2($E9E080-)・CZ-6BP2 は未実装
 * (状態はインスタンス構造体に base を持たせてあり、将来の拡張点)。 */

#define FPUBOARD_CIR_BASE   0x00E9E000u
#define FPUBOARD_CIR_LAST   0x00E9E01Fu

/* 開発者用の環境変数 MX68K_FPUBOARD=1(設定値に関わらず装着扱い)を読む。プロセス中1回だけ読む */
int fpuboard_env_forced(void);

/* mx68k_init() / mx68k_reset_hard() から呼ぶ。wired=配線確定値(呼出し側でラッチ済み)。
 * CIR 状態と FPU コンテキストを実機の RESET 相当(NULL リストアと同じ)に初期化し、
 * [P901-FPUBOARD] reset 行と要約行を1行ずつ出す。src はログ用("init" / "reset") */
void fpuboard_init(int wired, int enabled, int cpu_model, const char *src);

/* 配線済みかつ addr がボード1 の CIR 窓内なら 1。read/write フックの発火条件 */
int fpuboard_claims_addr(uint32_t addr);

/* P419 バスエラー合成の除外判定。claims と同じ判定だが、除外したアクセスを buserr_exempt として数える */
int fpuboard_buserr_exempt(uint32_t addr);

/* size は 1(byte)/ 2(word)。claims が 1 のときのみ呼ぶこと。ロングは Core 側で
 * $10→$12 の2回のワードアクセスに分割されて届く */
uint32_t fpuboard_read(uint32_t addr, int size);
void fpuboard_write(uint32_t addr, uint32_t val, int size);

/* ステートセーブ/ロード用のフィールドビジター(EmulatorBridge.c の STATE_FIELD と同じ規約)。
 * save=1 は状態→buf、save=0 は buf→状態。buf=NULL ならバイト長だけを返す。
 * 配線の有無に関わらず常に固定長。統計カウンタは保存しない */
uint32_t fpuboard_state_block(uint8_t *buf, int save);

#endif /* MX68K_FPUBOARD_BRIDGE_H */
