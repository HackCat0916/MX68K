#include "common.h"
#include <stdarg.h>

const char PrgTitle[] = "MX68K";

#define P6L_LEN 256
char p6l_buf[P6L_LEN];

void Error(const char* s)
{
    printf("%s Error: %s\n", PrgTitle, s);
}

void p6logd(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(p6l_buf, P6L_LEN, fmt, args);
    va_end(args);
    printf("%s", p6l_buf);
}
