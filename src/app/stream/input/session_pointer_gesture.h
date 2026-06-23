#pragma once

#include <stdbool.h>

#include <SDL_events.h>

typedef struct stream_input_t stream_input_t;

/** Reset any in-progress pointer press gesture. */
void pointer_gesture_reset(stream_input_t *input);

/**
 * Handle left-button press/release with long-press → right-click and drag detection.
 * @return true if the event was fully handled and must not be forwarded as a normal click.
 */
bool pointer_gesture_handle_mbutton(stream_input_t *input, const SDL_MouseButtonEvent *event);

/**
 * Update an active pointer press gesture on mouse motion.
 * @return true if motion was consumed by the gesture handler.
 */
bool pointer_gesture_handle_mmotion(stream_input_t *input, const SDL_MouseMotionEvent *event);
