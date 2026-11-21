#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Lookups in the JSON document json[0..len) by path, a dot-separated list of object keys
 * ("restart.available"). A key that is absent, or a document malformed on the way there, is missing.
 */

/** Stores the boolean at path in out; false when it is missing or not a boolean. */
bool gs_json_bool(const char *json, size_t len, const char *path, bool *out);

/** The string at path, unescaped to UTF-8 and malloc'd; NULL when it is missing or not a string. */
char *gs_json_string(const char *json, size_t len, const char *path);
