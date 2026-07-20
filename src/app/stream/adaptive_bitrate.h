#pragma once

#include "libgamestream/client.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ABR_MODE_BALANCED = 0,
    ABR_MODE_QUALITY = 1,
    ABR_MODE_LOW_LATENCY = 2,
} abr_mode_t;

typedef struct adaptive_bitrate_service adaptive_bitrate_service_t;

typedef struct {
    GS_CLIENT gs_client;
    const SERVER_DATA *server;
    int initial_bitrate;
    abr_mode_t mode;
    /** When true, only honor adaptive_bitrate_request_drop (no loss-based ramp). */
    bool recovery_only;
} adaptive_bitrate_config_t;

adaptive_bitrate_service_t *adaptive_bitrate_start(const adaptive_bitrate_config_t *config);

/* restore=false skips the bitrate-restore / server-disable HTTP round-trips --
 * used when the session ended in error/disconnect and the host is likely
 * unreachable (each call would otherwise block teardown on a timeout). */
void adaptive_bitrate_stop(adaptive_bitrate_service_t *service, bool restore);

/**
 * Ask the ABR thread to cut bitrate to percent_of_current (e.g. 75) and hold
 * upward probes for hold_ms. Safe to call from the video thread (async).
 */
bool adaptive_bitrate_request_drop(adaptive_bitrate_service_t *service, int percent_of_current,
                                   int hold_ms, const char *reason);

const char *abr_mode_to_string(abr_mode_t mode);
