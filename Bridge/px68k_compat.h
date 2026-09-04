// macOS compatibility header injected by project settings
// Provides declarations for functions that were part of the original Win32/GTK GUI layer
// but are missing on macOS. These stubs are implemented in Bridge/status_bridge.c etc.

#ifndef PX68K_COMPAT_H
#define PX68K_COMPAT_H

#include <stdint.h>

// Status bar functions (originally in Win32 GUI layer)
void StatBar_SetFDD(int32_t drive, const char* filename);
void StatBar_ParamFDD(int32_t drive, int32_t state, int32_t active, int32_t blink);

// Rename menu_items to avoid collision with Darwin ncurses menu.h function prototype.
// Core files see menu_items via this macro, but the actual definition in Bridge/winui.c
// uses the renamed symbol.
#define menu_items mx68k_menu_items
extern char menu_items[16][16][256];

#endif
