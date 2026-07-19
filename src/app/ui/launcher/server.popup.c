#include "server.popup.h"
#include "server.context_menu.h"

#include "app.h"
#include "backend/pcmanager.h"

#include "lvgl.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"

#include "util/i18n.h"

#include <SDL.h>

typedef struct server_popup_t {
    lv_obj_t *msgbox;
    launcher_fragment_t *launcher;
} server_popup_t;

static const char *server_popup_item_icon(const pclist_t *node) {
    if (node == NULL) {
        return MAT_SYMBOL_WARNING;
    }
    switch (node->state.code) {
        case SERVER_STATE_NONE:
        case SERVER_STATE_QUERYING:
            return MAT_SYMBOL_TV;
        case SERVER_STATE_AVAILABLE:
            return node->server->currentGame ? MAT_SYMBOL_ONDEMAND_VIDEO : MAT_SYMBOL_TV;
        case SERVER_STATE_NOT_PAIRED:
            return MAT_SYMBOL_LOCK;
        case SERVER_STATE_ERROR:
        case SERVER_STATE_OFFLINE:
            return MAT_SYMBOL_WARNING;
        default:
            return MAT_SYMBOL_TV;
    }
}

static void server_popup_item_deleted(lv_event_t *e) {
    void *uuid = lv_obj_get_user_data(lv_event_get_target(e));
    if (uuid) {
        SDL_free(uuid);
    }
}

static void server_popup_item_clicked(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    /* Only react to clicks on actual list buttons, not the surrounding container. */
    if (lv_obj_get_parent(target) != lv_event_get_current_target(e)) { return; }
    const uuidstr_t *uuid = (const uuidstr_t *) lv_obj_get_user_data(target);
    if (!uuid || !popup->launcher) {
        lv_msgbox_close_async(popup->msgbox);
        return;
    }
    /* Snapshot the UUID since closing the popup destroys the list (and our user_data). */
    uuidstr_t snapshot = *uuid;
    lv_msgbox_close_async(popup->msgbox);
    launcher_select_server(popup->launcher, &snapshot);
}

static void server_popup_item_longpress(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (lv_obj_get_parent(target) != lv_event_get_current_target(e)) { return; }
    const uuidstr_t *uuid = (const uuidstr_t *) lv_obj_get_user_data(target);
    if (!uuid) { return; }
    /* Reuse the existing server context menu (rename / pair / unpair). It opens as a
     * separate modal, so we close ourselves first to avoid stacking two modals on
     * top of each other and confusing the focus group. */
    uuidstr_t snapshot = *uuid;
    lv_msgbox_close_async(popup->msgbox);
    lv_fragment_t *fragment = lv_fragment_create(&server_menu_class, &snapshot);
    lv_obj_t *menu = lv_fragment_create_obj(fragment, NULL);
    lv_obj_add_event_cb(menu, ui_cb_destroy_fragment, LV_EVENT_DELETE, fragment);
}

static void server_popup_deleted(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    SDL_free(popup);
}

static void server_popup_msgbox_cb(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (msgbox != popup->msgbox) {
        return;
    }
    /* Button 0 = "Close" in lv_msgbox_create_i18n. */
    if (lv_msgbox_get_active_btn(msgbox) == 0) {
        lv_msgbox_close_async(msgbox);
    }
}

void server_popup_open(launcher_fragment_t *controller) {
    if (!controller) { return; }

    static const char *btn_texts[] = {translatable("Close"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Select server"), NULL, btn_texts, false);

    server_popup_t *popup = SDL_malloc(sizeof(server_popup_t));
    popup->msgbox = msgbox;
    popup->launcher = controller;
    lv_obj_set_user_data(msgbox, popup);
    lv_obj_add_event_cb(msgbox, server_popup_deleted, LV_EVENT_DELETE, popup);
    lv_obj_add_event_cb(msgbox, server_popup_msgbox_cb, LV_EVENT_VALUE_CHANGED, popup);

    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, lv_dpx(8), 0);
    lv_obj_set_style_pad_gap(content, lv_dpx(8), 0);
    /* The popup width is constrained so very long PC names get truncated rather than
     * blowing the dialog out across the whole screen. */
    lv_obj_set_width(msgbox, LV_PCT(60));

    lv_obj_t *list = lv_list_create(content);
    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_DPX(280));
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_radius(list, lv_dpx(8), 0);
    lv_obj_add_event_cb(list, server_popup_item_clicked, LV_EVENT_SHORT_CLICKED, popup);
    lv_obj_add_event_cb(list, server_popup_item_longpress, LV_EVENT_LONG_PRESSED, popup);

    int count = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        const SERVER_DATA *server = cur->server;
        if (server == NULL) { continue; }
        const char *icon = server_popup_item_icon(cur);
        /* lv_list_add_btn's icon parameter is rendered as an lv_img, not a label, so
         * it can't display glyphs from our custom icon font (they'd show as a missing
         * "tofu" glyph box). Add the button without an icon, then insert our own icon
         * label styled with the icon font, matching the pattern used elsewhere (e.g.
         * the top bar server button / close buttons in settings). */
        lv_obj_t *item = lv_list_add_btn(list, NULL, server->hostname);
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (cur->selected) {
            lv_obj_add_state(item, LV_STATE_CHECKED);
        }

        lv_obj_t *icon_label = lv_label_create(item);
        lv_obj_set_style_text_font(icon_label, lv_theme_moonlight_get_iconfont_small(item), 0);
        lv_label_set_text_static(icon_label, icon);
        lv_obj_clear_flag(icon_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(icon_label, 0);
        uuidstr_t *uuid = SDL_malloc(sizeof(uuidstr_t));
        *uuid = cur->id;
        lv_obj_set_user_data(item, uuid);
        lv_obj_add_event_cb(item, server_popup_item_deleted, LV_EVENT_DELETE, NULL);
        count++;
    }

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(content);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_text_static(empty,
                                 locstr("No paired computers yet. Use the \"+\" button on the top bar to add one."));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_center(msgbox);
}
