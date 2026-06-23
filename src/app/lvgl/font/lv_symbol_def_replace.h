#pragma once

#ifdef LV_SYMBOL_DEF_H
#error "Should not happen"
#endif

#include "material_icons_regular_symbols.h"

#define LV_SYMBOL_DEF_H

#define LV_SYMBOL_OK MAT_SYMBOL_CHECK
#define LV_SYMBOL_DOWN MAT_SYMBOL_ARROW_DROP_DOWN
#define LV_SYMBOL_UP MAT_SYMBOL_KEYBOARD_ARROW_UP
#define LV_SYMBOL_CLOSE MAT_SYMBOL_CLOSE
#define LV_SYMBOL_BULLET "\xE2\x80\xA2"
#define LV_SYMBOL_LEFT MAT_SYMBOL_KEYBOARD_ARROW_LEFT
/* Necessários para LVGL keyboard (Material Icons: backspace e14a, keyboard e312, keyboard_return e31b, arrow_right e5df) */
#define LV_SYMBOL_BACKSPACE "\xEE\x85\x8A"
#define LV_SYMBOL_NEW_LINE "\xEE\x8C\x9B"
#define LV_SYMBOL_KEYBOARD "\xEE\x8C\x92"
#define LV_SYMBOL_RIGHT "\xEE\x97\x9F"