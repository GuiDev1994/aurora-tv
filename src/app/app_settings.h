/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <Limelight.h>

#include <stdbool.h>
#include "ss4s/video.h"

typedef struct window_state_t {
    int x, y, w, h;
} window_state_t;

typedef struct app_settings_t {
    STREAM_CONFIGURATION stream;
    int debug_level;
    char *decoder;
    char *audio_backend;
    char *audio_device;
    char *language;
    bool sops;
    bool localaudio;
    bool fullscreen;
    window_state_t window_state;
    int rotate;
    bool unsupported;
    bool quitappafter;
    bool viewonly;
    bool absmouse;
    bool hardware_mouse;
    bool virtual_mouse;
    bool swap_abxy;
    bool syskey_capture;
    bool hid_passthrough;
    int hid_passthrough_port;
    bool hdr;   /* HDR10 (PQ) over HEVC Main10 or AV1 Main10 when host and decoder support it */
    bool force_full_color_range; /* SDR only: request full-range YUV (0-255) from host. No effect when HDR is on. */
    /**
     * webOS Starfish: nominal-frame PTS pacing + fixed small negative presentation offset
     * for tighter vsync without adding latency when the stream runs late.
     */
    bool video_tight_sync;
    bool hevc;
    /** Sunshine: negotiate AV1 Main8/Main10 when decoder exposes SS4S_VIDEO_AV1. */
    bool av1;
    bool show_stats_on_start;
    bool show_stats_compact;
    int stick_deadzone;
    /**
     * Sent to host as STREAM_CONFIGURATION.clientRefreshRateX100 (Hz * 100, e.g. 11994 = 119.94 Hz).
     * 0 = omit (host default frame pacing).
     */
    int client_refresh_rate_x100;
    char *conf_dir;
    char *ini_path;
    char *condb_path;
    char *key_dir;
    bool conf_persistent;
} CONFIGURATION, *PCONFIGURATION, app_settings_t;

typedef struct audio_config_entry_t {
    int configuration;
    const char *value;
    const char *name;
} audio_config_entry_t;

extern const audio_config_entry_t audio_configs[];
extern const size_t audio_config_len;

#define CONF_NAME_MOONLIGHT "moonlight.ini"
#define CONF_NAME_HOSTS "hosts.ini"

#define RES_MERGE(w, h) (((w) & 0xFFFF) << 16 | ((h) & 0xFFFF))

#define RES_720P RES_MERGE(1280, 720)
#define RES_1080P RES_MERGE(1920, 1080)
#define RES_1440P RES_MERGE(2560, 1440)
/** ~90% of 4K (3584×2016); stable on recent LG TVs without full-4K input delay */
#define RES_3_6K RES_MERGE(3584, 2016)
#define RES_1800P RES_MERGE(3200, 1800)
#define RES_4K RES_MERGE(3840, 2160)

void settings_initialize(app_settings_t *config, char *conf_dir);

bool settings_read(app_settings_t *config);

bool settings_save(app_settings_t *config);

void settings_clear(app_settings_t *config);

int settings_optimal_bitrate(const SS4S_VideoCapabilities *capabilities, int w, int h, int fps);

bool audio_config_valid(int config);