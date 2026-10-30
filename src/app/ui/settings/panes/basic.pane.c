#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "pref_fps.h"
#include "pref_res.h"
#include "av_pane.h"
#include "ui/settings/settings.controller.h"

#include "util/i18n.h"
#include "ss4s.h"
#include "ss4s_modules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Compact stream settings (punktfunk FocusRow style): one labelled control per row,
 * plus the home screen grid layout. Advanced host/input/experimental stay in their panes.
 */
typedef struct {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_obj_t *bitrate_label;
    lv_obj_t *bitrate_slider;
    lv_obj_t *hevc_checkbox;
    lv_obj_t *av1_checkbox;
    lv_obj_t *hdr_checkbox;
    lv_obj_t *force_10bit_checkbox;

    pref_dropdown_string_entry_t *vdec_entries;
    int vdec_entries_len;
    pref_dropdown_string_entry_t *adec_entries;
    int adec_entries_len;

    pref_dropdown_int_entry_t surround_entries[3];
    int surround_entries_len;
} basic_pane_t;

static void pane_ctor(lv_fragment_t *self, void *args);

static void pane_dtor(lv_fragment_t *self);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void on_bitrate_changed(lv_event_t *e);

static void on_res_fps_updated(lv_event_t *e);

static void update_bitrate_label(basic_pane_t *pane);

static void hdr_state_update(basic_pane_t *pane);

static void hdr_state_update_cb(lv_event_t *e);

static void codec_checkbox_cb(lv_event_t *e);

static void module_changed_cb(lv_event_t *e);

static void backend_changed_cb(lv_event_t *e);

const lv_fragment_class_t settings_pane_basic_cls = {
    .constructor_cb = pane_ctor,
    .destructor_cb = pane_dtor,
    .create_obj_cb = create_obj,
    .instance_size = sizeof(basic_pane_t),
};

/* Home screen layout as columns × rows (columns come from apps_grid_columns()); the
 * label needs no translation. The grid is rebuilt when Settings closes. */
static const pref_dropdown_int_entry_t launcher_grid_entries[] = {
    {"8 × 3", 3, true},
    {"5 × 2", 2, false},
    {"3 × 1", 1, false},
};

#define BITRATE_STEP 1000

static void pane_ctor(lv_fragment_t *self, void *args) {
    basic_pane_t *pane = (basic_pane_t *) self;
    pane->parent = args;
    app_t *app = pane->parent->app;

    array_list_t modules = app->ss4s.modules;
    pane->vdec_entries = calloc(modules.size + 1, sizeof(pref_dropdown_string_entry_t));
    set_decoder_entry(&pane->vdec_entries[pane->vdec_entries_len++], locstr("Auto"), "auto", true);
    for (int module_idx = 0; module_idx < modules.size; module_idx++) {
        const SS4S_ModuleInfo *info = array_list_get(&modules, module_idx);
        const char *group = SS4S_ModuleInfoGetGroup(info);
        if (info->has_video && !contains_decoder_group(pane->vdec_entries, pane->vdec_entries_len, group)) {
            set_decoder_entry(&pane->vdec_entries[pane->vdec_entries_len++], info->name, group, false);
        }
    }
    pane->adec_entries = calloc(modules.size + 1, sizeof(pref_dropdown_string_entry_t));
    set_decoder_entry(&pane->adec_entries[pane->adec_entries_len++], locstr("Auto"), "auto", true);
    for (int module_idx = 0; module_idx < modules.size; module_idx++) {
        const SS4S_ModuleInfo *info = array_list_get(&modules, module_idx);
        const char *group = SS4S_ModuleInfoGetGroup(info);
        if (info->has_audio && !contains_decoder_group(pane->adec_entries, pane->adec_entries_len, group)) {
            set_decoder_entry(&pane->adec_entries[pane->adec_entries_len++], info->name, group, false);
        }
    }

    unsigned int supported_ch = app->ss4s.audio_cap.maxChannels;
    if (supported_ch < 6) {
        supported_ch = 8;
    }
    for (int i = 0; i < (int) audio_config_len; i++) {
        audio_config_entry_t config = audio_configs[i];
        if (supported_ch < CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(config.configuration)) {
            continue;
        }
        pref_dropdown_int_entry_t *entry = &pane->surround_entries[pane->surround_entries_len];
        entry->name = locstr(config.name);
        entry->value = config.configuration;
        entry->fallback = config.configuration == AUDIO_CONFIGURATION_STEREO;
        pane->surround_entries_len++;
    }
}

static void pane_dtor(lv_fragment_t *self) {
    basic_pane_t *pane = (basic_pane_t *) self;
    free(pane->vdec_entries);
    free(pane->adec_entries);
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    basic_pane_t *pane = (basic_pane_t *) self;
    settings_controller_t *parent = pane->parent;
    app_t *app = parent->app;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    int max_width = (int) app->ss4s.video_cap.maxWidth, max_height = (int) app->ss4s.video_cap.maxHeight;
    int native_width = app->ui.width, native_height = app->ui.height;
#if TARGET_WEBOS
    if (parent->panel_width > 0 && parent->panel_height > 0) {
        native_width = parent->panel_width;
        native_height = parent->panel_height;
    }
#endif
    if (max_width == 0 || max_height == 0) {
        max_width = native_width;
        max_height = native_height;
    }

    lv_obj_t *res_row = pref_focus_row(view, locstr("Resolution"));
    lv_obj_t *res_dropdown = pref_dropdown_res(res_row, native_width, native_height, max_width, max_height,
                                               &app_configuration->stream.width, &app_configuration->stream.height);
    pref_style_inline_control(res_dropdown);
    pref_row_bind_control(res_row, res_dropdown);
    lv_obj_add_event_cb(res_dropdown, on_res_fps_updated, LV_EVENT_VALUE_CHANGED, self);

    unsigned int max_fps = app->ss4s.video_cap.maxFps;
#if TARGET_WEBOS
    if (parent->panel_fps > 0 && (max_fps == 0 || parent->panel_fps < max_fps)) {
        max_fps = parent->panel_fps;
    }
#endif
    const static int fps_options[] = {30, 60, 90, 120, 144, 240, 0};
    lv_obj_t *fps_row = pref_focus_row(view, locstr("Frame rate"));
    lv_obj_t *fps_dropdown = pref_dropdown_fps(fps_row, fps_options, (int) max_fps, &app_configuration->stream.fps,
                                               &app_configuration->client_refresh_rate_x100);
    pref_style_inline_control(fps_dropdown);
    pref_row_bind_control(fps_row, fps_dropdown);
    lv_obj_add_event_cb(fps_dropdown, on_res_fps_updated, LV_EVENT_VALUE_CHANGED, self);

    lv_obj_t *br_row = pref_focus_row(view, locstr("Bitrate"));
    pane->bitrate_label = lv_label_create(br_row);
    lv_obj_set_width(pane->bitrate_label, LV_DPX(96));
    lv_obj_set_style_text_align(pane->bitrate_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(pane->bitrate_label, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    lv_obj_t *bitrate_slider = pref_slider(br_row, &app_configuration->stream.bitrate, SETTINGS_BITRATE_MIN_KBPS,
                                           SETTINGS_BITRATE_MAX_KBPS, BITRATE_STEP);
    lv_obj_set_width(bitrate_slider, LV_DPX(188));
    lv_obj_clear_flag(bitrate_slider, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    lv_obj_add_event_cb(bitrate_slider, on_bitrate_changed, LV_EVENT_VALUE_CHANGED, self);
    pane->bitrate_slider = bitrate_slider;
    pref_row_bind_control(br_row, bitrate_slider);
    update_bitrate_label(pane);

    lv_obj_t *vdec_row = pref_focus_row(view, locstr("Video backend"));
    lv_obj_t *vdec_dropdown = pref_dropdown_string(vdec_row, pane->vdec_entries, pane->vdec_entries_len,
                                                   &app_configuration->decoder);
    pref_style_inline_control(vdec_dropdown);
    pref_row_bind_control(vdec_row, vdec_dropdown);
    lv_obj_add_event_cb(vdec_dropdown, backend_changed_cb, LV_EVENT_VALUE_CHANGED, pane);

    lv_obj_t *hevc_checkbox = pref_checkbox(view, locstr("HEVC"), &app_configuration->hevc, false);
    pane->hevc_checkbox = hevc_checkbox;
    lv_obj_add_event_cb(hevc_checkbox, codec_checkbox_cb, LV_EVENT_VALUE_CHANGED, pane);

    lv_obj_t *av1_checkbox = pref_checkbox(view, locstr("AV1"), &app_configuration->av1, false);
    pane->av1_checkbox = av1_checkbox;
    if (app->ss4s.video_cap.codecs & SS4S_VIDEO_AV1) {
        lv_obj_clear_state(av1_checkbox, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(av1_checkbox, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(av1_checkbox, codec_checkbox_cb, LV_EVENT_VALUE_CHANGED, pane);

    pane->hdr_checkbox = pref_checkbox(view, locstr("HDR"), &app_configuration->hdr, false);
    lv_obj_add_event_cb(pane->hdr_checkbox, hdr_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);

    pane->force_10bit_checkbox = pref_checkbox(view, locstr("10-bit SDR"), &app_configuration->force_10bit, false);
    lv_obj_add_event_cb(pane->force_10bit_checkbox, hdr_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);
    hdr_state_update(pane);

    lv_obj_t *audio_row = pref_focus_row(view, locstr("Audio"));
    lv_obj_t *ch_dropdown = pref_dropdown_int(audio_row, pane->surround_entries, pane->surround_entries_len,
                                              &app_configuration->stream.audioConfiguration, NULL);
    pref_style_inline_control(ch_dropdown);
    pref_row_bind_control(audio_row, ch_dropdown);
    lv_obj_add_event_cb(ch_dropdown, module_changed_cb, LV_EVENT_VALUE_CHANGED, pane);

    lv_obj_t *adec_row = pref_focus_row(view, locstr("Audio backend"));
    lv_obj_t *adec_dropdown = pref_dropdown_string(adec_row, pane->adec_entries, pane->adec_entries_len,
                                                   &app_configuration->audio_backend);
    pref_style_inline_control(adec_dropdown);
    pref_row_bind_control(adec_row, adec_dropdown);
    lv_obj_add_event_cb(adec_dropdown, backend_changed_cb, LV_EVENT_VALUE_CHANGED, pane);
    lv_obj_t *grid_row = pref_focus_row(view, locstr("Game grid"));
    lv_obj_t *grid_dropdown = pref_dropdown_int(grid_row, launcher_grid_entries,
                                                sizeof(launcher_grid_entries) / sizeof(launcher_grid_entries[0]),
                                                &app_configuration->launcher_grid_rows, NULL);
    pref_style_inline_control(grid_dropdown);
    pref_row_bind_control(grid_row, grid_dropdown);

    return view;
}

static void on_bitrate_changed(lv_event_t *e) {
    update_bitrate_label(lv_event_get_user_data(e));
}

static void on_res_fps_updated(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    (void) pane;
}

static void update_bitrate_label(basic_pane_t *pane) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d Mbps", app_configuration->stream.bitrate / 1000);
    lv_label_set_text(pane->bitrate_label, buf);
}

static void hdr_state_update(basic_pane_t *pane) {
    app_t *app = pane->parent->app;
    const bool hevc_hdr = app_configuration->hevc && (app->ss4s.video_cap.codecs & SS4S_VIDEO_H265);
    const bool av1_hdr = app_configuration->av1 && (app->ss4s.video_cap.codecs & SS4S_VIDEO_AV1);
    const bool main10_codec = hevc_hdr || av1_hdr;
    bool hdr_ok = main10_codec && app->ss4s.video_cap.hdr;
    if (hdr_ok) {
        lv_obj_clear_state(pane->hdr_checkbox, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(pane->hdr_checkbox, LV_STATE_DISABLED);
    }
    if (main10_codec) {
        lv_obj_clear_state(pane->force_10bit_checkbox, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(pane->force_10bit_checkbox, LV_STATE_DISABLED);
    }
}

static void hdr_state_update_cb(lv_event_t *e) {
    hdr_state_update(lv_event_get_user_data(e));
}

static void codec_checkbox_cb(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    /* Selecting AV1 must not leave HEVC advertised (host would pick HEVC). */
    if (target == pane->av1_checkbox && app_configuration->av1 && app_configuration->hevc) {
        app_configuration->hevc = false;
        lv_obj_clear_state(pane->hevc_checkbox, LV_STATE_CHECKED);
    } else if (target == pane->hevc_checkbox && app_configuration->hevc && app_configuration->av1) {
        app_configuration->av1 = false;
        lv_obj_clear_state(pane->av1_checkbox, LV_STATE_CHECKED);
    }
    hdr_state_update(pane);
}

static void module_changed_cb(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    settings_controller_t *parent = pane->parent;
    parent->needs_stream_reconnect = true;
    hdr_state_update(pane);
}

static void backend_changed_cb(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_app_restart = true;
    module_changed_cb(e);
}
