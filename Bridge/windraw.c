#include "windraw.h"

uint8_t Draw_DrawFlag = 0;
uint32_t WinDraw_Pal32R = 0;
uint32_t WinDraw_Pal32G = 0;
uint32_t WinDraw_Pal32B = 0;

void WinDraw_ChangeSize(void)
{
    /* P533 [P533-XSNAP]: 本関数は依然 no-op(描画側の実体は無い)。
     * Core/px68k/x68k/crtc.c から CRTC レジスタ書込みごとに呼ばれるため、
     * 1 フレーム中に何回発火したかだけを計測アクセサへ通知する。
     * アクセサの中身は P533_ENABLE でガードされている(実体は
     * Bridge/EmulatorBridge.c)。 */
    extern void mx68k_diag_note_chsize_call(void);
    mx68k_diag_note_chsize_call();
}

void FASTCALL WinDraw_Draw(void)
{
}

void draw_soft_kbd(uint32_t x, uint32_t y, uint8_t LED)
{
    (void)x;
    (void)y;
    (void)LED;
}
