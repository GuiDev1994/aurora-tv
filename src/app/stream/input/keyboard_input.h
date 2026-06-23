#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct stream_input_t stream_input_t;

/** Sticky modifier state shared by soft keyboard and quick actions. */
typedef struct keyboard_modifier_state_t {
    bool shift;
    bool ctrl;
    bool alt;
    bool win;
} keyboard_modifier_state_t;

typedef struct keyboard_combo_step_t {
    short vk;
    char modifiers; /* MODIFIER_* flags for down/up; 0 for modifier keys themselves */
    bool down;
} keyboard_combo_step_t;

bool keyboard_input_available(const stream_input_t *input);

char keyboard_input_mods_to_flags(const keyboard_modifier_state_t *mods, bool syskey_capture);

void keyboard_input_send_key(stream_input_t *input, short keyCode, bool keyDown,
                             const keyboard_modifier_state_t *mods, bool syskey_capture);

void keyboard_input_send_modifier(stream_input_t *input, short vk, bool down);

void keyboard_input_force_release_all_modifiers(stream_input_t *input, bool syskey_capture);

void keyboard_input_release_toggles(stream_input_t *input, keyboard_modifier_state_t *mods,
                                    bool syskey_capture);

/** Send an ordered combo; always force-releases modifiers afterward. */
void keyboard_input_send_combo(stream_input_t *input, const keyboard_combo_step_t *steps, size_t count,
                             bool syskey_capture);
