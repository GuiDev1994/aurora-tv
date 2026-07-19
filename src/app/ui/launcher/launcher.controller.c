#include "app.h"
#include "app_launch.h"

#include "stream/session.h"

#include "add.dialog.h"
#include "apps.controller.h"
#include "launcher.controller.h"
#include "pair.dialog.h"
#include "server.context_menu.h"
#include "server.popup.h"

#include "ui/help/help.dialog.h"
#include "ui/root.h"
#include "ui/settings/settings.controller.h"

#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/util/lv_app_utils.h"
#include "lv_gridview.h"

#include "util/font.h"
#include "util/i18n.h"
#include "util/user_event.h"
#include "logging.h"

static void launcher_controller(lv_fragment_t *self, void *args);

static void controller_dtor(lv_fragment_t *self);

static void launcher_view_init(lv_fragment_t *self, lv_obj_t *view);

static void launcher_view_destroy(lv_fragment_t *self, lv_obj_t *view);

static bool launcher_event_cb(lv_fragment_t *self, int code, void *userdata);

static void on_pc_added(const uuidstr_t *uuid, void *userdata);

static void on_pc_updated(const uuidstr_t *uuid, void *userdata);

static void on_pc_removed(const uuidstr_t *uuid, void *userdata);

static void cb_topbar_focused(lv_event_t *event);

static void cb_topbar_key(lv_event_t *event);

static void cb_detail_focused(lv_event_t *event);

static void cb_detail_cancel(lv_event_t *event);

static void cb_detail_key(lv_event_t *event);

static void launcher_profile_dropdown_clicked(lv_event_t *event);

static void launcher_profile_dropdown_key_preprocess(lv_event_t *event);

static void launcher_profile_dropdown_key(lv_event_t *event);

static void launcher_profile_dropdown_esc_preprocess(lv_event_t *event);

static bool launcher_close_profile_dropdown(launcher_fragment_t *fragment, lv_obj_t *target);

static void cb_server_btn_clicked(lv_event_t *event);

static void open_manual_add(lv_event_t *event);

static void open_settings(lv_event_t *event);

static void open_help(lv_event_t *event);

static void select_pc(launcher_fragment_t *controller, const uuidstr_t *uuid);

static void launcher_try_auto_resume(launcher_fragment_t *controller, const uuidstr_t *uuid);

static void launcher_handle_app_foreground(launcher_fragment_t *controller);

/* Switch the active focus group between top-bar and detail (game rail).
 * Replaces the old "set_detail_opened" semantics now that both areas are visible
 * simultaneously - we just route input to whichever group the user is in. */
static void focus_topbar(launcher_fragment_t *controller);

static void focus_detail(launcher_fragment_t *controller);

static void launcher_async_try_focus_detail(void *userdata);

static void launcher_clear_nav_key_focus(launcher_fragment_t *c);

static void launcher_clear_detail_key_focus(launcher_fragment_t *c);

static void show_decoder_error();

static void show_conf_persistent_error();

static void decoder_error_cb(lv_event_t *e);

static void populate_selected_host(launcher_fragment_t *controller);

const lv_fragment_class_t launcher_controller_class = {
        .constructor_cb = launcher_controller,
        .destructor_cb = controller_dtor,
        .create_obj_cb = launcher_win_create,
        .obj_created_cb = launcher_view_init,
        .obj_will_delete_cb = launcher_view_destroy,
        .event_cb = launcher_event_cb,
        .instance_size = sizeof(launcher_fragment_t),
};

static const pcmanager_listener_t pcmanager_callbacks = {
        .added = on_pc_added,
        .updated = on_pc_updated,
        .removed = on_pc_removed,
};

static launcher_fragment_t *current_instance = NULL;

launcher_fragment_t *launcher_instance() {
    return current_instance;
}

void launcher_select_server(launcher_fragment_t *controller, const uuidstr_t *uuid) {
    const pclist_t *node = uuid ? pcmanager_node(pcmanager, uuid) : NULL;
    if (!node) {
        select_pc(controller, NULL);
        launcher_refresh_server_label(controller);
        return;
    }
    if (node->state.code == SERVER_STATE_NOT_PAIRED) {
        pair_dialog_open(uuid);
        return;
    }
    if (!node->selected) {
        select_pc(controller, uuid);
    }
    launcher_refresh_server_label(controller);
    /* After picking a server, move focus into the game rail. */
    focus_detail(controller);
}

void launcher_restore_nav_focus(launcher_fragment_t *controller) {
    if (!controller) {
        return;
    }
    focus_topbar(controller);
}

void launcher_refresh_server_label(launcher_fragment_t *controller) {
    if (!controller || !controller->server_label) { return; }
    const pclist_t *selected = NULL;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->selected) {
            selected = cur;
            break;
        }
    }
    if (selected != NULL && selected->server != NULL && selected->server->hostname != NULL) {
        lv_label_set_text(controller->server_label, selected->server->hostname);
    } else {
        lv_label_set_text_static(controller->server_label, locstr("Select server"));
    }
}

static void launcher_controller(lv_fragment_t *self, void *args) {
    launcher_fragment_t *fragment = (launcher_fragment_t *) self;
    launcher_fragment_args_t *fargs = args;
    fragment->global = fargs->app;
    fragment->pane_initialized = false;
    fragment->first_created = true;
    fragment->launch_params = fargs->params;
    fragment->settings_fragment = NULL;
    fragment->active_dropdown = NULL;
}

static void controller_dtor(lv_fragment_t *self) {
    launcher_fragment_t *controller = (launcher_fragment_t *) self;
    lv_style_reset(&controller->topbar_btn_style);
}

static void launcher_view_init(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    launcher_fragment_t *fragment = (launcher_fragment_t *) self;
    pcmanager_register_listener(pcmanager, &pcmanager_callbacks, fragment);

    /* Top-bar input wiring: focus enters → switch to nav_group; KEY handled for
     * LEFT/RIGHT (move between buttons) and UP (enter game rail above the bar);
     * CANCEL on the bar surfaces the standard quit confirmation. */
    lv_obj_add_event_cb(fragment->nav, cb_topbar_focused, LV_EVENT_FOCUSED, fragment);
    lv_obj_add_event_cb(fragment->nav, cb_topbar_key, LV_EVENT_KEY, fragment);
    lv_obj_add_event_cb(fragment->nav, app_quit_confirm, LV_EVENT_CANCEL, fragment);

    lv_obj_add_event_cb(fragment->detail, cb_detail_focused, LV_EVENT_FOCUSED, fragment);
    lv_obj_add_event_cb(fragment->detail, cb_detail_cancel, LV_EVENT_CANCEL, fragment);
    lv_obj_add_event_cb(fragment->detail, cb_detail_key, LV_EVENT_KEY, fragment);

    /* Top-bar action buttons. Existing handlers are reused untouched. */
    lv_obj_add_event_cb(fragment->server_btn, cb_server_btn_clicked, LV_EVENT_CLICKED, fragment);
    lv_obj_add_event_cb(fragment->add_btn, open_manual_add, LV_EVENT_CLICKED, fragment);
    lv_obj_add_event_cb(fragment->pref_btn, open_settings, LV_EVENT_CLICKED, fragment);
    lv_obj_add_event_cb(fragment->help_btn, open_help, LV_EVENT_CLICKED, fragment);
    lv_obj_add_event_cb(fragment->quit_btn, app_quit_confirm, LV_EVENT_CLICKED, fragment);

    populate_selected_host(fragment);

    /* Auto-load the previously selected PC (if any) and request status refresh
     * for the others; same behavior as before. */
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->selected) {
            select_pc(fragment, &cur->id);
            continue;
        }
        pcmanager_request_update(pcmanager, &cur->id, NULL, NULL);
    }
    launcher_refresh_server_label(fragment);
    fragment->pane_initialized = true;
    pcmanager_auto_discovery_start(pcmanager);

    /* Defer initial focus so apps/detail_group are populated when a host is already selected. */
    current_instance = fragment;
    lv_async_call(launcher_async_try_focus_detail, fragment);

    if (fragment->first_created) {
        if (!app_decoder_or_embedded_present(fragment->global)) {
            show_decoder_error();
        }
        if (!app_configuration->conf_persistent) {
            show_conf_persistent_error();
        }
    }
    fragment->first_created = false;
}

static void launcher_view_destroy(lv_fragment_t *self, lv_obj_t *view) {
    launcher_fragment_t *controller = (launcher_fragment_t *) self;
    LV_UNUSED(view);
    current_instance = NULL;
    if (controller->settings_fragment) {
        lv_fragment_del(controller->settings_fragment);
        controller->settings_fragment = NULL;
    }
    app_input_set_group(&controller->global->ui.input, NULL);
    pcmanager_auto_discovery_stop(pcmanager);

    controller->pane_initialized = false;
    controller->launch_params = NULL;

    lv_group_del(controller->nav_group);
    lv_group_del(controller->detail_group);

    pcmanager_unregister_listener(pcmanager, &pcmanager_callbacks);
}

static bool launcher_event_cb(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    launcher_fragment_t *fragment = (launcher_fragment_t *) self;
    if (code == USER_SIZE_CHANGED) {
        lv_obj_set_size(self->obj, fragment->global->ui.width, fragment->global->ui.height);
    } else if (code == USER_APP_FOREGROUND) {
        launcher_handle_app_foreground(fragment);
    }
    return false;
}

void on_pc_added(const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(uuid);
    launcher_fragment_t *controller = userdata;
    populate_selected_host(controller);
    launcher_refresh_server_label(controller);
}

void on_pc_updated(const uuidstr_t *uuid, void *userdata) {
    launcher_fragment_t *controller = userdata;
    /* Hostname can change after a successful query; keep the top-bar label in sync. */
    launcher_refresh_server_label(controller);
    launcher_try_auto_resume(controller, uuid);
}

void on_pc_removed(const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(uuid);
    launcher_fragment_t *controller = userdata;
    launcher_refresh_server_label(controller);
}

static void cb_server_btn_clicked(lv_event_t *event) {
    launcher_fragment_t *controller = lv_event_get_user_data(event);
    server_popup_open(controller);
}

static void select_pc(launcher_fragment_t *controller, const uuidstr_t *uuid) {
    if (uuid) {
        apps_fragment_arg_t arg = {.global = controller->global, .host = *uuid};
        const app_launch_params_t *params = controller->launch_params;
        if (!controller->def_app_requested && params != NULL && uuidstr_t_equals_t(uuid, &params->default_host_uuid)) {
            controller->def_app_requested = true;
            arg.def_app = params->default_app_id;
        }
        if (controller->pending_def_app != 0) {
            arg.def_app = controller->pending_def_app;
            controller->pending_def_app = 0;
        }
        lv_fragment_t *fragment = lv_fragment_create(&apps_controller_class, &arg);
        lv_fragment_manager_replace(controller->base.child_manager, fragment, &controller->detail);
        pcmanager_select(pcmanager, uuid);
    } else {
        lv_fragment_manager_pop(controller->base.child_manager);
    }
}

static void launcher_auto_resume_async(void *userdata) {
    launcher_fragment_t *controller = userdata;
    // The launcher may have been destroyed between scheduling and firing.
    if (current_instance != controller) { return; }
    const uuidstr_t *uuid = &controller->auto_resume_uuid;
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    if (node == NULL || node->state.code != SERVER_STATE_AVAILABLE) { return; }
    int current = pcmanager_node_current_app(node);
    if (current == 0) { return; }
    commons_log_info("UI", "Auto-resuming running app %d on host %s", current, node->server->hostname);
    // Inject the running app as def_app; apps.controller's def_app auto-launch
    // path resumes it (gs_start_app resumes when appId == currentGame).
    controller->pending_def_app = current;
    select_pc(controller, uuid);
}

static void launcher_try_auto_resume(launcher_fragment_t *controller, const uuidstr_t *uuid) {
    if (!app_configuration->autoresume) { return; }
    // Only fire once per app start; let an explicit CLI/deep-link launch take priority.
    if (controller->auto_resume_done || controller->def_app_requested) { return; }
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    if (node == NULL || node->state.code != SERVER_STATE_AVAILABLE) { return; }
    if (pcmanager_node_current_app(node) == 0) { return; }
    // Set the guard immediately so repeated host updates (incl. the refresh after a
    // stream ends, when currentGame is still set) can neither re-trigger nor
    // double-schedule the resume.
    controller->auto_resume_done = true;
    controller->auto_resume_uuid = *uuid;
    // Defer the actual host select: select_pc() replaces the apps fragment, whose
    // teardown unregisters a pcmanager listener — unsafe while iterating listeners
    // inside the notify() that called us.
    lv_async_call(launcher_auto_resume_async, controller);
}

// Handle a background->foreground transition (USER_APP_FOREGROUND). The cold-start
// auto-resume is one-shot (auto_resume_done), so without this the resume never fires
// when Aurora stays alive in the background and the user switches back to it on the TV.
static void launcher_handle_app_foreground(launcher_fragment_t *controller) {
    if (!app_configuration->autoresume) { return; }
    // If a stream is already live (the foreground event raced ahead of teardown, or we
    // never actually left the stream), there is nothing to resume — and re-arming the
    // guard here would let a periodic host update spuriously relaunch over the session.
    if (session_is_streaming(controller->global->session)) { return; }
    // Re-arm the one-shot guard so this foreground transition can auto-resume again.
    // This is safe w.r.t. the manual-quit footgun: quitting a stream is in-app
    // navigation and never emits USER_APP_FOREGROUND, so the guard is only re-armed on a
    // genuine background->foreground switch (e.g. webOS Home -> back to Aurora).
    controller->auto_resume_done = false;
    // Force a fresh query of the selected host; the resulting on_pc_updated re-attempts
    // auto-resume (via launcher_try_auto_resume) with up-to-date currentGame info.
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->selected) {
            pcmanager_request_update(pcmanager, &cur->id, NULL, NULL);
            break;
        }
    }
}

static void cb_detail_focused(lv_event_t *event) {
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    if (!fragment->pane_initialized || fragment->detail_changing) { return; }
    lv_fragment_t *detail_fragment = lv_fragment_manager_find_by_container(fragment->base.child_manager,
                                                                           fragment->detail);
    if (!detail_fragment || lv_obj_get_parent(event->target) != detail_fragment->obj) { return; }
    focus_detail(fragment);
}

static void cb_detail_cancel(lv_event_t *event) {
    launcher_fragment_t *controller = lv_event_get_user_data(event);
    /* CANCEL inside the rail moves focus to the bottom bar. */
    focus_topbar(controller);
}

static void cb_detail_key(lv_event_t *event) {
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    if (lv_event_get_key(event) == LV_KEY_UP) {
        focus_topbar(fragment);
    }
}

static bool launcher_close_profile_dropdown(launcher_fragment_t *fragment, lv_obj_t *target) {
    lv_obj_t *dropdown = fragment->profile_dropdown;
    if (dropdown == NULL) {
        return false;
    }
    if (fragment->active_dropdown != dropdown && target != dropdown) {
        return false;
    }
    if (!lv_dropdown_is_open(dropdown) && fragment->active_dropdown == NULL) {
        return false;
    }
    fragment->active_dropdown = NULL;
    if (lv_dropdown_is_open(dropdown)) {
        lv_dropdown_close(dropdown);
    }
    lv_group_focus_obj(dropdown);
    return true;
}

static void launcher_profile_dropdown_clicked(lv_event_t *event) {
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_obj_has_state(target, LV_STATE_CHECKED)) {
        fragment->active_dropdown = target;
    } else {
        fragment->active_dropdown = NULL;
    }
}

static void launcher_profile_dropdown_esc_preprocess(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_KEY || lv_event_get_key(event) != LV_KEY_ESC) {
        return;
    }
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    if (launcher_close_profile_dropdown(fragment, lv_event_get_target(event))) {
        lv_event_stop_processing(event);
    }
}

static void launcher_profile_dropdown_key_preprocess(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    if (target != fragment->profile_dropdown || fragment->active_dropdown) {
        return;
    }
    switch (lv_event_get_key(event)) {
        case LV_KEY_UP:
            lv_event_stop_processing(event);
            return;
        case LV_KEY_DOWN: {
            focus_detail(fragment);
            lv_event_stop_processing(event);
            return;
        }
        case LV_KEY_LEFT:
            lv_group_focus_prev(fragment->nav_group);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_RIGHT:
            lv_group_focus_next(fragment->nav_group);
            lv_event_stop_processing(event);
            return;
        default:
            return;
    }
}

static void launcher_profile_dropdown_key(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    if (!fragment->active_dropdown) {
        return;
    }
    switch (lv_event_get_key(event)) {
        case LV_KEY_UP:
        case LV_KEY_DOWN:
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            lv_event_stop_bubbling(event);
            return;
        default:
            return;
    }
}

void launcher_attach_profile_dropdown_nav(launcher_fragment_t *fragment) {
    lv_obj_t *dropdown = fragment->profile_dropdown;
    if (dropdown == NULL) {
        return;
    }
    lv_obj_add_event_cb(dropdown, launcher_profile_dropdown_clicked, LV_EVENT_CLICKED, fragment);
    lv_obj_add_event_cb(dropdown, launcher_profile_dropdown_key_preprocess,
                        LV_EVENT_KEY | LV_EVENT_PREPROCESS, fragment);
    lv_obj_add_event_cb(dropdown, launcher_profile_dropdown_key, LV_EVENT_KEY, fragment);
    lv_obj_add_event_cb(dropdown, launcher_profile_dropdown_esc_preprocess,
                        LV_EVENT_KEY | LV_EVENT_PREPROCESS, fragment);
}

static void cb_topbar_focused(lv_event_t *event) {
    launcher_fragment_t *controller = lv_event_get_user_data(event);
    if (!controller->pane_initialized) { return; }
    focus_topbar(controller);
}

static void cb_topbar_key(lv_event_t *event) {
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    if (fragment->active_dropdown) {
        return;
    }
    switch (lv_event_get_key(event)) {
        case LV_KEY_LEFT: {
            lv_group_focus_prev(fragment->nav_group);
            break;
        }
        case LV_KEY_RIGHT: {
            lv_group_focus_next(fragment->nav_group);
            break;
        }
        case LV_KEY_UP: {
            lv_fragment_t *detail_fragment = lv_fragment_manager_find_by_container(fragment->base.child_manager,
                                                                                   fragment->detail);
            if (detail_fragment) {
                focus_detail(fragment);
            }
            break;
        }
        case LV_KEY_DOWN: {
            focus_detail(fragment);
            break;
        }
    }
}

static void launcher_clear_nav_key_focus(launcher_fragment_t *c) {
    if (!c->nav) {
        return;
    }
    uint32_t n = lv_obj_get_child_cnt(c->nav);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_clear_state(lv_obj_get_child(c->nav, i), LV_STATE_FOCUS_KEY);
    }
}

static void launcher_clear_detail_key_focus(launcher_fragment_t *c) {
    lv_obj_t *f = lv_group_get_focused(c->detail_group);
    if (f) {
        lv_obj_clear_state(f, LV_STATE_FOCUS_KEY);
    }
}

static void focus_topbar(launcher_fragment_t *controller) {
    launcher_clear_detail_key_focus(controller);
    bool key = app_ui_get_input_mode(&controller->global->ui.input) & UI_INPUT_MODE_BUTTON_FLAG;
    app_input_set_group(&controller->global->ui.input, controller->nav_group);
    if (key) {
        lv_obj_t *cur = lv_group_get_focused(controller->nav_group);
        if (!cur) {
            lv_group_focus_obj(controller->server_btn);
            cur = controller->server_btn;
        }
        if (cur) {
            lv_obj_add_state(cur, LV_STATE_FOCUS_KEY);
        }
    }
}

static void focus_detail(launcher_fragment_t *controller) {
    launcher_clear_nav_key_focus(controller);
    bool key = app_ui_get_input_mode(&controller->global->ui.input) & UI_INPUT_MODE_BUTTON_FLAG;
    app_input_set_group(&controller->global->ui.input, controller->detail_group);
    if (key) {
        lv_obj_t *cur = lv_group_get_focused(controller->detail_group);
        if (!cur) {
            lv_group_focus_next(controller->detail_group);
            cur = lv_group_get_focused(controller->detail_group);
        }
        if (cur) {
            if (lv_obj_check_type(cur, &lv_gridview_class)) {
                int index = lv_gridview_get_focused_index(cur);
                if (index < 0) {
                    index = 0;
                }
                lv_gridview_focus(cur, index);
                /* lv_gridview_focus() sends its FOCUSED event via
                 * lv_indev_get_act(), which is NULL here (this is a
                 * programmatic focus-route, not a real keypress) -- LVGL's
                 * own event handler only applies LV_STATE_FOCUS_KEY for a
                 * genuine keypad/encoder indev, so the tile ends up
                 * "focused" internally (grid->focused_index is correct) but
                 * visually shows no selection outline at all. Force the
                 * state directly, same as the non-gridview branch below
                 * already does. */
                lv_obj_t *focused_item = lv_gridview_get_item_view(cur, index);
                if (focused_item) {
                    lv_obj_add_state(focused_item, LV_STATE_FOCUS_KEY);
                }
            } else {
                lv_obj_add_state(cur, LV_STATE_FOCUS_KEY);
            }
        }
    }
}

static void launcher_async_try_focus_detail(void *userdata) {
    launcher_fragment_t *fragment = userdata;
    if (!fragment->pane_initialized) {
        return;
    }
    lv_fragment_t *apps =
            lv_fragment_manager_find_by_container(fragment->base.child_manager, fragment->detail);
    if (apps != NULL && lv_group_get_obj_count(fragment->detail_group) > 0) {
        focus_detail(fragment);
    } else {
        focus_topbar(fragment);
    }
}

static void open_manual_add(lv_event_t *event) {
    LV_UNUSED(event);
    lv_fragment_t *fragment = lv_fragment_create(&add_dialog_class, NULL);
    lv_obj_t *msgbox = lv_fragment_create_obj(fragment, NULL);
    lv_obj_add_event_cb(msgbox, ui_cb_destroy_fragment, LV_EVENT_DELETE, fragment);
}

static void open_settings(lv_event_t *event) {
    launcher_fragment_t *self = lv_event_get_user_data(event);
    if (self->settings_fragment) {
        return;
    }
    settings_open_args_t sargs = {.app = self->global, .launcher = self};
    lv_fragment_t *fragment = lv_fragment_create(&settings_controller_cls, &sargs);
    lv_fragment_create_obj(fragment, self->settings_layer);
    self->settings_fragment = fragment;
    lv_obj_clear_flag(self->settings_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(self->settings_layer);
}

static void show_decoder_error() {
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("No working decoder"), "placeholder", btn_txts, false);
    lv_obj_add_event_cb(msgbox, decoder_error_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *msgview = lv_msgbox_get_text(msgbox);
#if FEATURE_EMBEDDED_SHELL
    lv_label_set_text_static(msgview, locstr("Streaming can't work without a valid decoder.\n"
                                             "(If your device supports moonlight-embedded, install it "
                                             "and Aurora will use it automatically.)"));
#else
    lv_label_set_text_static(msgview, locstr("Streaming can't work without a valid decoder."));
#endif
    lv_obj_center(msgbox);
}

static void show_conf_persistent_error() {
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Can't save settings"), "placeholder", btn_txts, false);
    lv_obj_add_event_cb(msgbox, decoder_error_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *msgview = lv_msgbox_get_text(msgbox);
    lv_label_set_text_static(msgview, locstr("Can't find a writable directory to save settings. Settings and pairing "
                                             "information will be lost when the TV is turned off.\n\n"
                                             "(If you're using webOS 3.0 or newer, restart the TV may fix this issue.)"));
    lv_obj_center(msgbox);
}

static void decoder_error_cb(lv_event_t *e) {
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    lv_msgbox_close_async(msgbox);
}

static void open_help(lv_event_t *event) {
    LV_UNUSED(event);
    help_dialog_create();
}

static void populate_selected_host(launcher_fragment_t *controller) {
    /* Easy and dirty way to select preferred host. */
    if (controller->def_host_selected || controller->launch_params == NULL ||
        uuidstr_is_empty(&controller->launch_params->default_host_uuid)) {
        return;
    }
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (uuidstr_t_equals_t(&cur->id, &controller->launch_params->default_host_uuid)) {
            commons_log_info("UI", "Host %s was selected", cur->server->hostname);
            pcmanager_select(pcmanager, &cur->id);
            controller->def_host_selected = true;
            break;
        }
    }
}
