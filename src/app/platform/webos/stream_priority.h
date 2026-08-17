#pragma once

#include <stdbool.h>

/**
 * Rooted webOS stream boost: CPU governor, page lock, nice, net buffers.
 * Does not apply SCHED_FIFO — that starved the compositor and caused hitch.
 */
typedef struct webos_stream_priority_state webos_stream_priority_state_t;

webos_stream_priority_state_t *webos_stream_priority_enter(void);

void webos_stream_priority_leave(webos_stream_priority_state_t *state);
