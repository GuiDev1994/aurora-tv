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
#define HBCHANNEL_SERVICE_DIR_CRYPTO \
    "/media/cryptofs/apps/usr/palm/services/org.webosbrew.hbchannel.service"
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
    bool picture_mode_ok;
};

static bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool webos_game_mode_is_rooted(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = (path_is_dir(HBCHANNEL_SERVICE_DIR) || path_is_dir(HBCHANNEL_SERVICE_DIR_CRYPTO)) ? 1 : 0;
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

static char *exec_root_cmd(const char *command) {
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
        commons_log_warn("GameMode", "hbchannel exec call failed");
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

static char *exec_root_luna(const char *method, const char *inner_body_json) {
    char command[1400];
    int n = snprintf(command, sizeof(command),
                     "luna-send -n 1 luna://com.webos.settingsservice/%s '%s'",
                     method, inner_body_json);
    if (n < 0 || (size_t) n >= sizeof(command)) {
        commons_log_error("GameMode", "luna-send command too long");
        return NULL;
    }
    return exec_root_cmd(command);
}

static char *parse_json_string_path(const char *json, const char *obj_key, const char *field) {
    if (json == NULL) {
        return NULL;
    }
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema, 0);
    char *result = NULL;
    if (parser && jdomparser_feed(parser, json, (int) strlen(json)) && jdomparser_end(parser)) {
        jvalue_ref root = jdomparser_get_result(parser);
        jvalue_ref obj = jobject_get(root, j_cstr_to_buffer(obj_key));
        if (jis_object(obj)) {
            result = jstring_dup(jobject_get(obj, j_cstr_to_buffer(field)));
        }
    }
    if (parser) {
        jdomparser_release(&parser);
    }
    return result;
}

static char *get_config(const char *key) {
    char payload[384];
    snprintf(payload, sizeof(payload), "{\"configNames\":[\"%s\"]}", key);
    char *reply = NULL;
    if (HLunaServiceCallSync("luna://com.webos.service.config/getConfigs", payload, true, &reply) &&
        reply != NULL && json_return_value_true(reply)) {
        char *v = parse_json_string_path(reply, "configs", key);
        free(reply);
        if (v != NULL) {
            return v;
        }
    }
    free(reply);
    char command[512];
    snprintf(command, sizeof(command),
             "luna-send -n 1 luna://com.webos.service.config/getConfigs '{\"configNames\":[\"%s\"]}'",
             key);
    char *out = exec_root_cmd(command);
    char *v = parse_json_string_path(out, "configs", key);
    free(out);
    return v;
}

static bool set_config(const char *key, const char *value) {
    char payload[384];
    snprintf(payload, sizeof(payload), "{\"configs\":{\"%s\":\"%s\"}}", key, value);
    char *reply = NULL;
    if (HLunaServiceCallSync("luna://com.webos.service.config/setConfigs", payload, true, &reply) &&
        reply != NULL && json_return_value_true(reply)) {
        free(reply);
        return true;
    }
    free(reply);
    char command[512];
    snprintf(command, sizeof(command),
             "luna-send -n 1 luna://com.webos.service.config/setConfigs '{\"configs\":{\"%s\":\"%s\"}}'",
             key, value);
    char *out = exec_root_cmd(command);
    bool ok = json_return_value_true(out);
    if (!ok && out != NULL) {
        commons_log_warn("GameMode", "setConfigs %s=%s: %s", key, value, out);
    }
    free(out);
    return ok;
}

static char *get_setting(const char *category, const char *key) {
    if (category != NULL && strcmp(category, "config") == 0) {
        return get_config(key);
    }
    char body[320];
    snprintf(body, sizeof(body),
             "{\"category\":\"%s\",\"keys\":[\"%s\"]}", category, key);
    char *out = exec_root_luna("getSystemSettings", body);
    if (out == NULL || !json_return_value_true(out)) {
        snprintf(body, sizeof(body),
                 "{\"category\":\"%s\",\"keys\":[\"%s\"],\"current_app\":true}", category, key);
        free(out);
        out = exec_root_luna("getSystemSettings", body);
    }
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

static bool set_setting_body(const char *body, bool log_fail) {
    char *out = exec_root_luna("setSystemSettings", body);
    if (out == NULL) {
        return false;
    }
    bool ok = json_return_value_true(out);
    if (!ok && log_fail) {
        commons_log_warn("GameMode", "setSystemSettings rejected: %s", out);
    }
    free(out);
    return ok;
}

static bool set_setting(const char *category, const char *key, const char *value) {
    if (category != NULL && strcmp(category, "config") == 0) {
        return set_config(key, value);
    }
    char body[384];
    /* webOS 10.3 (C5): pictureMode/soundMode reject current_app ("???") / DBTYPE.
     * truMotion-style keys often need current_app. Try global first, then app. */
    snprintf(body, sizeof(body),
             "{\"category\":\"%s\",\"settings\":{\"%s\":\"%s\"}}",
             category, key, value);
    if (set_setting_body(body, false)) {
        return true;
    }
    snprintf(body, sizeof(body),
             "{\"category\":\"%s\",\"settings\":{\"%s\":\"%s\"},\"current_app\":true}",
             category, key, value);
    return set_setting_body(body, true);
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

static bool apply_one(webos_game_mode_state_t *state, const char *category, const char *key,
                      const char *value) {
    char *previous = get_setting(category, key);
    if (previous != NULL && strcmp(previous, value) == 0) {
        commons_log_info("GameMode", "%s.%s already %s", category, key, value);
        free(previous);
        return true;
    }
    if (!set_setting(category, key, value)) {
        commons_log_warn("GameMode", "failed to set %s.%s=%s", category, key, value);
        free(previous);
        return false;
    }
    commons_log_info("GameMode", "%s.%s -> %s (was %s)", category, key, value,
                     previous ? previous : "(unknown)");
    state_push(state, category, key, previous);
    return true;
}

static bool apply_one_any(webos_game_mode_state_t *state, const char *category, const char *key,
                          const char *const *values, size_t nvalues, bool try_all) {
    for (size_t i = 0; i < nvalues; i++) {
        if (values[i] == NULL || values[i][0] == '\0') {
            continue;
        }
        if (apply_one(state, category, key, values[i])) {
            return true;
        }
        if (!try_all) {
            break;
        }
    }
    commons_log_info("GameMode", "skipped %s.%s (not in this firmware/context)", category, key);
    return false;
}

webos_game_mode_state_t *webos_game_mode_enter(bool hdr) {
    if (!webos_game_mode_is_rooted()) {
        return NULL;
    }
    webos_game_mode_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }

    commons_log_info("GameMode", "app Game+IGR (not HDMI ALLM; overlay D= is NDL queue, unchanged)");
    /* Connecting UI is still SDR. hdrGame is rejected ("no matched extended item")
     * until HDR actually engages — start with SDR Game, then HDR aliases. */
    static const char *pic_sdr[] = {"game", "hdrGame", "dolbyHdrGame"};
    (void) hdr;
    state->picture_mode_ok = apply_one_any(state, "picture", "pictureMode",
                                           pic_sdr, sizeof(pic_sdr) / sizeof(pic_sdr[0]), true);

    static const char *sound_vals[] = {"game", "standard"};
    apply_one_any(state, "sound", "soundMode", sound_vals, sizeof(sound_vals) / sizeof(sound_vals[0]), true);

    if (state->picture_mode_ok) {
        static const char *off_vals[] = {"off"};
        static const char *on_vals[] = {"on"};
        apply_one_any(state, "picture", "truMotionMode", off_vals, 1, false);
        apply_one_any(state, "picture", "instantGameResponse", on_vals, 1, false);
        apply_one_any(state, "picture", "gameOptimiser", on_vals, 1, false);
    }

    if (state->count == 0) {
        commons_log_warn("GameMode", "no settings applied");
        free(state);
        return NULL;
    }
    return state;
}

webos_game_mode_state_t *webos_game_mode_lock_soc_hz(webos_game_mode_state_t *state, int hz) {
    if (!webos_game_mode_is_rooted() || hz < 60 || hz > 120) {
        return state;
    }
    if (state == NULL) {
        state = calloc(1, sizeof(*state));
        if (state == NULL) {
            return NULL;
        }
    }
    static const char *alts120[] = {"120Hz", "120"};
    const char *vals60[] = {"60Hz", "60"};
    const char *const *vals = (hz == 60) ? vals60 : alts120;
    size_t nvals = 2;
    if (apply_one_any(state, "config", "tv.hw.SoCOutputFrameRate", vals, nvals, true)) {
        commons_log_info("GameMode", "SoC output locked to %s (avoid 144Hz pulldown vs 120 fps)",
                         vals[0]);
    }
    return state;
}

void webos_game_mode_on_hdr(webos_game_mode_state_t *state, bool hdr) {
    if (state == NULL || !hdr) {
        return;
    }
    static const char *pic_hdr[] = {"hdrGame", "dolbyHdrGame"};
    if (apply_one_any(state, "picture", "pictureMode", pic_hdr, 2, true)) {
        static const char *peak[] = {"high", "medium"};
        apply_one_any(state, "picture", "peakBrightness", peak, 2, false);
        state->picture_mode_ok = true;
    }
}

void webos_game_mode_restore(webos_game_mode_state_t *state) {
    if (state == NULL) {
        return;
    }
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
