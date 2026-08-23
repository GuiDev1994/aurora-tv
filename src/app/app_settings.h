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
    bool autoresume;
    bool viewonly;
    bool absmouse;
    bool hardware_mouse;
    bool virtual_mouse;
    bool swap_abxy;
    bool syskey_capture;
    bool hdr;   /* HDR10 (PQ) over HEVC Main10 or AV1 Main10 when host and decoder support it */
    /** Negotiate HEVC/AV1 Main10 without requiring HDR (SDR 10-bit; less banding). */
    bool force_10bit;
    bool force_full_color_range; /* SDR only: request full-range YUV (0-255) from host. No effect when HDR is on. */
    /** Report pad battery to host (Vibepollo/Sunshine virtual gamepads). Default on. */
    bool report_gamepad_battery;
    bool hevc;
    /** Sunshine/Apollo: negotiate AV1 Main8/Main10 when decoder exposes SS4S_VIDEO_AV1. */
    bool av1;
    /** Periodic HEVC IDR refresh interval in ms (0 = off, min 500 when enabled, step 500). */
    int idr_refresh_interval_ms;
    /**
     * webOS/NDL: how many frames to keep in the renderer's buffer by handing it a
     * future PTS, so the TV releases frames on its own vsync instead of on arrival.
     * 0 = off (present as soon as decoded). Each frame costs one frame of latency.
     */
    int render_queue_frames;
    /** Decode surround Opus in the client and feed PCM, skipping backend transcode. */
    bool surround_pcm;
    bool show_stats_on_start;
    bool show_stats_compact;
    /** On-screen log overlay preference (Yellow cycles Off/Live/Frozen). */
    bool show_logs;
    int stick_deadzone;
    /**
     * Sent to host as STREAM_CONFIGURATION.clientRefreshRateX100 (Hz * 100, e.g. 11994 = 119.94 Hz).
     * 0 = omit (host default frame pacing).
     */
    int client_refresh_rate_x100;
    /**
     * When true on webOS, map preset 30/60/120/240 fps to NTSC fractional rates
     * (e.g. 120 → 11988). When false, presets use integer fps (client_refresh_rate_x100 = 0).
     */
    bool use_ntsc_refresh;
    bool auto_adjust_bitrate;
    int abr_mode;
    /**
     * webOS rooted only: switch picture/sound to Game for the stream (not HDMI ALLM).
     * Default on; UI row only shown when Homebrew Channel elevated service is present.
     */
    bool game_mode;
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
#define RES_1800P RES_MERGE(3200, 1800)
/** ~90% of 4K (3584×2016); practical limit on LG C5 without cumulative 4K delay */
#define RES_3_6K RES_MERGE(3584, 2016)
#define RES_4K RES_MERGE(3840, 2160)

/** Fixed decode-unit reassembly buffer (megabytes). */
#define VDEC_REASSEMBLY_BUFFER_MB 2

void settings_initialize(app_settings_t *config, char *conf_dir);

bool settings_read(app_settings_t *config);

bool settings_save(app_settings_t *config);

/** Keep stream.fps aligned with client_refresh_rate_x100 when a fractional rate is set. */
void settings_sync_refresh_rate(app_settings_t *config);

/** webOS: apply NTSC x100 only when use_ntsc_refresh for 30/60/120/240; else clear for presets. */
void settings_reconcile_refresh_rate(app_settings_t *config);

/** NTSC refresh rate (Hz × 100) for a nominal FPS preset, or 0 if not mapped (e.g. 144). */
int settings_ntsc_refresh_rate_x100_for_fps(int nominal_fps);

/** On webOS, set or clear client_refresh_rate_x100 for a preset FPS based on use_ntsc_refresh. */
void settings_apply_ntsc_preset_refresh(app_settings_t *config, int nominal_fps);

void settings_clear(app_settings_t *config);

int settings_optimal_bitrate(const SS4S_VideoCapabilities *capabilities, int w, int h, int fps);

bool audio_config_valid(int config);