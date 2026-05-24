#include "settings.controller.h"

#include <string.h>

#include "ui/root.h"

#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/ext/lv_child_group.h"
#include "lvgl/util/lv_app_utils.h"

#include "util/user_event.h"
#include "util/font.h"
#include "util/i18n.h"
#include "stream/session.h"
#include "lvgl/theme/lv_theme_moonlight.h"

#include "../launcher/launcher.controller.h"

typedef struct {
    const char *icon;
    const char *name;
    const lv_fragment_class_t *cls;
} settings_entry_t;

static const settings_entry_t entries[] = {
        {MAT_SYMBOL_SETTINGS,        translatable("Basic Settings"), &settings_pane_basic_cls},
        {MAT_SYMBOL_DESKTOP_WINDOWS, translatable("Host Settings"),  &settings_pane_host_cls},
        {MAT_SYMBOL_SPORTS_ESPORTS,  translatable("Input Settings"), &settings_pane_input_cls},
        {MAT_SYMBOL_VOLUME_UP,       translatable("Audio Settings"), &settings_pane_audio_cls},
        {MAT_SYMBOL_VIDEO_SETTINGS,  translatable("Video Settings"), &settings_pane_video_cls},
        {MAT_SYMBOL_HELP,            translatable("About"),          &settings_pane_about_cls},
};
static const int entries_len = sizeof(entries) / sizeof(settings_entry_t);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view);

static void on_entry_focus(lv_event_t *event);

static void on_entry_click(lv_event_t *event);

static void on_nav_key(lv_event_t *event);

static void on_detail_key(lv_event_t *e);

static void on_back_request(lv_event_t *e);

static void on_tab_key(lv_event_t *event);

static void on_tab_content_key(lv_event_t *e);

static void on_dropdown_clicked(lv_event_t *event);

static void settings_controller_ctor(lv_fragment_t *self, void *args);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void detail_defocus(settings_controller_t *controller, lv_event_t *e);

static bool detail_item_needs_lrkey(lv_obj_t *obj);

static void show_pane(settings_controller_t *controller, const lv_fragment_class_t *cls);

static void settings_close(lv_event_t *e);

static void settings_finish_close(settings_controller_t *fragment);

static bool settings_try_close(settings_controller_t *fragment);

static void stream_reconnect_confirm_cb(lv_event_t *e);

static void settings_apply_locale_if_needed(settings_controller_t *controller);

static void pane_child_added(lv_event_t *e);

static void settings_launcher_detach(settings_controller_t *fragment);

static void settings_close_pane_popup(settings_controller_t *c);

static void settings_show_pane_popup(settings_controller_t *c, const lv_fragment_class_t *cls);

static void on_launcher_embedded_view_created(settings_controller_t *controller);

static void embed_cancel_cb(lv_event_t *e);

static void settings_embed_refocus_appbar(settings_controller_t *c);

static void embed_popup_add_objs_recursive(lv_obj_t *parent, lv_group_t *g);

static lv_obj_t *embed_popup_first_focusable(lv_obj_t *parent);

static void embed_popup_attach_key_handlers(lv_obj_t *parent, settings_controller_t *c);

#define UI_IS_MINI(width) ((width) < LV_DPX(240))

const lv_fragment_class_t settings_controller_cls = {
        .constructor_cb = settings_controller_ctor,
        .create_obj_cb = settings_win_create,
        .obj_created_cb = on_view_created,
        .obj_deleted_cb = on_destroy_view,
        .event_cb = on_event,
        .instance_size = sizeof(settings_controller_t),
};

static void settings_controller_ctor(lv_fragment_t *self, void *args) {
    settings_controller_t *fragment = (settings_controller_t *) self;
    settings_open_args_t *open = (settings_open_args_t *) args;
    fragment->app = open->app;
    fragment->launcher_host = open->launcher;
    fragment->pane_mbox = NULL;
    fragment->pane_fragment = NULL;
    fragment->pane_popup_group = NULL;
    fragment->embed_root = NULL;
    fragment->embed_appbar = NULL;
    fragment->needs_stream_reconnect = false;
    fragment->needs_locale_reapply = false;
    fragment->mini = fragment->pending_mini = UI_IS_MINI(fragment->app->ui.width);
    os_info_get(&fragment->os_info);
#if TARGET_WEBOS
    if (!SDL_webOSGetPanelResolution(&fragment->panel_width, &fragment->panel_height)) {
        fragment->panel_width = 1920;
        fragment->panel_height = 1080;
    }
    if (!SDL_webOSGetRefreshRate(&fragment->panel_fps)) {
        fragment->panel_fps = 60;
    }
#endif
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    settings_controller_t *controller = (settings_controller_t *) self;
    if (controller->launcher_host) {
        on_launcher_embedded_view_created(controller);
        return;
    }
    lv_obj_add_event_cb(controller->close_btn, settings_close, LV_EVENT_CLICKED, controller);
    if (controller->mini) {
        controller->nav_group = lv_group_create();
        controller->tab_groups = lv_mem_alloc(sizeof(lv_group_t *) * entries_len);
        app_input_set_group(&controller->app->ui.input, controller->nav_group);

        lv_obj_t *btns = lv_tabview_get_tab_btns(controller->tabview);
        lv_obj_set_style_text_font(btns, lv_theme_moonlight_get_iconfont_large(btns), 0);
        lv_group_remove_obj(btns);

        lv_group_add_obj(controller->nav_group, controller->nav);
        lv_obj_add_event_cb(controller->nav, on_tab_key, LV_EVENT_KEY, controller);

        for (int i = 0; i < entries_len; ++i) {
            settings_entry_t entry = entries[i];
            lv_group_t *tab_group = lv_group_create();
            controller->tab_groups[i] = tab_group;
            lv_obj_t *page = lv_tabview_add_tab(controller->tabview, entry.icon);
            lv_obj_add_event_cb(page, cb_child_group_add, LV_EVENT_CHILD_CREATED, tab_group);
            lv_obj_add_event_cb(page, pane_child_added, LV_EVENT_CHILD_CREATED, controller);
            lv_fragment_t *pane = lv_fragment_create(entry.cls, controller);
            lv_fragment_create_obj(pane, page);
            lv_obj_set_user_data(page, pane);

            lv_obj_t *tab_focused = lv_group_get_focused(tab_group);
            if (tab_focused) {
                lv_obj_clear_state(tab_focused, LV_STATE_FOCUS_KEY);
            }
        }
    } else {
        controller->nav_group = lv_group_create();
        controller->detail_group = lv_group_create();
        lv_group_set_wrap(controller->detail_group, false);

        lv_obj_add_event_cb(controller->nav, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->nav_group);
        lv_obj_add_event_cb(controller->detail, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->detail_group);
        lv_obj_add_event_cb(controller->detail, pane_child_added, LV_EVENT_CHILD_CREATED, controller);
        lv_obj_add_event_cb(controller->detail, on_back_request, LV_EVENT_CANCEL, controller);

        lv_obj_add_event_cb(controller->nav, on_entry_focus, LV_EVENT_FOCUSED, controller);
        lv_obj_add_event_cb(controller->nav, on_entry_click, LV_EVENT_CLICKED, controller);
        lv_obj_add_event_cb(controller->nav, on_nav_key, LV_EVENT_KEY, controller);
        lv_obj_add_event_cb(controller->nav, on_back_request, LV_EVENT_CANCEL, controller);

        app_input_set_group(&controller->app->ui.input, controller->nav_group);

        for (int i = 0; i < entries_len; ++i) {
            settings_entry_t entry = entries[i];
            lv_obj_t *item_view = lv_list_add_btn(controller->nav, entry.icon, locstr(entry.name));
            lv_btn_set_icon_font(item_view, lv_theme_moonlight_get_iconfont_normal(item_view));

            lv_obj_set_style_bg_opa(item_view, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
            lv_obj_add_flag(item_view, LV_OBJ_FLAG_EVENT_BUBBLE);
            item_view->user_data = (void *) entry.cls;
        }
        show_pane(controller, entries[0].cls);
    }
}

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view) {
    settings_controller_t *controller = (settings_controller_t *) self;
    LV_UNUSED(view);
    settings_save(app_configuration);
    settings_apply_locale_if_needed(controller);

    if (controller->launcher_host) {
        settings_close_pane_popup(controller);
        launcher_restore_nav_focus(controller->launcher_host);
        lv_group_del(controller->nav_group);
        return;
    }
    app_input_set_group(&controller->app->ui.input, NULL);
    if (controller->mini) {
        for (int i = 0; i < entries_len; i++) {
            lv_group_del(controller->tab_groups[i]);
        }
        lv_mem_free(controller->tab_groups);
        lv_group_del(controller->nav_group);
    } else {
        lv_group_del(controller->nav_group);
        lv_group_del(controller->detail_group);
    }
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    settings_controller_t *controller = (settings_controller_t *) self;
    app_ui_t *ui = &controller->app->ui;
    switch (code) {
        case USER_SIZE_CHANGED: {
            lv_obj_set_size(self->obj, ui->width, ui->height);
            if (controller->launcher_host) {
                break;
            }
            bool mini = UI_IS_MINI(ui->width);
            if (mini != controller->mini) {
                controller->pending_mini = mini;
                lv_fragment_recreate_obj(self);
            }
            break;
        }
    }
    return false;
}

static void on_entry_focus(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    if (controller->launcher_host) {
        return;
    }
    if (controller->base.managed->destroying_obj) { return; }
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_obj_get_parent(target) != controller->nav) { return; }
    lv_fragment_t *pane = lv_fragment_manager_get_top(controller->base.child_manager);
    lv_fragment_class_t *cls = target->user_data;
    if (pane && pane->cls == cls) {
        return;
    }
    for (int i = 0, j = (int) lv_obj_get_child_cnt(controller->nav); i < j; i++) {
        lv_obj_t *child = lv_obj_get_child(controller->nav, i);
        if (child == target) {
            lv_obj_add_state(child, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(child, LV_STATE_CHECKED);
        }
    }
    show_pane(controller, cls);
}

static void show_pane(settings_controller_t *controller, const lv_fragment_class_t *cls) {
    if (controller->launcher_host) {
        settings_show_pane_popup(controller, cls);
        return;
    }
    lv_fragment_t *fragment = lv_fragment_create(cls, controller);
    lv_fragment_manager_replace(controller->base.child_manager, fragment, &controller->detail);
    lv_obj_scroll_to_y(controller->detail, 0, LV_ANIM_OFF);
    lv_obj_t *focused = lv_group_get_focused(controller->detail_group);
    lv_event_send(focused, LV_EVENT_DEFOCUSED, NULL);
}

static void on_entry_click(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_obj_get_parent(target) != controller->nav) { return; }
    lv_fragment_t *pane = lv_fragment_manager_find_by_container(controller->base.child_manager,
                                                                controller->detail);
    if (!pane) { return; }
    lv_obj_t *first_focusable = NULL;
    for (int i = 0, j = (int) lv_obj_get_child_cnt(pane->obj); i < j; i++) {
        lv_obj_t *child = lv_obj_get_child(pane->obj, i);
        if (lv_obj_get_group(child)) {
            first_focusable = child;
            break;
        }
    }
    if (!first_focusable) { return; }
    app_input_set_group(&controller->app->ui.input, controller->detail_group);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) { return; }
    lv_group_focus_obj(first_focusable);
}

static void on_nav_key(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    switch (lv_event_get_key(event)) {
        case LV_KEY_DOWN: {
            lv_obj_t *target = lv_event_get_target(event);
            if (lv_obj_get_parent(target) != controller->nav) { return; }
            lv_group_t *group = controller->nav_group;
            lv_group_focus_next(group);
            break;
        }
        case LV_KEY_UP: {
            lv_obj_t *target = lv_event_get_target(event);
            if (lv_obj_get_parent(target) != controller->nav) { return; }
            lv_group_t *group = controller->nav_group;
            lv_group_focus_prev(group);
            break;
        }
        case LV_KEY_RIGHT: {
            lv_obj_t *target = lv_event_get_target(event);
            if (lv_obj_get_parent(target) != controller->nav) { return; }
            on_entry_click(event);
            break;
        }
    }
}

static void on_detail_key(lv_event_t *e) {
    settings_controller_t *controller = e->user_data;
    if (controller->mini) {
        on_tab_content_key(e);
        return;
    }
    lv_group_t *nav_detail = controller->pane_popup_group ? controller->pane_popup_group : controller->detail_group;
    if (!nav_detail) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(e);
    const uint32_t key = lv_event_get_key(e);

    if (controller->pane_popup_group != NULL) {
        switch (key) {
            case LV_KEY_ESC:
                if (controller->active_dropdown) {
                    lv_dropdown_close(controller->active_dropdown);
                    controller->active_dropdown = NULL;
                    return;
                }
                settings_close_pane_popup(controller);
                return;
            case LV_KEY_UP:
                if (controller->active_dropdown) {
                    return;
                }
                lv_group_focus_prev(nav_detail);
                return;
            case LV_KEY_DOWN:
                if (controller->active_dropdown) {
                    return;
                }
                lv_group_focus_next(nav_detail);
                return;
            case LV_KEY_LEFT:
                if (controller->active_dropdown) {
                    return;
                }
                if (detail_item_needs_lrkey(target)) {
                    return;
                }
                lv_group_focus_prev(nav_detail);
                return;
            case LV_KEY_RIGHT:
                if (controller->active_dropdown) {
                    return;
                }
                if (detail_item_needs_lrkey(target)) {
                    return;
                }
                if (lv_obj_has_class(target, &lv_dropdown_class)) {
                    lv_dropdown_close(target);
                    controller->active_dropdown = NULL;
                    return;
                }
                lv_group_focus_next(nav_detail);
                return;
            default:
                return;
        }
    }

    switch (key) {
        case LV_KEY_UP: {
            if (controller->active_dropdown) { return; }
            lv_group_focus_prev(nav_detail);
            break;
        }
        case LV_KEY_DOWN: {
            if (controller->active_dropdown) { return; }
            lv_group_focus_next(nav_detail);
            break;
        }
        case LV_KEY_LEFT: {
            if (detail_item_needs_lrkey(target)) { return; }
            if (controller->active_dropdown) { return; }
            detail_defocus(controller, e);
            break;
        }
        case LV_KEY_RIGHT: {
            if (detail_item_needs_lrkey(target)) { return; }
            if (controller->active_dropdown) { return; }
            if (lv_obj_has_class(target, &lv_dropdown_class)) {
                lv_dropdown_close(target);
                controller->active_dropdown = NULL;
            }
            break;
        }
    }
}

static void on_back_request(lv_event_t *e) {
    if (lv_event_get_param(e) == NULL) { return; }
    settings_controller_t *controller = e->user_data;
    if (controller->active_dropdown) {
        controller->active_dropdown = NULL;
        return;
    }
    if (lv_obj_has_state(controller->detail, LV_STATE_FOCUS_KEY)) {
        detail_defocus(controller, e);
    } else {
        settings_close(e);
    }
}

static void on_tab_key(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    switch (lv_event_get_key(event)) {
        case LV_KEY_LEFT: {
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            if (act <= 0) { return; }
            lv_tabview_set_act(controller->tabview, act - 1, true);
            break;
        }
        case LV_KEY_RIGHT: {
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            if (act >= entries_len) { return; }
            lv_tabview_set_act(controller->tabview, act + 1, true);
            break;
        }
        case LV_KEY_UP:
        case LV_KEY_DOWN:
        case LV_KEY_ENTER: {
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            lv_group_t *content_group = controller->tab_groups[act];
            if (lv_group_get_obj_count(content_group) == 0) {
                break;
            }
            app_input_set_group(&controller->app->ui.input, content_group);
            lv_obj_t *focused = lv_group_get_focused(content_group);
            if (focused) {
                lv_obj_add_state(focused, LV_STATE_FOCUS_KEY);
            }
            break;
        }
    }
}

static void on_tab_content_key(lv_event_t *e) {
    settings_controller_t *controller = e->user_data;
    switch (lv_event_get_key(e)) {
        case LV_KEY_DOWN: {
            if (controller->active_dropdown) { return; }
            lv_obj_t *target = lv_event_get_target(e);
            if (lv_obj_get_parent(target) == controller->tabview) { return; }
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            lv_group_t *group = controller->tab_groups[act];
            lv_group_focus_next(group);
            break;
        }
        case LV_KEY_UP: {
            if (controller->active_dropdown) { return; }
            lv_obj_t *target = lv_event_get_target(e);
            if (lv_obj_get_parent(target) == controller->tabview) { return; }
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            lv_group_t *group = controller->tab_groups[act];
            lv_group_focus_prev(group);
            break;
        }
        case LV_KEY_LEFT: {
            lv_obj_t *target = lv_event_get_target(e);
            if (detail_item_needs_lrkey(target)) { return; }
            break;
        }
        case LV_KEY_RIGHT: {
            lv_obj_t *target = lv_event_get_target(e);
            if (detail_item_needs_lrkey(target)) { return; }
            if (controller->active_dropdown) { return; }
            if (lv_obj_has_class(target, &lv_dropdown_class)) {
                lv_dropdown_close(target);
                controller->active_dropdown = NULL;
            }
            break;
        }
    }
}

static bool detail_item_needs_lrkey(lv_obj_t *obj) {
    if (lv_obj_has_class(obj, &lv_slider_class)) {
        return true;
    } else {
        return false;
    }
}

static void detail_defocus(settings_controller_t *controller, lv_event_t *e) {
    (void) e;
    lv_obj_t *detail_focused = lv_group_get_focused(controller->detail_group);
    if (detail_focused) {
        lv_event_send(detail_focused, LV_EVENT_DEFOCUSED, lv_indev_get_act());
    }
    app_input_set_group(&controller->app->ui.input, controller->nav_group);
    lv_obj_t *nav_focused = lv_group_get_focused(controller->nav_group);
    if (nav_focused) {
        lv_obj_add_state(nav_focused, LV_STATE_FOCUS_KEY);
    }
}

static void on_dropdown_clicked(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_obj_has_state(target, LV_STATE_CHECKED)) {
        controller->active_dropdown = target;
    } else {
        controller->active_dropdown = NULL;
    }
}

static void settings_apply_locale_if_needed(settings_controller_t *controller) {
#ifdef FEATURE_I18N_LANGUAGE_SETTINGS
    if (!controller->needs_locale_reapply || app_configuration->language == NULL || app_configuration->language[0] == '\0' ||
        strcmp(app_configuration->language, "auto") == 0) {
        return;
    }
    i18n_setlocale(app_configuration->language);
#endif
}

static void settings_finish_close(settings_controller_t *fragment) {
    settings_close_pane_popup(fragment);
    settings_launcher_detach(fragment);
    lv_fragment_del((lv_fragment_t *) fragment);
}

static bool settings_try_close(settings_controller_t *fragment) {
    if (fragment->needs_stream_reconnect && fragment->app->session != NULL && session_is_streaming(fragment->app->session)) {
        static const char *btn_txts[] = {translatable("Later"), translatable("Reconnect streaming"), ""};
        lv_obj_t *msgbox =
                lv_msgbox_create_i18n(NULL, NULL,
                                      locstr("Settings apply on the next streaming session. Reconnect now to use them "
                                             "right away?"),
                                      btn_txts, false);
        lv_obj_center(msgbox);
        lv_obj_add_event_cb(msgbox, stream_reconnect_confirm_cb, LV_EVENT_VALUE_CHANGED, fragment);
        return true;
    }
    settings_finish_close(fragment);
    return false;
}

static void settings_close(lv_event_t *e) {
    (void) settings_try_close(lv_event_get_user_data(e));
}

static void stream_reconnect_confirm_cb(lv_event_t *e) {
    settings_controller_t *fragment = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    uint16_t selected = lv_msgbox_get_active_btn(msgbox);
    if (selected == 1 && fragment->app->session != NULL) {
        session_interrupt(fragment->app->session, false, STREAMING_INTERRUPT_USER);
    }
    lv_msgbox_close_async(msgbox);
    settings_finish_close(fragment);
}

static void embed_cancel_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    if (c->pane_popup_group != NULL) {
        settings_close_pane_popup(c);
        return;
    }
    (void) settings_try_close(c);
}

static void pane_child_added(lv_event_t *e) {
    settings_controller_t *controller = lv_event_get_user_data(e);
    lv_obj_t *child = lv_event_get_param(e);
    if (!child || !lv_obj_is_group_def(child)) { return; }
    lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(child, on_detail_key, LV_EVENT_KEY, controller);
    if (lv_obj_has_class(child, &lv_dropdown_class)) {
        lv_obj_add_event_cb(child, on_dropdown_clicked, LV_EVENT_CLICKED, controller);
    }
}

/* ------------------------------------------------------------------------- */
/* Launcher-embedded settings: second AppBar + pane popups (below main top bar) */
/* ------------------------------------------------------------------------- */

static void settings_embed_refocus_appbar(settings_controller_t *c) {
    if (!c->embed_appbar || !c->nav_group) {
        return;
    }
    uint32_t n = lv_obj_get_child_cnt(c->embed_appbar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(c->embed_appbar, i);
        if (lv_obj_check_type(ch, &lv_btn_class)) {
            lv_group_focus_obj(ch);
            if (app_ui_get_input_mode(&c->app->ui.input) & UI_INPUT_MODE_BUTTON_FLAG) {
                lv_obj_add_state(ch, LV_STATE_FOCUS_KEY);
            }
            break;
        }
    }
}

static void settings_launcher_detach(settings_controller_t *fragment) {
    if (fragment->launcher_host) {
        fragment->launcher_host->settings_fragment = NULL;
        lv_obj_add_flag(fragment->launcher_host->settings_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void embed_pane_mbox_delete_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    if (c->pane_popup_group != NULL) {
        app_input_remove_modal_group(&c->app->ui.input, c->pane_popup_group);
        lv_group_del(c->pane_popup_group);
        c->pane_popup_group = NULL;
    }
    if (c->pane_fragment != NULL) {
        lv_fragment_del(c->pane_fragment);
        c->pane_fragment = NULL;
    }
    c->pane_mbox = NULL;
    if (c->launcher_host) {
        settings_embed_refocus_appbar(c);
    }
}

static void settings_close_pane_popup(settings_controller_t *c) {
    if (!c->pane_mbox) {
        return;
    }
    lv_obj_t *mbox = c->pane_mbox;
    c->pane_mbox = NULL;

    if (c->pane_popup_group != NULL) {
        app_input_remove_modal_group(&c->app->ui.input, c->pane_popup_group);
        lv_group_del(c->pane_popup_group);
        c->pane_popup_group = NULL;
    }

    if (c->pane_fragment) {
        lv_fragment_del(c->pane_fragment);
        c->pane_fragment = NULL;
    }
    /* Synchronous close: async would run DELETE after settings_finish_close may have freed
     * this controller, leaving embed_pane_mbox_delete_cb with a dangling user_data. */
    lv_msgbox_close(mbox);

    if (c->launcher_host) {
        settings_embed_refocus_appbar(c);
    }
}

static void embed_popup_add_objs_recursive(lv_obj_t *parent, lv_group_t *g) {
    uint32_t n = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        if (lv_obj_is_group_def(ch)) {
            lv_group_add_obj(g, ch);
        }
        embed_popup_add_objs_recursive(ch, g);
    }
}

static lv_obj_t *embed_popup_first_focusable(lv_obj_t *parent) {
    uint32_t n = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        if (lv_obj_is_group_def(ch)) {
            return ch;
        }
        lv_obj_t *inner = embed_popup_first_focusable(ch);
        if (inner) {
            return inner;
        }
    }
    return NULL;
}

static void embed_popup_attach_key_handlers(lv_obj_t *parent, settings_controller_t *c) {
    uint32_t n = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        embed_popup_attach_key_handlers(ch, c);
        if (lv_obj_is_group_def(ch)) {
            lv_obj_add_flag(ch, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_event_cb(ch, on_detail_key, LV_EVENT_KEY, c);
        }
    }
}

static void embed_style_msgbox_close_red(lv_obj_t *mbox) {
    lv_obj_t *xb = lv_msgbox_get_close_btn(mbox);
    if (xb == NULL) {
        return;
    }
    lv_obj_t *lb = lv_btn_find_label(xb);
    if (lb == NULL) {
        return;
    }
    lv_label_set_text_static(lb, MAT_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lb, lv_theme_moonlight_get_iconfont_normal(mbox), 0);
    lv_obj_set_style_text_color(lb, lv_palette_main(LV_PALETTE_RED), 0);
}

static void settings_show_pane_popup(settings_controller_t *c, const lv_fragment_class_t *cls) {
    settings_close_pane_popup(c);

    const char *title = locstr("Settings");
    for (int i = 0; i < entries_len; i++) {
        if (entries[i].cls == cls) {
            title = locstr(entries[i].name);
            break;
        }
    }

    lv_obj_t *mbox = lv_msgbox_create(NULL, title, NULL, NULL, true);
    embed_style_msgbox_close_red(mbox);
    lv_disp_t *disp = lv_obj_get_disp(mbox);
    const lv_coord_t hor = lv_disp_get_hor_res(disp);
    const lv_coord_t ver = lv_disp_get_ver_res(disp);
    lv_obj_set_size(mbox, hor * 92 / 100, ver * 92 / 100);

    lv_obj_t *content = lv_msgbox_get_content(mbox);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_style_max_height(content, LV_PCT(90), 0);
    lv_obj_set_style_pad_all(content, lv_dpx(12), 0);

    c->pane_mbox = mbox;
    c->pane_popup_group = lv_group_create();
    lv_group_set_wrap(c->pane_popup_group, true);

    lv_fragment_t *pane = lv_fragment_create(cls, c);
    lv_fragment_create_obj(pane, content);
    c->pane_fragment = pane;

    embed_popup_attach_key_handlers(content, c);
    embed_popup_add_objs_recursive(content, c->pane_popup_group);

    lv_obj_t *close_btn = lv_msgbox_get_close_btn(mbox);
    if (close_btn != NULL) {
        lv_group_add_obj(c->pane_popup_group, close_btn);
        lv_obj_add_flag(close_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(close_btn, on_detail_key, LV_EVENT_KEY, c);
    }

    app_input_push_modal_group(&c->app->ui.input, c->pane_popup_group);
    lv_obj_t *first = embed_popup_first_focusable(content);
    if (first) {
        lv_group_focus_obj(first);
        if (app_ui_get_input_mode(&c->app->ui.input) & UI_INPUT_MODE_BUTTON_FLAG) {
            lv_obj_add_state(first, LV_STATE_FOCUS_KEY);
        }
    }

    lv_obj_add_event_cb(mbox, embed_pane_mbox_delete_cb, LV_EVENT_DELETE, c);
    lv_obj_center(mbox);
}

static void embed_entry_btn_cb(lv_event_t *e) {
    settings_controller_t *controller = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    const lv_fragment_class_t *cls = lv_obj_get_user_data(btn);
    if (cls) {
        settings_show_pane_popup(controller, cls);
    }
}

static void embed_fechar_btn_cb(lv_event_t *e) {
    settings_close(e);
}

static void embed_backdrop_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    settings_close_pane_popup(c);
}

static void embed_appbar_key(lv_event_t *e) {
    settings_controller_t *controller = lv_event_get_user_data(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            lv_group_focus_prev(controller->nav_group);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            lv_group_focus_next(controller->nav_group);
            break;
        default:
            break;
    }
}

static void on_launcher_embedded_view_created(settings_controller_t *controller) {
    controller->nav_group = lv_group_create();
    lv_group_set_wrap(controller->nav_group, true);
    lv_group_set_editing(controller->nav_group, false);
    lv_obj_add_event_cb(controller->embed_appbar, embed_appbar_key, LV_EVENT_KEY, controller);

    uint32_t n = lv_obj_get_child_cnt(controller->embed_appbar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(controller->embed_appbar, i);
        if (lv_obj_check_type(ch, &lv_btn_class)) {
            lv_group_add_obj(controller->nav_group, ch);
        }
    }

    app_input_set_group(&controller->app->ui.input, controller->nav_group);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(controller->embed_appbar, i);
        if (lv_obj_check_type(ch, &lv_btn_class)) {
            lv_group_focus_obj(ch);
            lv_obj_add_state(ch, LV_STATE_FOCUS_KEY);
            break;
        }
    }
}

lv_obj_t *settings_launcher_embedded_create(lv_fragment_t *self, lv_obj_t *parent) {
    settings_controller_t *c = (settings_controller_t *) self;

    lv_obj_t *root = lv_obj_create(parent);
    c->embed_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_70, 0);

    lv_obj_t *bar = lv_obj_create(root);
    c->embed_appbar = bar;
    c->nav = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_DPX(44));
    lv_obj_set_layout(bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(bar, LV_DPX(4), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_darken(lv_color_hex(0x2f3237), 4), 0);
    lv_obj_set_scroll_dir(bar, LV_DIR_HOR);
    lv_obj_set_style_pad_ver(bar, LV_DPX(4), 0);

    for (int i = 0; i < entries_len; i++) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_size(btn, LV_DPX(36), LV_DPX(36));
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text_static(lab, entries[i].icon);
        lv_obj_set_style_text_font(lab, lv_theme_moonlight_get_iconfont_small(bar), 0);
        lv_obj_set_style_text_color(lab, lv_color_white(), 0);
        lv_obj_center(lab);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void *) entries[i].cls);
        lv_obj_add_event_cb(btn, embed_entry_btn_cb, LV_EVENT_CLICKED, c);
        lv_obj_add_event_cb(btn, embed_cancel_cb, LV_EVENT_CANCEL, c);
    }

    lv_obj_t *sp = lv_obj_create(bar);
    lv_obj_remove_style_all(sp);
    lv_obj_set_height(sp, LV_DPX(4));
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t *close_btn = lv_btn_create(bar);
    c->close_btn = close_btn;
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(close_btn, LV_DPX(36), LV_DPX(36));
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_t *clab = lv_label_create(close_btn);
    lv_label_set_text_static(clab, MAT_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(clab, lv_theme_moonlight_get_iconfont_small(bar), 0);
    lv_obj_set_style_text_color(clab, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_center(clab);
    lv_obj_clear_flag(clab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, embed_fechar_btn_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(close_btn, embed_cancel_cb, LV_EVENT_CANCEL, c);

    lv_obj_t *backdrop = lv_obj_create(root);
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_width(backdrop, LV_PCT(100));
    lv_obj_set_flex_grow(backdrop, 1);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, embed_backdrop_cb, LV_EVENT_CLICKED, c);

    return root;
}
