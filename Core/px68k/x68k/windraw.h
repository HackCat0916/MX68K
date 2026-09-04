#ifndef _WINDRAW_H_
#define _WINDRAW_H_

#include "../common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t Draw_DrawFlag;
extern uint32_t WinDraw_Pal32R;
extern uint32_t WinDraw_Pal32G;
extern uint32_t WinDraw_Pal32B;

void WinDraw_ChangeSize(void);
void FASTCALL WinDraw_Draw(void);
void draw_soft_kbd(uint32_t x, uint32_t y, uint8_t LED);

#ifdef __cplusplus
}
#endif

#endif
