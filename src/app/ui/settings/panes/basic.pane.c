#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "pref_fps.h"
#include "pref_res.h"
#include "ui/settings/settings.controller.h"

#include "util/i18n.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_obj_t *res_warning;
    lv_obj_t *bitrate_label;
    lv_obj_t *bitrate_slider;
    lv_obj_t *bitrate_warning;
    lv_obj_t *refresh_rate_ta;

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

static void on_refresh_rate_committed(lv_event_t *e);

static void on_refresh_rate_minus(lv_event_t *e);

static void on_refresh_rate_plus(lv_event_t *e);

static void refresh_rate_sync_ta(lv_obj_t *ta);

static void sync_custom_fps_to_refresh_rate(basic_pane_t *pane);

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

    pref_title_label(view, locstr("Client refresh rate (Hz)"));
    lv_obj_t *rr_row = lv_obj_create(view);
    lv_obj_remove_style_all(rr_row);
    lv_obj_set_width(rr_row, LV_PCT(100));
    lv_obj_set_style_pad_column(rr_row, lv_disp_dpx(lv_obj_get_disp(rr_row), 8), 0);
    lv_obj_set_flex_flow(rr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rr_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *rr_ta = lv_textarea_create(rr_row);
    pane->refresh_rate_ta = rr_ta;
    lv_textarea_set_one_line(rr_ta, true);
    lv_textarea_set_accepted_chars(rr_ta, "0123456789.");
    lv_textarea_set_max_length(rr_ta, 10);
    lv_textarea_set_placeholder_text(rr_ta, locstr("Auto"));
    lv_obj_set_flex_grow(rr_ta, 1);
    refresh_rate_sync_ta(rr_ta);
    lv_obj_add_event_cb(rr_ta, on_refresh_rate_committed, LV_EVENT_DEFOCUSED, pane);

    lv_obj_t *btn_m = lv_btn_create(rr_row);
    lv_obj_set_height(btn_m, LV_SIZE_CONTENT);
    lv_obj_t *lbl_m = lv_label_create(btn_m);
    lv_label_set_text(lbl_m, "-");
    lv_obj_center(lbl_m);
    lv_obj_add_event_cb(btn_m, on_refresh_rate_minus, LV_EVENT_CLICKED, pane);

    lv_obj_t *btn_p = lv_btn_create(rr_row);
    lv_obj_set_height(btn_p, LV_SIZE_CONTENT);
    lv_obj_t *lbl_p = lv_label_create(btn_p);
    lv_label_set_text(lbl_p, "+");
    lv_obj_center(lbl_p);
    lv_obj_add_event_cb(btn_p, on_refresh_rate_plus, LV_EVENT_CLICKED, pane);

    pref_desc_label(view,
                    locstr("Optional. Leave empty for default. Sent to the host as client refresh x100 for frame "
                           "pacing (e.g. 119.94 for VRR game mode). Reconnect stream after changing."),
                    false);

    pane->res_warning = lv_label_create(view);
    lv_obj_add_flag(pane->res_warning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(pane->res_warning, LV_PCT(100));
    lv_obj_set_style_text_font(pane->res_warning, lv_theme_get_font_small(view), 0);
    lv_obj_set_style_text_color(pane->res_warning, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_long_mode(pane->res_warning, LV_LABEL_LONG_WRAP);

    pane->bitrate_label = pref_title_label(view, locstr("Video bitrate"));

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
    lv_obj_t *lang_label = pref_title_label(view, "Language");
    if (strcmp(locstr("Language"), "Language") != 0) {
        lv_label_set_text_fmt(lang_label, "%s (Language)", locstr("Language"));
    }

    lv_obj_t *language_dropdown = pref_dropdown_string(view, pane->lang_entries, pane->lang_entries_len,
                                                       &app_configuration->language);
    lv_obj_add_event_cb(language_dropdown, pref_mark_restart_cb, LV_EVENT_VALUE_CHANGED, pane);
    lv_obj_set_width(language_dropdown, LV_PCT(100));
#endif

    update_bitrate_label(pane);
    update_bitrate_hint(pane);

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
    sync_custom_fps_to_refresh_rate(pane);
    int bitrate = settings_optimal_bitrate(&pane->parent->app->ss4s.video_cap, app_configuration->stream.width,
                                           app_configuration->stream.height, app_configuration->stream.fps);
    if (bitrate > app_configuration->stream.bitrate) {
        lv_slider_set_value(pane->bitrate_slider, bitrate / BITRATE_STEP, LV_ANIM_OFF);
        app_configuration->stream.bitrate = lv_slider_get_value(pane->bitrate_slider) * BITRATE_STEP;
    }
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

static void on_fullscreen_updated(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    app_set_fullscreen(pane->parent->app, app_configuration->fullscreen);
}

static void update_bitrate_label(basic_pane_t *pane) {
    lv_label_set_text_fmt(pane->bitrate_label, locstr("Video bitrate - %d kbps"), app_configuration->stream.bitrate);
}

static void update_bitrate_hint(basic_pane_t *pane) {
    app_t *app = pane->parent->app;
    if (app->ss4s.video_cap.suggestedBitrate > 0 &&
        app_configuration->stream.bitrate > app->ss4s.video_cap.suggestedBitrate) {
        lv_obj_clear_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(pane->bitrate_warning, locstr("Higher bitrate may cause performance issue, "
                                                               "try with caution."));
    } else {
        lv_obj_add_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_rate_sync_ta(lv_obj_t *ta) {
    if (app_configuration->client_refresh_rate_x100 > 0) {
        char b[24];
        snprintf(b, sizeof b, "%.2f", app_configuration->client_refresh_rate_x100 / 100.0);
        lv_textarea_set_text(ta, b);
    } else {
        lv_textarea_set_text(ta, "");
    }
}

static void sync_custom_fps_to_refresh_rate(basic_pane_t *pane) {
    if (pane->refresh_rate_ta == NULL || app_configuration->client_refresh_rate_x100 > 0) {
        return;
    }
    int fps = app_configuration->stream.fps;
    if (fps == 30 || fps == 60 || fps == 90 || fps == 120 || fps == 144 || fps == 240) {
        return;
    }
    int x100 = fps * 100;
    if (x100 < 2300) {
        x100 = 2300;
    }
    if (x100 > 24000) {
        x100 = 24000;
    }
    app_configuration->client_refresh_rate_x100 = x100;
    refresh_rate_sync_ta(pane->refresh_rate_ta);
}

static void refresh_rate_parse_and_store(lv_obj_t *ta, basic_pane_t *pane) {
    const char *s = lv_textarea_get_text(ta);
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        app_configuration->client_refresh_rate_x100 = 0;
        pane->parent->needs_stream_reconnect = true;
        return;
    }
    char *end = NULL;
    double hz = strtod(s, &end);
    (void) end;
    if (hz < 23.0) {
        hz = 23.0;
    }
    if (hz > 240.0) {
        hz = 240.0;
    }
    int x100 = (int) (hz * 100.0 + 0.5);
    if (x100 < 2300) {
        x100 = 2300;
    }
    if (x100 > 24000) {
        x100 = 24000;
    }
    app_configuration->client_refresh_rate_x100 = x100;
    refresh_rate_sync_ta(ta);
    pane->parent->needs_stream_reconnect = true;
}

static void on_refresh_rate_committed(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DEFOCUSED) {
        return;
    }
    basic_pane_t *pane = lv_event_get_user_data(e);
    refresh_rate_parse_and_store(lv_event_get_target(e), pane);
}

static void on_refresh_rate_minus(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    int v = app_configuration->client_refresh_rate_x100;
    if (v <= 0) {
        v = 6000;
    }
    v -= 1;
    if (v < 2300) {
        v = 2300;
    }
    app_configuration->client_refresh_rate_x100 = v;
    refresh_rate_sync_ta(pane->refresh_rate_ta);
    pane->parent->needs_stream_reconnect = true;
}

static void on_refresh_rate_plus(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    int v = app_configuration->client_refresh_rate_x100;
    if (v <= 0) {
        v = 6000;
    }
    v += 1;
    if (v > 24000) {
        v = 24000;
    }
    app_configuration->client_refresh_rate_x100 = v;
    refresh_rate_sync_ta(pane->refresh_rate_ta);
    pane->parent->needs_stream_reconnect = true;
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
