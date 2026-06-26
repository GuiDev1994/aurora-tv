#if defined(TARGET_WEBOS)

#include "hid_passthrough_panel.h"

#include "ctm/ctm_state.h"
#include "ctm/ctm_settings.h"
#include "hid_passthrough/hid_passthrough_manager.h"
#include "stream/session.h"

#include "util/bus.h"
#include "util/i18n.h"
#include "util/user_event.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HID_PT_MAX_ROWS 64

typedef struct {
    lv_obj_t *container;
    lv_obj_t *sheet;
    lv_obj_t *status_label;
    lv_obj_t *error_label;
    lv_obj_t *list;
    lv_obj_t *composite_row;
    lv_obj_t *composite_cb;
    lv_obj_t *customize_panel;
    lv_obj_t *customize_title;
    lv_obj_t *latency_label;
    lv_obj_t *latency_slider;
    lv_obj_t *audio_dropdown;
    lv_obj_t *speaker_label;
    lv_obj_t *speaker_slider;
    lv_obj_t *headset_label;
    lv_obj_t *headset_slider;
    lv_obj_t *haptics_row;
    lv_obj_t *haptics_label;
    lv_obj_t *haptics_slider;
    lv_obj_t *reset_settings_btn;
    lv_obj_t *refresh_btn;
    lv_obj_t *close_btn;
    lv_group_t *group;
    session_t *session;
    hid_passthrough_panel_close_cb on_close;
    void *on_close_userdata;
    lv_timer_t *refresh_timer;
    lv_obj_t *row_buttons[HID_PT_MAX_ROWS];
    lv_obj_t *plug_labels[HID_PT_MAX_ROWS];
    int selected_index;
    char selected_key[96];
    int list_width;
    uint64_t last_sig;
    bool have_rendered;
} hid_pt_panel_t;

#define DS_LATENCY_MIN 20
#define DS_LATENCY_MAX 200
#define DS_VOLUME_MAX 100
#define DS_HAPTICS_MAX 200

static logical_device_t *panel_selected_item(const hid_pt_panel_t *panel)
{
    if (!panel || panel->selected_index < 0 || panel->selected_index >= g_devices.count) {
        return NULL;
    }
    return &g_devices.items[panel->selected_index];
}

static bool item_is_playstation_audio(const logical_device_t *item)
{
    if (!item) {
        return false;
    }
    const char *kind = bridge_kind_for_item(item);
    return strcmp(kind, "ds5") == 0 || strcmp(kind, "ds4") == 0;
}

static bool selected_item_is_ds5(const hid_pt_panel_t *panel)
{
    logical_device_t *item = panel_selected_item(panel);
    return item && strcmp(bridge_kind_for_item(item), "ds5") == 0;
}

static bool selected_item_is_flydigi(const hid_pt_panel_t *panel)
{
    if (!panel || panel->selected_index < 0 || panel->selected_index >= g_devices.count) {
        return false;
    }
    const logical_device_t *item = &g_devices.items[panel->selected_index];
    return is_flydigi_logical_device(item) ||
           (item->usb_busid[0] && is_flydigi_usb_busid(item->usb_busid)) ||
           (strcmp(item->vid, "04b4") == 0 && strcmp(item->pid, "2412") == 0) ||
           contains_ci(item->name, "flydigi") || contains_ci(item->name, "vader") ||
           contains_ci(item->name, "apex");
}

static void style_row(lv_obj_t *row, bool selected) {
    lv_obj_set_style_radius(row, LV_DPX(8), 0);
    lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(row, selected ? lv_color_hex(0x38bdf8) : lv_color_hex(0x263540), 0);
    lv_obj_set_style_bg_color(row, selected ? lv_color_hex(0x12384a) : lv_color_hex(0x18232c), 0);
    lv_obj_set_style_bg_color(row, selected ? lv_color_hex(0x16455d) : lv_color_hex(0x202e38), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
}

static void update_row_styles(hid_pt_panel_t *panel) {
    for (int i = 0; i < g_devices.count && i < HID_PT_MAX_ROWS; ++i) {
        if (panel->row_buttons[i]) {
            style_row(panel->row_buttons[i], i == panel->selected_index);
        }
    }
}

static void panel_update_status(hid_pt_panel_t *panel);
static void update_device_options(hid_pt_panel_t *panel);

static void panel_request_close(hid_pt_panel_t *panel) {
    (void) panel;
    bus_pushevent(USER_CLOSE_HID_PANEL, NULL, NULL);
}

static void back_btn_cb(lv_event_t *e) {
    panel_request_close(lv_event_get_user_data(e));
}

static bool panel_item_needs_lrkey(lv_obj_t *obj)
{
    return lv_obj_has_class(obj, &lv_slider_class) || lv_obj_has_class(obj, &lv_dropdown_class);
}

static void row_focus_cb(lv_event_t *event)
{
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    if (!panel || lv_event_get_code(event) != LV_EVENT_FOCUSED) {
        return;
    }
    lv_obj_t *row = lv_event_get_target(event);
    int index = (int) (intptr_t) lv_obj_get_user_data(row);
    if (index < 0 || index >= g_devices.count) {
        return;
    }
    panel->selected_index = index;
    snprintf(panel->selected_key, sizeof(panel->selected_key), "%s", g_devices.items[index].key);
    update_row_styles(panel);
    update_device_options(panel);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

static void control_key_cb(lv_event_t *event)
{
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    if (!panel || !panel->group || lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    uint32_t key = lv_event_get_key(event);
    bool editing = lv_group_get_editing(panel->group);

    switch (key) {
        case LV_KEY_ESC:
            if (editing && panel_item_needs_lrkey(target)) {
                lv_group_set_editing(panel->group, false);
                lv_event_stop_processing(event);
                return;
            }
            if (lv_obj_has_class(target, &lv_dropdown_class) && lv_dropdown_is_open(target)) {
                lv_dropdown_close(target);
                lv_event_stop_processing(event);
                return;
            }
            panel_request_close(panel);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_ENTER:
            if (lv_obj_has_class(target, &lv_slider_class)) {
                lv_group_set_editing(panel->group, true);
                lv_event_stop_processing(event);
                return;
            }
            if (lv_obj_has_class(target, &lv_dropdown_class) && !lv_dropdown_is_open(target)) {
                lv_dropdown_open(target);
                lv_group_set_editing(panel->group, true);
                lv_event_stop_processing(event);
                return;
            }
            return;
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            if (panel_item_needs_lrkey(target) && (editing || lv_obj_has_class(target, &lv_slider_class))) {
                return;
            }
            if (key == LV_KEY_RIGHT) {
                lv_group_focus_next(panel->group);
            } else {
                lv_group_focus_prev(panel->group);
            }
            lv_event_stop_processing(event);
            return;
        case LV_KEY_UP:
            if (lv_obj_has_class(target, &lv_dropdown_class) && lv_dropdown_is_open(target)) {
                return;
            }
            lv_group_focus_prev(panel->group);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_DOWN:
            if (lv_obj_has_class(target, &lv_dropdown_class) && lv_dropdown_is_open(target)) {
                return;
            }
            lv_group_focus_next(panel->group);
            lv_event_stop_processing(event);
            return;
        default:
            break;
    }
}

static void panel_key_cb(lv_event_t *event) {
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    if (!panel || !panel->group || lv_event_get_target(event) != lv_event_get_current_target(event)) {
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    control_key_cb(event);
}

static void panel_attach_keys(lv_obj_t *obj, hid_pt_panel_t *panel)
{
    if (!obj || !panel) {
        return;
    }
    lv_obj_add_event_cb(obj, control_key_cb, LV_EVENT_KEY, panel);
    uint32_t child_cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_cnt; ++i) {
        panel_attach_keys(lv_obj_get_child(obj, i), panel);
    }
}

static void composite_toggle_cb(lv_event_t *event) {
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    if (!panel || panel->selected_index < 0 || panel->selected_index >= g_devices.count) {
        return;
    }
    logical_device_t *item = &g_devices.items[panel->selected_index];
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!settings) {
        return;
    }
    settings->composite_passthrough = lv_obj_has_state(panel->composite_cb, LV_STATE_CHECKED);
    apply_settings_to_session(item);
}

static void update_latency_label(hid_pt_panel_t *panel)
{
    if (!panel || !panel->latency_label) {
        return;
    }
    int ms = (int) lv_slider_get_value(panel->latency_slider);
    lv_label_set_text_fmt(panel->latency_label, locstr("Audio/haptics latency — %d ms (default: 48 ms)"), ms);
}

static void update_speaker_label(hid_pt_panel_t *panel)
{
    if (!panel || !panel->speaker_label) {
        return;
    }
    int pct = (int) lv_slider_get_value(panel->speaker_slider);
    lv_label_set_text_fmt(panel->speaker_label, locstr("Speaker volume — %d%%"), pct);
}

static void update_headset_label(hid_pt_panel_t *panel)
{
    if (!panel || !panel->headset_label) {
        return;
    }
    int pct = (int) lv_slider_get_value(panel->headset_slider);
    lv_label_set_text_fmt(panel->headset_label, locstr("Headphone jack volume — %d%%"), pct);
}

static void update_haptics_label(hid_pt_panel_t *panel)
{
    if (!panel || !panel->haptics_label) {
        return;
    }
    int pct = (int) lv_slider_get_value(panel->haptics_slider);
    lv_label_set_text_fmt(panel->haptics_label, locstr("Haptics strength — %d%% (default: 100%%)"), pct);
}

static void sync_customize_ui_from_settings(hid_pt_panel_t *panel)
{
    logical_device_t *item = panel_selected_item(panel);
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!panel || !settings) {
        return;
    }
    if (panel->latency_slider) {
        int latency = (int) settings->latency_ms;
        if (latency < DS_LATENCY_MIN) {
            latency = DS_LATENCY_MIN;
        } else if (latency > DS_LATENCY_MAX) {
            latency = DS_LATENCY_MAX;
        }
        lv_slider_set_value(panel->latency_slider, latency, LV_ANIM_OFF);
        update_latency_label(panel);
    }
    if (panel->audio_dropdown) {
        lv_dropdown_set_selected(panel->audio_dropdown, (uint16_t) settings->audio_mode);
    }
    if (panel->speaker_slider) {
        int spk = (int) settings->speaker_volume_percent;
        if (spk > DS_VOLUME_MAX) {
            spk = DS_VOLUME_MAX;
        }
        lv_slider_set_value(panel->speaker_slider, spk, LV_ANIM_OFF);
        update_speaker_label(panel);
    }
    if (panel->headset_slider) {
        int hs = (int) settings->headset_volume_percent;
        if (hs > DS_VOLUME_MAX) {
            hs = DS_VOLUME_MAX;
        }
        lv_slider_set_value(panel->headset_slider, hs, LV_ANIM_OFF);
        update_headset_label(panel);
    }
    if (panel->haptics_slider) {
        int hap = (int) settings->haptics_gain_centi;
        if (hap > DS_HAPTICS_MAX) {
            hap = DS_HAPTICS_MAX;
        }
        lv_slider_set_value(panel->haptics_slider, hap, LV_ANIM_OFF);
        update_haptics_label(panel);
    }
    if (panel->haptics_row) {
        if (selected_item_is_ds5(panel)) {
            lv_obj_clear_flag(panel->haptics_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panel->haptics_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void customize_setting_changed(hid_pt_panel_t *panel)
{
    logical_device_t *item = panel_selected_item(panel);
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!panel || !item || !settings) {
        return;
    }
    if (panel->latency_slider) {
        settings->latency_ms = (unsigned) lv_slider_get_value(panel->latency_slider);
    }
    if (panel->audio_dropdown) {
        settings->audio_mode = (tv_bridge_audio_mode_t) lv_dropdown_get_selected(panel->audio_dropdown);
    }
    if (panel->speaker_slider) {
        settings->speaker_volume_percent = (unsigned) lv_slider_get_value(panel->speaker_slider);
    }
    if (panel->headset_slider) {
        settings->headset_volume_percent = (unsigned) lv_slider_get_value(panel->headset_slider);
    }
    if (panel->haptics_slider && selected_item_is_ds5(panel)) {
        settings->haptics_gain_centi = (unsigned) lv_slider_get_value(panel->haptics_slider);
    }
    apply_settings_to_session(item);
}

static void latency_slider_cb(lv_event_t *event)
{
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    update_latency_label(panel);
    customize_setting_changed(panel);
}

static void speaker_slider_cb(lv_event_t *event)
{
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    update_speaker_label(panel);
    customize_setting_changed(panel);
}

static void headset_slider_cb(lv_event_t *event)
{
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    update_headset_label(panel);
    customize_setting_changed(panel);
}

static void haptics_slider_cb(lv_event_t *event)
{
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    update_haptics_label(panel);
    customize_setting_changed(panel);
}

static void audio_dropdown_cb(lv_event_t *event)
{
    customize_setting_changed(lv_event_get_user_data(event));
}

static void reset_settings_cb(lv_event_t *event)
{
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    logical_device_t *item = panel_selected_item(panel);
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!panel || !item || !settings) {
        return;
    }
    *settings = default_settings_for_item(item);
    sync_customize_ui_from_settings(panel);
    apply_settings_to_session(item);
}

static void update_customize_panel(hid_pt_panel_t *panel)
{
    if (!panel || !panel->customize_panel) {
        return;
    }
    logical_device_t *item = panel_selected_item(panel);
    bool show = item_is_playstation_audio(item);
    if (show) {
        ui_record_for_item(item);
        sync_customize_ui_from_settings(panel);
        if (panel->customize_title) {
            lv_label_set_text_fmt(panel->customize_title, "%s", item->name);
        }
        lv_obj_clear_flag(panel->customize_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel->customize_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_composite_row(hid_pt_panel_t *panel) {
    if (!panel || !panel->composite_row || !panel->composite_cb) {
        return;
    }
    bool show = selected_item_is_flydigi(panel);
    if (show) {
        logical_device_t *item = &g_devices.items[panel->selected_index];
        tv_bridge_worker_settings_t *settings = settings_for_item(item);
        if (settings) {
            if (settings->composite_passthrough) {
                lv_obj_add_state(panel->composite_cb, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(panel->composite_cb, LV_STATE_CHECKED);
            }
        }
    }
    if (show) {
        lv_obj_clear_flag(panel->composite_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel->composite_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_device_options(hid_pt_panel_t *panel)
{
    update_composite_row(panel);
    update_customize_panel(panel);
}

static void row_clicked_cb(lv_event_t *event) {
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    int index = (int) (intptr_t) lv_obj_get_user_data(lv_event_get_current_target(event));
    if (!panel || index < 0 || index >= g_devices.count) {
        return;
    }
    panel->selected_index = index;
    snprintf(panel->selected_key, sizeof(panel->selected_key), "%s", g_devices.items[index].key);
    update_row_styles(panel);
    update_device_options(panel);
}

static void plug_button_cb(lv_event_t *event) {
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    int index = (int) (intptr_t) lv_obj_get_user_data(lv_event_get_current_target(event));
    if (!panel || index < 0 || index >= g_devices.count) {
        return;
    }

    logical_device_t *item = &g_devices.items[index];
    bool requested_state = !item->plugged;
    if (requested_state) {
        ctm_clear_plug_error();
        if (!plug_in_item(item)) {
            panel_update_status(panel);
            return;
        }
    } else {
        stop_session(item->key);
        ctm_clear_plug_error();
    }

    item->plugged = requested_state;
    set_plug_key(item->key, item->plugged);
    /* The user took manual control of this device: stop auto-managing it (so a
     * deliberate plug-out is not re-plugged by the reconcile poll) until it
     * physically reconnects. */
    autoplug_mark_done(item->key);
    panel->selected_index = index;
    snprintf(panel->selected_key, sizeof(panel->selected_key), "%s", item->key);

    if (panel->plug_labels[index]) {
        lv_label_set_text(panel->plug_labels[index], item->plugged ? locstr("Plug out") : locstr("Plug in"));
    }
    update_row_styles(panel);
    update_device_options(panel);
    panel_update_status(panel);
}

static void render_device_list(hid_pt_panel_t *panel) {
    lv_obj_clean(panel->list);
    memset(panel->row_buttons, 0, sizeof(panel->row_buttons));
    memset(panel->plug_labels, 0, sizeof(panel->plug_labels));

    if (g_devices.count == 0) {
        lv_obj_t *empty = lv_label_create(panel->list);
        lv_label_set_text(empty, locstr("No HID devices visible to the native app"));
        lv_obj_set_style_text_color(empty, lv_color_hex(0xb6c5cf), 0);
        lv_obj_set_style_pad_all(empty, LV_DPX(18), 0);
        return;
    }

    const int row_h = LV_DPX(60);
    const int gap = LV_DPX(8);
    const int row_w = panel->list_width > 0 ? panel->list_width : LV_PCT(100);
    const int button_w = LV_DPX(112);
    int y = 0;

    for (int i = 0; i < g_devices.count && i < HID_PT_MAX_ROWS; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        lv_obj_t *row = lv_obj_create(panel->list);
        panel->row_buttons[i] = row;
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, panel);
        lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_FOCUSED, panel);
        lv_obj_add_event_cb(row, control_key_cb, LV_EVENT_KEY, panel);
        lv_obj_set_user_data(row, (void *) (intptr_t) i);
        style_row(row, i == panel->selected_index);
        lv_group_add_obj(panel->group, row);

        lv_obj_t *name = lv_label_create(row);
        char display_name[128];
        snprintf(display_name, sizeof(display_name), "%s", item->name);
        if (is_flydigi_logical_device(item)) {
            snprintf(display_name, sizeof(display_name), "%s (%s)", item->name,
                     flydigi_is_xinput_evdev_only(item) ? "XInput" :
                     flydigi_is_xinput_mode(item) ? "XInput" : "D-Input");
        }
        lv_label_set_text(name, display_name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, row_w - button_w - LV_DPX(30));
        lv_obj_set_pos(name, LV_DPX(16), LV_DPX(20));

        lv_obj_t *plug_btn = lv_btn_create(row);
        lv_obj_set_size(plug_btn, button_w, LV_DPX(38));
        lv_obj_align(plug_btn, LV_ALIGN_RIGHT_MID, LV_DPX(-14), 0);
        lv_obj_set_style_radius(plug_btn, LV_DPX(6), 0);
        lv_obj_set_style_bg_color(plug_btn, item->plugged ? lv_color_hex(0x7f1d1d) : lv_color_hex(0x0f766e), 0);
        lv_obj_set_style_bg_color(plug_btn, item->plugged ? lv_color_hex(0x991b1b) : lv_color_hex(0x0d9488),
                                  LV_STATE_PRESSED);
        lv_obj_add_event_cb(plug_btn, plug_button_cb, LV_EVENT_CLICKED, panel);
        lv_obj_add_event_cb(plug_btn, control_key_cb, LV_EVENT_KEY, panel);
        lv_obj_set_user_data(plug_btn, (void *) (intptr_t) i);
        lv_group_add_obj(panel->group, plug_btn);

        panel->plug_labels[i] = lv_label_create(plug_btn);
        lv_label_set_text(panel->plug_labels[i], item->plugged ? locstr("Plug out") : locstr("Plug in"));
        lv_obj_center(panel->plug_labels[i]);

        y += row_h + gap;
    }
}

static void panel_update_status(hid_pt_panel_t *panel) {
    if (!panel || !panel->status_label) {
        return;
    }
    if (panel->error_label) {
        const char *err = ctm_last_plug_error();
        if (err) {
            lv_label_set_text(panel->error_label, err);
            lv_obj_clear_flag(panel->error_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panel->error_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    char status[128];
    snprintf(status, sizeof(status), "%d device%s | Windows %s",
             g_devices.count,
             g_devices.count == 1 ? "" : "s",
             g_agent_online ? g_agent_host : "not found");
    lv_label_set_text(panel->status_label, status);
}

static void update_status_label(hid_pt_panel_t *panel) {
    panel_update_status(panel);
}

static uint64_t device_list_signature(void) {
    uint64_t sig = 1469598103934665603ULL;
#define SIG_MIX(p, n) do {                                              \
        const unsigned char *_b = (const unsigned char *) (p);           \
        for (size_t _i = 0; _i < (size_t) (n); ++_i) {                  \
            sig ^= _b[_i];                                              \
            sig *= 1099511628211ULL;                                    \
        }                                                               \
    } while (0)
    SIG_MIX(&g_devices.count, sizeof(g_devices.count));
    for (int i = 0; i < g_devices.count; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        unsigned char st = (unsigned char) (item->plugged ? 1 : 0);
        SIG_MIX(item->key, strlen(item->key));
        SIG_MIX(&st, 1);
    }
#undef SIG_MIX
    return sig;
}

static void focus_initial_target(hid_pt_panel_t *panel) {
    if (!panel || !panel->group) {
        return;
    }
    for (int i = 0; i < g_devices.count && i < HID_PT_MAX_ROWS; ++i) {
        if (panel->row_buttons[i]) {
            lv_group_focus_obj(panel->row_buttons[i]);
            return;
        }
    }
    if (panel->close_btn) {
        lv_group_focus_obj(panel->close_btn);
    }
}

static void refresh_devices(hid_pt_panel_t *panel) {
    if (!panel || !panel->session) {
        return;
    }
    session_ensure_hid_passthrough(panel->session);
    hid_passthrough_manager_t *mgr = session_get_hid_passthrough(panel->session);
    if (mgr) {
        hid_passthrough_manager_rescan(mgr);
    }

    int selected = -1;
    if (panel->selected_key[0]) {
        for (int i = 0; i < g_devices.count; ++i) {
            if (strcmp(g_devices.items[i].key, panel->selected_key) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && g_devices.count > 0) {
        selected = 0;
        snprintf(panel->selected_key, sizeof(panel->selected_key), "%s", g_devices.items[0].key);
    }
    panel->selected_index = selected;
    update_status_label(panel);

    uint64_t sig = device_list_signature();
    if (!panel->have_rendered || sig != panel->last_sig) {
        panel->last_sig = sig;
        panel->have_rendered = true;
        render_device_list(panel);
        focus_initial_target(panel);
    } else {
        update_row_styles(panel);
    }
    update_device_options(panel);
}

static void refresh_timer_cb(lv_timer_t *timer) {
    hid_pt_panel_t *panel = timer->user_data;
    if (panel && panel->container && lv_obj_is_valid(panel->container)) {
        refresh_devices(panel);
    }
}

static void refresh_button_cb(lv_event_t *event) {
    hid_pt_panel_t *panel = lv_event_get_user_data(event);
    refresh_devices(panel);
}

static void panel_delete_cb(lv_event_t *e) {
    hid_pt_panel_t *panel = lv_event_get_user_data(e);
    if (!panel) {
        return;
    }
    if (panel->refresh_timer) {
        lv_timer_del(panel->refresh_timer);
        panel->refresh_timer = NULL;
    }
    if (panel->group) {
        lv_group_del(panel->group);
    }
    free(panel);
}

void hid_passthrough_panel_refresh(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    if (panel) {
        refresh_devices(panel);
    }
}

void hid_passthrough_panel_focus_initial(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    if (panel) {
        focus_initial_target(panel);
    }
}

lv_group_t *hid_passthrough_panel_get_group(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    return panel ? panel->group : NULL;
}

lv_obj_t *hid_passthrough_panel_create(lv_obj_t *parent, session_t *session,
                                       hid_passthrough_panel_close_cb on_close, void *userdata) {
    hid_pt_panel_t *panel = calloc(1, sizeof(*panel));
    if (!panel) {
        return NULL;
    }
    panel->session = session;
    panel->on_close = on_close;
    panel->on_close_userdata = userdata;
    panel->selected_index = -1;
    panel->group = lv_group_create();
    lv_group_set_wrap(panel->group, true);

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_50, 0);
    lv_obj_set_user_data(cont, panel);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sheet = lv_obj_create(cont);
    panel->sheet = sheet;
    lv_obj_set_size(sheet, LV_DPX(680), LV_DPX(520));
    lv_obj_center(sheet);
    lv_obj_set_style_max_width(sheet, LV_PCT(92), 0);
    lv_obj_set_style_max_height(sheet, LV_PCT(88), 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x121a20), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sheet, LV_DPX(12), 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0x2c3d49), 0);
    lv_obj_set_style_shadow_width(sheet, LV_DPX(24), 0);
    lv_obj_set_style_shadow_opa(sheet, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(sheet, panel_key_cb, LV_EVENT_KEY, panel);

    lv_obj_t *header = lv_obj_create(sheet);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_DPX(56));
    lv_obj_set_style_bg_color(header, lv_color_hex(0x18232c), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, LV_DPX(12), 0);
    lv_obj_set_style_pad_hor(header, LV_DPX(14), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, locstr("HID Devices"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xf5f8fa), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *refresh_btn = lv_btn_create(header);
    panel->refresh_btn = refresh_btn;
    lv_obj_set_size(refresh_btn, LV_DPX(88), LV_DPX(36));
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, LV_DPX(-98), 0);
    lv_obj_add_event_cb(refresh_btn, refresh_button_cb, LV_EVENT_CLICKED, panel);
    lv_obj_add_event_cb(refresh_btn, control_key_cb, LV_EVENT_KEY, panel);
    lv_group_add_obj(panel->group, refresh_btn);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, locstr("Refresh"));
    lv_obj_center(refresh_lbl);

    lv_obj_t *close_btn = lv_btn_create(header);
    panel->close_btn = close_btn;
    lv_obj_set_size(close_btn, LV_DPX(88), LV_DPX(36));
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(close_btn, back_btn_cb, LV_EVENT_CLICKED, panel);
    lv_obj_add_event_cb(close_btn, control_key_cb, LV_EVENT_KEY, panel);
    lv_group_add_obj(panel->group, close_btn);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, locstr("Close"));
    lv_obj_center(close_lbl);

    lv_obj_t *status_row = lv_obj_create(sheet);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_width(status_row, LV_PCT(100));
    lv_obj_set_height(status_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(status_row, LV_DPX(14), 0);
    lv_obj_set_style_pad_bottom(status_row, LV_DPX(4), 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(status_row, LV_DPX(2), 0);

    panel->status_label = lv_label_create(status_row);
    lv_label_set_text(panel->status_label, locstr("starting"));
    lv_obj_set_style_text_color(panel->status_label, lv_color_hex(0xaab6bf), 0);
    lv_obj_set_style_text_font(panel->status_label, lv_theme_get_font_small(panel->status_label), 0);

    panel->error_label = lv_label_create(status_row);
    lv_label_set_text(panel->error_label, "");
    lv_obj_set_style_text_color(panel->error_label, lv_color_hex(0xf87171), 0);
    lv_obj_set_style_text_font(panel->error_label, lv_theme_get_font_small(panel->error_label), 0);
    lv_obj_set_width(panel->error_label, LV_PCT(100));
    lv_label_set_long_mode(panel->error_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(panel->error_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *devices_title = lv_label_create(sheet);
    lv_label_set_text(devices_title, locstr("Devices"));
    lv_obj_set_style_text_color(devices_title, lv_color_hex(0xf5f8fa), 0);
    lv_obj_set_style_pad_left(devices_title, LV_DPX(18), 0);
    lv_obj_set_style_pad_top(devices_title, LV_DPX(14), 0);

    panel->composite_row = lv_obj_create(sheet);
    lv_obj_remove_style_all(panel->composite_row);
    lv_obj_set_width(panel->composite_row, LV_PCT(100));
    lv_obj_set_height(panel->composite_row, LV_DPX(44));
    lv_obj_set_style_pad_left(panel->composite_row, LV_DPX(18), 0);
    lv_obj_add_flag(panel->composite_row, LV_OBJ_FLAG_HIDDEN);
    panel->composite_cb = lv_checkbox_create(panel->composite_row);
    lv_checkbox_set_text(panel->composite_cb, locstr("Recognize as native Flydigi on PC"));
    lv_obj_set_style_text_color(panel->composite_cb, lv_color_hex(0xdbe4ea), 0);
    lv_obj_align(panel->composite_cb, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(panel->composite_cb, composite_toggle_cb, LV_EVENT_VALUE_CHANGED, panel);
    lv_group_add_obj(panel->group, panel->composite_cb);

    panel->list = lv_obj_create(sheet);
    lv_obj_set_width(panel->list, LV_PCT(100));
    lv_obj_set_height(panel->list, LV_DPX(168));
    lv_obj_set_style_bg_opa(panel->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(panel->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(panel->list, LV_DPX(12), 0);
    lv_obj_add_flag(panel->list, LV_OBJ_FLAG_SCROLLABLE);
    panel->list_width = lv_obj_get_width(panel->list) - LV_DPX(24);
    if (panel->list_width < LV_DPX(200)) {
        panel->list_width = LV_DPX(520);
    }

    panel->customize_panel = lv_obj_create(sheet);
    lv_obj_remove_style_all(panel->customize_panel);
    lv_obj_set_width(panel->customize_panel, LV_PCT(100));
    lv_obj_set_height(panel->customize_panel, LV_DPX(260));
    lv_obj_set_flex_grow(panel->customize_panel, 1);
    lv_obj_set_style_bg_color(panel->customize_panel, lv_color_hex(0x152028), 0);
    lv_obj_set_style_bg_opa(panel->customize_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel->customize_panel, 1, 0);
    lv_obj_set_style_border_color(panel->customize_panel, lv_color_hex(0x2c3d49), 0);
    lv_obj_set_style_radius(panel->customize_panel, LV_DPX(6), 0);
    lv_obj_set_style_pad_all(panel->customize_panel, LV_DPX(12), 0);
    lv_obj_set_flex_flow(panel->customize_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel->customize_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(panel->customize_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel->customize_panel, LV_OBJ_FLAG_HIDDEN);

    panel->customize_title = lv_label_create(panel->customize_panel);
    lv_label_set_text(panel->customize_title, locstr("Controller settings"));
    lv_obj_set_style_text_color(panel->customize_title, lv_color_hex(0xf5f8fa), 0);

    panel->latency_label = lv_label_create(panel->customize_panel);
    lv_obj_set_style_text_color(panel->latency_label, lv_color_hex(0xdbe4ea), 0);
    panel->latency_slider = lv_slider_create(panel->customize_panel);
    lv_slider_set_range(panel->latency_slider, DS_LATENCY_MIN, DS_LATENCY_MAX);
    lv_obj_set_width(panel->latency_slider, LV_PCT(100));
    lv_obj_add_event_cb(panel->latency_slider, latency_slider_cb, LV_EVENT_VALUE_CHANGED, panel);
    lv_group_add_obj(panel->group, panel->latency_slider);

    lv_obj_t *audio_lbl = lv_label_create(panel->customize_panel);
    lv_label_set_text(audio_lbl, locstr("Audio output"));
    lv_obj_set_style_text_color(audio_lbl, lv_color_hex(0xdbe4ea), 0);
    panel->audio_dropdown = lv_dropdown_create(panel->customize_panel);
    lv_dropdown_set_options(panel->audio_dropdown,
                            locstr("Auto (game decides)\nOff\nController speaker\nHeadphone jack\nSpeaker + jack"));
    lv_obj_set_width(panel->audio_dropdown, LV_PCT(100));
    lv_obj_add_event_cb(panel->audio_dropdown, audio_dropdown_cb, LV_EVENT_VALUE_CHANGED, panel);
    lv_group_add_obj(panel->group, panel->audio_dropdown);

    panel->speaker_label = lv_label_create(panel->customize_panel);
    lv_obj_set_style_text_color(panel->speaker_label, lv_color_hex(0xdbe4ea), 0);
    panel->speaker_slider = lv_slider_create(panel->customize_panel);
    lv_slider_set_range(panel->speaker_slider, 0, DS_VOLUME_MAX);
    lv_obj_set_width(panel->speaker_slider, LV_PCT(100));
    lv_obj_add_event_cb(panel->speaker_slider, speaker_slider_cb, LV_EVENT_VALUE_CHANGED, panel);
    lv_group_add_obj(panel->group, panel->speaker_slider);

    panel->headset_label = lv_label_create(panel->customize_panel);
    lv_obj_set_style_text_color(panel->headset_label, lv_color_hex(0xdbe4ea), 0);
    panel->headset_slider = lv_slider_create(panel->customize_panel);
    lv_slider_set_range(panel->headset_slider, 0, DS_VOLUME_MAX);
    lv_obj_set_width(panel->headset_slider, LV_PCT(100));
    lv_obj_add_event_cb(panel->headset_slider, headset_slider_cb, LV_EVENT_VALUE_CHANGED, panel);
    lv_group_add_obj(panel->group, panel->headset_slider);

    panel->haptics_row = lv_obj_create(panel->customize_panel);
    lv_obj_remove_style_all(panel->haptics_row);
    lv_obj_set_width(panel->haptics_row, LV_PCT(100));
    lv_obj_set_height(panel->haptics_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel->haptics_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel->haptics_row, LV_OBJ_FLAG_SCROLLABLE);
    panel->haptics_label = lv_label_create(panel->haptics_row);
    lv_obj_set_style_text_color(panel->haptics_label, lv_color_hex(0xdbe4ea), 0);
    panel->haptics_slider = lv_slider_create(panel->haptics_row);
    lv_slider_set_range(panel->haptics_slider, 0, DS_HAPTICS_MAX);
    lv_obj_set_width(panel->haptics_slider, LV_PCT(100));
    lv_obj_add_event_cb(panel->haptics_slider, haptics_slider_cb, LV_EVENT_VALUE_CHANGED, panel);
    lv_group_add_obj(panel->group, panel->haptics_slider);

    panel->reset_settings_btn = lv_btn_create(panel->customize_panel);
    lv_obj_set_size(panel->reset_settings_btn, LV_DPX(180), LV_DPX(40));
    lv_obj_add_event_cb(panel->reset_settings_btn, reset_settings_cb, LV_EVENT_CLICKED, panel);
    lv_group_add_obj(panel->group, panel->reset_settings_btn);
    lv_obj_t *reset_lbl = lv_label_create(panel->reset_settings_btn);
    lv_label_set_text(reset_lbl, locstr("Reset to defaults"));
    lv_obj_center(reset_lbl);

    panel_attach_keys(panel->customize_panel, panel);

    panel->container = cont;
    lv_obj_add_event_cb(cont, panel_delete_cb, LV_EVENT_DELETE, panel);
    panel->refresh_timer = lv_timer_create(refresh_timer_cb, 2000, panel);

    hid_passthrough_panel_refresh(cont);
    return cont;
}

#endif
