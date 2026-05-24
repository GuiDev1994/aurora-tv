#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include "app.h"

#include "ui/config.h"
#include "util/navkey.h"
#include "os_info.h"

#include "app_settings.h"

typedef struct app_t app_t;

/** Launcher passes this struct; standalone callers must not use `launcher`. */
typedef struct settings_open_args_t {
    app_t *app;
    struct launcher_fragment_t *launcher;
} settings_open_args_t;

typedef struct {
    lv_fragment_t base;
    app_t *app;

    bool mini, pending_mini;

    /** Embedded below launcher top bar when non-NULL; traditional full-window UI otherwise. */
    struct launcher_fragment_t *launcher_host;

    lv_obj_t *nav;
    lv_group_t *nav_group;

    lv_obj_t *detail;
    lv_group_t *detail_group;

    lv_obj_t *tabview;
    lv_group_t **tab_groups;

    lv_obj_t *close_btn;

    lv_obj_t *active_dropdown;

    /** Launcher overlay: second AppBar row + dimmed body (embedded mode only). */
    lv_obj_t *embed_root;
    lv_obj_t *embed_appbar;

    /** Modal pane opened from the embedded AppBar (popup); destroyed when closed. */
    lv_obj_t *pane_mbox;
    lv_fragment_t *pane_fragment;
    /** Launcher embed only: focus group for msgbox content + close while pane popup is open. */
    lv_group_t *pane_popup_group;

    os_info_t os_info;
    /** Video/audio/streaming params: prompt reconnect if a session is active. */
    bool needs_stream_reconnect;
    /** Language changed: apply locale on close (no app quit). */
    bool needs_locale_reapply;
#if TARGET_WEBOS
    int panel_width;
    int panel_height;
    int panel_fps;
#endif
} settings_controller_t;

lv_obj_t *settings_win_create(lv_fragment_t *self, lv_obj_t *parent);

/** Second-row settings shell under the launcher top bar (nav buttons + dim backdrop). */
lv_obj_t *settings_launcher_embedded_create(lv_fragment_t *self, lv_obj_t *parent);

extern const lv_fragment_class_t settings_controller_cls;
extern const lv_fragment_class_t settings_pane_basic_cls;
extern const lv_fragment_class_t settings_pane_host_cls;
extern const lv_fragment_class_t settings_pane_input_cls;
extern const lv_fragment_class_t settings_pane_audio_cls;
extern const lv_fragment_class_t settings_pane_video_cls;
extern const lv_fragment_class_t settings_pane_about_cls;
