#ifndef _winx68k_keyboard
#define _winx68k_keyboard

#include "common.h"

#define KeyBufSize 128

extern	uint8_t	KeyBuf[KeyBufSize];
extern	uint8_t	KeyBufWP;
extern	uint8_t	KeyBufRP;
extern	uint8_t	KeyTable[];
extern	uint8_t	KeyEnable;
extern	uint8_t	KeyIntFlag;

struct keyboard_key {
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;
	char *s;
	char *s2;
	uint8_t c;
};

extern struct keyboard_key kbd_key[];
extern int32_t  kbd_kx, kbd_ky;
extern int32_t kbd_x, kbd_y, kbd_w, kbd_h;

void Keyboard_Init(void);
void Keymap_Init(void);
void Keyboard_KeyDown(uint32_t vkcode,uint32_t phcode);
void Keyboard_KeyUp(uint32_t vkcode,uint32_t phcode);
void Keyboard_Int(void);
void send_keycode(uint8_t code, int32_t flag);
int32_t Keyboard_get_key_ptr(int32_t x, int32_t y);
void Keyboard_skbd(void);
int32_t Keyboard_IsSwKeyboard(void);
void Keyboard_ToggleSkbd(void);

#endif //_winx68k_keyboard
