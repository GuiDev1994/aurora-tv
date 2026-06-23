#pragma once

#include <lvgl.h>
#include <stdbool.h>

typedef struct stream_input_t stream_input_t;
typedef struct session_t session_t;

/** Create and show the soft keyboard overlay (Xbox-style). */
lv_obj_t *soft_keyboard_create(lv_obj_t *parent, session_t *session,
                               void (*on_close_cb)(void *userdata), void *userdata);

/** Get the focus group for the keyboard. Call app_input_set_group with this when keyboard opens. */
lv_group_t *soft_keyboard_get_group(lv_obj_t *keyboard_container);

/** Focus the key matrix (for gamepad navigation). */
void soft_keyboard_focus_keys(lv_obj_t *keyboard_container);
