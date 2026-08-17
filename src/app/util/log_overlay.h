#pragma once

#include <stdbool.h>
#include <lvgl.h>

#include "logging.h"

typedef enum log_overlay_state {
    LOG_OVERLAY_OFF = 0,
    LOG_OVERLAY_LIVE = 1,
    LOG_OVERLAY_FROZEN = 2,
} log_overlay_state_t;

void log_overlay_init(void);

void log_overlay_deinit(void);

/** Yellow-button cycle: Off → Live → Frozen → Off. */
void log_overlay_cycle(void);

log_overlay_state_t log_overlay_get_state(void);

/** Settings toggle: true → Live, false → Off. */
void log_overlay_set_enabled(bool enabled);

/**
 * Re-apply Settings show_logs when entering a stream (panel may have been
 * destroyed / buried; also restores Live if Yellow left it Frozen).
 */
void log_overlay_reassert(void);

bool log_overlay_is_enabled(void);

/** commons_log listener — push a formatted line into the ring when capturing. */
void log_overlay_on_log(commons_log_level level, const char *tag, const char *message);
