#include "app.h"
#include "app_session.h"
#include "streaming.controller.h"
#include "soft_keyboard.h"
#include "hid_passthrough_panel.h"

#include <SDL.h>
#include "stream/video/session_video.h"
#include "ui/root.h"
#include "ui/common/progress_dialog.h"
#include "lvgl/lv_ext_utils.h"
#include "lvgl/util/lv_app_utils.h"

#include "util/user_event.h"
#include "util/i18n.h"
#include "logging.h"

#include <string.h>

static void exit_streaming(lv_event_t *event);

static void suspend_streaming(lv_event_t *event);

static void open_keyboard(lv_event_t *event);

static void toggle_vmouse(lv_event_t *event);

static void open_hid_devices(lv_event_t *event);

static void hid_panel_close_cb(void *userdata);

static void hide_overlay(lv_event_t *event);
static void hide_overlay_impl(streaming_controller_t *controller);

static void on_cancel_key(lv_event_t *event);

static void soft_keyboard_close_cb(void *userdata);

static bool show_overlay(streaming_controller_t *controller);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view);

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void constructor(lv_fragment_t *self, void *args);

static void controller_dtor(lv_fragment_t *self);

static void overlay_key_cb(lv_event_t *e);

static void update_buttons_layout(streaming_controller_t *controller);

static void pin_toggle(lv_event_t *e);

/** Pretty codec label for stats (matches session_video video_format_name strings). */
static const char *streaming_codec_display(const char *fmt) {
    if (fmt == NULL || fmt[0] == '\0') {
        return "-";
    }
    if (strcmp(fmt, "H264") == 0) {
        return "H.264";
    }
    if (strcmp(fmt, "H265") == 0) {
        return "H.265";
    }
    if (strcmp(fmt, "H265 10bit") == 0) {
        return "H.265 10bit";
    }
    if (strcmp(fmt, "AV1 8bit") == 0) {
        return "AV1";
    }
    if (strcmp(fmt, "AV1 10bit") == 0) {
        return "AV1 10bit";
    }
    return fmt;
}

static void network_test_timer_cb(lv_timer_t *timer) {
    streaming_controller_t *controller = (streaming_controller_t *) timer->user_data;
    if (!controller) {
        return;
    }
    if (controller->network_test_timer) {
        lv_timer_del(controller->network_test_timer);
        controller->network_test_timer = NULL;
    }

    const VIDEO_STATS *dst = &vdec_summary_stats;
    const VIDEO_INFO *info = &vdec_stream_info;
    (void) info;

    uint32_t measuredBps = dst->currentBitrateKbps;
    float dropPct = 0.0f;
    if (dst->totalFrames > 0) {
        dropPct = (float) dst->networkDroppedFrames * 100.0f / (float) dst->totalFrames;
    }

    char body[256];
    if (measuredBps == 0) {
        snprintf(body, sizeof(body),
                 "%s\n\n%s",
                 locstr("Network speed test could not gather enough data."),
                 locstr("Check your connection and try again."));
    } else {
        /* Converter para Mbps e aplicar margem de segurança de ~20% */
        float measuredMbps = (float) measuredBps / 1000000.0f;
        float recommendedMbps = measuredMbps * 0.8f;
        snprintf(body, sizeof(body),
                 "%s\n\n"
                 "%s: %.1f Mbps\n"
                 "%s: %.1f Mbps\n"
                 "%s: %u ms\n"
                 "%s: %.2f%%",
                 locstr("Network speed test finished."),
                 locstr("Measured throughput"), measuredMbps,
                 locstr("Recommended max bitrate"), recommendedMbps,
                 locstr("Network RTT (avg)"), dst->rtt,
                 locstr("Network frame drop"), dropPct);
    }

    static const char *btns[] = { "OK", "" };
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL,
                                             locstr("Network speed test"),
                                             body,
                                             btns,
                                             false);
    lv_obj_center(dialog);

    /* Encerra o streaming após o teste */
    if (controller->global && controller->global->session) {
        session_interrupt(controller->global->session, true, STREAMING_INTERRUPT_USER);
    }
}

static void network_test_timer_cb(lv_timer_t *timer);

const lv_fragment_class_t streaming_controller_class = {
        .constructor_cb = constructor,
        .destructor_cb = controller_dtor,
        .create_obj_cb = streaming_scene_create,
        .obj_created_cb = on_view_created,
        .obj_will_delete_cb = on_delete_obj,
        .obj_deleted_cb = on_obj_deleted,
        .event_cb = on_event,
        .instance_size = sizeof(streaming_controller_t),
};

static bool overlay_showing = false, overlay_pinned = false;
static streaming_controller_t *current_controller = NULL;

bool streaming_overlay_shown() {
    return overlay_showing;
}

bool streaming_soft_keyboard_shown() {
    return current_controller != NULL && current_controller->soft_kbd != NULL;
}

#if defined(TARGET_WEBOS)
bool streaming_hid_panel_shown() {
    return current_controller != NULL && current_controller->hid_panel != NULL;
}
#endif

bool streaming_stats_shown() {
    return overlay_showing || overlay_pinned;
}

bool streaming_refresh_stats() {
    streaming_controller_t *controller = current_controller;
    if (!controller) { return false; }
    if (!streaming_stats_shown()) {
        return false;
    }
    app_t *app = controller->global;
    const struct VIDEO_STATS *dst = &vdec_summary_stats;
    const struct VIDEO_INFO *info = &vdec_stream_info;

    if (controller->stats_compact_label != NULL) {
        /* Format: 3840×2160 HDR H.265 10bit FPS 120 N 1/3ms H 4ms D 8ms TL 13ms FD 0.00% 45.0 Mbps */
        float renderFps = dst->decodedFps;
        int displayRate = 60;
#if defined(TARGET_WEBOS)
        if (SDL_webOSGetRefreshRate(&displayRate) && displayRate > 0 && renderFps > (float) displayRate) {
            renderFps = (float) displayRate;
        }
#else
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate > 0) {
            displayRate = mode.refresh_rate;
            if (renderFps > (float) displayRate) {
                renderFps = (float) displayRate;
            }
        }
#endif
        float netMs = (float) dst->rtt;
        float hostMs = 0.0f;
        float submitMs = 0.0f;
        float decOnlyMs = 0.0f;
        if (dst->submittedFrames) {
            if (vdec_stream_info.has_host_latency) {
                hostMs = (float) dst->totalCaptureLatency / (float) dst->submittedFrames / 10.0f;
            }
            if (vdec_stream_info.has_decoder_latency) {
                submitMs = (float) dst->totalSubmitTime / (float) dst->submittedFrames;
                decOnlyMs = dst->avgDecoderLatency;
            }
        }
        float totalMs = netMs + hostMs + submitMs + decOnlyMs;
        float fdPct = (dst->totalFrames > 0)
            ? (float) dst->networkDroppedFrames / (float) dst->totalFrames * 100.0f
            : 0.0f;
        const char *codec = streaming_codec_display(vdec_stream_info.format);
        int w = info->width > 0 ? info->width : 0;
        int h = info->height > 0 ? info->height : 0;
        const char *hdr_str = app_configuration->hdr ? "HDR" : "SDR";
        float bitrateMbps = (float) dst->currentBitrateKbps / 1000000.0f;
        lv_label_set_text_fmt(controller->stats_compact_label,
                              "%d\u00d7%d %s %s FPS %.0f N %u/%ums H %.0fms S %.0fms D %.0fms TL %.0fms FD %.2f%% %.1f Mbps",
                              w, h, hdr_str, codec, renderFps,
                              (unsigned) dst->rtt, (unsigned) dst->rttVariance,
                              hostMs, submitMs, decOnlyMs, totalMs, fdPct, bitrateMbps);
        /* Quality dot: green ≤25ms, yellow ≤30ms, red >30ms */
        if (controller->stats_quality_indicator) {
            lv_color_t qc = totalMs <= 25.0f ? lv_palette_main(LV_PALETTE_GREEN)
                          : totalMs <= 30.0f ? lv_palette_main(LV_PALETTE_YELLOW)
                          : lv_palette_main(LV_PALETTE_RED);
            lv_obj_set_style_text_color(controller->stats_quality_indicator, qc, 0);
        }
        return true;
    }

    const char *hdr_str = app_configuration->hdr ? "HDR" : "SDR";
    const char *codec = streaming_codec_display(vdec_stream_info.format);
    if (info->width > 0 && info->height > 0) {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%d\u00d7%d %s %s (%s)", info->width, info->height, hdr_str,
                              codec, SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    } else {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%s %s (%s)", codec, hdr_str,
                              SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    }
    lv_label_set_text_fmt(controller->stats_items.audio, "%s, %s (%s)", audio_stream_info.format,
                          audio_stream_info.channels, SS4S_ModuleInfoGetId(app->ss4s.selection.audio_module));
    lv_label_set_text_fmt(controller->stats_items.rtt, "%d ms (var. %d ms)", dst->rtt, dst->rttVariance);
    lv_label_set_text_fmt(controller->stats_items.net_fps, "%.2f FPS", dst->receivedFps);
    float renderFps = dst->decodedFps;
#if defined(TARGET_WEBOS)
    int displayRate = 0;
    if (SDL_webOSGetRefreshRate(&displayRate) && displayRate > 0 && renderFps > (float) displayRate) {
        renderFps = (float) displayRate;
    }
#else
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate > 0
        && renderFps > (float) mode.refresh_rate) {
        renderFps = (float) mode.refresh_rate;
    }
#endif
    lv_label_set_text_fmt(controller->stats_items.render_fps, "%.2f FPS", renderFps);
    lv_label_set_text_fmt(controller->stats_items.bitrate, "%u Mbps", dst->currentBitrateKbps / 1000000);

        if (dst->submittedFrames) {
            lv_label_set_text_fmt(controller->stats_items.drop_rate, "%.2f%%",
                                  (float) dst->networkDroppedFrames / (float) dst->totalFrames * 100);
            if (vdec_stream_info.has_host_latency) {
                float avgCapLatency = (float) dst->totalCaptureLatency / (float) dst->submittedFrames / 10.0f;
                lv_label_set_text_fmt(controller->stats_items.host_latency, "avg %.2f ms", avgCapLatency);
            } else {
                lv_label_set_text_fmt(controller->stats_items.host_latency, "not available");
            }
            if (vdec_stream_info.has_decoder_latency) {
                float avgSubmitTime = (float) dst->totalSubmitTime / (float) dst->submittedFrames;
                lv_label_set_text_fmt(controller->stats_items.vdec_latency, "submit %.2f ms + decode %.2f ms",
                                      avgSubmitTime, dst->avgDecoderLatency);
            } else {
                lv_label_set_text_fmt(controller->stats_items.vdec_latency, "not available");
            }
    } else {
        lv_label_set_text(controller->stats_items.drop_rate, "-");
        lv_label_set_text_fmt(controller->stats_items.host_latency, "-");
        lv_label_set_text_fmt(controller->stats_items.vdec_latency, "-");
    }
    return true;
}

void streaming_notice_show(const char *message) {
    streaming_controller_t *controller = current_controller;
    if (!controller) { return; }
    lv_label_set_text(controller->notice_label, message);
    if (message && message[0]) {
        lv_obj_clear_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

static void constructor(lv_fragment_t *self, void *args) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    current_controller = controller;

    overlay_showing = false;

    streaming_styles_init(controller);

    const streaming_scene_arg_t *arg = (streaming_scene_arg_t *) args;
    controller->global = arg->global;
    controller->network_test = arg->network_test;
    controller->network_test_duration = arg->network_test_duration ? arg->network_test_duration : 10;
    controller->network_test_timer = NULL;
    app_session_begin(arg->global, &arg->uuid, &arg->app);
}

static void controller_dtor(lv_fragment_t *self) {
    streaming_controller_t *fragment = (streaming_controller_t *) self;
    if (fragment->network_test_timer) {
        lv_timer_del(fragment->network_test_timer);
        fragment->network_test_timer = NULL;
    }
#if defined(TARGET_WEBOS)
    if (fragment->hid_panel) {
        lv_obj_del(fragment->hid_panel);
        fragment->hid_panel = NULL;
    }
#endif
    streaming_styles_reset(fragment);
    if (current_controller == fragment) {
        current_controller = NULL;
    }
    fragment->soft_kbd = NULL; /* Will be deleted with parent */
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    switch (code) {
        case USER_STREAM_CONNECTING: {
            controller->progress = progress_dialog_create(locstr("Connecting..."));
            if (lv_obj_check_type(controller->progress->parent, &lv_msgbox_backdrop_class)) {
                lv_obj_set_style_bg_opa(controller->progress->parent, LV_OPA_TRANSP, 0);
            }
            lv_obj_set_width(controller->progress, LV_DPX(300));
            lv_obj_set_height(controller->progress, LV_DPX(100));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
        case USER_STREAM_OPEN: {
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);

            bool show_stats = app_configuration->show_stats_on_start || controller->network_test;
            if (show_stats) {
                lv_obj_set_parent(controller->stats, lv_layer_top());
                if (app_configuration->show_stats_compact) {
                    lv_obj_align(controller->stats, LV_ALIGN_TOP_LEFT, 0, 0);
                } else {
                    lv_obj_align(controller->stats, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
                }
                lv_obj_add_state(controller->stats, LV_STATE_USER_1);
                lv_obj_add_state(controller->stats_pin, LV_STATE_CHECKED);
                overlay_pinned = true;
                streaming_refresh_stats();
            }

            if (controller->network_test && controller->network_test_timer == NULL) {
                uint32_t period_ms = (uint32_t) controller->network_test_duration * 1000U;
                if (period_ms == 0) {
                    period_ms = 10000U;
                }
                controller->network_test_timer = lv_timer_create(network_test_timer_cb, period_ms, controller);
            }
            break;
        }
        case USER_STREAM_CLOSE: {
            controller->progress = progress_dialog_create(locstr("Disconnecting..."));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->stats, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case USER_STREAM_FINISHED: {
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            lv_async_call((lv_async_cb_t) lv_fragment_del, controller);
            break;
        }
        case USER_OPEN_OVERLAY: {
            show_overlay(controller);
            return true;
        }
        case USER_CLOSE_SOFT_KEYBOARD: {
            if (streaming_soft_keyboard_shown()) {
                soft_keyboard_close_cb(controller);
            }
            return true;
        }
        case USER_OPEN_SOFT_KEYBOARD: {
            if (controller->soft_kbd) {
                return true;
            }
            hide_overlay_impl(controller);
            session_screen_keyboard_opened(controller->global->session);
            controller->soft_kbd = soft_keyboard_create(
                controller->detached_root,
                controller->global->session,
                soft_keyboard_close_cb,
                controller);
            lv_group_t *kbd_group = soft_keyboard_get_group(controller->soft_kbd);
            if (kbd_group) {
                app_input_set_group(&controller->global->ui.input, kbd_group);
            }
            app_set_mouse_grab(&controller->global->input, false);
            return true;
        }
        case USER_SIZE_CHANGED: {
            update_buttons_layout(controller);
            streaming_overlay_resized(controller);
            return false;
        }
        default: {
            break;
        }
    }
    return false;
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    app_input_set_group(&controller->global->ui.input, controller->group);
    lv_obj_add_event_cb(controller->quit_btn, exit_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->suspend_btn, suspend_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->kbd_btn, open_keyboard, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->vmouse_btn, toggle_vmouse, LV_EVENT_CLICKED, self);
#if defined(TARGET_WEBOS)
    if (controller->hid_devices_btn) {
        lv_obj_add_event_cb(controller->hid_devices_btn, open_hid_devices, LV_EVENT_CLICKED, self);
    }
#endif
    lv_obj_add_event_cb(controller->base.obj, hide_overlay, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->overlay, overlay_key_cb, LV_EVENT_KEY, controller);
    lv_obj_add_event_cb(controller->base.obj, on_cancel_key, LV_EVENT_CANCEL, controller);

    lv_obj_t *notice = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(notice, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(notice, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
    lv_obj_set_style_radius(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_hor(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_ver(notice, LV_DPX(3), 0);
    lv_obj_set_style_border_opa(notice, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(notice, lv_color_black(), 0);
    lv_obj_t *notice_label = lv_label_create(notice);
    lv_obj_set_size(notice_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(notice_label, lv_theme_get_font_small(view), 0);
    lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);

    controller->notice = notice;
    controller->notice_label = notice_label;

    lv_obj_add_event_cb(controller->stats_pin, pin_toggle, LV_EVENT_VALUE_CHANGED, controller->stats);

#if !defined(TARGET_WEBOS)
    const app_settings_t *settings = &controller->global->settings;
    if (settings->syskey_capture) {
        SDL_SetWindowGrab(controller->global->ui.window, SDL_TRUE);
    }
#endif
}

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    if (controller->notice) {
        lv_obj_del(controller->notice);
    }
    if (controller->stats->parent != controller->overlay) {
        lv_obj_del(controller->stats);
    }
    app_input_set_group(&controller->global->ui.input, NULL);
    lv_group_del(controller->group);

#if !defined(TARGET_WEBOS)
    SDL_SetWindowGrab(controller->global->ui.window, SDL_FALSE);
#endif
}

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    lv_obj_del(controller->detached_root);
}

static void exit_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    session_interrupt(self->global->session, true, STREAMING_INTERRUPT_USER);
}

static void suspend_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    session_interrupt(self->global->session, false, STREAMING_INTERRUPT_USER);
}

static void soft_keyboard_close_cb(void *userdata) {
    streaming_controller_t *controller = userdata;
    if (!controller || !controller->soft_kbd) {
        return;
    }
    lv_obj_t *kbd_obj = controller->soft_kbd;
    controller->soft_kbd = NULL;
    app_input_set_group(&controller->global->ui.input, controller->group);
    app_set_mouse_grab(&controller->global->input, true);
    if (controller->global->session) {
        session_screen_keyboard_closed(controller->global->session);
    }
    lv_obj_del(kbd_obj);
}

static void open_keyboard(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    if (controller->soft_kbd) {
        return; /* Already showing */
    }
    session_screen_keyboard_opened(controller->global->session);
    controller->soft_kbd = soft_keyboard_create(
        lv_layer_top(),
        controller->global->session,
        soft_keyboard_close_cb,
        controller);
    lv_group_t *kbd_group = soft_keyboard_get_group(controller->soft_kbd);
    if (kbd_group) {
        app_input_set_group(&controller->global->ui.input, kbd_group);
    }
    /* Mostrar cursor e liberar mouse para Magic Remote / ponteiro funcionarem */
    app_set_mouse_grab(&controller->global->input, false);
}

static void toggle_vmouse(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    app_t *app = controller->global;
    session_toggle_vmouse(app->session);
}

#if defined(TARGET_WEBOS)
static void hid_panel_close_cb(void *userdata) {
    streaming_controller_t *controller = userdata;
    if (!controller) {
        return;
    }
    if (controller->hid_panel) {
        lv_obj_del(controller->hid_panel);
        controller->hid_panel = NULL;
    }
    app_input_set_group(&controller->global->ui.input, controller->group);
}

static void open_hid_devices(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (!controller->global->session || controller->hid_panel) {
        return;
    }
    hide_overlay_impl(controller);
    controller->hid_panel = hid_passthrough_panel_create(
            lv_layer_top(),
            controller->global->session,
            hid_panel_close_cb,
            controller);
    if (!controller->hid_panel) {
        return;
    }
    lv_group_t *group = hid_passthrough_panel_get_group(controller->hid_panel);
    if (group) {
        app_input_set_group(&controller->global->ui.input, group);
    }
}
#endif

bool show_overlay(streaming_controller_t *controller) {
    if (overlay_showing) {
        return false;
    }
    overlay_showing = true;
    lv_obj_clear_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);

    lv_area_t coords = controller->video->coords;
    streaming_enter_overlay(controller->global->session, coords.x1, coords.y1, lv_area_get_width(&coords),
                            lv_area_get_height(&coords));
    streaming_refresh_stats();

    app_stop_text_input(&controller->global->ui.input);

    update_buttons_layout(controller);

    return true;
}

/* B/Back: close keyboard or HID panel if shown, else hide overlay */
static void on_cancel_key(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (streaming_soft_keyboard_shown()) {
        soft_keyboard_close_cb(controller);
#if defined(TARGET_WEBOS)
    } else if (streaming_hid_panel_shown()) {
        hid_panel_close_cb(controller);
#endif
    } else {
        hide_overlay(event);
    }
}

static void hide_overlay_impl(streaming_controller_t *controller) {
    app_input_set_button_points(&controller->global->ui.input, NULL);
    lv_obj_add_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);
    if (!overlay_showing) {
        return;
    }
    overlay_showing = false;
    app_set_mouse_grab(&controller->global->input, true);
    streaming_enter_fullscreen(controller->global->session);
}

static void hide_overlay(lv_event_t *event) {
    hide_overlay_impl((streaming_controller_t *) lv_event_get_user_data(event));
}

static void overlay_key_cb(lv_event_t *e) {
    streaming_controller_t *controller = lv_event_get_user_data(e);
    lv_group_t *group = controller->group;
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT:
            lv_group_focus_prev(group);
            break;
        case LV_KEY_RIGHT:
            lv_group_focus_next(group);
            break;
        default:
            break;
    }
}

static void update_buttons_layout(streaming_controller_t *controller) {
    lv_area_t coords;
    lv_obj_get_coords(controller->quit_btn, &coords);
    lv_area_center(&coords, &controller->button_points[1]);
    lv_obj_get_coords(controller->suspend_btn, &coords);
    lv_area_center(&coords, &controller->button_points[3]);
    lv_obj_get_coords(controller->kbd_btn, &coords);
    lv_area_center(&coords, &controller->button_points[4]);
#if defined(TARGET_WEBOS)
    if (controller->hid_devices_btn) {
        lv_obj_get_coords(controller->hid_devices_btn, &coords);
        lv_area_center(&coords, &controller->button_points[5]);
    }
#endif
    app_input_set_button_points(&controller->global->ui.input, controller->button_points);
}

static void pin_toggle(lv_event_t *e) {
    lv_obj_t *toggle_view = lv_event_get_user_data(e);
    lv_fragment_t *fragment = lv_obj_get_user_data(toggle_view);
    bool checked = lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED);
    bool pinned = toggle_view->parent != fragment->obj;
    overlay_pinned = checked;
    if (checked == pinned) {
        return;
    }
    if (checked) {
        lv_obj_set_parent(toggle_view, lv_layer_top());
        lv_obj_add_state(toggle_view, LV_STATE_USER_1);
    } else {
        lv_obj_set_parent(toggle_view, fragment->obj);
        lv_obj_clear_state(toggle_view, LV_STATE_USER_1);
    }
}
