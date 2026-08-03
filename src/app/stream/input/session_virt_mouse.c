#include "session_virt_mouse.h"
#include "session_input.h"

#include <SDL_stdinc.h>
#include <math.h>

/** Tuned for smooth but usable left-stick scrolling (not page jumps). */
#define VMOUSE_SCROLL_SENSITIVITY 1.85
#define VMOUSE_SCROLL_DEADZONE    5500
#define VMOUSE_SCROLL_MAX_TICK    58
#define VMOUSE_SCROLL_DIVIDER     14.0

static short calc_mouse_movement(short axis);

static short calc_scroll_delta(short axis);

static short clamp_scroll_tick(double value);

static Uint32 vmouse_timer_callback(Uint32 interval, void *param);

void session_input_set_vmouse_active(session_input_vmouse_t *vmouse, bool active) {
    vmouse->state.active = active;
    if (!active) {
        vmouse_set_vector(vmouse, 0, 0);
        vmouse_set_scroll(vmouse, 0, 0);
        vmouse_set_trigger(vmouse, 0, 0);
    }
}

bool session_input_is_vmouse_active(session_input_vmouse_t *vmouse) {
    return vmouse->state.active;
}

void vmouse_set_vector(session_input_vmouse_t *vmouse, short x, short y) {
    vmouse->state.x = calc_mouse_movement(x);
    vmouse->state.y = calc_mouse_movement((short) -SDL_max(y, -32767));
    vmouse_update_timer(vmouse);
}

void vmouse_set_scroll(session_input_vmouse_t *vmouse, short x, short y) {
    vmouse->state.scroll_x = calc_scroll_delta(x);
    vmouse->state.scroll_y = calc_scroll_delta(y);
    vmouse_update_timer(vmouse);
}

void vmouse_set_trigger(session_input_vmouse_t *vmouse, char l, char r) {
    const char trigger_threshold = 64;
    bool ldown = l > trigger_threshold, rdown = r > trigger_threshold;
    if (vmouse->state.l != ldown) {
        LiSendMouseButtonEvent(ldown ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }
    if (vmouse->state.r != rdown) {
        LiSendMouseButtonEvent(rdown ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    }
    vmouse->state.l = ldown;
    vmouse->state.r = rdown;
}

void vmouse_set_modifier(session_input_vmouse_t *vmouse, bool v) {
    (void) v;
}

void vmouse_update_timer(session_input_vmouse_t *vmouse) {
    if (vmouse->state.x || vmouse->state.y || vmouse->state.scroll_x || vmouse->state.scroll_y) {
        if (!vmouse->timer_id) {
            vmouse->timer_id = SDL_AddTimer(0, vmouse_timer_callback, vmouse);
        }
    } else if (vmouse->timer_id) {
        SDL_RemoveTimer(vmouse->timer_id);
        vmouse->timer_id = 0;
    }
}

static Uint32 vmouse_timer_callback(Uint32 interval, void *param) {
    (void) interval;
    session_input_vmouse_t *vmouse = param;
    if (!vmouse->state.active) {
        return 0;
    }

    short speed = 4;
    double speed_divider = 32 - SDL_max(0, SDL_min(speed, 16));
    double x = vmouse->state.x / speed_divider;
    double y = vmouse->state.y / speed_divider;
    double abs_x = SDL_fabs(x), abs_y = SDL_fabs(y);

    if (vmouse->state.x || vmouse->state.y) {
        LiSendMouseMoveEvent((short) (abs_x > 1 ? x : x / abs_x), (short) (abs_y > 1 ? y : y / abs_y));
    }

    if (vmouse->state.scroll_x || vmouse->state.scroll_y) {
        double scroll_x = vmouse->state.scroll_x / VMOUSE_SCROLL_DIVIDER;
        double scroll_y = vmouse->state.scroll_y / VMOUSE_SCROLL_DIVIDER;
        short sy = clamp_scroll_tick(scroll_y);
        short sx = clamp_scroll_tick(scroll_x);
        if (sy != 0) {
            /* Positive high-res scroll = wheel up / content moves down naturally with stick-up. */
            LiSendHighResScrollEvent(sy);
        }
        if (sx != 0) {
            LiSendHighResHScrollEvent(sx);
        }
    }

    double move_activity = SDL_max(abs_x, abs_y);
    double scroll_activity = SDL_max(SDL_fabs(vmouse->state.scroll_x), SDL_fabs(vmouse->state.scroll_y))
                             / VMOUSE_SCROLL_DIVIDER;
    double activity = SDL_max(move_activity, scroll_activity);
    if (activity <= 0) {
        return 0;
    }
    if (move_activity <= 0 && scroll_activity > 0) {
        return (Uint32) SDL_max(12, SDL_min(28, (int) (18 / SDL_max(0.25, scroll_activity))));
    }
    return (Uint32) SDL_max(5, SDL_min(5 / activity, 20));
}

static short calc_mouse_movement(short axis) {
    short abs_axis = (short) (axis > 0 ? axis : -axis);
    short threshold = 4096;
    if (abs_axis < threshold) { return 0; }
    return (short) (SDL_sqrt(abs_axis - threshold) * (axis > 0 ? 1 : -1));
}

static short calc_scroll_delta(short axis) {
    short abs_axis = (short) (axis > 0 ? axis : -axis);
    if (abs_axis < VMOUSE_SCROLL_DEADZONE) {
        return 0;
    }
    double t = (abs_axis - VMOUSE_SCROLL_DEADZONE) / (32767.0 - VMOUSE_SCROLL_DEADZONE);
    if (t > 1.0) {
        t = 1.0;
    }
    double curved = t * t * VMOUSE_SCROLL_SENSITIVITY;
    short mag = (short) (curved * 160.0);
    if (mag < 1) {
        mag = 1;
    }
    return (short) (axis > 0 ? mag : -mag);
}

static short clamp_scroll_tick(double value) {
    if (value == 0) {
        return 0;
    }
    short v = (short) value;
    if (v == 0) {
        v = value > 0 ? 1 : -1;
    }
    if (v > VMOUSE_SCROLL_MAX_TICK) {
        return VMOUSE_SCROLL_MAX_TICK;
    }
    if (v < -VMOUSE_SCROLL_MAX_TICK) {
        return (short) -VMOUSE_SCROLL_MAX_TICK;
    }
    return v;
}
