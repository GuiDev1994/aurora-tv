#pragma once

#include <lvgl.h>

typedef struct session_t session_t;

typedef void (*hid_passthrough_panel_close_cb)(void *userdata);

#if defined(TARGET_WEBOS)

lv_obj_t *hid_passthrough_panel_create(lv_obj_t *parent, session_t *session,
                                       hid_passthrough_panel_close_cb on_close, void *userdata);

void hid_passthrough_panel_refresh(lv_obj_t *panel);

lv_group_t *hid_passthrough_panel_get_group(lv_obj_t *panel);

#else

static inline lv_obj_t *hid_passthrough_panel_create(lv_obj_t *parent, session_t *session,
                                                     hid_passthrough_panel_close_cb on_close, void *userdata) {
    (void) parent;
    (void) session;
    (void) on_close;
    (void) userdata;
    return NULL;
}

static inline void hid_passthrough_panel_refresh(lv_obj_t *panel) {
    (void) panel;
}

static inline lv_group_t *hid_passthrough_panel_get_group(lv_obj_t *panel) {
    (void) panel;
    return NULL;
}

#endif
