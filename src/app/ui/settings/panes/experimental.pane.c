#include "app.h"

#include "pref_obj.h"
#include "ui/settings/settings.controller.h"
#include "util/i18n.h"
#include "util/log_overlay.h"
#include "lvgl/util/lv_app_utils.h"
#include "app_settings.h"

#if TARGET_WEBOS
#include "platform/webos/game_mode.h"
#endif

#include <stdio.h>
#include <string.h>

typedef struct experimental_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;
    lv_obj_t *idr_checkbox;
    lv_obj_t *idr_row;
    lv_obj_t *idr_slider;
    lv_obj_t *idr_value_label;
    int idr_refresh_slider_value;
    lv_obj_t *abr_dropdown;
    pref_dropdown_int_entry_t abr_entries[3];
    pref_dropdown_int_entry_t audio_lead_entries[7];
    pref_dropdown_int_entry_t surround_51_entries[2];
#if FEATURE_I18N_LANGUAGE_SETTINGS
    pref_dropdown_string_entry_t lang_entries[16];
    int lang_entries_len;
#endif
} experimental_pane_t;

static void pane_ctor(lv_fragment_t *self, void *args);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

#if FEATURE_I18N_LANGUAGE_SETTINGS
static void language_changed_cb(lv_event_t *e);

static void reload_ui_after_locale(void *userdata);
#endif

static void on_show_logs_changed(lv_event_t *e);

static void reconnect_cb(lv_event_t *e);

static void reset_defaults_clicked(lv_event_t *e);

static void reset_defaults_confirm_cb(lv_event_t *e);

static void abr_state_update(experimental_pane_t *pane);

static void abr_checkbox_cb(lv_event_t *e);

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
    pane->abr_entries[0] = (pref_dropdown_int_entry_t) {locstr("Balanced"), 0, true};
    pane->abr_entries[1] = (pref_dropdown_int_entry_t) {locstr("Quality"), 1, false};
    pane->abr_entries[2] = (pref_dropdown_int_entry_t) {locstr("Low latency"), 2, false};
    /* Deep entries test whether NDL drops packets it considers late (#80); each ms delays sound. */
    pane->audio_lead_entries[0] = (pref_dropdown_int_entry_t) {"0 ms", 0, false};
    pane->audio_lead_entries[1] = (pref_dropdown_int_entry_t) {"20 ms", 20, true};
    pane->audio_lead_entries[2] = (pref_dropdown_int_entry_t) {"40 ms", 40, false};
    pane->audio_lead_entries[3] = (pref_dropdown_int_entry_t) {"60 ms", 60, false};
    pane->audio_lead_entries[4] = (pref_dropdown_int_entry_t) {"80 ms", 80, false};
    pane->audio_lead_entries[5] = (pref_dropdown_int_entry_t) {"100 ms", 100, false};
    pane->audio_lead_entries[6] = (pref_dropdown_int_entry_t) {"150 ms", 150, false};
    pane->surround_51_entries[0] = (pref_dropdown_int_entry_t) {locstr("Off (Opus passthrough)"),
                                                                 SURROUND_51_PASSTHROUGH, true};
    pane->surround_51_entries[1] = (pref_dropdown_int_entry_t) {locstr("PCM (other soundbars)"),
                                                                SURROUND_51_PCM, false};
#if FEATURE_I18N_LANGUAGE_SETTINGS
    pane->lang_entries_len = 0;
    for (int i = 0; i18n_entry_at(i)->locale && pane->lang_entries_len < 16; i++) {
        const i18n_entry_t *e = i18n_entry_at(i);
        pane->lang_entries[pane->lang_entries_len].name = locstr(e->name);
        pane->lang_entries[pane->lang_entries_len].value = e->locale;
        pane->lang_entries[pane->lang_entries_len].fallback = strcmp(e->locale, "auto") == 0;
        pane->lang_entries_len++;
    }
#endif
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    experimental_pane_t *pane = (experimental_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    pref_header(view, locstr("Experimental"));

#if FEATURE_I18N_LANGUAGE_SETTINGS
    lv_obj_t *lang_row = pref_focus_row(view, locstr("Language"));
    lv_obj_t *lang_dd = pref_dropdown_string(lang_row, pane->lang_entries, (size_t) pane->lang_entries_len,
                                            &app_configuration->language);
    pref_style_inline_control(lang_dd);
    pref_row_bind_control(lang_row, lang_dd);
    lv_obj_add_event_cb(lang_dd, language_changed_cb, LV_EVENT_VALUE_CHANGED, pane);
#endif

    lv_obj_t *logs = pref_checkbox(view, locstr("Show logs"),
                                   &app_configuration->show_logs, false);
    lv_obj_add_event_cb(logs, on_show_logs_changed, LV_EVENT_VALUE_CHANGED, NULL);
    if (app_configuration->show_logs) {
        log_overlay_set_enabled(true);
    }

#if TARGET_WEBOS
    if (webos_game_mode_is_rooted()) {
        pref_checkbox(view, locstr("Game mode"), &app_configuration->game_mode, false);
    }
#endif

    pref_header(view, locstr("Video"));

    lv_obj_t *full_range = pref_checkbox(view, locstr("Full range YUV (SDR only)"),
                                         &app_configuration->force_full_color_range, false);
    lv_obj_add_event_cb(full_range, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);

    lv_obj_t *sliced = pref_checkbox(view, locstr("HEVC sliced frames"),
                                     &app_configuration->hevc_sliced_frames, false);
    lv_obj_add_event_cb(sliced, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);

    lv_obj_t *idr_checkbox = lv_checkbox_create(view);
    lv_checkbox_set_text(idr_checkbox, locstr("Periodic decoder refresh (HEVC)"));
    lv_obj_set_size(idr_checkbox, LV_PCT(100), PREF_ROW_HEIGHT);
    lv_obj_set_style_pad_hor(idr_checkbox, LV_DPX(16), 0);
    lv_obj_set_style_pad_top(idr_checkbox, LV_DPX(12), 0);
    lv_obj_set_style_pad_bottom(idr_checkbox, LV_DPX(8), 0);
    lv_obj_set_style_radius(idr_checkbox, LV_DPX(8), 0);
    if (app_configuration->idr_refresh_interval_ms >= 500) {
        lv_obj_add_state(idr_checkbox, LV_STATE_CHECKED);
    }
    pane->idr_checkbox = idr_checkbox;
    pane->idr_refresh_slider_value = app_configuration->idr_refresh_interval_ms >= 500
            ? app_configuration->idr_refresh_interval_ms
            : 10000;
    pref_checkbox_prepare_for_dpad(idr_checkbox);
    pane->idr_row = pref_focus_row(view, locstr("Refresh interval"));
    pane->idr_value_label = lv_obj_get_child(pane->idr_row, 0);
    lv_obj_t *idr_slider = pref_slider(pane->idr_row, &pane->idr_refresh_slider_value, 500, 60000, 500);
    pref_style_inline_control(idr_slider);
    pref_row_bind_control(pane->idr_row, idr_slider);
    pane->idr_slider = idr_slider;
    lv_obj_add_event_cb(idr_checkbox, idr_checkbox_activate, LV_EVENT_CLICKED, pane);
    lv_obj_add_event_cb(idr_slider, idr_refresh_slider_cb, LV_EVENT_VALUE_CHANGED, pane);
    idr_refresh_state_update(pane);

#if TARGET_WEBOS
    pref_header(view, locstr("Audio"));
    lv_obj_t *lead_row = pref_focus_row(view, locstr("Audio buffer (5.1 passthrough)"));
    lv_obj_t *lead_dd = pref_dropdown_int(lead_row, pane->audio_lead_entries,
                                          sizeof(pane->audio_lead_entries) / sizeof(pane->audio_lead_entries[0]),
                                          &app_configuration->audio_lead_ms, NULL);
    pref_style_inline_control(lead_dd);
    pref_row_bind_control(lead_row, lead_dd);
    lv_obj_add_event_cb(lead_dd, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);

    lv_obj_t *pcm_row = pref_focus_row(view, locstr("5.1 audio mode"));
    lv_obj_t *pcm_dd = pref_dropdown_int(pcm_row, pane->surround_51_entries,
                                         sizeof(pane->surround_51_entries) / sizeof(pane->surround_51_entries[0]),
                                         &app_configuration->surround_51_mode, NULL);
    pref_style_inline_control(pcm_dd);
    pref_row_bind_control(pcm_row, pcm_dd);
    lv_obj_add_event_cb(pcm_dd, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("PCM avoids cuts on some soundbars but may mix channels on LG bars."), false);

    lv_obj_t *seq_pts = pref_checkbox(view, locstr("Sequential audio timestamps"),
                                      &app_configuration->audio_sequential_pts, false);
    lv_obj_add_event_cb(seq_pts, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Fixes 5.1 passthrough cutting out. Turn off only to compare."), false);
#endif

    pref_header(view, locstr("Bitrate"));

    lv_obj_t *abr_checkbox = pref_checkbox(view, locstr("Adaptive bitrate"),
                                           &app_configuration->auto_adjust_bitrate, false);
    lv_obj_t *abr_row = pref_focus_row(view, locstr("Profile"));
    pane->abr_dropdown = pref_dropdown_int(abr_row, pane->abr_entries,
                                           sizeof(pane->abr_entries) / sizeof(pane->abr_entries[0]),
                                           &app_configuration->abr_mode, NULL);
    pref_style_inline_control(pane->abr_dropdown);
    pref_row_bind_control(abr_row, pane->abr_dropdown);
    lv_obj_add_event_cb(abr_checkbox, abr_checkbox_cb, LV_EVENT_VALUE_CHANGED, pane);
    lv_obj_add_event_cb(pane->abr_dropdown, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    abr_state_update(pane);
    pref_desc_label(view, locstr("Rubylight hosts adapt the bitrate themselves, so it stays off with them."), false);

    pref_header(view, locstr("Reset"));
    lv_obj_t *reset_btn = lv_btn_create(view);
    pref_style_full_button(reset_btn);
    lv_obj_t *reset_lbl = lv_label_create(reset_btn);
    lv_label_set_text(reset_lbl, locstr("Reset all settings to defaults"));
    lv_obj_center(reset_lbl);
    lv_obj_add_event_cb(reset_btn, reset_defaults_clicked, LV_EVENT_CLICKED, pane);

    return view;
}

#if FEATURE_I18N_LANGUAGE_SETTINGS
static void language_changed_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    if (pane->parent) {
        pane->parent->needs_locale_reapply = true;
    }
    settings_save(app_configuration);
    if (app_configuration->language == NULL || app_configuration->language[0] == '\0' ||
        strcmp(app_configuration->language, "auto") == 0) {
        app_init_locale();
    } else {
        i18n_setlocale(app_configuration->language);
    }
    lv_async_call(reload_ui_after_locale, NULL);
}

static void reload_ui_after_locale(void *userdata) {
    (void) userdata;
    if (global == NULL) {
        return;
    }
    app_ui_close(&global->ui);
    app_ui_open(&global->ui, true, NULL);
}
#endif

static void reconnect_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    if (pane->parent) {
        pane->parent->needs_stream_reconnect = true;
    }
}

static void reset_defaults_clicked(lv_event_t *e) {
    static const char *btns[] = {translatable("Cancel"), translatable("Reset"), ""};
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, locstr("Reset settings"),
                                           locstr("Restore all settings to defaults? Pairing is kept. "
                                                  "You must reconnect the stream."),
                                           btns, false);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, reset_defaults_confirm_cb, LV_EVENT_VALUE_CHANGED,
                        lv_event_get_user_data(e));
}

static void reset_defaults_confirm_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_msgbox_get_active_btn(mbox) == 1 && app_configuration != NULL) {
        settings_restore_defaults(app_configuration);
        if (pane->parent) {
            pane->parent->needs_stream_reconnect = true;
        }
        log_overlay_set_enabled(app_configuration->show_logs);
    }
    lv_msgbox_close_async(mbox);
}

static void abr_state_update(experimental_pane_t *pane) {
    if (app_configuration->auto_adjust_bitrate) {
        lv_obj_clear_state(pane->abr_dropdown, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(pane->abr_dropdown, LV_STATE_DISABLED);
    }
}

static void abr_checkbox_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    reconnect_cb(e);
    abr_state_update(pane);
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
        lv_obj_add_state(pane->idr_row, LV_STATE_DISABLED);
        lv_obj_add_state(pane->idr_slider, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(pane->idr_checkbox, LV_STATE_DISABLED);
        if (refresh_on) {
            lv_obj_clear_state(pane->idr_row, LV_STATE_DISABLED);
            lv_obj_clear_state(pane->idr_slider, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(pane->idr_row, LV_STATE_DISABLED);
            lv_obj_add_state(pane->idr_slider, LV_STATE_DISABLED);
        }
    }
    if (refresh_on) {
        lv_label_set_text_fmt(pane->idr_value_label, "%s - %.1f s",
                              locstr("Refresh interval"),
                              app_configuration->idr_refresh_interval_ms / 1000.0);
    } else {
        lv_label_set_text(pane->idr_value_label, locstr("Refresh interval"));
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
