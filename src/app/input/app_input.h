#pragma once

#include <SDL_haptic.h>
#include <SDL_joystick.h>
#include <SDL_version.h>
#include "config.h"
#include "gamecontrollerdb_updater.h"

#include "lvgl.h"
#include "lvgl/input/lv_drv_sdl_key.h"

#if TARGET_WEBOS
#include "platform/webos/dualsense_usb.h"
#endif

typedef struct app_t app_t;

typedef struct app_gamepad_sensor_state_t {
    uint32_t periodMs;
    uint32_t lastTimestamp;
    float data[3];
} app_gamepad_sensor_state_t;

typedef struct app_gamepad_state_t {
    SDL_JoystickID instance_id;
    SDL_GameController *controller;
#if FEATURE_GAMEPAD_TOUCHPAD_GRAB
    struct gamepad_touchpad_t *touchpad;
#endif // FEATURE_GAMEPAD_TOUCHPAD_GRAB
    SDL_JoystickGUID guid;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    uint32_t serial_crc;
#endif
    short gs_id;
    char leftTrigger, rightTrigger;
    short leftStickX, leftStickY;
    short rightStickX, rightStickY;
    int buttons;
#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_Haptic *haptic;
    int haptic_effect_id;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    app_gamepad_sensor_state_t accelState;
    app_gamepad_sensor_state_t gyroState;
#endif
    uint8_t lastBatteryState;
    uint8_t lastBatteryPercentage;
#if TARGET_WEBOS
    /** Wired DualSense hidraw feedback; NULL when BT or open failed. */
    dualsense_usb_t *ds_usb;
#endif
} app_gamepad_state_t;

typedef struct app_input_t {
    struct app_t *app;
    commons_gcdb_updater_t gcdb_updater;
    SDL_Surface *blank_cursor_surface;
    size_t max_num_gamepads;
    app_gamepad_state_t gamepads[16];
    size_t gamepads_count;
    short activeGamepadMask;
} app_input_t;


void app_input_init(app_input_t *input, app_t *app);

void app_input_deinit(app_input_t *input);

void app_input_handle_event(app_input_t *input, const SDL_Event *event);
