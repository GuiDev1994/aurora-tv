#include "keyboard_input.h"

#include "session_input.h"
#include "vk.h"

#include <Limelight.h>

bool keyboard_input_available(const stream_input_t *input) {
    return input != NULL && input->started && !input->view_only;
}

char keyboard_input_mods_to_flags(const keyboard_modifier_state_t *mods, bool syskey_capture) {
    char m = 0;
    if (mods == NULL) {
        return 0;
    }
    if (mods->shift) {
        m |= MODIFIER_SHIFT;
    }
    if (mods->ctrl) {
        m |= MODIFIER_CTRL;
    }
    if (mods->alt) {
        m |= MODIFIER_ALT;
    }
    if (mods->win && syskey_capture) {
        m |= MODIFIER_META;
    }
    return m;
}

void keyboard_input_send_key(stream_input_t *input, short keyCode, bool keyDown,
                             const keyboard_modifier_state_t *mods, bool syskey_capture) {
    if (!keyboard_input_available(input)) {
        return;
    }
    stream_input_send_key_event(input, keyCode, keyDown,
                                keyboard_input_mods_to_flags(mods, syskey_capture));
}

void keyboard_input_send_modifier(stream_input_t *input, short vk, bool down) {
    if (!keyboard_input_available(input)) {
        return;
    }
    stream_input_send_key_event(input, vk, down, 0);
}

void keyboard_input_force_release_all_modifiers(stream_input_t *input, bool syskey_capture) {
    if (input == NULL) {
        return;
    }
    keyboard_input_send_modifier(input, VK_LSHIFT, false);
    keyboard_input_send_modifier(input, VK_RSHIFT, false);
    keyboard_input_send_modifier(input, 0x10 /* VK_SHIFT (generic) */, false);
    keyboard_input_send_modifier(input, VK_LCONTROL, false);
    keyboard_input_send_modifier(input, VK_RCONTROL, false);
    keyboard_input_send_modifier(input, 0x11 /* VK_CONTROL (generic) */, false);
    keyboard_input_send_modifier(input, VK_LMENU, false);
    keyboard_input_send_modifier(input, VK_RMENU, false);
    keyboard_input_send_modifier(input, 0x12 /* VK_MENU (generic) */, false);
    if (syskey_capture) {
        keyboard_input_send_modifier(input, VK_LWIN, false);
        keyboard_input_send_modifier(input, VK_RWIN, false);
    }
}

void keyboard_input_release_toggles(stream_input_t *input, keyboard_modifier_state_t *mods,
                                    bool syskey_capture) {
    if (mods == NULL) {
        return;
    }
    if (mods->shift) {
        mods->shift = false;
        keyboard_input_send_modifier(input, VK_SHIFT, false);
        keyboard_input_send_modifier(input, VK_LSHIFT, false);
        keyboard_input_send_modifier(input, VK_RSHIFT, false);
    }
    if (mods->ctrl) {
        mods->ctrl = false;
        keyboard_input_send_modifier(input, VK_LCONTROL, false);
        keyboard_input_send_modifier(input, VK_RCONTROL, false);
    }
    if (mods->alt) {
        mods->alt = false;
        keyboard_input_send_modifier(input, VK_LMENU, false);
        keyboard_input_send_modifier(input, VK_RMENU, false);
    }
    if (mods->win && syskey_capture) {
        mods->win = false;
        keyboard_input_send_modifier(input, VK_LWIN, false);
        keyboard_input_send_modifier(input, VK_RWIN, false);
    }
}

void keyboard_input_send_combo(stream_input_t *input, const keyboard_combo_step_t *steps, size_t count,
                             bool syskey_capture) {
    if (!keyboard_input_available(input) || steps == NULL || count == 0) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (!keyboard_input_available(input)) {
            break;
        }
        stream_input_send_key_event(input, steps[i].vk, steps[i].down, steps[i].modifiers);
    }
    keyboard_input_force_release_all_modifiers(input, syskey_capture);
}
