#include "app.h"

#include "launcher.controller.h"
#include "server.context_menu.h"

#include "backend/pcmanager.h"
#include "backend/types.h"
#include "libgamestream/client.h"

#include "errors.h"
#include "ui/common/progress_dialog.h"
#include "util/i18n.h"
#include "lvgl/util/lv_app_utils.h"
#include "util/nullable.h"
#include "util/bus.h"
#include "util/user_event.h"

#include <string.h>

typedef struct context_menu_t {
    lv_fragment_t base;
    uuidstr_t uuid;
    bool single_clicked;
} context_menu_t;

typedef struct bandwidth_test_t {
    GS_BANDWIDTH_PROBE probe;
    lv_obj_t *progress;
    /* The bitrate "Lower bitrate" sets; 0 when the dialog doesn't offer it. */
    int lower_to_kbps;
} bandwidth_test_t;

typedef struct host_power_t {
    pcmanager_host_power_t request;
    uuidstr_t uuid;
    lv_obj_t *progress;
    /* Held by the request in flight and by each dialog showing it; the last one frees it. */
    int refs;
} host_power_t;

static void menu_ctor(lv_fragment_t *self, void *arg);

static void menu_dtor(lv_fragment_t *self);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *parent);

static void context_menu_cancel_cb(lv_event_t *e);

static void context_menu_short_click_cb(lv_event_t *e);

static void context_menu_click_cb(lv_event_t *e);

static void open_info(const pclist_t *node);

static void show_hidden_apps(const pclist_t *node);

static void forget_host(const pclist_t *node);

static void configure_wake(const pclist_t *node);

static void test_bandwidth(const pclist_t *node);

static void bandwidth_test_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void bandwidth_result_cb(lv_event_t *e);

static void bandwidth_result_free_cb(lv_event_t *e);

static void restart_pc(const pclist_t *node);

static void shutdown_pc(const pclist_t *node);

static void host_power_start(const pclist_t *node, GS_POWER_KIND kind);

static void host_power_checked_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void host_power_confirm_cb(lv_event_t *e);

static void host_power_done_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void host_power_message(host_power_t *power, const char *text);

static void host_power_unref_cb(lv_event_t *e);

static void wake_settings_cb(lv_event_t *e);

static void info_action_cb(lv_event_t *e);

const lv_fragment_class_t server_menu_class = {
        .constructor_cb = menu_ctor,
        .destructor_cb = menu_dtor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(context_menu_t)
};

static void menu_ctor(lv_fragment_t *self, void *arg) {
    context_menu_t *controller = (context_menu_t *) self;
    controller->uuid = *(const uuidstr_t *) arg;
}

static void menu_dtor(lv_fragment_t *self) {
    LV_UNUSED(self);
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *parent) {
    LV_UNUSED(parent);
    context_menu_t *controller = (context_menu_t *) self;

    const pclist_t *node = pcmanager_node(pcmanager, &controller->uuid);
    if (!node) {
        lv_obj_t *empty = lv_msgbox_create(NULL, "Unknown", NULL, NULL, false);
        lv_msgbox_close_async(empty);
        return empty;
    }
    lv_obj_t *msgbox = lv_msgbox_create(NULL, node->server->hostname, NULL, NULL, false);
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(content, context_menu_cancel_cb, LV_EVENT_CANCEL, controller);
    lv_obj_add_event_cb(content, context_menu_short_click_cb, LV_EVENT_SHORT_CLICKED, controller);
    lv_obj_add_event_cb(content, context_menu_click_cb, LV_EVENT_CLICKED, controller);

    lv_obj_t *info_btn = lv_list_add_btn(content, NULL, locstr("Info"));
    lv_obj_add_flag(info_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(info_btn, open_info);

    lv_obj_t *show_hidden_btn = lv_list_add_btn(content, NULL, locstr("Show hidden apps"));
    lv_obj_add_flag(show_hidden_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(show_hidden_btn, show_hidden_apps);

    lv_obj_t *wake_btn = lv_list_add_btn(content, NULL, locstr("Wake settings"));
    lv_obj_add_flag(wake_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(wake_btn, configure_wake);

    if (node->state.code == SERVER_STATE_AVAILABLE && node->server->bandwidthProbeVersion > 0) {
        lv_obj_t *bandwidth_btn = lv_list_add_btn(content, NULL, locstr("Test connection speed"));
        lv_obj_add_flag(bandwidth_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(bandwidth_btn, test_bandwidth);
    }

    if (node->state.code == SERVER_STATE_AVAILABLE && node->server->nativeApiVersion > 0) {
        lv_obj_t *restart_btn = lv_list_add_btn(content, NULL, locstr("Restart PC"));
        lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(restart_btn, restart_pc);

        lv_obj_t *shutdown_btn = lv_list_add_btn(content, NULL, locstr("Shut down PC"));
        lv_obj_add_flag(shutdown_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(shutdown_btn, shutdown_pc);
    }

    if (pcmanager_can_forget(pcmanager, &controller->uuid)) {
        lv_obj_t *forget_btn = lv_list_add_btn(content, NULL, locstr("Forget"));
        lv_obj_add_flag(forget_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(forget_btn, forget_host);
    }

    lv_obj_t *cancel_btn = lv_list_add_btn(content, NULL, locstr("Cancel"));
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(msgbox);
    return msgbox;
}

static void context_menu_cancel_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    if (target->parent != lv_event_get_current_target(e)) { return; }
    lv_msgbox_close(lv_event_get_current_target(e)->parent);
}

static void context_menu_short_click_cb(lv_event_t *e) {
    context_menu_t *controller = lv_event_get_user_data(e);
    controller->single_clicked = true;
}

static void context_menu_click_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    context_menu_t *controller = lv_event_get_user_data(e);
    if (!controller->single_clicked) { return; }
    lv_obj_t *current_target = lv_event_get_current_target(e);
    if (target->parent != current_target) { return; }
    void *target_userdata = lv_obj_get_user_data(target);
    lv_obj_t *mbox = lv_event_get_current_target(e)->parent;
    const pclist_t *node = pcmanager_node(pcmanager, &controller->uuid);
    lv_msgbox_close(mbox);
    if (!node) {
        return;
    }
    if (target_userdata == open_info) {
        open_info(node);
    } else if (target_userdata == forget_host) {
        forget_host(node);
    } else if (target_userdata == show_hidden_apps) {
        show_hidden_apps(node);
    } else if (target_userdata == configure_wake) {
        configure_wake(node);
    } else if (target_userdata == test_bandwidth) {
        test_bandwidth(node);
    } else if (target_userdata == restart_pc) {
        restart_pc(node);
    } else if (target_userdata == shutdown_pc) {
        shutdown_pc(node);
    }
}

static void open_info(const pclist_t *node) {
    static const char *btn_txts[] = {translatable("OK"), ""};
    const SERVER_DATA *server = node->server;
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, server->hostname, "placeholder", btn_txts, false);
    lv_obj_t *message = lv_msgbox_get_text(mbox);
    lv_label_set_text_fmt(message, locstr("IP address: %s\nGPU: %s\nSupports 4K: %s\n"
                                          "Supports HDR: %s\nHost Software Version: %s\n"
                                          "GeForce Experience: %s"),
                          server->serverInfo.address,
                          str_null_or_empty(server->gpuType) ? locstr("Unknown") : server->gpuType,
                          server->supports4K ? locstr("YES") : locstr("NO"),
                          server->supportsHdr ? locstr("YES") : locstr("NO"),
                          server->serverInfo.serverInfoAppVersion,
                          server->isGfe ? server->serverInfo.serverInfoGfeVersion : locstr("NO"));
    lv_obj_add_event_cb(mbox, info_action_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

static void show_hidden_apps(const pclist_t *node) {
    uuidstr_t *id = calloc(1, sizeof(uuidstr_t));
    *id = node->id;
    bus_pushevent(USER_SHOW_HIDDEN_APPS, id, NULL);
}

static void forget_host(const pclist_t *node) {
    bool selected = node->selected;
    pcmanager_forget(pcmanager, &node->id);
    if (selected) {
        launcher_fragment_t *launcher = launcher_instance();
        launcher_select_server(launcher, NULL);
    }
}

static void test_bandwidth(const pclist_t *node) {
    bandwidth_test_t *test = SDL_calloc(1, sizeof(bandwidth_test_t));
    test->progress = progress_dialog_create(locstr("Testing connection speed..."));
    if (!pcmanager_probe_bandwidth(pcmanager, &node->id, &test->probe, bandwidth_test_cb, test)) {
        lv_msgbox_close(test->progress);
        SDL_free(test);
    }
}

static void bandwidth_test_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    bandwidth_test_t *test = userdata;
    lv_msgbox_close(test->progress);
    test->progress = NULL;
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    const char *title = node != NULL && node->server != NULL ? node->server->hostname : NULL;
    static const char *ok_btns[] = {translatable("OK"), ""};
    static const char *lower_btns[] = {translatable("Keep"), translatable("Lower bitrate"), ""};

    int current_kbps = app_configuration->stream.bitrate;
    int link_kbps = settings_bitrate_for_link(test->probe.mbps);
    if (result == GS_OK && link_kbps < current_kbps) {
        test->lower_to_kbps = link_kbps;
    }
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, title, "placeholder", test->lower_to_kbps > 0 ? lower_btns : ok_btns,
                                           false);
    lv_obj_t *message = lv_msgbox_get_text(mbox);
    int mbps = (int) (test->probe.mbps + 0.5);
    if (result != GS_OK && test->probe.refused_status == 409) {
        lv_label_set_text(message, locstr("Stop streaming from this PC before testing the connection speed."));
    } else if (result != GS_OK && test->probe.refused_status == 429) {
        int wait_secs = test->probe.retry_after_secs > 0 ? test->probe.retry_after_secs : 5;
        lv_label_set_text_fmt(message, locstr("Test again in %d s."), wait_secs);
    } else if (result != GS_OK) {
        lv_label_set_text_fmt(message, locstr("Connection speed test failed.\n\nDetails: %s"),
                              error != NULL ? error : "");
    } else if (test->lower_to_kbps > 0) {
        lv_label_set_text_fmt(message, locstr("Connection speed: %d Mbps\nBitrate: %d Mbps\n\n"
                                              "This connection may not carry that bitrate. Suggested: %d Mbps"),
                              mbps, current_kbps / 1000, test->lower_to_kbps / 1000);
    } else {
        lv_label_set_text_fmt(message, locstr("Connection speed: %d Mbps\nBitrate: %d Mbps\n\n"
                                              "This connection can carry that bitrate."),
                              mbps, current_kbps / 1000);
    }
    lv_obj_add_event_cb(mbox, bandwidth_result_cb, LV_EVENT_VALUE_CHANGED, test);
    lv_obj_add_event_cb(mbox, bandwidth_result_free_cb, LV_EVENT_DELETE, test);
    lv_obj_center(mbox);
}

static void bandwidth_result_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    bandwidth_test_t *test = lv_event_get_user_data(e);
    if (test->lower_to_kbps > 0 && lv_msgbox_get_active_btn(mbox) == 1) {
        app_configuration->stream.bitrate = test->lower_to_kbps;
        settings_save(app_configuration);
    }
    lv_msgbox_close_async(mbox);
}

static void bandwidth_result_free_cb(lv_event_t *e) {
    SDL_free(lv_event_get_user_data(e));
}

static void restart_pc(const pclist_t *node) {
    host_power_start(node, GS_POWER_RESTART);
}

static void shutdown_pc(const pclist_t *node) {
    host_power_start(node, GS_POWER_SHUTDOWN);
}

static void host_power_start(const pclist_t *node, GS_POWER_KIND kind) {
    host_power_t *power = SDL_calloc(1, sizeof(host_power_t));
    power->uuid = node->id;
    power->refs = 1;
    power->request.kind = kind;
    power->progress = progress_dialog_create(locstr("Checking the PC..."));
    if (!pcmanager_host_power(pcmanager, &node->id, &power->request, host_power_checked_cb, power)) {
        lv_msgbox_close(power->progress);
        SDL_free(power);
    }
}

static const char *host_title(const uuidstr_t *uuid) {
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    return node != NULL && node->server != NULL ? node->server->hostname : NULL;
}

static const char *host_power_failure(const host_power_t *power, const char *error) {
    const GS_POWER_ACTION *action = gs_host_power_get_action(&power->request.power, power->request.kind);
    const GS_NATIVE_STATUS *status = &power->request.status;
    if (!action->available && action->reason != NULL) {
        return action->reason;
    }
    if (status->http_status == 404) {
        /* Rubylight 3.0 to 3.5 have the native API but no /power. */
        return locstr("Update Rubylight on this PC to restart or shut it down from Aurora.");
    }
    if (status->message[0] != '\0') {
        return status->message;
    }
    if (status->http_status == 0 || str_null_or_empty(error)) {
        return locstr("The PC did not answer.");
    }
    return error;
}

static void host_power_checked_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(uuid);
    host_power_t *power = userdata;
    lv_msgbox_close(power->progress);
    power->progress = NULL;
    const pcmanager_host_power_t *request = &power->request;
    if (result != GS_OK || !gs_host_power_get_action(&request->power, request->kind)->available ||
        request->power.token == NULL) {
        host_power_message(power, host_power_failure(power, error));
        return;
    }
    static const char *restart_btns[] = {translatable("Cancel"), translatable("Restart"), ""};
    static const char *shutdown_btns[] = {translatable("Cancel"), translatable("Shut down"), ""};
    bool restart = request->kind == GS_POWER_RESTART;
    const char *text;
    if (restart) {
        text = locstr("Restart the PC now? A program with unsaved work on it can stop the restart.");
    } else if (request->power.wake_from_off) {
        text = locstr("Shut down the PC now? A program with unsaved work on it can stop the shutdown.");
    } else {
        text = locstr("Shut down the PC now? A program with unsaved work on it can stop the shutdown.\n\n"
                      "The PC has no wired network ready to wake it, so Aurora may not be able to turn it on again.");
    }
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, host_title(&power->uuid), text,
                                           restart ? restart_btns : shutdown_btns, false);
    lv_obj_add_event_cb(mbox, host_power_confirm_cb, LV_EVENT_VALUE_CHANGED, power);
    lv_obj_add_event_cb(mbox, host_power_unref_cb, LV_EVENT_DELETE, power);
    lv_obj_center(mbox);
}

static void host_power_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    host_power_t *power = lv_event_get_user_data(e);
    if (lv_msgbox_get_active_btn(mbox) == 1 && !power->request.act) {
        power->request.act = true;
        gs_host_power_clear(&power->request.power);
        power->progress = progress_dialog_create(power->request.kind == GS_POWER_RESTART
                                                 ? locstr("Restarting the PC...")
                                                 : locstr("Shutting down the PC..."));
        if (pcmanager_host_power(pcmanager, &power->uuid, &power->request, host_power_done_cb, power)) {
            power->refs++;
        } else {
            lv_msgbox_close(power->progress);
            power->progress = NULL;
        }
    }
    lv_msgbox_close_async(mbox);
}

static void host_power_done_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(uuid);
    host_power_t *power = userdata;
    lv_msgbox_close(power->progress);
    power->progress = NULL;
    if (result == GS_OK) {
        host_power_message(power, power->request.kind == GS_POWER_RESTART ? locstr("The PC is restarting.")
                                                                          : locstr("The PC is shutting down."));
    } else if (power->request.accepted) {
        /* After a 202 the worker fails only on a refusal from Windows. */
        host_power_message(power, power->request.power.last_refusal);
    } else {
        host_power_message(power, host_power_failure(power, error));
    }
}

/* Shows text; the dialog takes over the finished request's reference. */
static void host_power_message(host_power_t *power, const char *text) {
    static const char *ok_btns[] = {translatable("OK"), ""};
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, host_title(&power->uuid), text, ok_btns, false);
    lv_obj_add_event_cb(mbox, info_action_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(mbox, host_power_unref_cb, LV_EVENT_DELETE, power);
    lv_obj_center(mbox);
}

static void host_power_unref_cb(lv_event_t *e) {
    host_power_t *power = lv_event_get_user_data(e);
    if (--power->refs > 0) {
        return;
    }
    gs_host_power_clear(&power->request.power);
    SDL_free(power);
}

typedef struct wake_settings_ctx_t {
    uuidstr_t uuid;
    lv_obj_t *method_dropdown;
    lv_obj_t *url_input;
} wake_settings_ctx_t;

static void wake_settings_free_cb(lv_event_t *e) {
    wake_settings_ctx_t *ctx = lv_event_get_user_data(e);
    SDL_free(ctx);
}

static void configure_wake(const pclist_t *node) {
    const SERVER_DATA *server = node->server;
    static const char *btn_txts[] = {translatable("Cancel"), translatable("Save"), ""};
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, server->hostname, NULL, btn_txts, false);
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, lv_dpx(8), 0);

    wake_settings_ctx_t *ctx = SDL_calloc(1, sizeof(wake_settings_ctx_t));
    ctx->uuid = node->id;
    lv_obj_add_event_cb(mbox, wake_settings_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(mbox, wake_settings_free_cb, LV_EVENT_DELETE, ctx);

    lv_obj_t *method_label = lv_label_create(content);
    lv_label_set_text_static(method_label, locstr("Wake method"));

    ctx->method_dropdown = lv_dropdown_create(content);
    lv_dropdown_set_options(ctx->method_dropdown, "WoL\nHTTP");
    lv_dropdown_set_selected(ctx->method_dropdown, server->wake_method == WAKE_METHOD_HTTP ? 1 : 0);
    lv_obj_set_width(ctx->method_dropdown, LV_PCT(100));

    lv_obj_t *url_label = lv_label_create(content);
    lv_label_set_text_static(url_label, locstr("HTTP wake URL"));

    ctx->url_input = lv_textarea_create(content);
    lv_textarea_set_one_line(ctx->url_input, true);
    lv_textarea_set_placeholder_text(ctx->url_input, "http://host:port/wake");
    lv_obj_set_width(ctx->url_input, LV_PCT(100));
    if (server->wake_url) {
        lv_textarea_set_text(ctx->url_input, server->wake_url);
    }
    lv_obj_center(mbox);
}

static void wake_settings_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    wake_settings_ctx_t *ctx = lv_event_get_user_data(e);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
    if (btn_id != 1) {
        lv_msgbox_close_async(mbox);
        return;
    }
    int wake_method = lv_dropdown_get_selected(ctx->method_dropdown) == 1 ? WAKE_METHOD_HTTP : WAKE_METHOD_WOL;
    const char *wake_url = lv_textarea_get_text(ctx->url_input);
    pcmanager_set_wake_settings(pcmanager, &ctx->uuid, wake_method, wake_url);
    lv_msgbox_close_async(mbox);
}

static void info_action_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    lv_msgbox_close_async(mbox);
}