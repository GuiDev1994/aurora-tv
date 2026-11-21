#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Whether a termination code means the host itself ended the stream, so a Rubylight host may say
 * why (RustHostLastFailure): its termination reasons as moonlight-common-c maps them, and HRESULTs
 * passed through as they are (0x80004005). Lost connections and this device's own failures are not.
 */
bool host_failure_ended_stream(int termination_code);

/** Appends line to message (size bytes, NUL-terminated) after a blank line, or as the message when it is empty. */
void host_failure_append(char *message, size_t size, const char *line);
