#include "session_pointer_gesture.h"

#include "app.h"
#include "stream/input/session_input.h"
#include "stream/session_priv.h"

#include <Limelight.h>
#include <SDL.h>

#define GESTURE_LONG_PRESS_MS 1000
#define GESTURE_DRAG_SLOP_PX  10

static bool pointer_gesture_available(const stream_input_t *input) {
    if (input->view_only) {
        return false;
    }
    if (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) {
        return false;
    }
    return true;
}

static bool pointer_gesture_eligible(const stream_input_t *input, int which) {
    if (which == SDL_TOUCH_MOUSEID) {
        return true;
    }
#if defined(TARGET_WEBOS)
    (void) input;
    if (!app_get_mouse_relative()) {
        return true;
    }
#else
    (void) input;
    (void) which;
#endif
    return false;
}

static void pointer_gesture_send_position(const stream_input_t *input, int x, int y) {
    LiSendMousePositionEvent((short) x, (short) y,
                             (short) input->session->display_width,
                             (short) input->session->display_height);
}

static void pointer_gesture_begin_drag(stream_input_t *input, int x, int y) {
    input->pointerGestureDragging = true;
    pointer_gesture_send_position(input, x, y);
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    input->pointerGestureLeftDown = true;
}

static int pointer_gesture_distance_sq(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    return dx * dx + dy * dy;
}

void pointer_gesture_reset(stream_input_t *input) {
    if (input->started && input->pointerGestureLeftDown) {
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }
    input->pointerGestureActive = false;
    input->pointerGestureDragging = false;
    input->pointerGestureLeftDown = false;
    input->pointerGesturePressTime = 0;
    input->pointerGestureStartX = 0;
    input->pointerGestureStartY = 0;
}

bool pointer_gesture_handle_mbutton(stream_input_t *input, const SDL_MouseButtonEvent *event) {
    if (!pointer_gesture_available(input)) {
        return false;
    }

    if (event->button != SDL_BUTTON_LEFT) {
        if (event->state == SDL_PRESSED && input->pointerGestureActive) {
            pointer_gesture_reset(input);
        }
        return false;
    }

    if (!pointer_gesture_eligible(input, event->which)) {
        if (input->pointerGestureActive) {
            pointer_gesture_reset(input);
        }
        return false;
    }

    if (event->state == SDL_PRESSED) {
        input->pointerGestureActive = true;
        input->pointerGestureDragging = false;
        input->pointerGestureLeftDown = false;
        input->pointerGesturePressTime = event->timestamp != 0 ? event->timestamp : SDL_GetTicks();
        input->pointerGestureStartX = event->x;
        input->pointerGestureStartY = event->y;
        return true;
    }

    if (!input->pointerGestureActive) {
        return false;
    }

    uint32_t releasedAt = event->timestamp != 0 ? event->timestamp : SDL_GetTicks();
    uint32_t heldMs = releasedAt - input->pointerGesturePressTime;

    pointer_gesture_send_position(input, event->x, event->y);

    if (input->pointerGestureDragging) {
        if (input->pointerGestureLeftDown) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        }
    } else if (heldMs >= GESTURE_LONG_PRESS_MS) {
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    } else {
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }

    input->pointerGestureActive = false;
    input->pointerGestureDragging = false;
    input->pointerGestureLeftDown = false;
    input->pointerGesturePressTime = 0;
    return true;
}

bool pointer_gesture_handle_mmotion(stream_input_t *input, const SDL_MouseMotionEvent *event) {
    if (!input->pointerGestureActive || input->view_only) {
        return false;
    }
    if (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) {
        return false;
    }

    if (!input->pointerGestureDragging) {
        int slopSq = GESTURE_DRAG_SLOP_PX * GESTURE_DRAG_SLOP_PX;
        if (pointer_gesture_distance_sq(input->pointerGestureStartX, input->pointerGestureStartY,
                                        event->x, event->y) >= slopSq) {
            pointer_gesture_begin_drag(input, event->x, event->y);
            return true;
        }
        return false;
    }

    if (app_get_mouse_relative() && event->which != SDL_TOUCH_MOUSEID) {
        LiSendMouseMoveEvent((short) event->xrel, (short) event->yrel);
    } else {
        pointer_gesture_send_position(input, event->x, event->y);
    }
    return true;
}
