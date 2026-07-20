#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "pref_fps.h"
#include "pref_res.h"
#include "ui/settings/settings.controller.h"
#include "profile/profile_manager.h"

#include "util/i18n.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_obj_t *res_warning;
    lv_obj_t *bitrate_label;
    lv_obj_t *bitrate_slider;
    lv_obj_t *bitrate_warning;
    lv_obj_t *profile_dropdown;
    lv_obj_t *abr_checkbox;
    lv_obj_t *abr_mode_dropdown;

    pref_dropdown_string_entry_t *lang_entries;
    int lang_entries_len;
} basic_pane_t;

static void pane_ctor(lv_fragment_t *self, void *args);

static void pane_dtor(lv_fragment_t *self);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void on_bitrate_changed(lv_event_t *e);

static void on_res_fps_updated(lv_event_t *e);

static void on_fullscreen_updated(lv_event_t *e);

static void update_bitrate_label(basic_pane_t *pane);

static void init_locale_entries(basic_pane_t *pane);

static void pref_mark_restart_cb(lv_event_t *e);

static void update_bitrate_hint(basic_pane_t *pane);

static void on_profile_changed(lv_event_t *e);

static void on_save_profile_clicked(lv_event_t *e);

static void on_abr_changed(lv_event_t *e);

static void on_abr_mode_changed(lv_event_t *e);

static void on_ntsc_refresh_changed(lv_event_t *e);

static void refresh_profile_dropdown(basic_pane_t *pane);

static void on_new_profile_clicked(lv_event_t *e);

static void on_rename_profile_clicked(lv_event_t *e);

static void on_delete_profile_clicked(lv_event_t *e);

static void basic_pane_add_profile_section(basic_pane_t *pane, lv_obj_t *view);

const lv_fragment_class_t settings_pane_basic_cls = {
    .constructor_cb = pane_ctor,
    .destructor_cb = pane_dtor,
    .create_obj_cb = create_obj,
    .instance_size = sizeof(basic_pane_t),
};
#define BITRATE_STEP 1000

static void pane_ctor(lv_fragment_t *self, void *args) {
    basic_pane_t *pane = (basic_pane_t *) self;
    pane->parent = args;
#ifdef FEATURE_I18N_LANGUAGE_SETTINGS
    init_locale_entries(pane);
#endif
}

static void pane_dtor(lv_fragment_t *self) {
    basic_pane_t *pane = (basic_pane_t *) self;
#ifdef FEATURE_I18N_LANGUAGE_SETTINGS
    lv_mem_free(pane->lang_entries);
#endif
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    basic_pane_t *pane = (basic_pane_t *) self;
    settings_controller_t *parent = pane->parent;
    app_t *app = parent->app;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_ROW_WRAP);

    pane->abr_checkbox = pref_checkbox(view, locstr("Adaptive bitrate"), &app_configuration->auto_adjust_bitrate, false);
    lv_obj_add_event_cb(pane->abr_checkbox, on_abr_changed, LV_EVENT_VALUE_CHANGED, self);

    pref_title_label(view, locstr("ABR mode"));
    pane->abr_mode_dropdown = lv_dropdown_create(view);
    lv_dropdown_set_options(pane->abr_mode_dropdown, "Balanced\nQuality\nLow latency");
    lv_dropdown_set_selected(pane->abr_mode_dropdown, app_configuration->abr_mode);
    lv_obj_set_width(pane->abr_mode_dropdown, LV_PCT(100));
    lv_obj_add_event_cb(pane->abr_mode_dropdown, on_abr_mode_changed, LV_EVENT_VALUE_CHANGED, self);

    pref_title_label(view, locstr("Resolution and FPS"));


    int max_width = (int) app->ss4s.video_cap.maxWidth, max_height = (int) app->ss4s.video_cap.maxHeight;
    int native_width = app->ui.width, native_height = app->ui.height;

#if TARGET_WEBOS
    if (parent->panel_width > 0 && parent->panel_height > 0) {
        native_width = parent->panel_width;
        native_height = parent->panel_height;
    }

    commons_log_info("Settings", "Panel native resolution: %d x %d, maximum video resolution: %d x %d",
                     native_width, native_height, max_width, max_height);
#endif
    if (max_width == 0 || max_height == 0) {
        max_width = native_width;
        max_height = native_height;
    }

    lv_obj_t *res_dropdown = pref_dropdown_res(view, native_width, native_height, max_width, max_height,
                                               &app_configuration->stream.width, &app_configuration->stream.height);
    lv_obj_set_width(res_dropdown, LV_PCT(60));
    lv_obj_add_event_cb(res_dropdown, on_res_fps_updated, LV_EVENT_VALUE_CHANGED, self);

    unsigned int max_fps = app->ss4s.video_cap.maxFps;
#if TARGET_WEBOS
    if (parent->panel_fps > 0 && (max_fps == 0 || parent->panel_fps < max_fps)) {
        max_fps = parent->panel_fps;
    }
#endif
    const static int fps_options[] = {30, 60, 90, 120, 144, 240, 0};
    lv_obj_t *fps_dropdown = pref_dropdown_fps(view, fps_options, (int) max_fps, &app_configuration->stream.fps,
                                               &app_configuration->client_refresh_rate_x100);
    lv_obj_set_flex_grow(fps_dropdown, 1);
    lv_obj_add_event_cb(fps_dropdown, on_res_fps_updated, LV_EVENT_VALUE_CHANGED, self);

    lv_obj_t *ntsc_checkbox = pref_checkbox(view, locstr("Use NTSC refresh (59.94 / 119.88 Hz)"),
                                            &app_configuration->use_ntsc_refresh, false);
    lv_obj_add_event_cb(ntsc_checkbox, on_ntsc_refresh_changed, LV_EVENT_VALUE_CHANGED, self);
    pref_desc_label(view,
                    locstr("When enabled, 60/120 FPS presets use fractional NTSC pacing (59.94/119.88). "
                           "When disabled, presets use integer 60/120 like Moonlight mobile. "
                           "At 4K the host virtual-display mode stays integer (120) to avoid a black "
                           "screen on some webOS TVs; NTSC is still sent as clientRefreshRateX100 for "
                           "encode pacing. Custom FPS still allows any fractional rate."),
                    false);

    pref_desc_label(view,
                    locstr("Tip: choose Custom FPS to enter a fractional refresh rate (e.g. 119.94 for VRR game "
                           "mode). The exact value is sent to the host for frame pacing."),
                    false);

    pane->res_warning = lv_label_create(view);
    lv_obj_add_flag(pane->res_warning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(pane->res_warning, LV_PCT(100));
    lv_obj_set_style_text_font(pane->res_warning, lv_theme_get_font_small(view), 0);
    lv_obj_set_style_text_color(pane->res_warning, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_long_mode(pane->res_warning, LV_LABEL_LONG_WRAP);

    pane->bitrate_label = pref_title_label(view, locstr("Video bitrate"));

    /* User-facing slider cap. Default max 300 Mbps for stable streaming on webOS. */
    unsigned int max = 300000;
    lv_obj_t *bitrate_slider = pref_slider(view, &app_configuration->stream.bitrate, 5000, (int) max, BITRATE_STEP);
    lv_obj_set_width(bitrate_slider, LV_PCT(100));
    lv_obj_add_event_cb(bitrate_slider, on_bitrate_changed, LV_EVENT_VALUE_CHANGED, self);
    pane->bitrate_slider = bitrate_slider;

    pane->bitrate_warning = lv_label_create(view);
    lv_obj_add_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(pane->bitrate_warning, LV_PCT(100));
    lv_obj_set_style_text_font(pane->bitrate_warning, lv_theme_get_font_small(view), 0);
    lv_obj_set_style_text_color(pane->bitrate_warning, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_long_mode(pane->bitrate_warning, LV_LABEL_LONG_WRAP);

#if !FEATURE_FORCE_FULLSCREEN
    lv_obj_t *checkbox = pref_checkbox(view, locstr("Fullscreen UI"), &app_configuration->fullscreen, false);
    if (app->ss4s.video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_AREA_DEST) {
        lv_obj_add_event_cb(checkbox, on_fullscreen_updated, LV_EVENT_VALUE_CHANGED, pane);
    } else {
        lv_obj_add_state(checkbox, LV_STATE_DISABLED);
        pref_desc_label(view, locstr("Can't use windowed UI for this decoder"), false);
    }
#endif

    lv_obj_t *show_stats_checkbox = pref_checkbox(view, locstr("Show performance stats on stream start"),
                                                   &app_configuration->show_stats_on_start, false);
    pref_desc_label(view, locstr("Start streaming with performance overlay visible and pinned."), false);

    lv_obj_t *show_stats_compact_checkbox = pref_checkbox(view, locstr("Compact performance stats (single line)"),
                                                          &app_configuration->show_stats_compact, false);
    pref_desc_label(view, locstr("Show minimalist one-line stats like Moonlight Android (FPS, RTT, bitrate)."), false);

#ifdef FEATURE_I18N_LANGUAGE_SETTINGS
    lv_obj_t *lang_label = pref_title_label(view, locstr("Language"));

    lv_obj_t *language_dropdown = pref_dropdown_string(view, pane->lang_entries, pane->lang_entries_len,
                                                       &app_configuration->language);
    lv_obj_add_event_cb(language_dropdown, pref_mark_restart_cb, LV_EVENT_VALUE_CHANGED, pane);
    lv_obj_set_width(language_dropdown, LV_PCT(100));
#endif

    update_bitrate_label(pane);
    update_bitrate_hint(pane);

    basic_pane_add_profile_section(pane, view);

    return view;
}

static void on_bitrate_changed(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
    update_bitrate_label(pane);
    update_bitrate_hint(pane);
}

static void on_res_fps_updated(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
    /* Do NOT auto-bump bitrate to match the new resolution/FPS. The user is the sole owner
     * of the bitrate slider; previously this handler called settings_optimal_bitrate() and
     * forced the slider up to that value (e.g. 300 Mbps), silently overwriting whatever the
     * user had chosen. That behavior was confusing and effectively prevented running high
     * resolutions at moderate bitrates. */
    if (app_configuration->stream.width > 1920 && app_configuration->stream.height > 1080 &&
        app_configuration->stream.fps > 60) {
        lv_obj_clear_flag(pane->res_warning, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(pane->res_warning, locstr("Your computer may not perform well when using this "
                                                           "resolution and framerate."));
    } else {
        lv_obj_add_flag(pane->res_warning, LV_OBJ_FLAG_HIDDEN);
    }
    update_bitrate_label(pane);
    update_bitrate_hint(pane);
}

static void on_ntsc_refresh_changed(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
    settings_apply_ntsc_preset_refresh(app_configuration, app_configuration->stream.fps);
}

static void on_fullscreen_updated(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    app_set_fullscreen(pane->parent->app, app_configuration->fullscreen);
}

static void update_bitrate_label(basic_pane_t *pane) {
    lv_label_set_text_fmt(pane->bitrate_label, locstr("Video bitrate - %d kbps"), app_configuration->stream.bitrate);
}

static void update_bitrate_hint(basic_pane_t *pane) {
    app_t *app = pane->parent->app;
    const int bitrate = app_configuration->stream.bitrate;
    if (bitrate > 250000) {
        lv_obj_clear_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(pane->bitrate_warning,
                                 locstr("Above 250 Mbps usually brings no quality gain and can cause "
                                        "packet loss as the connection becomes less stable."));
    } else if (app->ss4s.video_cap.suggestedBitrate > 0 &&
               bitrate > app->ss4s.video_cap.suggestedBitrate) {
        lv_obj_clear_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(pane->bitrate_warning, locstr("Higher bitrate may cause performance issue, "
                                                               "try with caution."));
    } else {
        lv_obj_add_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
    }
}

static void init_locale_entries(basic_pane_t *pane) {
    pane->lang_entries = lv_mem_alloc(sizeof(pref_dropdown_string_entry_t) * (I18N_LOCALES_LEN + 2));
    lv_memset_00(pane->lang_entries, sizeof(pref_dropdown_string_entry_t) * (I18N_LOCALES_LEN + 2));
    for (int i = 0; i < 2; i++) {
        pref_dropdown_string_entry_t *def_entry = &pane->lang_entries[i];
        const i18n_entry_t *entry = i18n_entry_at(i);
        def_entry->value = entry->locale;
        def_entry->name = locstr(entry->name);
        def_entry->fallback = i == 0;
        pane->lang_entries_len++;
    }
    char *input = strdup(I18N_LOCALES), *tok = NULL, *saveptr = input;
    while ((tok = strtok_r(saveptr, ";", &saveptr)) != NULL) {
        const i18n_entry_t *entry = i18n_entry(tok);
        if (entry) {
            pref_dropdown_string_entry_t *pref_entry = &pane->lang_entries[pane->lang_entries_len];
            pref_entry->value = entry->locale;
            pref_entry->name = entry->name;
            pane->lang_entries_len++;
        }
    }
    free(input);
}

static void pref_mark_restart_cb(lv_event_t *e) {
    basic_pane_t *pane = (basic_pane_t *) lv_event_get_user_data(e);
    settings_controller_t *parent = pane->parent;
    parent->needs_locale_reapply |= strcasecmp(i18n_locale(), app_configuration->language) != 0;
}

static void refresh_profile_dropdown(basic_pane_t *pane) {
    char options[512] = {0};
    int active_index = 0;
    const char *active_id = profile_manager_active_id();
    for (int i = 0; i < profile_manager_count(); i++) {
        const streaming_profile_t *profile = profile_manager_get(i);
        if (!profile) {
            continue;
        }
        if (i > 0) {
            strcat(options, "\n");
        }
        strcat(options, profile->name);
        if (active_id && strcmp(profile->id, active_id) == 0) {
            active_index = i;
        }
    }
    lv_dropdown_set_options(pane->profile_dropdown, options[0] ? options : "Default");
    lv_dropdown_set_selected(pane->profile_dropdown, active_index);
}

static void basic_pane_add_profile_section(basic_pane_t *pane, lv_obj_t *view) {
    pref_header(view, locstr("Streaming profile"));

    pane->profile_dropdown = lv_dropdown_create(view);
    lv_obj_set_width(pane->profile_dropdown, LV_PCT(100));
    refresh_profile_dropdown(pane);
    lv_obj_add_event_cb(pane->profile_dropdown, on_profile_changed, LV_EVENT_VALUE_CHANGED, pane);

    lv_obj_t *profile_actions = lv_obj_create(view);
    lv_obj_remove_style_all(profile_actions);
    lv_obj_set_width(profile_actions, LV_PCT(100));
    lv_obj_set_flex_flow(profile_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(profile_actions, lv_dpx(8), 0);

    lv_obj_t *save_profile_btn = lv_btn_create(profile_actions);
    lv_obj_t *save_profile_label = lv_label_create(save_profile_btn);
    lv_label_set_text(save_profile_label, locstr("Save to profile"));
    lv_obj_add_event_cb(save_profile_btn, on_save_profile_clicked, LV_EVENT_CLICKED, pane);

    lv_obj_t *new_profile_btn = lv_btn_create(profile_actions);
    lv_obj_t *new_profile_label = lv_label_create(new_profile_btn);
    lv_label_set_text(new_profile_label, locstr("New"));
    lv_obj_add_event_cb(new_profile_btn, on_new_profile_clicked, LV_EVENT_CLICKED, pane);

    lv_obj_t *rename_profile_btn = lv_btn_create(profile_actions);
    lv_obj_t *rename_profile_label = lv_label_create(rename_profile_btn);
    lv_label_set_text(rename_profile_label, locstr("Rename"));
    lv_obj_add_event_cb(rename_profile_btn, on_rename_profile_clicked, LV_EVENT_CLICKED, pane);

    lv_obj_t *delete_profile_btn = lv_btn_create(profile_actions);
    lv_obj_t *delete_profile_label = lv_label_create(delete_profile_btn);
    lv_label_set_text(delete_profile_label, locstr("Delete"));
    lv_obj_add_event_cb(delete_profile_btn, on_delete_profile_clicked, LV_EVENT_CLICKED, pane);
}

static void on_profile_changed(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    uint16_t index = lv_dropdown_get_selected(pane->profile_dropdown);
    const streaming_profile_t *profile = profile_manager_get(index);
    if (!profile) {
        return;
    }
    profile_manager_set_active(profile->id);
    profile_manager_apply_to_settings(app_configuration);
    lv_event_send(pane->bitrate_slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static void on_save_profile_clicked(lv_event_t *e) {
  basic_pane_t *pane = lv_event_get_user_data(e);
  const streaming_profile_t *active = profile_manager_get_active();
  if (active) {
    profile_manager_save_from_settings(app_configuration, active->id);
  }
  (void) pane;
}

static void on_abr_changed(lv_event_t *e) {
    (void) e;
}

static void on_abr_mode_changed(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    app_configuration->abr_mode = (int) lv_dropdown_get_selected(pane->abr_mode_dropdown);
    if (app_configuration->abr_mode < 0 || app_configuration->abr_mode > 2) {
        app_configuration->abr_mode = 0;
    }
}

static void on_new_profile_clicked(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "Profile %d", profile_manager_count() + 1);
    char new_id[37];
    if (profile_manager_create(name_buf, app_configuration, new_id, sizeof(new_id))) {
        profile_manager_set_active(new_id);
        refresh_profile_dropdown(pane);
    }
}

static void on_rename_profile_clicked(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    const streaming_profile_t *active = profile_manager_get_active();
    if (!active) {
        return;
    }
    char new_name[64];
    snprintf(new_name, sizeof(new_name), "%s (2)", active->name);
    if (profile_manager_rename(active->id, new_name)) {
        refresh_profile_dropdown(pane);
    }
}

static void on_delete_profile_clicked(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    const streaming_profile_t *active = profile_manager_get_active();
    if (!active || profile_manager_count() <= 1) {
        return;
    }
    if (profile_manager_delete(active->id)) {
        profile_manager_apply_to_settings(app_configuration);
        refresh_profile_dropdown(pane);
        lv_event_send(pane->bitrate_slider, LV_EVENT_VALUE_CHANGED, NULL);
    }
}
