#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "av_pane.h"
#include "app_settings.h"

#include "ui/settings/settings.controller.h"

#include "util/i18n.h"
#include "ss4s.h"
#include "ss4s_modules.h"

typedef struct video_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_obj_t *conflict_hint;
    lv_obj_t *hdr_checkbox;
    lv_obj_t *hdr_hint;
    lv_obj_t *idr_refresh_checkbox;
    lv_obj_t *idr_refresh_slider;
    lv_obj_t *idr_refresh_hint;
    int idr_refresh_slider_value;
    bool idr_refresh_on;

    pref_dropdown_string_entry_t *vdec_entries;
    int vdec_entries_len;
} video_pane_t;

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void obj_created(lv_fragment_t *self, lv_obj_t *obj);

static void pane_ctor(lv_fragment_t *self, void *args);

static void pane_dtor(lv_fragment_t *self);

static void module_changed_cb(lv_event_t *e);

static void hdr_state_update_cb(lv_event_t *e);

static void hdr_more_click_cb(lv_event_t *e);

static void hdr_state_update(video_pane_t *controller);

static void idr_refresh_state_update(video_pane_t *controller);

static void idr_refresh_checkbox_cb(lv_event_t *e);

static void idr_checkbox_activate(lv_event_t *e);

static void idr_refresh_hevc_cb(lv_event_t *e);

static void idr_refresh_slider_cb(lv_event_t *e);

const lv_fragment_class_t settings_pane_video_cls = {
        .constructor_cb = pane_ctor,
        .destructor_cb = pane_dtor,
        .create_obj_cb = create_obj,
        .obj_created_cb = obj_created,
        .instance_size = sizeof(video_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    video_pane_t *pane = (video_pane_t *) self;
    pane->parent = args;
    array_list_t modules = pane->parent->app->ss4s.modules;
    pane->vdec_entries = calloc(modules.size + 1, sizeof(pref_dropdown_string_entry_t));

    set_decoder_entry(&pane->vdec_entries[pane->vdec_entries_len++], locstr("Auto"), "auto", true);
    for (int module_idx = 0; module_idx < modules.size; module_idx++) {
        const SS4S_ModuleInfo *info = array_list_get(&modules, module_idx);
        const char *group = SS4S_ModuleInfoGetGroup(info);
        if (info->has_video && !contains_decoder_group(pane->vdec_entries, pane->vdec_entries_len, group)) {
            set_decoder_entry(&pane->vdec_entries[pane->vdec_entries_len++], info->name, group, false);
        }
    }
}

static void pane_dtor(lv_fragment_t *self) {
    video_pane_t *pane = (video_pane_t *) self;
    free(pane->vdec_entries);
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    video_pane_t *controller = (video_pane_t *) self;
    app_t *app = controller->parent->app;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *decoder_label = pref_title_label(view, locstr("Video decoder"));
    lv_label_set_text_fmt(decoder_label, locstr("Video decoder - using %s"),
                          SS4S_ModuleInfoGetName(app->ss4s.selection.video_module));
    lv_obj_t *vdec_dropdown = pref_dropdown_string(view, controller->vdec_entries, controller->vdec_entries_len,
                                                   &app_configuration->decoder);
    lv_obj_set_width(vdec_dropdown, LV_PCT(100));
#if FEATURE_EMBEDDED_SHELL
    if (!app_is_decoder_valid(app) && app_has_embedded(app)) {
        lv_obj_add_flag(decoder_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(vdec_dropdown, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    lv_obj_t *conflict_hint = pref_desc_label(view, NULL, false);
    controller->conflict_hint = conflict_hint;

    pref_header(view, locstr("Video Settings"));

    lv_obj_t *hevc_checkbox = pref_checkbox(view, locstr("Use H265 when possible"), &app_configuration->hevc, false);
    lv_obj_t *hevc_hint = pref_desc_label(view, NULL, false);
    if (app->ss4s.video_cap.codecs & SS4S_VIDEO_H265) {
        lv_obj_clear_state(hevc_checkbox, LV_STATE_DISABLED);
        lv_label_set_text(hevc_hint, locstr("H265 usually has better graphics, and supports HDR."));
    } else {
        lv_obj_add_state(hevc_checkbox, LV_STATE_DISABLED);
        lv_label_set_text_fmt(hevc_hint, locstr("%s decoder doesn't support H265 codec."),
                              SS4S_ModuleInfoGetName(app->ss4s.selection.video_module));
    }

    lv_obj_t *hdr_checkbox = pref_checkbox(view, locstr("HDR"), &app_configuration->hdr, false);
    lv_obj_t *hdr_hint = pref_desc_label(view, NULL, false);
    controller->hdr_checkbox = hdr_checkbox;
    controller->hdr_hint = hdr_hint;

    hdr_state_update(controller);

    lv_obj_t *hdr_more = pref_desc_label(view, locstr("Learn more about HDR feature."), true);
    lv_obj_set_style_text_color(hdr_more, lv_theme_get_color_primary(hdr_more), 0);
    lv_obj_add_flag(hdr_more, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(vdec_dropdown, module_changed_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_add_event_cb(hevc_checkbox, hdr_state_update_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_add_event_cb(hdr_checkbox, hdr_state_update_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_add_event_cb(hdr_more, hdr_more_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *idr_checkbox = lv_checkbox_create(view);
    lv_checkbox_set_text(idr_checkbox, locstr("Periodic decoder refresh (HEVC)"));
    if (app_configuration->idr_refresh_interval_ms >= 500) {
        lv_obj_add_state(idr_checkbox, LV_STATE_CHECKED);
    }
    controller->idr_refresh_checkbox = idr_checkbox;
    controller->idr_refresh_slider_value = app_configuration->idr_refresh_interval_ms >= 500
            ? app_configuration->idr_refresh_interval_ms
            : 10000;
    pref_checkbox_prepare_for_dpad(idr_checkbox);
    lv_obj_t *idr_slider = pref_slider(view, &controller->idr_refresh_slider_value, 500, 60000, 500);
    controller->idr_refresh_slider = idr_slider;
    lv_obj_t *idr_hint = pref_desc_label(view,
        locstr("Request a keyframe every N seconds during HEVC streams to reduce long-session artifact drift. "
               "Off by default; use 10–30 s if you see blockiness or color smearing. Minimum 0.5 s."),
        false);
    controller->idr_refresh_hint = idr_hint;
    /* LVGL already synthesizes a CLICKED event for a focused object on ENTER
     * key release (see indev_keypad_proc in lv_indev.c), so also listening on
     * LV_EVENT_KEY here double-fires the toggle for one remote OK press. */
    lv_obj_add_event_cb(idr_checkbox, idr_checkbox_activate, LV_EVENT_CLICKED, controller);
    lv_obj_add_event_cb(idr_slider, idr_refresh_slider_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_add_event_cb(hevc_checkbox, idr_refresh_hevc_cb, LV_EVENT_VALUE_CHANGED, controller);
    idr_refresh_state_update(controller);

#if TARGET_WEBOS
    pref_checkbox(view, locstr("Smooth frame pacing"), &app_configuration->smooth_frame_pacing, false);
    pref_desc_label(view,
                    locstr("Stamp video presentation timestamps on a steady frame grid instead of "
                           "wall-clock feed time. Reduces hitching when camera pans at high bitrate "
                           "(e.g. 3.6K HDR HEVC). Disable only for A/B comparison."),
                    false);
#endif

    pref_header(view, locstr("Color"));
    pref_checkbox(view, locstr("Full range YUV (SDR only)"), &app_configuration->force_full_color_range, false);
    pref_desc_label(view,
                    locstr("Request full-range (0-255) color from the host for SDR streams. "
                           "Has no effect when HDR is enabled — HDR always uses limited range "
                           "(SMPTE ST 2084 standard). Disable if SDR colors look washed out."),
                    false);

    return view;
}

static void obj_created(lv_fragment_t *self, lv_obj_t *obj) {
    video_pane_t *fragment = (video_pane_t *) self;
    update_conflict_hint(fragment->parent->app, fragment->conflict_hint);
}

static void module_changed_cb(lv_event_t *e) {
    video_pane_t *fragment = (video_pane_t *) lv_event_get_user_data(e);
    settings_controller_t *parent = fragment->parent;
    parent->needs_stream_reconnect = true;
    update_conflict_hint(parent->app, fragment->conflict_hint);
}

static void hdr_state_update_cb(lv_event_t *e) {
    video_pane_t *controller = (video_pane_t *) lv_event_get_user_data(e);
    hdr_state_update(controller);
}

static void hdr_state_update(video_pane_t *controller) {
    app_t *app = controller->parent->app;
    const bool want_hevc_hdr = app_configuration->hevc && (app->ss4s.video_cap.codecs & SS4S_VIDEO_H265);
    if (app->ss4s.video_cap.hdr == 0) {
        lv_obj_add_state(controller->hdr_checkbox, LV_STATE_DISABLED);
        lv_label_set_text_fmt(controller->hdr_hint, locstr("%s decoder doesn't support HDR."),
                              SS4S_ModuleInfoGetName(app->ss4s.selection.video_module));
    } else if (!want_hevc_hdr) {
        lv_obj_add_state(controller->hdr_checkbox, LV_STATE_DISABLED);
        lv_label_set_text(controller->hdr_hint,
                          locstr("Enable H265 to stream HDR10 from the host."));
    } else {
        lv_obj_clear_state(controller->hdr_checkbox, LV_STATE_DISABLED);
        lv_label_set_text(controller->hdr_hint,
                          locstr("HDR10 (PQ) when the host streams HEVC Main10. "
                                 "HDR always uses limited color range (SMPTE ST 2084 standard)."));
    }
}

static void hdr_more_click_cb(lv_event_t *e) {
    (void) e;
    app_open_url("https://github.com/mariotaku/moonlight-tv/wiki/HDR-Support");
}

static void idr_refresh_state_update(video_pane_t *controller) {
    app_t *app = controller->parent->app;
    const bool hevc_capable = (app->ss4s.video_cap.codecs & SS4S_VIDEO_H265) != 0;
    const bool hevc_on = app_configuration->hevc && hevc_capable;
    const bool refresh_on = app_configuration->idr_refresh_interval_ms >= 500;
    controller->idr_refresh_on = refresh_on;
    if (refresh_on) {
        lv_obj_add_state(controller->idr_refresh_checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(controller->idr_refresh_checkbox, LV_STATE_CHECKED);
    }
    if (!hevc_on) {
        lv_obj_add_state(controller->idr_refresh_checkbox, LV_STATE_DISABLED);
        lv_obj_add_state(controller->idr_refresh_slider, LV_STATE_DISABLED);
        lv_label_set_text(controller->idr_refresh_hint,
                          locstr("Enable H265 to use periodic decoder refresh."));
    } else {
        lv_obj_clear_state(controller->idr_refresh_checkbox, LV_STATE_DISABLED);
        if (refresh_on) {
            lv_obj_clear_state(controller->idr_refresh_slider, LV_STATE_DISABLED);
            lv_label_set_text_fmt(controller->idr_refresh_hint,
                                  locstr("Request a keyframe every %.1f seconds during HEVC streams."),
                                  app_configuration->idr_refresh_interval_ms / 1000.0);
        } else {
            lv_obj_add_state(controller->idr_refresh_slider, LV_STATE_DISABLED);
            lv_label_set_text(controller->idr_refresh_hint,
                              locstr("Request a keyframe every N seconds during HEVC streams to reduce "
                                     "long-session artifact drift. Off by default (0.5–60 s when enabled)."));
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
    video_pane_t *controller = (video_pane_t *) lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_target(e);
    if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
        if (controller->idr_refresh_slider_value < 500) {
            controller->idr_refresh_slider_value = 10000;
        }
        app_configuration->idr_refresh_interval_ms = controller->idr_refresh_slider_value;
    } else {
        app_configuration->idr_refresh_interval_ms = 0;
    }
    idr_refresh_state_update(controller);
}

static void idr_refresh_slider_cb(lv_event_t *e) {
    video_pane_t *controller = (video_pane_t *) lv_event_get_user_data(e);
    if (app_configuration->idr_refresh_interval_ms >= 500) {
        app_configuration->idr_refresh_interval_ms = controller->idr_refresh_slider_value;
    }
    idr_refresh_state_update(controller);
}

static void idr_refresh_hevc_cb(lv_event_t *e) {
    video_pane_t *controller = (video_pane_t *) lv_event_get_user_data(e);
    hdr_state_update(controller);
    idr_refresh_state_update(controller);
}
