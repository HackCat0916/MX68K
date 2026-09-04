#include "keyboard.h"
#include "common.h"

uint8_t KeyBufWP = 0;
uint8_t KeyBufRP = 0;
uint8_t KeyBuf[KeyBufSize];
uint8_t KeyEnable = 1;
uint8_t KeyIntFlag = 0;
uint8_t KeyTable[256];

struct keyboard_key kbd_key[1];
int32_t kbd_kx = 0, kbd_ky = 0;
int32_t kbd_x = 0, kbd_y = 0, kbd_w = 0, kbd_h = 0;

void Keyboard_Init(void) {
    KeyBufWP = 0;
    KeyBufRP = 0;
    KeyIntFlag = 0;
}

void Keymap_Init(void) {
}

void Keyboard_KeyDown(uint32_t vkcode, uint32_t phcode) {
    (void)vkcode;
    (void)phcode;
}

void Keyboard_KeyUp(uint32_t vkcode, uint32_t phcode) {
    (void)vkcode;
    (void)phcode;
}

void Keyboard_Int(void) {
}

void send_keycode(uint8_t code, int32_t flag) {
    (void)code;
    (void)flag;
}

int32_t Keyboard_get_key_ptr(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    return -1;
}

void Keyboard_skbd(void) {
}

int32_t Keyboard_IsSwKeyboard(void) {
    return 0;
}

void Keyboard_ToggleSkbd(void) {
}
