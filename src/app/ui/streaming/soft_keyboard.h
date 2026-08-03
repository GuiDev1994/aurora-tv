#pragma once

#include <lvgl.h>
#include <stdbool.h>

typedef struct stream_input_t stream_input_t;
typedef struct session_t session_t;

typedef enum {
    SOFT_KBD_GP_TOGGLE_LAYER = 1,
    SOFT_KBD_GP_ARROW_LEFT,
    SOFT_KBD_GP_ARROW_RIGHT,
} soft_kbd_gp_action_t;

/** Create and show the soft keyboard overlay (Windows-style). */
lv_obj_t *soft_keyboard_create(lv_obj_t *parent, session_t *session,
                               void (*on_close_cb)(void *userdata), void *userdata);

/** Get the focus group for the keyboard. Call app_input_set_group with this when keyboard opens. */
lv_group_t *soft_keyboard_get_group(lv_obj_t *keyboard_container);

/** Focus the key matrix (for gamepad navigation). */
void soft_keyboard_focus_keys(lv_obj_t *keyboard_container);

/**
 * Gamepad shortcuts while the soft keyboard is open (LT / LB / RB).
 * Returns true if the action was handled. Arrow actions honor pressed/released.
 */
bool soft_keyboard_gamepad_shortcut(soft_kbd_gp_action_t action, bool pressed);
