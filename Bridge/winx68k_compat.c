// macOS compatibility: defines symbols originally in winx68k.c that are
// referenced by other Core/px68k files. winx68k.c itself is excluded on macOS.

#include <stdint.h>
#include "prop.h"

Config_t Config = {0};
int32_t HLINE_TOTAL = 0;
int32_t VLINE_TOTAL = 0;
int32_t VLINE = 0;
int32_t x68k_vline = 0;
