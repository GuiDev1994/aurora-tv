#pragma once

#include "lvgl.h"

/** AMOLED + Stremio-inspired palette (pure black base, violet accent). */
#define ML_COLOR_BG           0x000000
#define ML_COLOR_SURFACE      0x121212
#define ML_COLOR_SURFACE_ALT  0x1A1A1A
#define ML_COLOR_SURFACE_HI   0x242424
#define ML_COLOR_BORDER       0x2A2A2A
#define ML_COLOR_PRIMARY      0x8B5CF6
#define ML_COLOR_PRIMARY_DIM  0x6D28D9
/** TV settings / launcher focus ring (blue — distinct from violet accent). */
#define ML_COLOR_FOCUS        0x3B82F6
#define ML_COLOR_TEXT         0xF5F5F5
#define ML_COLOR_TEXT_MUTED   0x9CA3AF

static inline lv_color_t ml_color_hex(uint32_t c) {
    return lv_color_hex(c);
}
