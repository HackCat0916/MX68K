// macOS 互換: 本来 winx68k.c にあり、他の Core/px68k ファイルから参照される
// シンボルを定義する。winx68k.c 自体は macOS ではビルドから除外している。

#include <stdint.h>
#include "prop.h"

Config_t Config = {0};
int32_t HLINE_TOTAL = 0;
int32_t VLINE_TOTAL = 0;
int32_t VLINE = 0;
int32_t x68k_vline = 0;
