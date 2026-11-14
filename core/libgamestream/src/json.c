#include "json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32

typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static bool skip_value(cursor_t *c, int depth);

static void skip_ws(cursor_t *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

/* At the opening quote; leaves c past the closing one. */
static bool skip_string(cursor_t *c) {
    c->p++;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char) *c->p++;
        if (ch == '"') {
            return true;
        }
        if (ch == '\\') {
            if (c->p >= c->end) {
                return false;
            }
            c->p++;
        } else if (ch < 0x20) {
            return false;
        }
    }
    return false;
}

static bool skip_literal(cursor_t *c, const char *literal) {
    size_t len = strlen(literal);
    if ((size_t) (c->end - c->p) < len || memcmp(c->p, literal, len) != 0) {
        return false;
    }
    c->p += len;
    return true;
}

static bool skip_number(cursor_t *c) {
    if (c->p < c->end && *c->p == '-') {
        c->p++;
    }
    const char *digits = c->p;
    while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' || *c->p == 'e' || *c->p == 'E' ||
                             *c->p == '+' || *c->p == '-')) {
        c->p++;
    }
    return c->p > digits;
}

/* Reads an object member's key and leaves c past its colon. */
static bool read_key(cursor_t *c, const char **key, size_t *key_len) {
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    *key = c->p + 1;
    if (!skip_string(c)) {
        return false;
    }
    *key_len = (size_t) (c->p - 1 - *key);
    skip_ws(c);
    if (c->p >= c->end || *c->p != ':') {
        return false;
    }
    c->p++;
    return true;
}

static bool skip_container(cursor_t *c, int depth, char close, bool object) {
    if (depth > MAX_DEPTH) {
        return false;
    }
    c->p++;
    skip_ws(c);
    if (c->p < c->end && *c->p == close) {
        c->p++;
        return true;
    }
    while (c->p < c->end) {
        const char *key;
        size_t key_len;
        if (object && !read_key(c, &key, &key_len)) {
            return false;
        }
        if (!skip_value(c, depth + 1)) {
            return false;
        }
        skip_ws(c);
        if (c->p >= c->end) {
            return false;
        }
        if (*c->p == ',') {
            c->p++;
        } else if (*c->p == close) {
            c->p++;
            return true;
        } else {
            return false;
        }
    }
    return false;
}

static bool skip_value(cursor_t *c, int depth) {
    skip_ws(c);
    if (c->p >= c->end) {
        return false;
    }
    switch (*c->p) {
        case '"':
            return skip_string(c);
        case '{':
            return skip_container(c, depth, '}', true);
        case '[':
            return skip_container(c, depth, ']', false);
        case 't':
            return skip_literal(c, "true");
        case 'f':
            return skip_literal(c, "false");
        case 'n':
            return skip_literal(c, "null");
        default:
            return skip_number(c);
    }
}

/* Leaves c at the first character of the value at path. */
static bool find(cursor_t *c, const char *path) {
    const char *segment = path;
    for (int depth = 0; depth <= MAX_DEPTH; depth++) {
        skip_ws(c);
        if (*segment == '\0') {
            return c->p < c->end;
        }
        const char *dot = strchr(segment, '.');
        size_t segment_len = dot != NULL ? (size_t) (dot - segment) : strlen(segment);
        if (c->p >= c->end || *c->p != '{') {
            return false;
        }
        c->p++;
        bool found = false;
        while (!found) {
            const char *key;
            size_t key_len;
            if (!read_key(c, &key, &key_len)) {
                return false;
            }
            if (key_len == segment_len && memcmp(key, segment, segment_len) == 0) {
                found = true;
            } else {
                if (!skip_value(c, depth + 1)) {
                    return false;
                }
                skip_ws(c);
                if (c->p >= c->end || *c->p != ',') {
                    return false;
                }
                c->p++;
            }
        }
        segment = dot != NULL ? dot + 1 : segment + segment_len;
    }
    return false;
}

/* Leaves at on the first character of the well-formed value at path. */
static bool locate(const char *json, size_t len, const char *path, cursor_t *at) {
    if (json == NULL || path == NULL) {
        return false;
    }
    cursor_t c = {json, json + len};
    if (!find(&c, path)) {
        return false;
    }
    *at = c;
    return skip_value(&c, 0);
}

bool gs_json_bool(const char *json, size_t len, const char *path, bool *out) {
    cursor_t at;
    if (!locate(json, len, path, &at) || (*at.p != 't' && *at.p != 'f')) {
        return false;
    }
    *out = *at.p == 't';
    return true;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool read_hex4(const char *p, const char *end, uint32_t *out) {
    if (end - p < 4) {
        return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hex_value(p[i]);
        if (digit < 0) {
            return false;
        }
        value = value << 4 | (uint32_t) digit;
    }
    *out = value;
    return true;
}

static size_t put_utf8(char *out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char) (0xC0 | cp >> 6);
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char) (0xE0 | cp >> 12);
        out[1] = (char) (0x80 | (cp >> 6 & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | cp >> 18);
    out[1] = (char) (0x80 | (cp >> 12 & 0x3F));
    out[2] = (char) (0x80 | (cp >> 6 & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

char *gs_json_string(const char *json, size_t len, const char *path) {
    cursor_t at;
    if (!locate(json, len, path, &at) || *at.p != '"') {
        return NULL;
    }
    const char *p = at.p + 1;
    skip_string(&at);
    const char *end = at.p - 1;
    /* An escape never grows: \uXXXX is 6 bytes for at most 3, a surrogate pair 12 for 4. */
    char *out = malloc((size_t) (end - p) + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t n = 0;
    while (p < end) {
        if (*p != '\\') {
            out[n++] = *p++;
            continue;
        }
        p++;
        char esc = *p++;
        switch (esc) {
            case '"':
            case '\\':
            case '/':
                out[n++] = esc;
                break;
            case 'b':
                out[n++] = '\b';
                break;
            case 'f':
                out[n++] = '\f';
                break;
            case 'n':
                out[n++] = '\n';
                break;
            case 'r':
                out[n++] = '\r';
                break;
            case 't':
                out[n++] = '\t';
                break;
            case 'u': {
                uint32_t cp;
                if (!read_hex4(p, end, &cp)) {
                    free(out);
                    return NULL;
                }
                p += 4;
                uint32_t low;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u' &&
                    read_hex4(p + 2, end, &low) && low >= 0xDC00 && low < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    p += 6;
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                n += put_utf8(out + n, cp);
                break;
            }
            default:
                free(out);
                return NULL;
        }
    }
    out[n] = '\0';
    return out;
}
