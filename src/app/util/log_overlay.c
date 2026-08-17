#include "log_overlay.h"

#include "app.h"
#include "app_settings.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"
#include "stream/session.h"
#include "stream/video/session_video.h"

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define LOG_OVERLAY_CAPACITY 100
#define LOG_OVERLAY_LINE_MAX 200
#define LOG_OVERLAY_REFRESH_MS 100
#define LOG_OVERLAY_VISIBLE 12
#define LOG_OVERLAY_HEARTBEAT_MS 1000

static char ring[LOG_OVERLAY_CAPACITY][LOG_OVERLAY_LINE_MAX];
static int ring_count = 0;
static int ring_head = 0;
static SDL_mutex *ring_lock = NULL;
static volatile int capture = 0;
static volatile int dirty = 0;

static log_overlay_state_t state = LOG_OVERLAY_OFF;
static char frozen[LOG_OVERLAY_CAPACITY][LOG_OVERLAY_LINE_MAX];
static int frozen_count = 0;

static lv_obj_t *panel = NULL;
static lv_obj_t *label = NULL;
static lv_timer_t *refresh_timer = NULL;
static Uint32 last_heartbeat_ms = 0;

static char last_body[LOG_OVERLAY_LINE_MAX];
static int last_repeat = 0;
static Uint32 last_idr_ms = 0;

static void ensure_ui(void);
static void rebuild_label_text(void);
static void set_panel_visible(bool visible);
static void apply_live(void);
static void apply_off(void);
static void refresh_timer_cb(lv_timer_t *t);
static void push_line_fmt(const char *tag, const char *fmt, ...);

static const char *level_char(commons_log_level level) {
    switch (level) {
        case COMMONS_LOG_LEVEL_FATAL:
            return "F";
        case COMMONS_LOG_LEVEL_ERROR:
            return "E";
        case COMMONS_LOG_LEVEL_WARN:
            return "W";
        case COMMONS_LOG_LEVEL_INFO:
            return "I";
        case COMMONS_LOG_LEVEL_DEBUG:
            return "D";
        default:
            return "V";
    }
}

static bool tag_priority(const char *tag) {
    if (tag == NULL) {
        return false;
    }
    return strcmp(tag, "Session") == 0
           || strcmp(tag, "SS4S") == 0
           || strcmp(tag, "SMP") == 0
           || strcmp(tag, "NDL") == 0
           || strcmp(tag, "ABR") == 0
           || strcmp(tag, "APP") == 0
           || strcmp(tag, "Video") == 0
           || strcmp(tag, "Log") == 0;
}

static bool limelight_interesting(const char *message) {
    if (message == NULL) {
        return false;
    }
    if (strstr(message, "Unrecoverable") != NULL
        || strstr(message, "RFI") != NULL
        || strstr(message, "post-invalidation") != NULL
        || strstr(message, "Invalidate reference") != NULL
        || strstr(message, "Network") != NULL
        || strstr(message, "Requesting IDR") != NULL
        || strstr(message, "Waiting for IDR") != NULL
        || strstr(message, "consecutive drop") != NULL) {
        return true;
    }
    if (strstr(message, "IDR frame request") != NULL) {
        Uint32 now = SDL_GetTicks();
        if (last_idr_ms != 0 && (now - last_idr_ms) < 500u) {
            return false;
        }
        last_idr_ms = now;
        return true;
    }
    return false;
}

static bool should_capture(commons_log_level level, const char *tag, const char *message) {
    if (level <= COMMONS_LOG_LEVEL_WARN) {
        return true;
    }
    if (tag_priority(tag)) {
        return true;
    }
    if (tag != NULL && strcmp(tag, "Limelight") == 0) {
        return limelight_interesting(message);
    }
    return false;
}

static void ring_push_unlocked(const char *line) {
    const char *body = line;
    const char *bracket = strchr(line, ']');
    if (bracket != NULL && bracket[1] == ' ') {
        body = bracket + 2;
    }
    if (last_repeat > 0 && strcmp(body, last_body) == 0) {
        last_repeat++;
        int prev = (ring_head - 1 + LOG_OVERLAY_CAPACITY) % LOG_OVERLAY_CAPACITY;
        snprintf(ring[prev], LOG_OVERLAY_LINE_MAX, "%s (x%d)", line, last_repeat);
        return;
    }
    strncpy(last_body, body, LOG_OVERLAY_LINE_MAX - 1);
    last_body[LOG_OVERLAY_LINE_MAX - 1] = '\0';
    last_repeat = 1;

    strncpy(ring[ring_head], line, LOG_OVERLAY_LINE_MAX - 1);
    ring[ring_head][LOG_OVERLAY_LINE_MAX - 1] = '\0';
    ring_head = (ring_head + 1) % LOG_OVERLAY_CAPACITY;
    if (ring_count < LOG_OVERLAY_CAPACITY) {
        ring_count++;
    }
}

static int copy_recent_unlocked(char out[][LOG_OVERLAY_LINE_MAX], int max_lines) {
    int n = ring_count < max_lines ? ring_count : max_lines;
    int start = (ring_head - n + LOG_OVERLAY_CAPACITY) % LOG_OVERLAY_CAPACITY;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % LOG_OVERLAY_CAPACITY;
        memcpy(out[i], ring[idx], LOG_OVERLAY_LINE_MAX);
    }
    return n;
}

static void push_line_fmt(const char *tag, const char *fmt, ...) {
    if (!capture || ring_lock == NULL) {
        return;
    }
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[LOG_OVERLAY_LINE_MAX];
    snprintf(line, sizeof(line), "%ld.%03ld I [%s] %s",
             (long) ts.tv_sec, ts.tv_nsec / 1000000L,
             tag != NULL ? tag : "Log", msg);

    SDL_LockMutex(ring_lock);
    ring_push_unlocked(line);
    SDL_UnlockMutex(ring_lock);
    dirty = 1;
}

static void heartbeat_tick(void) {
    if (state != LOG_OVERLAY_LIVE || !capture) {
        return;
    }
    Uint32 now = SDL_GetTicks();
    if (last_heartbeat_ms != 0 && (now - last_heartbeat_ms) < LOG_OVERLAY_HEARTBEAT_MS) {
        return;
    }
    last_heartbeat_ms = now;

    struct VIDEO_STATS st;
    memset(&st, 0, sizeof(st));
    vdec_stats_snapshot(&st);

    push_line_fmt("Session", "live rx=%.1f de=%.1f D=%.1fms RQ=%d t=%u",
                  st.receivedFps, st.decodedFps, st.avgDecoderLatency,
                  st.videoRenderQueue, (unsigned) (now / 1000u));
}

static void rebuild_label_text(void) {
    if (label == NULL || !lv_obj_is_valid(label)) {
        return;
    }
    static char text[LOG_OVERLAY_CAPACITY * 96];
    static char lines[LOG_OVERLAY_CAPACITY][LOG_OVERLAY_LINE_MAX];
    size_t off = 0;
    text[0] = '\0';

    int n = 0;
    if (state == LOG_OVERLAY_FROZEN) {
        n = frozen_count;
        for (int i = 0; i < n; i++) {
            memcpy(lines[i], frozen[i], LOG_OVERLAY_LINE_MAX);
        }
    } else if (state == LOG_OVERLAY_LIVE && ring_lock != NULL) {
        SDL_LockMutex(ring_lock);
        n = copy_recent_unlocked(lines, LOG_OVERLAY_CAPACITY);
        SDL_UnlockMutex(ring_lock);
    }

    const char *mode = state == LOG_OVERLAY_FROZEN ? "LOG (frozen)" : "LOG (live)";
    off += (size_t) snprintf(text + off, sizeof(text) - off,
                             "%s — Yellow: Live/Frozen/Off | Settings ON = auto Live\n", mode);

    int start = n > LOG_OVERLAY_VISIBLE ? n - LOG_OVERLAY_VISIBLE : 0;
    for (int i = start; i < n && off + 2 < sizeof(text); i++) {
        char clipped[120];
        strncpy(clipped, lines[i], sizeof(clipped) - 1);
        clipped[sizeof(clipped) - 1] = '\0';
        off += (size_t) snprintf(text + off, sizeof(text) - off, "%s\n", clipped);
    }
    lv_label_set_text(label, text);
    if (panel != NULL && lv_obj_is_valid(panel)) {
        lv_obj_update_layout(panel);
        lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_move_foreground(panel);
    }
    dirty = 0;
}

static void set_panel_visible(bool visible) {
    if (panel == NULL || !lv_obj_is_valid(panel)) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel);
        rebuild_label_text();
    } else {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void destroy_ui(void) {
    if (refresh_timer != NULL) {
        lv_timer_del(refresh_timer);
        refresh_timer = NULL;
    }
    if (panel != NULL) {
        if (lv_obj_is_valid(panel)) {
            lv_obj_del(panel);
        }
        panel = NULL;
        label = NULL;
    }
}

static void ensure_ui(void) {
    if (panel != NULL && lv_obj_is_valid(panel) && label != NULL && lv_obj_is_valid(label)) {
        lv_obj_move_foreground(panel);
        return;
    }
    panel = NULL;
    label = NULL;

    lv_obj_t *parent = lv_layer_sys();
    if (parent == NULL) {
        parent = lv_layer_top();
    }
    if (parent == NULL) {
        return;
    }

    panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, ml_color_hex(ML_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_pad_left(panel, LV_DPX(10), 0);
    lv_obj_set_style_pad_right(panel, LV_DPX(10), 0);
    lv_obj_set_style_pad_top(panel, LV_DPX(6), 0);
    lv_obj_set_style_pad_bottom(panel, LV_DPX(24), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    label = lv_label_create(panel);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(panel), 0);
    lv_obj_set_style_text_line_space(label, LV_DPX(0), 0);
    lv_label_set_text(label, "");
}

static void refresh_timer_cb(lv_timer_t *t) {
    (void) t;
    if (state != LOG_OVERLAY_LIVE) {
        return;
    }
    if (panel == NULL || !lv_obj_is_valid(panel)) {
        ensure_ui();
        if (panel != NULL) {
            set_panel_visible(true);
        }
    }
    heartbeat_tick();
    if (dirty) {
        rebuild_label_text();
    }
}

static void apply_live(void) {
    ensure_ui();
    capture = 1;
    state = LOG_OVERLAY_LIVE;
    dirty = 1;
    last_heartbeat_ms = 0;
    if (refresh_timer == NULL) {
        refresh_timer = lv_timer_create(refresh_timer_cb, LOG_OVERLAY_REFRESH_MS, NULL);
    } else {
        lv_timer_resume(refresh_timer);
    }
    set_panel_visible(true);
    if (app_configuration) {
        app_configuration->show_logs = true;
    }
    push_line_fmt("Log", "Live started");
}

static void apply_off(void) {
    capture = 0;
    state = LOG_OVERLAY_OFF;
    if (refresh_timer) {
        lv_timer_pause(refresh_timer);
    }
    set_panel_visible(false);
    if (app_configuration) {
        app_configuration->show_logs = false;
    }
}

void log_overlay_init(void) {
    if (ring_lock == NULL) {
        ring_lock = SDL_CreateMutex();
    }
    last_body[0] = '\0';
    last_repeat = 0;
    last_idr_ms = 0;
    last_heartbeat_ms = 0;
    commons_log_set_listener(log_overlay_on_log);
    ensure_ui();
    if (refresh_timer == NULL) {
        refresh_timer = lv_timer_create(refresh_timer_cb, LOG_OVERLAY_REFRESH_MS, NULL);
        lv_timer_pause(refresh_timer);
    }
    if (app_configuration != NULL && app_configuration->show_logs) {
        log_overlay_set_enabled(true);
    }
}

void log_overlay_deinit(void) {
    commons_log_set_listener(NULL);
    destroy_ui();
    if (ring_lock != NULL) {
        SDL_DestroyMutex(ring_lock);
        ring_lock = NULL;
    }
    capture = 0;
    dirty = 0;
    state = LOG_OVERLAY_OFF;
}

void log_overlay_reassert(void) {
    ensure_ui();
    if (app_configuration != NULL && app_configuration->show_logs) {
        apply_live();
    } else if (state == LOG_OVERLAY_LIVE || state == LOG_OVERLAY_FROZEN) {
        set_panel_visible(true);
    }
}

void log_overlay_on_log(commons_log_level level, const char *tag, const char *message) {
    if (!capture || message == NULL) {
        return;
    }
    if (!should_capture(level, tag, message)) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[LOG_OVERLAY_LINE_MAX];
    snprintf(line, sizeof(line), "%ld.%03ld %s [%s] %s",
             (long) ts.tv_sec, ts.tv_nsec / 1000000L,
             level_char(level), tag != NULL ? tag : "?", message);
    if (ring_lock == NULL) {
        return;
    }
    SDL_LockMutex(ring_lock);
    ring_push_unlocked(line);
    SDL_UnlockMutex(ring_lock);
    dirty = 1;
}

log_overlay_state_t log_overlay_get_state(void) {
    return state;
}

bool log_overlay_is_enabled(void) {
    return state != LOG_OVERLAY_OFF;
}

void log_overlay_set_enabled(bool enabled) {
    if (enabled) {
        apply_live();
    } else {
        apply_off();
    }
}

void log_overlay_cycle(void) {
    ensure_ui();
    switch (state) {
        case LOG_OVERLAY_OFF:
            apply_live();
            break;
        case LOG_OVERLAY_LIVE:
            if (ring_lock) {
                SDL_LockMutex(ring_lock);
                frozen_count = copy_recent_unlocked(frozen, LOG_OVERLAY_CAPACITY);
                SDL_UnlockMutex(ring_lock);
            }
            capture = 0;
            state = LOG_OVERLAY_FROZEN;
            if (refresh_timer) {
                lv_timer_pause(refresh_timer);
            }
            rebuild_label_text();
            set_panel_visible(true);
            if (app_configuration) {
                app_configuration->show_logs = true;
            }
            break;
        case LOG_OVERLAY_FROZEN:
        default:
            apply_off();
            break;
    }
}
