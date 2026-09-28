#pragma once

/**
 * USB keyboard EVIOCGRAB on webOS: take /dev/input/event* keyboards away from
 * surface-manager so F-keys / Home / Insert reach the stream (punktfunk approach).
 * Magic Remote stays on SDL (name denylist LGE*).
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct keyboard_evdev_t keyboard_evdev_t;

typedef void (*keyboard_evdev_key_fn)(short vk, bool down, char modifiers, void *userdata);

/** Start reader thread; returns NULL if no keyboard node could be opened. */
keyboard_evdev_t *keyboard_evdev_start(keyboard_evdev_key_fn cb, void *userdata);

void keyboard_evdev_stop(keyboard_evdev_t *kbd);

/** True if a HID keyboard event was seen recently (use to drop SDL echoes). */
bool keyboard_evdev_busy(const keyboard_evdev_t *kbd);
