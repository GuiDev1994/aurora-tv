#pragma once

/**
 * Wired DualSense feedback on webOS via /dev/hidraw* (USB only).
 *
 * Host (Vibepollo/Sunshine) still sends Moonlight 0x5503/LED packets; this module
 * applies them as full 0x02 output reports. Bluetooth pads need a separate Luna
 * path and are not claimed here. Rumble stays on SDL/EV_FF — reports never set
 * the compatible-vibration valid flag.
 */

#include <stdbool.h>
#include <stdint.h>

#include <SDL_gamecontroller.h>

typedef struct dualsense_usb_t dualsense_usb_t;

/** Open hidraw for this PS5 pad if it is on USB; NULL if Bluetooth / unavailable. */
dualsense_usb_t *dualsense_usb_open(SDL_GameController *controller);

void dualsense_usb_close(dualsense_usb_t *ds);

bool dualsense_usb_set_adaptive_triggers(dualsense_usb_t *ds, uint8_t eventFlags,
                                         uint8_t typeLeft, uint8_t typeRight,
                                         const uint8_t *left, const uint8_t *right);

bool dualsense_usb_set_lightbar(dualsense_usb_t *ds, uint8_t r, uint8_t g, uint8_t b);

bool dualsense_usb_set_player_led(dualsense_usb_t *ds, uint8_t ledValue);

bool dualsense_usb_set_mic_led(dualsense_usb_t *ds, uint8_t ledState);
