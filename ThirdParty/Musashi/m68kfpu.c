/* MX68K作成のスタブ(Musashi原本の m68kfpu.c ではない)。
 * 原本はライセンス表記が無いため取り込まず、m68kcpu.c:51 の #include を満たす最小の置換のみ置く。
 * 68000型では FPU 命令は m68k_in.c の CPU_TYPE_IS_030_PLUS 判定で line-1111 例外へ回り、ここへは来ない。
 * P869: FPU非装着の構成として、030型以上で到達した F2xx/F3xx を line-1111 例外にする
 * (XEiJ irpFgen/irpFbccWord と同じ)。FPU装着モード(Phase 3)は未実装(MX68K_VENDOR.txt 参照)。
 * m68kcpu.c の中へ m68kcpu.h の後で #include される(REG_PPC/REG_IR/m68ki_exception_1111 は定義済み)。 */
#include <stdlib.h>

/* P869: スタブ到達の観測フック(既定NULL)。Bridge/mx_cpu_musashi.c が実験モデル選択時に登録する。
 * ★型は Bridge/mx_cpu_musashi.c の extern 宣言と一字一句同じにすること */
void (*mx68k_musashi_stub_hook)(int kind, unsigned int ppc, unsigned int ir, unsigned int ext) = 0;
#define MX68K_STUB_FPU_FLINE 1   /* Bridge/mx_cpu_musashi.c・m68kmmu.h の同名定数と一致させること */

void m68040_fpu_op0(void)
{
    if (mx68k_musashi_stub_hook) mx68k_musashi_stub_hook(MX68K_STUB_FPU_FLINE, REG_PPC, REG_IR, 0);
    m68ki_exception_1111();   /* 積まれるPCは REG_PPC(命令先頭)なので、ここまでの読み出し語数に依存しない */
}

void m68040_fpu_op1(void)
{
    if (mx68k_musashi_stub_hook) mx68k_musashi_stub_hook(MX68K_STUB_FPU_FLINE, REG_PPC, REG_IR, 0);
    m68ki_exception_1111();
}
