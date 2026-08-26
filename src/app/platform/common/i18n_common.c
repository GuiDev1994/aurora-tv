#include "util/i18n.h"

#include <string.h>

static const i18n_entry_t i18n_locales[] = {
        {"auto", translatable("System Language")},
        {"en-US", "English"},
        {"cs", "Čeština"},
        {"de", "Deutsch"},
        {"es", "Español"},
        {"fr", "Français"},
        {"it", "Italiano"},
#if DEBUG
        {"ja", "日本語"},
#endif
        {"ko", "조선말"},
        {"nl", "Dutch"},
        {"pl", "Polski"},
        {"pt-BR", "Português (Brasil)"},
        {"ro", "Română"},
        {"ru", "Русский"},
        {"zh-CN", "简体中文",
#if OS_WINDOWS
                .font =        "Microsoft YaHei"
#endif
        },
        {NULL, NULL},
};

const i18n_entry_t *i18n_entry_at(int index) {
    return &i18n_locales[index];
}

const i18n_entry_t *i18n_entry(const char *locale) {
    if (!locale) return NULL;
    char locale_tmp[16];
    strncpy(locale_tmp, locale, sizeof(locale_tmp) - 1);
    locale_tmp[sizeof(locale_tmp) - 1] = '\0';
    char *dot = strchr(locale_tmp, '.');
    if (dot) {
        *dot = '\0';
    }
    char *at = strchr(locale_tmp, '@');
    if (at) {
        *at = '\0';
    }
    char *current_pos = strchr(locale_tmp, '_');
    while (current_pos) {
        *current_pos = '-';
        current_pos = strchr(current_pos, '_');
    }
    for (int i = 0; i18n_locales[i].locale; i++) {
        const char *item_loc = i18n_locales[i].locale;
        if (strchr(item_loc, '-')) {
            if (strcasecmp(locale_tmp, item_loc) == 0) {
                return &i18n_locales[i];
            }
        } else if (strncasecmp(locale_tmp, item_loc, 2) == 0) {
            return &i18n_locales[i];
        }
    }
    /* TV/SDL often reports language-only "pt". Map it onto pt-BR (or es-ES, …). */
    if (strlen(locale_tmp) >= 2) {
        for (int i = 0; i18n_locales[i].locale; i++) {
            const char *item_loc = i18n_locales[i].locale;
            if (strchr(item_loc, '-') && strncasecmp(locale_tmp, item_loc, 2) == 0) {
                return &i18n_locales[i];
            }
        }
    }
    return NULL;
}
