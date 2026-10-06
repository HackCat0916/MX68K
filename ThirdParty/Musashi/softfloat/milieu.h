/* MX68K作成のスタブ(SoftFloat原本の milieu.h ではない)。
 * m68kcpu.h:99 の #include を満たすための型定義のみ。SoftFloat本体(Release 2b)は取り込まない。
 * P897: flag は softfloat.h(ThirdParty/softfloat_2a、typedef uint8_t flag)が定義するのでここでは定義しない。
 * int8 以下は上流Musashi本体(m68kops.c・m68kcpu.h)が使うので残す。 */
#ifndef MX68K_SOFTFLOAT_MILIEU_STUB_H
#define MX68K_SOFTFLOAT_MILIEU_STUB_H
#include <stdint.h>

typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef uint8_t bits8;
typedef int8_t sbits8;
typedef uint16_t bits16;
typedef int16_t sbits16;
typedef uint32_t bits32;
typedef int32_t sbits32;
typedef uint64_t bits64;
typedef int64_t sbits64;

#endif
