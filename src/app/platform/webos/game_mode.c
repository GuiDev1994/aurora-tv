#include "game_mode.h"

#include "lunasynccall.h"
#include "logging.h"

#include <pbnjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HBCHANNEL_SERVICE_DIR \
    "/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service"
#define URI_HBCHANNEL_EXEC "luna://org.webosbrew.hbchannel.service/exec"

typedef struct applied_setting {
    const char *category;
    const char *key;
    char *restore_to;
} applied_setting_t;

struct webos_game_mode_state {
    applied_setting_t *items;
    size_t count;
    size_t capacity;
};

static bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool webos_game_mode_is_rooted(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = path_is_dir(HBCHANNEL_SERVICE_DIR) ? 1 : 0;
        commons_log_info("GameMode", "webosbrew root probe: %s",
                         cached ? "rooted (hbchannel service present)" : "not rooted");
    }
    return cached == 1;
}

static char *jstring_dup(jvalue_ref v) {
    if (!jis_string(v)) {
        return NULL;
    }
    raw_buffer buf = jstring_get(v);
    if (buf.m_str == NULL) {
        return NULL;
    }
    return strndup(buf.m_str, buf.m_len);
}

static bool json_return_value_true(const char *json) {
    if (json == NULL) {
        return false;
    }
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema, 0);
    if (parser == NULL || !jdomparser_feed(parser, json, (int) strlen(json)) || !jdomparser_end(parser)) {
        if (parser) {
            jdomparser_release(&parser);
        }
        return false;
    }
    jvalue_ref root = jdomparser_get_result(parser);
    jvalue_ref rv = jobject_get(root, J_CSTR_TO_BUF("returnValue"));
    bool ok = false;
    if (jis_boolean(rv)) {
        jboolean_get(rv, &ok);
    }
    jdomparser_release(&parser);
    return ok;
}

static char *exec_root_luna(const char *method, const char *inner_body_json) {
    char command[1024];
    int n = snprintf(command, sizeof(command),
                     "luna-send -n 1 luna://com.webos.settingsservice/%s '%s'",
                     method, inner_body_json);
    if (n < 0 || (size_t) n >= sizeof(command)) {
        commons_log_error("GameMode", "luna-send command too long");
        return NULL;
    }

    jvalue_ref payload = jobject_create_var(
            jkeyval(J_CSTR_TO_JVAL("command"), jstring_create(command)),
            J_END_OBJ_DECL);
    if (!jis_valid(payload)) {
        return NULL;
    }
    const char *payload_str = jvalue_tostring_simple(payload);
    char *reply = NULL;
    bool called = HLunaServiceCallSync(URI_HBCHANNEL_EXEC, payload_str, true, &reply);
    j_release(&payload);
    if (!called || reply == NULL) {
        commons_log_warn("GameMode", "hbchannel exec call failed for %s", method);
        free(reply);
        return NULL;
    }
    if (!json_return_value_true(reply)) {
        commons_log_warn("GameMode", "hbchannel exec rejected: %s", reply);
        free(reply);
        return NULL;
    }

    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema, 0);
    char *stdout_s = NULL;
    if (parser && jdomparser_feed(parser, reply, (int) strlen(reply)) && jdomparser_end(parser)) {
        jvalue_ref root = jdomparser_get_result(parser);
        stdout_s = jstring_dup(jobject_get(root, J_CSTR_TO_BUF("stdoutString")));
    }
    if (parser) {
        jdomparser_release(&parser);
    }
    free(reply);
    return stdout_s;
}

static char *get_setting(const char *category, const char *key) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"category\":\"%s\",\"keys\":[\"%s\"]}", category, key);
    char *out = exec_root_luna("getSystemSettings", body);
    if (out == NULL) {
        return NULL;
    }
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema, 0);
    char *result = NULL;
    if (parser && jdomparser_feed(parser, out, (int) strlen(out)) && jdomparser_end(parser)) {
        jvalue_ref root = jdomparser_get_result(parser);
        jvalue_ref settings = jobject_get(root, J_CSTR_TO_BUF("settings"));
        if (jis_object(settings)) {
            result = jstring_dup(jobject_get(settings, j_cstr_to_buffer(key)));
        }
    }
    if (parser) {
        jdomparser_release(&parser);
    }
    free(out);
    return result;
}

static bool set_setting(const char *category, const char *key, const char *value) {
    char body[320];
    snprintf(body, sizeof(body),
             "{\"category\":\"%s\",\"settings\":{\"%s\":\"%s\"}}",
             category, key, value);
    char *out = exec_root_luna("setSystemSettings", body);
    if (out == NULL) {
        return false;
    }
    bool ok = json_return_value_true(out);
    if (!ok) {
        commons_log_warn("GameMode", "setSystemSettings rejected: %s", out);
    }
    free(out);
    return ok;
}

static void state_push(webos_game_mode_state_t *state, const char *category, const char *key,
                       char *restore_to) {
    if (state->count >= state->capacity) {
        size_t ncap = state->capacity ? state->capacity * 2 : 4;
        applied_setting_t *ni = realloc(state->items, ncap * sizeof(*ni));
        if (ni == NULL) {
            free(restore_to);
            return;
        }
        state->items = ni;
        state->capacity = ncap;
    }
    state->items[state->count++] = (applied_setting_t){
            .category = category,
            .key = key,
            .restore_to = restore_to,
    };
}

static void apply_one(webos_game_mode_state_t *state, const char *category, const char *key,
                      const char *value) {
    char *previous = get_setting(category, key);
    if (!set_setting(category, key, value)) {
        commons_log_warn("GameMode", "failed to set %s.%s=%s", category, key, value);
        free(previous);
        return;
    }
    commons_log_info("GameMode", "%s.%s -> %s (was %s)", category, key, value,
                     previous ? previous : "(unknown)");
    char *restore = NULL;
    if (previous != NULL && strcmp(previous, value) != 0) {
        restore = previous;
        previous = NULL;
    }
    free(previous);
    state_push(state, category, key, restore);
}

static void apply_one_any(webos_game_mode_state_t *state, const char *category, const char *key,
                          const char *const *values, size_t nvalues) {
    for (size_t i = 0; i < nvalues; i++) {
        if (values[i] == NULL || values[i][0] == '\0') {
            continue;
        }
        char *previous = get_setting(category, key);
        if (set_setting(category, key, values[i])) {
            commons_log_info("GameMode", "%s.%s -> %s (was %s)", category, key, values[i],
                             previous ? previous : "(unknown)");
            char *restore = NULL;
            if (previous != NULL && strcmp(previous, values[i]) != 0) {
                restore = previous;
                previous = NULL;
            }
            free(previous);
            state_push(state, category, key, restore);
            return;
        }
        free(previous);
    }
    commons_log_warn("GameMode", "no accepted value for %s.%s (skipped)", category, key);
}

webos_game_mode_state_t *webos_game_mode_enter(bool hdr) {
    if (!webos_game_mode_is_rooted()) {
        return NULL;
    }
    webos_game_mode_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    apply_one(state, "picture", "pictureMode", hdr ? "hdrGame" : "game");
    apply_one(state, "sound", "soundMode", "game");
    apply_one(state, "picture", "peakBrightness", "high");

    static const char *off_vals[] = {"off", "Off", "false", "0", "disabled", "none"};
    static const char *on_vals[] = {"on", "On", "true", "1", "enabled"};
    static const char *energy_vals[] = {"off", "Off", "minimum", "min", "false", "0"};

    apply_one_any(state, "picture", "truMotionMode", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "oledMotionPro", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "realCinema", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "motionEyeCare", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "noiseReduction", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "mpegNoiseReduction", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "dynamicContrast", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "dynamicColor", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "superResolution", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "aiPictureMode", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "aiBrightness", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "hdrDynamicToneMapping", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "energySaving", energy_vals, sizeof(energy_vals) / sizeof(energy_vals[0]));
    apply_one_any(state, "sound", "autoVolume", off_vals, sizeof(off_vals) / sizeof(off_vals[0]));
    apply_one_any(state, "picture", "instantGameResponse", on_vals, sizeof(on_vals) / sizeof(on_vals[0]));
    apply_one_any(state, "picture", "gameOptimiser", on_vals, sizeof(on_vals) / sizeof(on_vals[0]));

    if (state->count == 0) {
        free(state);
        return NULL;
    }
    return state;
}

void webos_game_mode_restore(webos_game_mode_state_t *state) {
    if (state == NULL) {
        return;
    }
    /* Reverse order so pictureMode/soundMode come back last. */
    for (size_t i = state->count; i > 0; i--) {
        applied_setting_t *a = &state->items[i - 1];
        if (a->restore_to == NULL) {
            continue;
        }
        if (set_setting(a->category, a->key, a->restore_to)) {
            commons_log_info("GameMode", "restored %s.%s=%s", a->category, a->key, a->restore_to);
        } else {
            commons_log_warn("GameMode", "restore failed %s.%s=%s", a->category, a->key,
                             a->restore_to);
        }
        free(a->restore_to);
    }
    free(state->items);
    free(state);
}
