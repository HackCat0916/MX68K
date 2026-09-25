// midi.c と midi_darwin.c が参照する winui グローバル変数の macOS 用スタブ。
// 本来は Win32/GTK のメニュー用グローバル変数であり、macOS では使用しない。

#include <stdint.h>

#define MENU_ITEMS_COLS 16

char mx68k_menu_items[16][MENU_ITEMS_COLS][256] = {0};
