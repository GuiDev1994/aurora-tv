#include "host_failure.h"

#include <stdio.h>
#include <string.h>

#include <Limelight.h>

bool host_failure_ended_stream(int termination_code) {
    switch (termination_code) {
        case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        case ML_ERROR_PROTECTED_CONTENT:
        case ML_ERROR_FRAME_CONVERSION:
            return true;
        default:
            return termination_code < -0xFFFF;
    }
}

void host_failure_append(char *message, size_t size, const char *line) {
    if (message == NULL || size == 0 || line == NULL || line[0] == '\0') {
        return;
    }
    size_t len = strnlen(message, size - 1);
    snprintf(message + len, size - len, len > 0 ? "\n\n%s" : "%s", line);
}
