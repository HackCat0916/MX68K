/* P901: パック10進実数(MC68881 の FMOVE.P 書式)と拡張精度実数の相互変換。
 * MX68K の独自実装(XEiJ/MAME のコードは参照していない)。softfloat・CPU 状態に依存しない純粋関数で、
 * ホストで単体試験できる(Scripts/fpu_packed_selftest.sh)。
 *
 * パック10進(96bit、in[0]=bit95-64、in[1]=bit63-32、in[2]=bit31-0)の配置
 * (Inside X68000 書籍p.108 図4 + FLOAT3.X の入出力コードで突合、P901 計画 S-1):
 *   bit95=仮数の符号 SM、bit94=指数の符号 SE、bit91-80=指数3桁BCD(百の位が bit91-88)、
 *   bit79-76=指数の4桁目(出力で指数が1000以上のときだけ使う、入力では無視)、
 *   bit67-64=整数部1桁、bit63-0=小数部16桁(bit63-60 が小数第1位)。
 *   ±∞/NaN: 指数の下位8bit(bit87-80)が $FF。小数部が全0なら∞、非0なら NaN。
 *
 * 拡張精度は (hi=符号+15bit指数, lo=明示的な整数ビットを含む64bit仮数)。
 * rnd は FPCR の丸めモード欄(0=RN 最近接偶数、1=RZ、2=RM、3=RP)。
 * 戻り値は例外フラグのビット集合(下記)。呼出し側が FPSR へ反映する。 */
#ifndef MX68K_FPU_PACKED_H
#define MX68K_FPU_PACKED_H

#include <stdint.h>

#define MX_PACKED_INEXACT 0x1   /* 結果が丸められた(入力=INEX1、出力=INEX2) */
#define MX_PACKED_OPERR   0x2   /* K ファクタ範囲外、または出力指数が3桁を超えた */

int mx_packed_to_ext(const uint32_t in[3], int rnd, uint16_t *hi, uint64_t *lo);

/* k: K ファクタ(-64..+17)。k>0 = 有効桁数、k<=0 = 小数点以下の桁数 */
int mx_ext_to_packed(uint16_t hi, uint64_t lo, int k, int rnd, uint32_t out[3]);

#endif /* MX68K_FPU_PACKED_H */
