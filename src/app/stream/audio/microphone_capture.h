#pragma once

#include <stdbool.h>

typedef struct microphone_capture_t microphone_capture_t;

microphone_capture_t *microphone_capture_create(const char *device_name);

bool microphone_capture_start(microphone_capture_t *capture);

void microphone_capture_stop(microphone_capture_t *capture);

void microphone_capture_destroy(microphone_capture_t *capture);
