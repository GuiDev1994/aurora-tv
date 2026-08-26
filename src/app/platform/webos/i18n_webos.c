#include "util/i18n.h"
#include "util/path.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <webosi18n_C.h>

#include "logging.h"

static ResBundleC *bundle = NULL;
static char locale_[16] = "\0";
static char language[4] = "\0";

/* Convert locale "pt-BR" to path format "pt/BR" to match PackageWebOS. */
static void locale_to_path(const char *locale, char *path, size_t path_size) {
    if (!locale || !path || path_size < 2) {
        return;
    }
    size_t i = 0, j = 0;
    while (locale[i] && j < path_size - 1) {
        path[j++] = (locale[i] == '-') ? '/' : locale[i];
        i++;
    }
    path[j] = '\0';
}

static char *strip_trailing_slash(char *path) {
    if (!path) {
        return path;
    }
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        path[--n] = '\0';
    }
    return path;
}

static bool bundle_is_translated(ResBundleC *b) {
    if (!b) {
        return false;
    }
    const char *probe = resBundle_getLocString(b, "[Localized Language]");
    if (probe != NULL && probe[0] && strcmp(probe, "[Localized Language]") != 0) {
        return true;
    }
    probe = resBundle_getLocString(b, "Cancel");
    return probe != NULL && strcmp(probe, "Cancel") != 0;
}

static bool try_load_bundle(const char *locale_tag, const char *root) {
    if (!locale_tag || !locale_tag[0] || !root || !root[0]) {
        return false;
    }
    ResBundleC *b = resBundle_createWithRootPath(locale_tag, "cstrings.json", root);
    if (!b) {
        return false;
    }
    if (!bundle_is_translated(b)) {
        commons_log_debug("I18N", "Bundle at %s/%s/cstrings.json is not translated", root, locale_tag);
        resBundle_destroy(b);
        return false;
    }
    if (bundle) {
        resBundle_destroy(bundle);
    }
    bundle = b;
    commons_log_info("I18N", "Loaded '%s' from %s/%s/cstrings.json", locale_tag, root, locale_tag);
    return true;
}

const char *locstr(const char *msgid) {
    if (!bundle) {
        return msgid;
    }
    const char *translated = resBundle_getLocString(bundle, msgid);
    return translated ? translated : msgid;
}

const char *i18n_locale() {
    return locale_;
}

bool i18n_is_loaded(void) {
    return bundle != NULL;
}

void i18n_setlocale(const char *locale) {
    if (bundle) {
        resBundle_destroy(bundle);
        bundle = NULL;
    }
    language[0] = '\0';
    locale_[0] = '\0';

    if (!locale || !locale[0] || strcmp(locale, "auto") == 0) {
        return;
    }

    size_t locale_len = strlen(locale) + 1;
    if (locale_len > sizeof(locale_)) {
        locale_len = sizeof(locale_);
    }
    SDL_memcpy(locale_, locale, locale_len);
    locale_[sizeof(locale_) - 1] = '\0';

    char locale_path[16];
    locale_to_path(locale, locale_path, sizeof(locale_path));

    char *base = SDL_GetBasePath();
    strip_trailing_slash(base);
    const char *home = SDL_getenv("HOME");

    const char *roots[3];
    int nroots = 0;
    if (base && base[0]) {
        roots[nroots++] = base;
    }
    if (home && home[0] && (!base || strcmp(home, base) != 0)) {
        roots[nroots++] = home;
    }

    bool loaded = false;
    for (int i = 0; i < nroots && !loaded; i++) {
        char *resources = path_join(roots[i], "resources");
        loaded = try_load_bundle(locale_path, resources);
        if (!loaded) {
            loaded = try_load_bundle(locale, resources);
        }
        if (!loaded) {
            loaded = try_load_bundle(locale, roots[i]);
        }
        free(resources);
    }
    if (base) {
        SDL_free(base);
    }

    if (!loaded) {
        commons_log_error("I18N", "Failed to load locale '%s'", locale);
        locale_[0] = '\0';
        return;
    }

    if (SDL_strlen(locale) > 2) {
        SDL_memcpy(language, locale, 2);
        language[2] = '\0';
    }
}

const char *app_get_locale_lang() {
    if (!language[0]) {
        return "C";
    }
    return language;
}
