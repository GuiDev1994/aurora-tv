#include "app.h"

#include "pref_obj.h"
#include "ui/settings/settings.controller.h"
#include "util/i18n.h"
#include "util/log_overlay.h"

#if TARGET_WEBOS
#include "platform/webos/game_mode.h"
#endif

#include <stdio.h>

typedef struct experimental_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;
    lv_obj_t *idr_checkbox;
    lv_obj_t *idr_slider;
    lv_obj_t *idr_hint;
    int idr_refresh_slider_value;
} experimental_pane_t;

static void pane_ctor(lv_fragment_t *self, void *args);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void on_show_logs_changed(lv_event_t *e);

static void idr_refresh_state_update(experimental_pane_t *pane);

static void idr_checkbox_activate(lv_event_t *e);

static void idr_refresh_checkbox_cb(lv_event_t *e);

static void idr_refresh_slider_cb(lv_event_t *e);

const lv_fragment_class_t settings_pane_experimental_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(experimental_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    experimental_pane_t *pane = (experimental_pane_t *) self;
    pane->parent = args;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    experimental_pane_t *pane = (experimental_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    pref_header(view, locstr("Experimental"));

    lv_obj_t *logs = pref_checkbox(view, locstr("Show logs"),
                                   &app_configuration->show_logs, false);
    lv_obj_add_event_cb(logs, on_show_logs_changed, LV_EVENT_VALUE_CHANGED, NULL);
    if (app_configuration->show_logs) {
        log_overlay_set_enabled(true);
    }

#if TARGET_WEBOS
    if (webos_game_mode_is_rooted()) {
        pref_checkbox(view, locstr("Game mode"), &app_configuration->game_mode, false);
        pref_desc_label(view,
                        locstr("Picture/sound Game for the stream (IGR on). Restored when it ends."),
                        false);
    }
#endif

    pref_header(view, locstr("Video"));

    lv_obj_t *idr_checkbox = lv_checkbox_create(view);
    lv_checkbox_set_text(idr_checkbox, locstr("Periodic decoder refresh (HEVC)"));
    if (app_configuration->idr_refresh_interval_ms >= 500) {
        lv_obj_add_state(idr_checkbox, LV_STATE_CHECKED);
    }
    pane->idr_checkbox = idr_checkbox;
    pane->idr_refresh_slider_value = app_configuration->idr_refresh_interval_ms >= 500
            ? app_configuration->idr_refresh_interval_ms
            : 10000;
    pref_checkbox_prepare_for_dpad(idr_checkbox);
    lv_obj_t *idr_slider = pref_slider(view, &pane->idr_refresh_slider_value, 500, 60000, 500);
    pane->idr_slider = idr_slider;
    pane->idr_hint = pref_desc_label(view,
        locstr("Optional HEVC keyframe every N ms (0.5–60 s). Off by default. "
               "Some TVs hitch at each refresh — leave OFF if you notice that."),
        false);
    lv_obj_add_event_cb(idr_checkbox, idr_checkbox_activate, LV_EVENT_CLICKED, pane);
    lv_obj_add_event_cb(idr_slider, idr_refresh_slider_cb, LV_EVENT_VALUE_CHANGED, pane);
    idr_refresh_state_update(pane);

    return view;
}

static void on_show_logs_changed(lv_event_t *e) {
    (void) e;
    log_overlay_set_enabled(app_configuration->show_logs);
}

static void idr_refresh_state_update(experimental_pane_t *pane) {
    const bool hevc_on = app_configuration->hevc;
    const bool refresh_on = app_configuration->idr_refresh_interval_ms >= 500;
    if (refresh_on) {
        lv_obj_add_state(pane->idr_checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(pane->idr_checkbox, LV_STATE_CHECKED);
    }
    if (!hevc_on) {
        lv_obj_add_state(pane->idr_checkbox, LV_STATE_DISABLED);
        lv_obj_add_state(pane->idr_slider, LV_STATE_DISABLED);
        lv_label_set_text(pane->idr_hint, locstr("Enable HEVC (Stream settings) to use periodic refresh."));
    } else {
        lv_obj_clear_state(pane->idr_checkbox, LV_STATE_DISABLED);
        if (refresh_on) {
            lv_obj_clear_state(pane->idr_slider, LV_STATE_DISABLED);
            lv_label_set_text_fmt(pane->idr_hint,
                                  locstr("Keyframe every %.1f s during HEVC. "
                                         "May cause a brief hitch on some TVs."),
                                  app_configuration->idr_refresh_interval_ms / 1000.0);
        } else {
            lv_obj_add_state(pane->idr_slider, LV_STATE_DISABLED);
            lv_label_set_text(pane->idr_hint,
                              locstr("Optional: keyframe every N seconds to reduce long-session "
                                     "artifact drift (0.5–60 s when enabled)."));
        }
    }
}

static void idr_checkbox_activate(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *cb = lv_event_get_current_target(e);
    if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
        lv_obj_clear_state(cb, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(cb, LV_STATE_CHECKED);
    }
    idr_refresh_checkbox_cb(e);
}

static void idr_refresh_checkbox_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_current_target(e);
    if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
        if (pane->idr_refresh_slider_value < 500) {
            pane->idr_refresh_slider_value = 10000;
        }
        app_configuration->idr_refresh_interval_ms = pane->idr_refresh_slider_value;
        if (pane->parent) {
            pane->parent->needs_stream_reconnect = true;
        }
    } else {
        app_configuration->idr_refresh_interval_ms = 0;
    }
    idr_refresh_state_update(pane);
}

static void idr_refresh_slider_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    if (app_configuration->idr_refresh_interval_ms >= 500) {
        app_configuration->idr_refresh_interval_ms = pane->idr_refresh_slider_value;
        if (pane->parent) {
            pane->parent->needs_stream_reconnect = true;
        }
    }
    idr_refresh_state_update(pane);
}
