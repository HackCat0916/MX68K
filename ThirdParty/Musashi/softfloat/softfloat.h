/* MX68K作成のスタブ(SoftFloat原本の softfloat.h ではない)。
 * m68kcpu.h:100 の #include と CPU構造体の floatx80 fpr[8](m68kcpu.h:953)を満たす型定義のみ。
 * 浮動小数点演算関数は宣言しない(呼ぶのは原本 m68kfpu.c だけで、それも取り込まない)。 */
#ifndef MX68K_SOFTFLOAT_STUB_H
#define MX68K_SOFTFLOAT_STUB_H
#include <stdint.h>

typedef uint32_t float32;
typedef uint64_t float64;
typedef struct { uint16_t high; uint64_t low; } floatx80;

#endif
