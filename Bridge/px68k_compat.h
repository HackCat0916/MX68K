// プロジェクト設定によって注入される macOS 互換ヘッダ。
// 元の Win32/GTK GUI 層に含まれていたが macOS には存在しない関数の宣言を提供する。
// これらのスタブは Bridge/status_bridge.c 等で実装している。

#ifndef PX68K_COMPAT_H
#define PX68K_COMPAT_H

#include <stdint.h>

// ステータスバー関数(本来は Win32 GUI 層にあったもの)
void StatBar_SetFDD(int32_t drive, const char* filename);
void StatBar_ParamFDD(int32_t drive, int32_t state, int32_t active, int32_t blink);

// Darwin の ncurses menu.h の関数プロトタイプとの衝突を避けるため menu_items を改名する。
// Core ファイルはこのマクロ経由で menu_items を参照するが、Bridge/winui.c の実定義は
// 改名後のシンボルを使う。
#define menu_items mx68k_menu_items
extern char menu_items[16][16][256];

#endif
