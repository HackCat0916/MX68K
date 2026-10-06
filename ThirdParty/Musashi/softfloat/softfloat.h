/* MX68K作成の転送shim(SoftFloat原本の softfloat.h ではない)——P897。
 * m68kcpu.h:100 の #include を、ThirdParty/softfloat_2a(Hatari経由の SoftFloat 2a)の本物の softfloat.h へ転送する。
 * CPU構造体の floatx80 fpr[8](m68kcpu.h:953)と m68kfpu.c(MAME m68kfpu.cpp 由来の移植版)が使う型・関数宣言を得る。
 * floatx80 のメンバ配置は旧スタブと同一({uint16_t high; uint64_t low;})なので CPU構造体のレイアウトは変わらない。 */
#include "../../softfloat_2a/softfloat.h"
