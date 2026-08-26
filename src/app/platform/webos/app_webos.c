#include "app.h"
#include "app_webos.h"
#include "app_launch.h"

#include <pbnjson.h>
#include <stdio.h>
#include <string.h>

#include "logging.h"
#include "util/path.h"
#include "util/i18n.h"

#include "lunasynccall.h"

static char locale_system[16];

static bool apply_locale_tag(const char *tag) {
    if (!tag || !tag[0]) {
        return false;
    }
    const i18n_entry_t *entry = i18n_entry(tag);
    const char *use = (entry && strcmp(entry->locale, "auto") != 0) ? entry->locale : tag;
    commons_log_info("APP", "Trying locale '%s' (from '%s')", use, tag);
    i18n_setlocale(use);
    return i18n_is_loaded();
}

/* webOS system UI locale. Backport SDL often returns nothing from GetPreferredLocales. */
static bool parse_ui_locale_json(const char *payload) {
    JSchemaInfo schema_info;
    jschema_info_init(&schema_info, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema_info, 0);
    bool ok = false;
    if (parser && jdomparser_feed(parser, payload, (int) strlen(payload)) && jdomparser_end(parser)) {
        jvalue_ref root = jdomparser_get_result(parser);
        jvalue_ref settings = jobject_get(root, J_CSTR_TO_BUF("settings"));
        if (jis_object(settings)) {
            jvalue_ref locale_info = jobject_get(settings, J_CSTR_TO_BUF("localeInfo"));
            jvalue_ref locales = jis_object(locale_info) ? jobject_get(locale_info, J_CSTR_TO_BUF("locales"))
                                                           : jobject_get(settings, J_CSTR_TO_BUF("locales"));
            if (jis_object(locales)) {
                jvalue_ref ui = jobject_get(locales, J_CSTR_TO_BUF("UI"));
                if (jis_string(ui)) {
                    raw_buffer buf = jstring_get(ui);
                    snprintf(locale_system, sizeof(locale_system), "%.*s", (int) buf.m_len, buf.m_str);
                    locale_system[sizeof(locale_system) - 1] = '\0';
                    commons_log_info("APP", "webOS UI locale: %s", locale_system);
                    ok = apply_locale_tag(locale_system);
                }
            }
        }
    }
    if (parser) {
        jdomparser_release(&parser);
    }
    return ok;
}

static bool apply_webos_system_locale(void) {
    static const char *bodies[] = {
            "{\"keys\":[\"localeInfo\"]}",
            "{\"category\":\"option\",\"keys\":[\"localeInfo\"]}",
    };
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        char *payload = NULL;
        if (!HLunaServiceCallSync("luna://com.webos.settingsservice/getSystemSettings",
                                 bodies[i], true, &payload) || !payload) {
            free(payload);
            continue;
        }
        bool ok = parse_ui_locale_json(payload);
        free(payload);
        if (ok) {
            return true;
        }
    }
    return false;
}

void app_open_url(const char *url) {
    SDL_OpenURL(url);
}

void app_init_locale() {
    if (app_configuration->language[0] && strcmp(app_configuration->language, "auto") != 0) {
        commons_log_debug("APP", "Override language to %s", app_configuration->language);
        i18n_setlocale(app_configuration->language);
        return;
    }
    if (apply_webos_system_locale()) {
        return;
    }
    SDL_Locale *locales = SDL_GetPreferredLocales();
    if (locales) {
        for (int i = 0; locales[i].language; i++) {
            if (locales[i].country) {
                snprintf(locale_system, sizeof(locale_system), "%s-%s", locales[i].language, locales[i].country);
            } else {
                strncpy(locale_system, locales[i].language, sizeof(locale_system));
            }
            locale_system[sizeof(locale_system) - 1] = '\0';
            if (apply_locale_tag(locale_system)) {
                SDL_free(locales);
                return;
            }
        }
        SDL_free(locales);
    }
    const char *lang = SDL_getenv("LANG");
    if (lang && lang[0] && apply_locale_tag(lang)) {
        return;
    }
    commons_log_warn("APP", "No translated locale loaded; UI stays in English");
}

app_launch_params_t *app_handle_launch(app_t *app, int argc, char *argv[]) {
    (void) app;
    if (argc < 2) {
        return NULL;
    }
    commons_log_info("APP", "Launched with parameters %s", argv[1]);
    JSchemaInfo schema_info;
    jschema_info_init(&schema_info, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema_info, 0);
    if (!jdomparser_feed(parser, argv[1], (int) strlen(argv[1]))) {
        jdomparser_release(&parser);
        return NULL;
    }
    if (!jdomparser_end(parser)) {
        jdomparser_release(&parser);
        return NULL;
    }
    jvalue_ref params_obj = jdomparser_get_result(parser);
    if (!jis_valid(params_obj)) {
        jdomparser_release(&parser);
        return NULL;
    }
    app_launch_params_t *params = calloc(1, sizeof(app_launch_params_t));
    jvalue_ref host_uuid = jobject_get(params_obj, J_CSTR_TO_BUF("host_uuid"));
    if (jis_string(host_uuid)) {
        raw_buffer buf = jstring_get(host_uuid);
        uuidstr_fromchars(&params->default_host_uuid, buf.m_len, buf.m_str);
    }
    jvalue_ref host_app_id = jobject_get(params_obj, J_CSTR_TO_BUF("host_app_id"));
    if (jis_number(host_app_id)) {
        if (jnumber_get_i32(host_app_id, &params->default_app_id) != CONV_OK) {
            params->default_app_id = 0;
        }
    } else if (jis_string(host_app_id)) {
        raw_buffer buf = jstring_get(host_app_id);
        char *id_str = strndup(buf.m_str, buf.m_len);
        params->default_app_id = strtol(id_str, NULL, 10);
        if (params->default_app_id < 0) {
            params->default_app_id = 0;
        }
        free(id_str);
    }
    jdomparser_release(&parser);
    return params;
}

void app_webos_open_ribbon() {
    static const char *payload = "{\"id\":\"com.webos.app.home\",\"params\":{\"launchType\":\"homeKey\"}}";
    HLunaServiceCallSync("luna://com.webos.applicationManager/launch", payload, true, NULL);
}