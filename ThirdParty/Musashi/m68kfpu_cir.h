/* MX68K作成のCIR入口ヘッダ(上流 Musashi のファイルではない)——P901。
 *
 * X68000 世代 FPU ボード(CZ-6BP1、MC68881)の CIR デバイス(Bridge/fpuboard_bridge.c)が、
 * m68kfpu.c の演算部を「純粋な演算」として呼ぶための口。FPU の状態はボード側が持つ
 * mx68k_fpu_ctx に置き、softfloat の型は外へ出さない(ステートセーブでそのまま直列化できる素の整数)。
 *
 * 各関数は入口で Musashi の FPU 状態(m68ki_cpu.fpr/fpcr/fpsr/fpiar/fpu_just_reset と
 * m68kfpu.c 内部の s_fpst 等)を退避し、ctx を載せて演算し、ctx へ書き戻してから退避値を
 * 復元する。CIR 経路からは USE_CYCLES・REG_PPC・REG_IR・READ_EA_xx・WRITE_EA_xx・
 * fpu_fline()(=CPU 例外)を一切呼ばない。CPU 非実行区間か、バスコールバックの中から呼ぶ。
 *
 * Bridge/ からは相対パス("../ThirdParty/Musashi/m68kfpu_cir.h")で include すること
 * (ThirdParty/Musashi は HEADER_SEARCH_PATHS に無い、MX68K_VENDOR.txt の Build notes 参照)。 */
#ifndef MX68K_M68KFPU_CIR_H
#define MX68K_M68KFPU_CIR_H

#include <stdint.h>

struct mx68k_fpu_ctx {
    uint16_t fp_hi[8];     /* FP0-7 の符号+指数(拡張精度の上位16bit) */
    uint64_t fp_lo[8];     /* FP0-7 の仮数(明示的な整数ビットを含む64bit) */
    uint32_t fpcr, fpsr, fpiar;
    uint8_t  just_reset;   /* 1 = リセット/NULLリストア後に FPU 命令が未実行(セーブは NULL を返す) */
};

/* リセット(NULL フレームの FRESTORE と同じ): FP0-7=NaN、FPCR/FPSR/FPIAR=0、just_reset=1 */
void mx68k_fpucir_reset(struct mx68k_fpu_ctx *ctx);

/* コマンドワードの opmode が 68881 で有効か(opclass 000 と、010 の FMOVECR 以外)。
 * 1=有効、0=無効。転送を要求する前の検査に使う */
int mx68k_fpucir_cmd_valid(uint16_t cmd);

/* opclass 000(レジスタ間)/010(外部→FPn、FMOVECR を含む)。
 * in はソース書式ごとのロング列(L/S/W/B=1語、D=2語[上位→下位]、X/P=3語。W は下位16bit、
 * B は下位8bit)。opclass 000 と FMOVECR では NULL でよい。戻り値 0=成功、<0=無効コマンド */
int mx68k_fpucir_gen(struct mx68k_fpu_ctx *ctx, uint16_t cmd, const uint32_t *in);

/* opclass 011(FPn→外部)。k は動的 K ファクタ(書式7のときだけ使う。下位7bitを符号付きとして扱う)。
 * out へ書式ごとのロング列を書く(W は下位16bit、B は下位8bit)。戻り値は転送ロング語数
 * (B/W/L/S=1、D=2、X/P=3)、<0=無効コマンド */
int mx68k_fpucir_out(struct mx68k_fpu_ctx *ctx, uint16_t cmd, int k, uint32_t out[3]);

/* opclass 100(外部→制御レジスタ)。regsel は bit2=FPCR、bit1=FPSR、bit0=FPIAR。
 * v は FPCR→FPSR→FPIAR の順で選ばれた本数だけ */
void mx68k_fpucir_ctl_write(struct mx68k_fpu_ctx *ctx, int regsel, const uint32_t *v);

/* opclass 101(制御レジスタ→外部)。同じ順で v へ書き、本数を返す */
int mx68k_fpucir_ctl_read(struct mx68k_fpu_ctx *ctx, int regsel, uint32_t *v);

/* コンディション CIR。pred は $00-$1F のみ(それ以外は -1)。1=成立、0=不成立 */
int mx68k_fpucir_condition(struct mx68k_fpu_ctx *ctx, int pred);

/* セーブフレーム(68881)を words へ組み立て、ロング語数を返す(NULL=1、IDLE=7)。
 * words[0] の上位16bit がセーブ CIR のフォーマットワード(版数<<8 | サイズ) */
int mx68k_fpucir_save_frame(const struct mx68k_fpu_ctx *ctx, uint32_t words[7]);

#endif /* MX68K_M68KFPU_CIR_H */
