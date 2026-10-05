/* MX68K作成のスタブ(Musashi原本の m68kmmu.h ではない)。
 * 原本は旧MAMEライセンス表記のため取り込まず、m68kcpu.c:52 の #include を満たす最小の置換のみ置く。
 * 68000型では has_pmmu=0 のため PMMU 命令は line-1111 例外へ回り、アドレス変換も呼ばれない。
 * m68kcpu.c の中へ m68kfpu.c の後で #include される(uint は m68kcpu.h、mx68k_musashi_stub_hook は m68kfpu.c で定義済み)。
 * P869: EC030(MMU非搭載)向けに、PMMU一般形式のうち Dn/(An) は no-op、それ以外は line-1111 例外にする
 * (XEiJ irpPgen の部分集合。残りのEAモードは命令長を一次資料無しに決められないため扱わない)。 */
#include <stdlib.h>

#define MX68K_STUB_PMMU_NOOP  2   /* Bridge/mx_cpu_musashi.c の同名定数と一致させること */
#define MX68K_STUB_PMMU_FLINE 3   /* Bridge/mx_cpu_musashi.c の同名定数と一致させること */

/* PMMU_ENABLED を立てる経路が無いので到達不能 */
uint pmmu_translate_addr(uint addr_in) { (void)addr_in; abort(); }

void m68881_mmu_ops(void)
{
    uint ppc = REG_PPC, ir = REG_IR, mode = (ir >> 3) & 7;
    if ((ir & 0x01C0) == 0 && (mode == 0 || mode == 2)) {
        uint ext = OPER_I_16();   /* 第2語を読み飛ばす。Dn/(An) はEA拡張語を持たずAnも更新しない */
        if (mx68k_musashi_stub_hook) mx68k_musashi_stub_hook(MX68K_STUB_PMMU_NOOP, ppc, ir, ext);
        return;                    /* EC030はMMU非搭載: 何もしない(XEiJ irpPgen と同じ) */
    }
    if (mx68k_musashi_stub_hook) mx68k_musashi_stub_hook(MX68K_STUB_PMMU_FLINE, ppc, ir, 0);
    m68ki_exception_1111();        /* 未対応の形式は上流EC030と同じ line-1111 に落とす(abort しない) */
}
