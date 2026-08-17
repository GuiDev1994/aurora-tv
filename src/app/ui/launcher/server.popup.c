#include "server.popup.h"
#include "server.context_menu.h"
#include "appitem.view.h"

#include "app.h"
#include "backend/pcmanager.h"
#include "ui/ui_input.h"

#include "lvgl.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"
#include "lv_gridview.h"

#include "util/i18n.h"

#include <SDL.h>
#include <string.h>

/** Visible rows in the popup viewport (scroll for more). */
#define SERVER_GRID_VISIBLE_ROWS 2

typedef struct server_popup_entry_t {
    uuidstr_t id;
    const char *hostname;
    SERVER_STATE state;
    bool selected;
    bool streaming;
} server_popup_entry_t;

typedef struct server_tile_holder_t {
    lv_obj_t *icon;
    lv_obj_t *title;
} server_tile_holder_t;

typedef struct server_popup_t {
    lv_obj_t *msgbox;
    lv_obj_t *grid;
    lv_group_t *group;
    launcher_fragment_t *launcher;
    server_popup_entry_t *entries;
    int entry_count;
    appitem_styles_t item_style;
    lv_coord_t col_width;
    lv_coord_t col_height;
    int col_count;
} server_popup_t;

static void server_popup_select_index(server_popup_t *popup, int index);

static const char *server_entry_icon(const server_popup_entry_t *entry) {
    if (entry == NULL) {
        return MAT_SYMBOL_WARNING;
    }
    switch (entry->state.code) {
        case SERVER_STATE_NONE:
        case SERVER_STATE_QUERYING:
            return MAT_SYMBOL_TV;
        case SERVER_STATE_AVAILABLE:
            return entry->streaming ? MAT_SYMBOL_ONDEMAND_VIDEO : MAT_SYMBOL_TV;
        case SERVER_STATE_NOT_PAIRED:
            return MAT_SYMBOL_LOCK;
        case SERVER_STATE_ERROR:
        case SERVER_STATE_OFFLINE:
            return MAT_SYMBOL_WARNING;
        default:
            return MAT_SYMBOL_TV;
    }
}

static int server_popup_col_count(int entry_count) {
    if (entry_count <= 1) {
        return 1;
    }
    if (entry_count <= 4) {
        return entry_count;
    }
    if (entry_count <= 8) {
        return 4;
    }
    return 6;
}

static void server_popup_update_grid(server_popup_t *popup) {
    if (popup == NULL || popup->grid == NULL) {
        return;
    }
    lv_obj_t *grid = popup->grid;
    lv_obj_t *content = lv_obj_get_parent(grid);
    lv_obj_update_layout(content);
    lv_obj_update_layout(grid);

    lv_coord_t view_w = lv_obj_get_width(grid);
    lv_coord_t view_h = lv_obj_get_height(grid);
    if (view_w <= 0) {
        view_w = (lv_coord_t) (lv_disp_get_hor_res(NULL) * 0.82);
    }
    if (view_h <= 0) {
        view_h = LV_DPX(360);
    }

    const int col_count = server_popup_col_count(popup->entry_count);
    lv_coord_t pad_l = lv_obj_get_style_pad_left(grid, 0);
    lv_coord_t pad_r = lv_obj_get_style_pad_right(grid, 0);
    lv_coord_t pad_t = lv_obj_get_style_pad_top(grid, 0);
    lv_coord_t pad_b = lv_obj_get_style_pad_bottom(grid, 0);
    lv_coord_t gap_col = lv_obj_get_style_pad_column(grid, 0);
    lv_coord_t gap_row = lv_obj_get_style_pad_row(grid, 0);

    lv_coord_t row_height =
            (view_h - pad_t - pad_b - gap_row * (SERVER_GRID_VISIBLE_ROWS - 1)) / SERVER_GRID_VISIBLE_ROWS;
    if (row_height < LV_DPX(72)) {
        row_height = LV_MAX(LV_DPX(72), view_h / SERVER_GRID_VISIBLE_ROWS);
    }

    /* Match home-screen cover aspect (600×800). */
    lv_coord_t col_width = row_height * 600 / 800;
    lv_coord_t max_col_width = (view_w - pad_l - pad_r - gap_col * (col_count - 1)) / col_count;
    if (col_width > max_col_width) {
        col_width = max_col_width;
    }
    if (col_width < LV_DPX(72)) {
        col_width = LV_DPX(72);
    }

    popup->col_count = col_count;
    popup->col_width = col_width;
    popup->col_height = row_height;
    popup->item_style.defcover_src.header.w = col_width;
    popup->item_style.defcover_src.header.h = row_height;

    lv_gridview_set_config(grid, col_count, row_height, LV_GRID_ALIGN_CENTER, LV_GRID_ALIGN_CENTER);
}

static void server_tile_deselected(lv_event_t *e) {
    lv_obj_clear_state(lv_event_get_target(e), LV_STATE_FOCUS_KEY);
}

static void server_tile_holder_delete_cb(lv_event_t *e) {
    SDL_free(lv_event_get_user_data(e));
}

static lv_obj_t *server_tile_create(server_popup_t *popup, lv_obj_t *parent) {
    appitem_styles_t *styles = &popup->item_style;
    lv_obj_t *item = lv_img_create(parent);
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(item, &styles->cover, 0);
    lv_obj_set_size(item, popup->col_width, popup->col_height);
    lv_img_set_src(item, &styles->defcover_src);
    lv_img_set_antialias(item, true);
    lv_obj_set_style_outline_opa(item, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(item, ml_color_hex(ML_COLOR_FOCUS), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(item, ml_color_hex(ML_COLOR_PRIMARY), LV_STATE_CHECKED);
    lv_obj_set_style_outline_opa(item, LV_OPA_COVER, LV_STATE_CHECKED);

    lv_obj_t *icon = lv_label_create(item);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(icon, lv_theme_moonlight_get_iconfont_large(item), 0);
    lv_obj_set_style_text_color(icon, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, LV_DPX(-8));

    lv_obj_t *title = lv_label_create(item);
    const lv_font_t *font = lv_theme_get_font_small(item);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_coord_t th = lv_obj_get_style_text_font(title, 0)->line_height + LV_DPX(8);
    lv_obj_set_size(title, LV_PCT(100), th);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_hor(title, LV_DPX(6), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(title, LV_ALIGN_BOTTOM_MID, 0, -LV_DPX(4));

    server_tile_holder_t *holder = SDL_malloc(sizeof(server_tile_holder_t));
    holder->icon = icon;
    holder->title = title;
    lv_obj_set_user_data(item, holder);
    lv_obj_add_event_cb(item, server_tile_deselected, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(item, server_tile_holder_delete_cb, LV_EVENT_DELETE, holder);
    return item;
}

static void server_tile_bind(server_popup_t *popup, lv_obj_t *item, int position) {
    if (popup == NULL || item == NULL || position < 0 || position >= popup->entry_count) {
        return;
    }
    server_tile_holder_t *holder = lv_obj_get_user_data(item);
    if (holder == NULL) {
        return;
    }
    const server_popup_entry_t *entry = &popup->entries[position];
    lv_label_set_text(holder->title, entry->hostname);
    lv_label_set_text_static(holder->icon, server_entry_icon(entry));
    if (entry->selected) {
        lv_obj_add_state(item, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(item, LV_STATE_CHECKED);
    }
}

static int server_adapter_count(lv_obj_t *grid, void *data) {
    (void) grid;
    server_popup_t *popup = data;
    return popup != NULL ? popup->entry_count : 0;
}

static lv_obj_t *server_adapter_create_view(lv_obj_t *parent) {
    server_popup_t *popup = lv_obj_get_user_data(parent);
    return server_tile_create(popup, parent);
}

static void server_adapter_bind_view(lv_obj_t *grid, lv_obj_t *item_view, void *data, int position) {
    (void) grid;
    server_tile_bind(data, item_view, position);
}

static const lv_gridview_adapter_t server_grid_adapter = {
        .item_count = server_adapter_count,
        .create_view = server_adapter_create_view,
        .bind_view = server_adapter_bind_view,
};

static void server_grid_focus_with_key(lv_obj_t *grid, int idx) {
    lv_gridview_focus(grid, idx);
    lv_obj_t *item = lv_gridview_get_item_view(grid, idx);
    if (item != NULL) {
        lv_obj_add_state(item, LV_STATE_FOCUS_KEY);
    }
}

static int server_popup_selected_index(const server_popup_t *popup) {
    if (popup == NULL) {
        return 0;
    }
    for (int i = 0; i < popup->entry_count; i++) {
        if (popup->entries[i].selected) {
            return i;
        }
    }
    return 0;
}

static void server_popup_select_index(server_popup_t *popup, int index) {
    if (popup == NULL || popup->launcher == NULL || index < 0 || index >= popup->entry_count) {
        return;
    }
    uuidstr_t snapshot = popup->entries[index].id;
    lv_msgbox_close_async(popup->msgbox);
    launcher_select_server(popup->launcher, &snapshot);
}

static void server_popup_item_click(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (popup == NULL || popup->grid == NULL || lv_obj_get_parent(target) != popup->grid) {
        return;
    }
    int index = lv_gridview_get_item_data_index(popup->grid, target);
    if (index >= 0) {
        server_popup_select_index(popup, index);
    }
}

static void server_popup_item_longpress(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (popup == NULL || popup->grid == NULL || lv_obj_get_parent(target) != popup->grid) {
        return;
    }
    int index = lv_gridview_get_item_data_index(popup->grid, target);
    if (index < 0 || index >= popup->entry_count) {
        return;
    }
    uuidstr_t snapshot = popup->entries[index].id;
    lv_msgbox_close_async(popup->msgbox);
    lv_fragment_t *fragment = lv_fragment_create(&server_menu_class, &snapshot);
    lv_obj_t *menu = lv_fragment_create_obj(fragment, NULL);
    lv_obj_add_event_cb(menu, ui_cb_destroy_fragment, LV_EVENT_DELETE, fragment);
}

static void server_popup_grid_focus_enter(lv_event_t *e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    server_popup_t *popup = lv_event_get_user_data(e);
    if (popup == NULL || popup->grid == NULL) {
        return;
    }
    int idx = server_popup_selected_index(popup);
    server_grid_focus_with_key(popup->grid, idx);
}

static void server_popup_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }
    server_popup_t *popup = lv_event_get_user_data(e);
    if (lv_event_get_key(e) == LV_KEY_ESC) {
        lv_msgbox_close_async(popup->msgbox);
    }
}

static void server_popup_deleted(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    if (popup->group != NULL) {
        app_input_remove_modal_group(&popup->launcher->global->ui.input, popup->group);
        lv_group_del(popup->group);
        popup->group = NULL;
    }
    appitem_style_deinit(&popup->item_style);
    SDL_free(popup->entries);
    SDL_free(popup);
}

static void server_popup_msgbox_cb(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (msgbox != popup->msgbox) {
        return;
    }
    if (lv_msgbox_get_active_btn(msgbox) == 0) {
        lv_msgbox_close_async(msgbox);
    }
}

void server_popup_open(launcher_fragment_t *controller) {
    if (!controller) {
        return;
    }

    int count = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server != NULL) {
            count++;
        }
    }

    server_popup_t *popup = SDL_malloc(sizeof(server_popup_t));
    memset(popup, 0, sizeof(*popup));
    popup->launcher = controller;
    popup->entry_count = count;
    appitem_style_init(&popup->item_style);

    if (count > 0) {
        popup->entries = SDL_malloc((size_t) count * sizeof(server_popup_entry_t));
        int i = 0;
        for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
            if (cur->server == NULL) {
                continue;
            }
            popup->entries[i].id = cur->id;
            popup->entries[i].hostname = cur->server->hostname;
            popup->entries[i].state = cur->state;
            popup->entries[i].selected = cur->selected;
            popup->entries[i].streaming = cur->server->currentGame != 0;
            i++;
        }
    }

    static const char *btn_texts[] = {translatable("Close"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Select server"), NULL, btn_texts, false);
    popup->msgbox = msgbox;
    lv_obj_set_user_data(msgbox, popup);
    lv_obj_add_event_cb(msgbox, server_popup_deleted, LV_EVENT_DELETE, popup);
    lv_obj_add_event_cb(msgbox, server_popup_msgbox_cb, LV_EVENT_VALUE_CHANGED, popup);
    lv_obj_add_event_cb(msgbox, server_popup_key_cb, LV_EVENT_KEY, popup);
    lv_obj_set_width(msgbox, LV_PCT(85));

    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, lv_dpx(16), 0);
    lv_obj_set_style_pad_gap(content, lv_dpx(12), 0);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(content);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_text_static(empty,
                                 locstr("No paired computers yet. Use the \"+\" button on the top bar to add one."));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_obj_t *grid = popup->grid = lv_gridview_create(content);
        lv_obj_set_user_data(grid, popup);
        lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_width(grid, LV_PCT(100));
        lv_obj_set_height(grid, LV_DPX(360));
        lv_obj_set_scroll_dir(grid, LV_DIR_VER);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_pad_hor(grid, lv_dpx(8), 0);
        lv_obj_set_style_pad_ver(grid, lv_dpx(8), 0);
        lv_obj_set_style_pad_row(grid, lv_dpx(16), 0);
        lv_obj_set_style_pad_column(grid, lv_dpx(16), 0);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(grid, 0, 0);
        lv_obj_set_style_anim_time(grid, 220, 0);
        lv_gridview_set_key_focus_clamp(grid, true);
        lv_gridview_set_adapter(grid, &server_grid_adapter);

        server_popup_update_grid(popup);
        lv_gridview_set_data(grid, popup);

        lv_obj_add_event_cb(grid, server_popup_item_click, LV_EVENT_SHORT_CLICKED, popup);
        lv_obj_add_event_cb(grid, server_popup_item_longpress, LV_EVENT_LONG_PRESSED, popup);
        lv_obj_add_event_cb(grid, server_popup_grid_focus_enter, LV_EVENT_FOCUSED, popup);
        lv_obj_add_event_cb(grid, server_popup_key_cb, LV_EVENT_KEY, popup);

        popup->group = lv_group_create();
        lv_group_set_wrap(popup->group, false);
        lv_group_add_obj(popup->group, grid);
        app_input_push_modal_group(&controller->global->ui.input, popup->group);
        app_input_set_group(&controller->global->ui.input, popup->group);
        server_grid_focus_with_key(grid, server_popup_selected_index(popup));
    }

    lv_obj_center(msgbox);
}
