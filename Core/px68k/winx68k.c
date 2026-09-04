#include "winx68k.h"
#include "x68k/prop.h"

uint8_t* FONT = NULL;

Config_t Config = {0};

int32_t HLINE_TOTAL = 0;
int32_t VLINE_TOTAL = 0;
int32_t VLINE = 0;
int32_t x68k_vline = 0;

char winx68k_dir[MAX_PATH] = "";
char winx68k_ini[MAX_PATH] = "";
int32_t BIOS030Flag = 0;
uint8_t FrameChanged = 0;

int32_t m68000_ICountBk = 0;
int32_t ICount = 0;

int32_t WinX68k_Reset(void) {
    return 0;
}

void WinDraw_InitWindowSize(uint32_t ScreenX, uint32_t ScreenY, uint32_t StartX, uint32_t StartY) {
    (void)ScreenX;
    (void)ScreenY;
    (void)StartX;
    (void)StartY;
}

BOOL is_installed_idle_process(void) {
    return FALSE;
}

void install_idle_process(void) {
}

void uninstall_idle_process(void) {
}

void draw_soft_kbd(uint32_t x, uint32_t y, uint8_t LED) {
    (void)x;
    (void)y;
    (void)LED;
}
