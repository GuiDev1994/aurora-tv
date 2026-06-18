#if defined(TARGET_WEBOS)

#include "hid_passthrough_panel.h"

#include "hid_passthrough/ctm_hid.h"
#include "hid_passthrough/hid_passthrough_manager.h"
#include "stream/session.h"

#include "util/i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    lv_obj_t *container;
    lv_obj_t *list;
    lv_group_t *group;
    session_t *session;
    hid_passthrough_panel_close_cb on_close;
    void *on_close_userdata;
} hid_pt_panel_t;

static void panel_close(hid_pt_panel_t *panel) {
    if (!panel) {
        return;
    }
    if (panel->on_close) {
        panel->on_close(panel->on_close_userdata);
    }
}

static void back_btn_cb(lv_event_t *e) {
    panel_close(lv_event_get_user_data(e));
}

static void panel_delete_cb(lv_event_t *e) {
    hid_pt_panel_t *panel = lv_event_get_user_data(e);
    if (panel->group) {
        lv_group_del(panel->group);
    }
    free(panel);
}

static void plug_btn_cb(lv_event_t *e) {
    hid_pt_panel_t *panel = lv_event_get_user_data(e);
    int index = (int) (intptr_t) lv_obj_get_user_data(lv_event_get_current_target(e));
    if (!panel || !panel->session || index < 0) {
        return;
    }
    hid_passthrough_manager_t *mgr = session_get_hid_passthrough(panel->session);
    if (!mgr) {
        return;
    }
    hid_pt_device_info_t info;
    if (hid_passthrough_manager_get_device(mgr, index, &info) != 0) {
        return;
    }
    if (info.plugged) {
        hid_passthrough_manager_unplug(mgr, info.path);
    } else {
        hid_passthrough_manager_plug(mgr, info.path);
    }
    hid_passthrough_panel_refresh(panel->container);
}

static lv_obj_t *make_row_button(lv_obj_t *parent, const char *label, lv_color_t color, int device_index,
                                 hid_pt_panel_t *panel) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_height(btn, LV_DPX(36));
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_add_event_cb(btn, plug_btn_cb, LV_EVENT_CLICKED, panel);
    lv_obj_set_user_data(btn, (void *) (intptr_t) device_index);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);
    lv_group_add_obj(panel->group, btn);
    return btn;
}

void hid_passthrough_panel_refresh(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    if (!panel || !panel->list) {
        return;
    }
    hid_passthrough_manager_t *mgr = session_get_hid_passthrough(panel->session);
    if (mgr) {
        hid_passthrough_manager_rescan(mgr);
    }

    lv_obj_clean(panel->list);
    int count = mgr ? hid_passthrough_manager_device_count(mgr) : 0;
    if (count <= 0) {
        lv_obj_t *empty = lv_label_create(panel->list);
        lv_label_set_text(empty, locstr("No HID gamepads visible to Aurora"));
        lv_obj_set_style_text_color(empty, lv_color_hex(0xaaaaaa), 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        hid_pt_device_info_t info;
        if (hid_passthrough_manager_get_device(mgr, i, &info) != 0) {
            continue;
        }

        lv_obj_t *row = lv_obj_create(panel->list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, LV_DPX(8), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, LV_DPX(6), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char title[96];
        const char *usage = ctm_hid_usage_label(info.usage_page, info.usage);
        if (info.product[0]) {
            snprintf(title, sizeof(title), "%s", info.product);
        } else {
            snprintf(title, sizeof(title), "%04x:%04x %s", info.vendor_id, info.product_id, usage);
        }

        lv_obj_t *text_col = lv_obj_create(row);
        lv_obj_remove_style_all(text_col);
        lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(text_col, 1);
        lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(text_col);
        lv_label_set_text(name, title);
        lv_obj_t *sub = lv_label_create(text_col);
        lv_label_set_text_fmt(sub, "%s  %s", info.path,
                              info.connected ? locstr("Bridged") : locstr("Idle"));
        lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(sub, lv_theme_get_font_small(row), 0);

        const char *btn_text = info.plugged ? locstr("Plug out") : locstr("Plug in");
        lv_color_t btn_color = info.plugged ? lv_palette_main(LV_PALETTE_RED) : lv_palette_main(LV_PALETTE_GREEN);
        make_row_button(row, btn_text, btn_color, i, panel);
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
    panel->group = lv_group_create();

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_80, 0);
    lv_obj_set_user_data(cont, panel);

    lv_obj_t *sheet = lv_obj_create(cont);
    lv_obj_set_width(sheet, LV_PCT(92));
    lv_obj_set_height(sheet, LV_PCT(70));
    lv_obj_center(sheet);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sheet, LV_DPX(10), 0);
    lv_obj_set_style_pad_all(sheet, LV_DPX(16), 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(sheet, LV_DPX(12), 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(sheet);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, locstr("HID Devices"));

    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_height(back_btn, LV_DPX(36));
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, panel);
    lv_group_add_obj(panel->group, back_btn);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, locstr("Back"));
    lv_obj_center(back_lbl);

    panel->list = lv_obj_create(sheet);
    lv_obj_set_width(panel->list, LV_PCT(100));
    lv_obj_set_flex_grow(panel->list, 1);
    lv_obj_set_flex_flow(panel->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel->list, LV_DPX(8), 0);
    lv_obj_set_style_bg_opa(panel->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(panel->list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(panel->list, LV_OBJ_FLAG_SCROLLABLE);

    panel->container = cont;
    lv_obj_add_event_cb(cont, panel_delete_cb, LV_EVENT_DELETE, panel);
    hid_passthrough_panel_refresh(cont);
    lv_group_focus_obj(back_btn);
    return cont;
}

#endif
