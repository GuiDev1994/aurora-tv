#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    GS_JSON_MISSING = 0,
    GS_JSON_NULL,
    GS_JSON_BOOL,
    GS_JSON_NUMBER,
    GS_JSON_STRING,
    GS_JSON_OBJECT,
    GS_JSON_ARRAY,
} GS_JSON_TYPE;

/**
 * Type of the value at path, a dot-separated list of object keys ("restart.available"), in the
 * JSON document json[0..len). GS_JSON_MISSING when a key is absent or the document is malformed
 * on the way there.
 */
GS_JSON_TYPE gs_json_type(const char *json, size_t len, const char *path);

/** The boolean at path, or fallback when it is missing or not a boolean. */
bool gs_json_bool(const char *json, size_t len, const char *path, bool fallback);

/** The string at path, unescaped to UTF-8 and malloc'd; NULL when it is missing or not a string. */
char *gs_json_string(const char *json, size_t len, const char *path);
