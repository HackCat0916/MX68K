#ifndef winx68k_common_h
#define winx68k_common_h

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "win32api/windows.h"

#define TRUE        1
#define FALSE       0
#define SUCCESS     0
#define FAILURE     1

#undef FASTCALL
#define FASTCALL

#define STDCALL
#define LABEL

#undef  __stdcall
#define __stdcall

#ifdef __cplusplus
extern "C" {
#endif

void Error(const char* s);
void p6logd(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
