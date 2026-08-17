#include "app.h"
#include "help.dialog.h"

#include "util/i18n.h"
#include "os_info.h"

#include <stdlib.h>
#include <stdio.h>

#if TARGET_WEBOS
#include <SDL.h>
#endif

static void menu_cancel_cb(lv_event_t *e);

static void help_dialog_resize_cb(lv_event_t *e);

static void help_open_url_cb(lv_event_t *e);

static void add_url_button(lv_obj_t *list, const char *title, const char *url);

static void add_about_line(lv_obj_t *list, const char *title, const char *value);

static const char *module_id(const SS4S_ModuleInfo *info);

lv_obj_t *help_dialog_create(app_t *app) {
    lv_obj_t *msgbox = lv_msgbox_create(NULL, locstr("Help"), NULL, NULL, true);
    lv_obj_clear_flag(msgbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lv_obj_get_parent(msgbox), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(msgbox, help_dialog_resize_cb, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_set_size(msgbox, LV_PCT(80), LV_PCT(90));

    lv_obj_t *close = lv_msgbox_get_close_btn(msgbox);
    lv_group_remove_obj(close);

    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *list = lv_list_create(content);
    lv_obj_add_event_cb(list, menu_cancel_cb, LV_EVENT_CANCEL, msgbox);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_border_side(list, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);

    lv_list_add_text(list, locstr("About"));
    if (app != NULL) {
        add_about_line(list, locstr("Version"), APP_VERSION);
        add_about_line(list, locstr("Video decoder"), module_id(app->ss4s.selection.video_module));
        add_about_line(list, locstr("Audio backend"), module_id(app->ss4s.selection.audio_module));
        char *os_str = os_info_str(&app->os_info);
        if (os_str != NULL) {
            add_about_line(list, locstr("System"), os_str);
            free(os_str);
        }
#if TARGET_WEBOS
        int panel_w = 0, panel_h = 0;
        if (SDL_webOSGetPanelResolution(&panel_w, &panel_h) && panel_w > 0 && panel_h > 0) {
            char resolution_text[32];
            snprintf(resolution_text, sizeof(resolution_text), "%d × %d", panel_w, panel_h);
            add_about_line(list, locstr("Screen resolution"), resolution_text);
        }
        int panel_fps = 0;
        if (SDL_webOSGetRefreshRate(&panel_fps) && panel_fps > 0) {
            char fps_text[32];
            if (panel_fps > 120) {
                snprintf(fps_text, sizeof(fps_text), "120 Hz (VRR max %d)", panel_fps);
            } else {
                snprintf(fps_text, sizeof(fps_text), "%d FPS", panel_fps);
            }
            add_about_line(list, locstr("Refresh rate"), fps_text);
        }
#endif
    }

    lv_list_add_text(list, locstr("Input"));
    add_url_button(list, locstr("Gamepad hotkeys"),
                   "https://github.com/mariotaku/moonlight-tv/wiki/Input-Shortcuts#gamepad");
    add_url_button(list, locstr("Remote controller keys"),
                   "https://github.com/mariotaku/moonlight-tv/wiki/Input-Shortcuts#webos-remote-controller");
    lv_list_add_text(list, locstr("More help"));
    add_url_button(list, locstr("Open moonlight-tv Wiki"),
                   "https://github.com/mariotaku/moonlight-tv/wiki");
    add_url_button(list, locstr("Open moonlight-stream Wiki"),
                   "https://github.com/moonlight-stream/moonlight-docs/wiki");

    lv_obj_center(msgbox);
    return msgbox;
}

static void menu_cancel_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    if (target->parent != lv_event_get_current_target(e)) return;
    lv_msgbox_close(lv_event_get_user_data(e));
}

static void help_dialog_resize_cb(lv_event_t *e) {
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    lv_obj_t *title = lv_msgbox_get_title(msgbox);
    lv_obj_t *close_btn = lv_msgbox_get_close_btn(msgbox);
    lv_coord_t title_height = LV_MAX(lv_obj_get_height(title), lv_obj_get_height(close_btn));
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_coord_t height = lv_obj_get_content_height(msgbox);
    lv_obj_set_height(content, height - title_height);
}

static void help_open_url_cb(lv_event_t *e) {
    app_open_url(lv_event_get_user_data(e));
}

static void add_url_button(lv_obj_t *list, const char *title, const char *url) {
    lv_obj_t *item = lv_list_add_btn(list, NULL, locstr(title));
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(item, help_open_url_cb, LV_EVENT_CLICKED, (void *) url);
}

static void add_about_line(lv_obj_t *list, const char *title, const char *value) {
    char line[192];
    snprintf(line, sizeof(line), "%s: %s", title, value != NULL ? value : "(null)");
    lv_list_add_text(list, line);
}

static const char *module_id(const SS4S_ModuleInfo *info) {
    const char *id = SS4S_ModuleInfoGetId(info);
    return id != NULL ? id : "Not available";
}
