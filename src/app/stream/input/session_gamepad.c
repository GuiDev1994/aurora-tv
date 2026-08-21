#include "app.h"

#include <Limelight.h>

#include "stream/input/session_input.h"

#include "util/bus.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#include "stream/session.h"
#include "stream/input/session_virt_mouse.h"
#include "logging.h"

#include <SDL.h>

#define QUIT_BUTTONS (PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG)
/** Hold Select (Back) this long to toggle pinned performance stats (Artemis-style). */
#define GAMEPAD_HOLD_STATS_MS 4000

static bool quit_combo_pressed = false;

/** Hold timer only detects Select→stats shortcut; Select is still forwarded to the host. */
static SDL_TimerID stats_hold_timer = 0;

static void release_buttons(stream_input_t *input, app_gamepad_state_t *gamepad);

static bool gamepad_combo_check(int buttons, short combo);

static bool sensor_state_needs_update(const app_gamepad_sensor_state_t *state, uint32_t timestamp,
                                      const float data[3]);

static bool vmouse_intercepted(stream_input_t *input, const app_gamepad_state_t *gamepad);

static bool filter_deadzone_2axis(stream_input_t *input, short *x, short *y);

static void stream_input_send_unannounced_gamepads(stream_input_t *input);

static void stream_input_send_buttons(stream_input_t *input, app_gamepad_state_t *gamepad);

static void stream_input_send_gamepad_battery(stream_input_t *input, app_gamepad_state_t *gamepad, bool force);

static void cancel_stats_hold(void);

static void cancel_all_holds(void);

static Uint32 stats_hold_timer_cb(Uint32 interval, void *param);

static bool stream_input_gamepad_sends_moonlight(const stream_input_t *input,
                                                 const app_gamepad_state_t *gamepad) {
    (void) input;
    return !input->view_only && gamepad != NULL;
}

static uint16_t stream_input_moonlight_active_mask(const stream_input_t *input)
{
    return (uint16_t) input->input->activeGamepadMask;
}

void stream_input_handle_cbutton(stream_input_t *input, const SDL_ControllerButtonEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }

    /* Physical Y/Triangle opens the soft keyboard only while virtual mouse is active. */
    if (event->type == SDL_CONTROLLERBUTTONDOWN && event->button == SDL_CONTROLLER_BUTTON_Y
        && session_input_is_vmouse_active(&input->vmouse)) {
        cancel_all_holds();
        bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
        return;
    }

    int button = 0;
    switch (event->button) {
        case SDL_CONTROLLER_BUTTON_A:
            button = app_configuration->swap_abxy ? B_FLAG : A_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            button = app_configuration->swap_abxy ? A_FLAG : B_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            button = app_configuration->swap_abxy ? X_FLAG : Y_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_X:
            button = app_configuration->swap_abxy ? Y_FLAG : X_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            button = UP_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            button = DOWN_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            button = RIGHT_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            button = LEFT_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_BACK:
            button = BACK_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_START:
            button = PLAY_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_GUIDE:
            button = SPECIAL_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            button = LS_CLK_FLAG;
            vmouse_set_modifier(&input->vmouse, event->state == SDL_PRESSED);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            button = RS_CLK_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            button = LB_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            button = RB_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_TOUCHPAD:
            button = TOUCHPAD_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_MISC1:
            button = MISC_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE1:
            button = PADDLE1_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE2:
            button = PADDLE2_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE3:
            button = PADDLE3_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE4:
            button = PADDLE4_FLAG;
            break;
        default:
            return;
    }
    if (event->type == SDL_CONTROLLERBUTTONDOWN) {
        gamepad->buttons |= button;
    } else {
        gamepad->buttons &= ~button;
    }

    /* Quit overlay combo still uses chord + release. */
    if (gamepad_combo_check(gamepad->buttons, QUIT_BUTTONS)) {
        cancel_all_holds();
        quit_combo_pressed = true;
        return;
    }
    if (gamepad->buttons == 0 && quit_combo_pressed) {
        quit_combo_pressed = false;
        release_buttons(input, gamepad);
        bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
        return;
    }

    /* Select always goes to the host. Hold timer only arms the stats shortcut. */
    if (event->type == SDL_CONTROLLERBUTTONDOWN && button == BACK_FLAG) {
        cancel_stats_hold();
        stats_hold_timer = SDL_AddTimer(GAMEPAD_HOLD_STATS_MS, stats_hold_timer_cb, NULL);
    } else if (event->type == SDL_CONTROLLERBUTTONUP && button == BACK_FLAG) {
        cancel_stats_hold();
    } else if (button != BACK_FLAG) {
        /* Mixing other buttons with a pending hold cancels the shortcut. */
        cancel_stats_hold();
    }

    stream_input_send_buttons(input, gamepad);
}

void stream_input_handle_caxis(stream_input_t *input, const SDL_ControllerAxisEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    switch (event->axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: {
            gamepad->leftStickX = SDL_max(event->value, -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_LEFTY: {
            // Signed values have one more negative value than
            // positive value, so inverting the sign on -32768
            // could actually cause the value to overflow and
            // wrap around to be negative again. Avoid that by
            // capping the value at 32767.
            gamepad->leftStickY = (short) -SDL_max(event->value, (short) -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_RIGHTX: {
            gamepad->rightStickX = SDL_max(event->value, -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_RIGHTY: {
            gamepad->rightStickY = (short) -SDL_max(event->value, (short) -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: {
            gamepad->leftTrigger = (char) (event->value * 255UL / 32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
            gamepad->rightTrigger = (char) (event->value * 255UL / 32767);
            break;
        }
        default:
            return;
    }
    filter_deadzone_2axis(input, &gamepad->leftStickX, &gamepad->leftStickY);
    filter_deadzone_2axis(input, &gamepad->rightStickX, &gamepad->rightStickY);

    if (vmouse_intercepted(input, gamepad)) {
        vmouse_set_vector(&input->vmouse, gamepad->rightStickX, gamepad->rightStickY);
        vmouse_set_scroll(&input->vmouse, gamepad->leftStickX, gamepad->leftStickY);
        vmouse_set_trigger(&input->vmouse, gamepad->leftTrigger, gamepad->rightTrigger);
    }

    if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
        return;
    }

    if (vmouse_intercepted(input, gamepad)) {
        LiSendMultiControllerEvent(gamepad->gs_id, input->input->activeGamepadMask, gamepad->buttons, 0, 0,
                                   0, 0, 0, 0);
    } else {
        LiSendMultiControllerEvent(gamepad->gs_id, input->input->activeGamepadMask, gamepad->buttons,
                                   gamepad->leftTrigger,
                                   gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY,
                                   gamepad->rightStickX, gamepad->rightStickY);
    }
}

void stream_input_handle_csensor(stream_input_t *input, const SDL_ControllerSensorEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
        return;
    }
    switch (event->sensor) {
        case SDL_SENSOR_ACCEL: {
            if (sensor_state_needs_update(&gamepad->accelState, event->timestamp, event->data)) {
                gamepad->accelState.lastTimestamp = event->timestamp;
                memcpy(gamepad->accelState.data, event->data, sizeof(gamepad->accelState.data));
                LiSendControllerMotionEvent(gamepad->gs_id, LI_MOTION_TYPE_ACCEL, event->data[0], event->data[1],
                                            event->data[2]);
            }
            break;
        }
        case SDL_SENSOR_GYRO: {
            if (sensor_state_needs_update(&gamepad->gyroState, event->timestamp, event->data)) {
                gamepad->gyroState.lastTimestamp = event->timestamp;
                memcpy(gamepad->gyroState.data, event->data, sizeof(gamepad->gyroState.data));
                // Convert rad/s to deg/s
                LiSendControllerMotionEvent(gamepad->gs_id, LI_MOTION_TYPE_GYRO,
                                            event->data[0] * 57.2957795f,
                                            event->data[1] * 57.2957795f,
                                            event->data[2] * 57.2957795f);
            }
            break;
        }
        default: {
            return;
        }
    }
}

void stream_input_handle_ctouchpad(stream_input_t *input, const SDL_ControllerTouchpadEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
        return;
    }
    if (event->touchpad != 0) {
        return;
    }
    uint8_t event_type;
    switch (event->type) {
        case SDL_CONTROLLERTOUCHPADUP: {
            event_type = LI_TOUCH_EVENT_UP;
            break;
        }
        case SDL_CONTROLLERTOUCHPADDOWN: {
            event_type = LI_TOUCH_EVENT_DOWN;
            break;
        }
        case SDL_CONTROLLERTOUCHPADMOTION: {
            event_type = LI_TOUCH_EVENT_MOVE;
            break;
        }
        default: {
            return;
        }
    }
    LiSendControllerTouchEvent(gamepad->gs_id, event_type, event->finger, event->x, event->y, event->pressure);
}

void stream_input_handle_cdevice(stream_input_t *input, const SDL_ControllerDeviceEvent *event) {
    if (event->type != SDL_CONTROLLERDEVICEREMOVED) {
        return;
    }
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
        return;
    }
    stream_input_send_gamepad_remove(input, gamepad);
}

void stream_input_handle_jdevice(stream_input_t *input, const SDL_JoyDeviceEvent *event) {
    app_gamepad_state_t *gamepad = NULL;
    if (event->type == SDL_JOYDEVICEADDED) {
#if SDL_VERSION_ATLEAST(2, 0, 6)
        SDL_JoystickID instance_id = SDL_JoystickGetDeviceInstanceID(event->which);
        if (instance_id < 0) {
            stream_input_send_unannounced_gamepads(input);
            return;
        }
        gamepad = app_input_gamepad_state_by_instance_id(input->input, instance_id);
        if (gamepad == NULL || input->view_only) {
            return;
        }
        stream_input_send_gamepad_arrive(input, gamepad);
#else
        stream_input_send_unannounced_gamepads(input);
#endif
    } else if (event->type == SDL_JOYDEVICEREMOVED) {
        gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
        if (gamepad == NULL || input->view_only) {
            return;
        }
        if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
            return;
        }
        stream_input_send_gamepad_remove(input, gamepad);
    }
}

void stream_input_send_gamepad_arrive(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
        return;
    }
    if (input->announcedGamepadMask & (1 << gamepad->gs_id)) {
        return;
    }
    input->announcedGamepadMask |= 1 << gamepad->gs_id;
    uint8_t type = LI_CTYPE_XBOX;
    uint16_t capabilities = LI_CCAP_ANALOG_TRIGGERS;
    commons_log_info("Input", "Controller %d arrived. Name: %s", gamepad->gs_id,
                     SDL_GameControllerName(gamepad->controller));
    switch (SDL_GameControllerGetType(gamepad->controller)) {
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
#endif
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: {
            type = LI_CTYPE_NINTENDO;
            capabilities &= ~LI_CCAP_ANALOG_TRIGGERS;
            break;
        }
        case SDL_CONTROLLER_TYPE_PS3: {
            type = LI_CTYPE_PS;
            break;
        }
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: {
            type = LI_CTYPE_PS;
            capabilities |= LI_CCAP_TOUCHPAD;
            commons_log_info("Input", "  controller capability: touchpad");
            break;
        }
        default: {
            break;
        }
    }
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (SDL_GameControllerHasRumble(gamepad->controller)) {
        capabilities |= LI_CCAP_RUMBLE;
        commons_log_info("Input", "  controller capability: rumble");
    }
    if (SDL_GameControllerHasRumbleTriggers(gamepad->controller)) {
        capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        commons_log_info("Input", "  controller capability: trigger rumble");
    }
#else
    capabilities |= LI_CCAP_RUMBLE;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        capabilities |= LI_CCAP_TRIGGER_RUMBLE;
#endif
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_GameControllerHasSensor(gamepad->controller, SDL_SENSOR_ACCEL)) {
        capabilities |= LI_CCAP_ACCEL;
        commons_log_info("Input", "  controller capability: accelerometer");
    }
    if (SDL_GameControllerHasSensor(gamepad->controller, SDL_SENSOR_GYRO)) {
        capabilities |= LI_CCAP_GYRO;
        commons_log_info("Input", "  controller capability: gyroscope");
    }
    if (SDL_GameControllerHasLED(gamepad->controller)) {
        capabilities |= LI_CCAP_RGB_LED;
        commons_log_info("Input", "  controller capability: RGB LED");
    }
#endif
    if (input->report_gamepad_battery) {
        SDL_Joystick *js = SDL_GameControllerGetJoystick(gamepad->controller);
        if (js != NULL) {
            SDL_JoystickPowerLevel level = SDL_JoystickCurrentPowerLevel(js);
            if (level != SDL_JOYSTICK_POWER_UNKNOWN) {
                capabilities |= LI_CCAP_BATTERY_STATE;
                commons_log_info("Input", "  controller capability: battery");
            }
        }
    }
    LiSendControllerArrivalEvent(gamepad->gs_id, stream_input_moonlight_active_mask(input), type, 0xFFFFFFFF,
                                 capabilities);
    stream_input_send_gamepad_battery(input, gamepad, true);
}

static void stream_input_send_gamepad_battery(stream_input_t *input, app_gamepad_state_t *gamepad, bool force) {
    if (!input->report_gamepad_battery || gamepad == NULL || gamepad->controller == NULL) {
        return;
    }
    SDL_Joystick *js = SDL_GameControllerGetJoystick(gamepad->controller);
    if (js == NULL) {
        return;
    }
    SDL_JoystickPowerLevel level = SDL_JoystickCurrentPowerLevel(js);
    uint8_t batteryState;
    uint8_t batteryPercentage;
    switch (level) {
        case SDL_JOYSTICK_POWER_UNKNOWN:
            return;
        case SDL_JOYSTICK_POWER_EMPTY:
            batteryState = LI_BATTERY_STATE_DISCHARGING;
            batteryPercentage = 5;
            break;
        case SDL_JOYSTICK_POWER_LOW:
            batteryState = LI_BATTERY_STATE_DISCHARGING;
            batteryPercentage = 20;
            break;
        case SDL_JOYSTICK_POWER_MEDIUM:
            batteryState = LI_BATTERY_STATE_DISCHARGING;
            batteryPercentage = 50;
            break;
        case SDL_JOYSTICK_POWER_FULL:
            batteryState = LI_BATTERY_STATE_FULL;
            batteryPercentage = 90;
            break;
        case SDL_JOYSTICK_POWER_WIRED:
            batteryState = LI_BATTERY_STATE_CHARGING;
            batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
            break;
        default:
            return;
    }
    if (!force && gamepad->lastBatteryState == batteryState &&
        gamepad->lastBatteryPercentage == batteryPercentage) {
        return;
    }
    gamepad->lastBatteryState = batteryState;
    gamepad->lastBatteryPercentage = batteryPercentage;
    LiSendControllerBatteryEvent((uint8_t) gamepad->gs_id, batteryState, batteryPercentage);
}

void stream_input_send_gamepad_remove(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (input->view_only || gamepad == NULL) {
        return;
    }
    if ((input->announcedGamepadMask & (1 << gamepad->gs_id)) == 0) {
        return;
    }
    input->announcedGamepadMask &= ~(1 << gamepad->gs_id);
    uint16_t activeGamepadMask =
            stream_input_moonlight_active_mask(input) & (uint16_t) ~(1u << (unsigned) gamepad->gs_id);
    commons_log_info("Input", "Controller %d removed (Moonlight mask 0x%x)", gamepad->gs_id, activeGamepadMask);
    LiSendMultiControllerEvent(gamepad->gs_id, (short) activeGamepadMask, 0, 0, 0, 0, 0, 0, 0);
}

static void stream_input_send_unannounced_gamepads(stream_input_t *input) {
    if (input->view_only) {
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

static void stream_input_send_buttons(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
        return;
    }
    LiSendMultiControllerEvent(gamepad->gs_id, input->input->activeGamepadMask, gamepad->buttons, gamepad->leftTrigger,
                               gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX,
                               gamepad->rightStickY);
    stream_input_send_gamepad_battery(input, gamepad, false);
}

static void cancel_stats_hold(void) {
    if (stats_hold_timer) {
        SDL_RemoveTimer(stats_hold_timer);
        stats_hold_timer = 0;
    }
}

static void cancel_all_holds(void) {
    cancel_stats_hold();
}

static Uint32 stats_hold_timer_cb(Uint32 interval, void *param) {
    (void) interval;
    (void) param;
    stats_hold_timer = 0;
    bus_pushevent(USER_TOGGLE_STATS_PIN, NULL, NULL);
    commons_log_info("Input", "Select held %dms — toggle performance stats", GAMEPAD_HOLD_STATS_MS);
    return 0;
}

static void release_buttons(stream_input_t *input, app_gamepad_state_t *gamepad) {
    gamepad->buttons = 0;
    gamepad->leftTrigger = 0;
    gamepad->rightTrigger = 0;
    gamepad->leftStickX = 0;
    gamepad->leftStickY = 0;
    gamepad->rightStickX = 0;
    gamepad->rightStickY = 0;
    cancel_all_holds();
    if (!stream_input_gamepad_sends_moonlight(input, gamepad)) {
        return;
    }
    LiSendMultiControllerEvent(gamepad->gs_id, input->input->activeGamepadMask, gamepad->buttons, gamepad->leftTrigger,
                               gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX,
                               gamepad->rightStickY);
}


static bool gamepad_combo_check(int buttons, short combo) {
    return (buttons & combo) == combo;
}

static bool sensor_state_needs_update(const app_gamepad_sensor_state_t *state, uint32_t timestamp,
                                      const float data[3]) {
    if (state->periodMs == 0 || !SDL_TICKS_PASSED(timestamp, state->lastTimestamp + state->periodMs)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (state->data[i] != data[i]) {
            return true;
        }
    }
    return false;
}

static bool vmouse_intercepted(stream_input_t *input, const app_gamepad_state_t *gamepad) {
    if (!session_input_is_vmouse_active(&input->vmouse)) {
        return false;
    }
    return gamepad->rightStickX != 0 || gamepad->rightStickY != 0 || gamepad->leftStickX != 0 ||
           gamepad->leftStickY != 0 || gamepad->leftTrigger != 0 || gamepad->rightTrigger != 0;
}

static bool filter_deadzone_2axis(stream_input_t *input, short *x, short *y) {
    uint32_t magnitude_pow2 = (uint32_t) (*x) * (*x) + (uint32_t) (*y) * (*y);
    uint32_t threshold_sqrt = 32768 * input->stick_deadzone / 100;
    if (magnitude_pow2 < threshold_sqrt * threshold_sqrt) {
        *x = 0;
        *y = 0;
        return true;
    }
    return false;
}
