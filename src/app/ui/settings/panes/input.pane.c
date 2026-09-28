#include "app.h"
#include "config.h"

#include "pref_obj.h"

#include "input/input_gamepad.h"
#include "util/i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct input_pane_t {
    lv_fragment_t base;

    lv_obj_t *absmouse_toggle;
    lv_obj_t *absmouse_hint;
    lv_obj_t *deadzone_label;
    lv_obj_t *deadzone_slider;
    lv_obj_t *swap_abxy_toggle;

    pref_dropdown_int_entry_t touchpad_mode_entries[2];
    lv_obj_t *touchpad_speed_label;

    lv_obj_t *gamepad_report_label;
    lv_obj_t *gamepad_test_result;
} input_pane_t;

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *view);

static void pane_ctor(lv_fragment_t *self, void *args);

static void on_touchpad_speed_changed(lv_event_t *e);

static void update_touchpad_speed_label(input_pane_t *pane);

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e);

static void hwmouse_state_update(input_pane_t *pane);
#endif

static void update_deadzone_label(input_pane_t *pane);

static void on_deadzone_changed(lv_event_t *e);

static void gamepad_report_update(input_pane_t *pane);

static void on_gamepad_refresh_clicked(lv_event_t *e);

static void on_gamepad_rumble_clicked(lv_event_t *e);

const lv_fragment_class_t settings_pane_input_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(input_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    (void) self;
    (void) args;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    input_pane_t *pane = (input_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pane->touchpad_mode_entries[0] = (pref_dropdown_int_entry_t) {
            locstr("Send as touchpad"), TOUCHPAD_MODE_NATIVE, true};
    pane->touchpad_mode_entries[1] = (pref_dropdown_int_entry_t) {
            locstr("Send as mouse"), TOUCHPAD_MODE_MOUSE, false};

    pref_checkbox(view, locstr("View-only mode"), &app_configuration->viewonly, false);
    pref_desc_label(view, locstr("Don't send mouse, keyboard or gamepad input to host computer."), false);

    pref_checkbox(view, locstr("Capture system keys"), &app_configuration->syskey_capture, false);
    pref_desc_label(view,
                    locstr("Send Win/Meta to the host. On webOS, Home is always captured for "
                           "streaming; turn this on to also forward Win. Restart the app after "
                           "changing. Default is on."),
                    false);

    pref_header(view, locstr("Mouse"));

#if FEATURE_INPUT_EVMOUSE
    lv_obj_t *hwmouse_toggle = pref_checkbox(view, locstr("Use mouse hardware"),
                                             &app_configuration->hardware_mouse, false);
    lv_obj_add_event_cb(hwmouse_toggle, hwmouse_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Use plugged mouse device only when streaming. "
                                 "This will have better performance, but absolute mouse mode will not be enabled."),
                    false);
#endif

    pane->absmouse_toggle = pref_checkbox(view, locstr("Absolute mouse mode"),
                                          &app_configuration->absmouse, false);
    pane->absmouse_hint = pref_desc_label(view, locstr("Better for remote desktop. "
                                                       "For some games, mouse will not work properly."), false);

    pref_header(view, locstr("Gamepad"));

    pane->deadzone_label = pref_title_label(view, locstr("Analog stick deadzone"));
    pane->deadzone_slider = pref_slider(view, &app_configuration->stick_deadzone, 0, 20, 1);
    lv_obj_set_width(pane->deadzone_slider, LV_PCT(100));
    lv_obj_add_event_cb(pane->deadzone_slider, on_deadzone_changed, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Note: Some games can enforce a larger deadzone "
                                 "than what Aurora is configured to use."),
                    false);

    pref_checkbox(view, locstr("Virtual mouse"), &app_configuration->virtual_mouse, false);
    pref_desc_label(view, locstr("When enabled, virtual mouse starts active at the beginning of a stream. "
                                 "Toggle anytime from the stream overlay Virtual Mouse button. "
                                 "Right stick moves the cursor, left stick scrolls, LT/RT are left/right mouse buttons."),
                    false);

    pane->swap_abxy_toggle = pref_checkbox(view, locstr("Swap ABXY buttons"), &app_configuration->swap_abxy, false);
    pref_desc_label(view, locstr("Swap A/B and X/Y gamepad buttons. Useful when you prefer Nintendo-like layouts."),
                    false);

    pref_header(view, locstr("Gamepad touchpad"));

    pref_title_label(view, locstr("Mode"));
    lv_obj_t *mode_dropdown = pref_dropdown_int(view, pane->touchpad_mode_entries,
                                                sizeof(pane->touchpad_mode_entries) /
                                                        sizeof(*pane->touchpad_mode_entries),
                                                &app_configuration->touchpad_mode, NULL);
    lv_obj_set_width(mode_dropdown, LV_PCT(100));
    pref_desc_label(view, locstr("Send as touchpad forwards touches to the host, for games "
                                 "with built-in touchpad support. Send as mouse uses the "
                                 "touchpad as a trackpad instead."), false);

    pane->touchpad_speed_label = pref_title_label(view, NULL);
    lv_obj_t *sensitivity_slider = pref_slider(view, &app_configuration->touchpad_speed,
                                               TOUCHPAD_SPEED_MIN,
                                               TOUCHPAD_SPEED_MAX, 5);
    lv_obj_set_width(sensitivity_slider, LV_PCT(100));
    lv_obj_add_event_cb(sensitivity_slider, on_touchpad_speed_changed,
                        LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Adjust pointer speed when the touchpad is used as a mouse."), false);

    pref_checkbox(view, locstr("Multi-touch gestures"),
                  &app_configuration->touchpad_multitouch, false);
    pref_desc_label(view, locstr("Use two fingers to scroll and to right click. "
                                 "Turn off if you trigger them by accident."), false);

    pref_checkbox(view, locstr("Natural scrolling"),
                  &app_configuration->touchpad_natural_scroll, false);
    pref_desc_label(view, locstr("Scrolling follows your fingers, like on a phone."), false);

    pref_header(view, locstr("Controller"));
    pref_checkbox(view, locstr("Report battery to host"), &app_configuration->report_gamepad_battery, false);
    pref_desc_label(view, locstr("Advertise battery state to Vibepollo/Sunshine so the host virtual pad "
                                 "matches Xbox, DualSense, DS4, or Switch Pro capabilities from this TV."),
                    false);

    pref_desc_label(view, locstr("Hold Select/Back for 4 seconds during streaming to pin or unpin performance stats. "
                                 "Open the on-screen keyboard from the stream overlay, Magic Remote BLUE, "
                                 "or gamepad Y while virtual mouse is active."),
                    false);

    pref_header(view, locstr("Gamepad test"));
    pane->gamepad_report_label = pref_desc_label(view, NULL, false);
    lv_obj_set_width(pane->gamepad_report_label, LV_PCT(100));

    lv_obj_t *refresh_btn = lv_btn_create(view);
    lv_obj_set_width(refresh_btn, LV_PCT(100));
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, locstr("Detect connected gamepads"));
    lv_obj_center(refresh_lbl);
    lv_obj_add_event_cb(refresh_btn, on_gamepad_refresh_clicked, LV_EVENT_CLICKED, pane);

    lv_obj_t *rumble_btn = lv_btn_create(view);
    lv_obj_set_width(rumble_btn, LV_PCT(100));
    lv_obj_t *rumble_lbl = lv_label_create(rumble_btn);
    lv_label_set_text(rumble_lbl, locstr("Test rumble"));
    lv_obj_center(rumble_lbl);
    lv_obj_add_event_cb(rumble_btn, on_gamepad_rumble_clicked, LV_EVENT_CLICKED, pane);

    pane->gamepad_test_result = pref_desc_label(view, NULL, false);
    lv_obj_set_width(pane->gamepad_test_result, LV_PCT(100));
    pref_desc_label(view, locstr("Rumble here is produced by this TV alone, with no host involved. "
                                 "If the pad shakes here but stays still in a game, the host is not "
                                 "sending rumble and the problem is not on this side."),
                    false);

    gamepad_report_update(pane);

#if FEATURE_INPUT_EVMOUSE
    hwmouse_state_update(pane);
#endif
    update_deadzone_label(pane);
    update_touchpad_speed_label(pane);
    return view;
}

static void on_touchpad_speed_changed(lv_event_t *e) {
    input_pane_t *pane = lv_event_get_user_data(e);
    update_touchpad_speed_label(pane);
}

static void update_touchpad_speed_label(input_pane_t *pane) {
    lv_label_set_text_fmt(pane->touchpad_speed_label, "%s - %d%%",
                          locstr("Pointer speed"), app_configuration->touchpad_speed);
}

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e) {
    hwmouse_state_update((input_pane_t *) lv_event_get_user_data(e));
}

static void hwmouse_state_update(input_pane_t *pane) {
    if (app_configuration->hardware_mouse) {
        lv_obj_add_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Absolute mouse mode can't be used when "
                                                      "\"Use mouse hardware\" enabled."));
    } else {
        lv_obj_clear_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Better for remote desktop. "
                                                      "For some games, mouse will not work properly."));
    }
}
#endif

static void update_deadzone_label(input_pane_t *pane) {
    lv_label_set_text_fmt(pane->deadzone_label, "%s - %d", locstr("Analog stick deadzone"),
                          app_configuration->stick_deadzone);
}

static void on_deadzone_changed(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    update_deadzone_label(pane);
}

static const char *gamepad_type_name(SDL_GameController *controller) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
    switch (SDL_GameControllerGetType(controller)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
            return "Xbox 360";
        case SDL_CONTROLLER_TYPE_XBOXONE:
            return "Xbox One";
        case SDL_CONTROLLER_TYPE_PS3:
            return "PlayStation 3";
        case SDL_CONTROLLER_TYPE_PS4:
            return "DualShock 4";
#if SDL_VERSION_ATLEAST(2, 0, 14)
        case SDL_CONTROLLER_TYPE_PS5:
            return "DualSense";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            return "Joy-Con (L)";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            return "Joy-Con (R)";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return "Joy-Con pair";
#endif
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            return "Switch Pro";
#if SDL_VERSION_ATLEAST(2, 0, 14)
        case SDL_CONTROLLER_TYPE_VIRTUAL:
            return "virtual";
#endif
        default:
            return "unrecognised";
    }
#else
    (void) controller;
    return "unknown";
#endif
}

static void gamepad_report_update(input_pane_t *pane) {
    char report[768];
    report[0] = '\0';
    size_t used = 0;
    int found = 0;
    app_input_t *input = global != NULL ? &global->input : NULL;
    short max_pads = input != NULL ? app_input_get_max_gamepads(input) : 0;

    for (short i = 0; i < max_pads && used + 1 < sizeof(report); i++) {
        app_gamepad_state_t *state = &input->gamepads[i];
        SDL_GameController *controller = state->controller;
        if (controller == NULL) {
            continue;
        }
        found++;
        const char *name = SDL_GameControllerName(controller);
        const char *path = NULL;
        const char *writable = locstr("unknown");
#if SDL_VERSION_ATLEAST(2, 24, 0)
        path = SDL_JoystickPath(SDL_GameControllerGetJoystick(controller));
        if (path != NULL) {
            writable = access(path, W_OK) == 0 ? locstr("writable") : locstr("READ-ONLY");
        }
#endif
        int rumble = 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        rumble = SDL_GameControllerHasRumble(controller) ? 1 : 0;
#endif
        int written = snprintf(report + used, sizeof(report) - used,
                               "#%d %s\n    %s: %s\n    %s: %s (%s)\n    %s: %s\n",
                               state->gs_id, name != NULL ? name : "?",
                               locstr("Model"), gamepad_type_name(controller),
                               locstr("Device"), path != NULL ? path : "?", writable,
                               locstr("Rumble"), rumble ? locstr("supported") : locstr("not supported"));
        if (written < 0) {
            break;
        }
        if ((size_t) written >= sizeof(report) - used) {
            used = sizeof(report) - 1;
            break;
        }
        used += (size_t) written;
    }
    if (found == 0) {
        lv_label_set_text(pane->gamepad_report_label, locstr("No gamepad connected."));
    } else {
        lv_label_set_text(pane->gamepad_report_label, report);
    }
}

static void on_gamepad_refresh_clicked(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    gamepad_report_update(pane);
    lv_label_set_text(pane->gamepad_test_result, "");
}

static void on_gamepad_rumble_clicked(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    app_input_t *input = global != NULL ? &global->input : NULL;
    short max_pads = input != NULL ? app_input_get_max_gamepads(input) : 0;

    for (short i = 0; i < max_pads; i++) {
        SDL_GameController *controller = input->gamepads[i].controller;
        if (controller == NULL) {
            continue;
        }
#if SDL_VERSION_ATLEAST(2, 0, 9)
        if (SDL_GameControllerRumble(controller, 0xC000, 0xC000, 1000) == 0) {
            lv_label_set_text(pane->gamepad_test_result,
                              locstr("Rumble sent. If nothing moved, the pad ignored it."));
        } else {
            lv_label_set_text_fmt(pane->gamepad_test_result, "%s: %s",
                                  locstr("SDL refused the rumble"), SDL_GetError());
        }
#else
        (void) controller;
        lv_label_set_text(pane->gamepad_test_result, locstr("SDL refused the rumble"));
#endif
        return;
    }
    lv_label_set_text(pane->gamepad_test_result, locstr("No gamepad connected."));
}
