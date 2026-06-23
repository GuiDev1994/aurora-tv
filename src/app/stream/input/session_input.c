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
    input->hid_passthrough = config->hid_passthrough;
    input->stick_deadzone = config->stick_deadzone;
    input->no_sdl_mouse = config->hardware_mouse;
#if FEATURE_INPUT_EVMOUSE
    if (!config->view_only && config->hardware_mouse) {
        session_evmouse_init(&input->evmouse, session);
    }
#endif
}

void session_input_deinit(stream_input_t *input) {
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
    if (input->hid_passthrough) {
        return;
    }
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