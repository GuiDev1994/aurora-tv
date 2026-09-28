/*
 * Copyright (c) 2023 Ningyuan Li <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "session_input.h"
#include "session_pointer_gesture.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "session_evmouse.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"

#if TARGET_WEBOS
#include "platform/webos/keyboard_evdev.h"
#include "logging.h"

static void session_keyboard_evdev_cb(short vk, bool down, char modifiers, void *userdata) {
    stream_input_t *input = userdata;
    if (input == NULL || !input->started || input->view_only) {
        return;
    }
    stream_input_send_key_event(input, vk, down, modifiers);
}
#endif

/* Pointer travel, in host pixels, for a finger swept across the full width of the
 * touchpad at 100% sensitivity. Vertical travel is derived from each pad's own
 * aspect ratio at motion time, so a given physical movement feels the same on
 * both axes regardless of which controller it came from. */
#define TOUCHPAD_MOUSE_BASE_SPEED 16.0f
#define TOUCHPAD_SCROLL_FINGER_SCALE 600.0f

void session_input_init(stream_input_t *input, session_t *session, app_input_t *app_input,
                        const session_config_t *config) {
    input->session = session;
    input->input = app_input;
    input->announcedGamepadMask = 0;
    input->remoteOkPressed = false;
    input->remoteOkPressedAt = 0;
    input->remoteOkModifiers = 0;
    input->pointerGestureActive = false;
    input->pointerGestureDragging = false;
    input->pointerGestureLeftDown = false;
    input->pointerGesturePressTime = 0;
    input->pointerGestureStartX = 0;
    input->pointerGestureStartY = 0;
    input->view_only = config->view_only;
    input->stick_deadzone = config->stick_deadzone;
    input->report_gamepad_battery = config->report_gamepad_battery;
    input->no_sdl_mouse = config->hardware_mouse;
    input->touchpad_mode = (uint8_t) config->touchpad_mode;
    input->touchpad_multitouch = config->touchpad_multitouch;
    input->touchpad_mouse_gain = TOUCHPAD_MOUSE_BASE_SPEED *
                                            (float) config->touchpad_speed;
    input->touchpad_scroll_scale = config->touchpad_natural_scroll
                                              ? TOUCHPAD_SCROLL_FINGER_SCALE
                                              : -TOUCHPAD_SCROLL_FINGER_SCALE;
    input->touchpads = NULL;
    input->touchpad_count = 0;
#if TARGET_WEBOS
    input->keyboard_evdev = NULL;
#endif
#if FEATURE_INPUT_EVMOUSE
    if (!config->view_only && config->hardware_mouse) {
        session_evmouse_init(&input->evmouse, session);
    }
#endif
#if TARGET_WEBOS
    if (!config->view_only) {
        input->keyboard_evdev = keyboard_evdev_start(session_keyboard_evdev_cb, input);
        if (input->keyboard_evdev != NULL) {
            commons_log_info("Input", "USB keyboard EVIOCGRAB active for stream");
        }
    }
#endif
}

void session_input_deinit(stream_input_t *input) {
    stream_input_touchpad_mouse_deinit(input);
#if TARGET_WEBOS
    keyboard_evdev_stop(input->keyboard_evdev);
    input->keyboard_evdev = NULL;
#endif
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (!config->view_only && config->hardware_mouse) {
        session_evmouse_deinit(&input->evmouse);
    }
#endif
}

void session_input_interrupt(stream_input_t *input) {
    pointer_gesture_reset(input);
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (!config->view_only && config->hardware_mouse) {
        session_evmouse_interrupt(&input->evmouse);
    }
#endif
}

void session_input_started(stream_input_t *input) {
    input->started = true;
    if (!input->view_only) {
        stream_input_touchpad_mouse_init(input);
    }
    /* Pick up pads whose JOYDEVICEADDED never reached us (2nd DualSense on webOS). */
    app_input_scan_gamepads(input->input);
    for (int i = 0, j = app_input_get_max_gamepads(input->input); i < j; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (gamepad == NULL) {
            continue;
        }
        stream_input_send_gamepad_arrive(input, gamepad);
    }
}

void session_input_stopped(stream_input_t *input) {
    pointer_gesture_reset(input);
    input->started = false;
    input->announcedGamepadMask = 0;
    input->remoteOkPressed = false;
    input->remoteOkPressedAt = 0;
    input->remoteOkModifiers = 0;
    stream_input_touchpad_mouse_deinit(input);
}

void session_input_screen_keyboard_opened(stream_input_t *input) {
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (config->hardware_mouse) {
        session_evmouse_disable(&input->evmouse);
    }
#endif
}

void session_input_screen_keyboard_closed(stream_input_t *input) {
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (config->hardware_mouse) {
        session_evmouse_enable(&input->evmouse);
    }
#endif
    stream_input_flush_pressed_keys(input);
}