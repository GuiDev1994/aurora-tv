#pragma once

#include <stdbool.h>
#include "ui/config.h"

#include "lvgl.h"
#include "lv_sdl_img.h"

#include "backend/pcmanager.h"

typedef struct app_t app_t;
typedef struct app_launch_params_t app_launch_params_t;

typedef struct launcher_fragment_args_t {
    app_t *app;
    const app_launch_params_t *params;
} launcher_fragment_args_t;

typedef struct launcher_fragment_t {
    lv_fragment_t base;

    app_t *global;

    /* Top-bar (formerly the side nav). Contains logo + Aurora label on the left,
     * and the action buttons on the right. */
    lv_obj_t *nav;
    /* Detail area below the top bar; full width, hosts the apps fragment (game rail). */
    lv_obj_t *detail;

    /* Server selector button shown in the top bar. Click opens the server popup
     * (full PC list with status icons, long-press for context menu). The label
     * inside the button reflects the currently selected PC. */
    lv_obj_t *server_btn;
    lv_obj_t *server_label;

    lv_obj_t *add_btn, *pref_btn, *help_btn, *quit_btn;

    /* Focus groups: nav_group for top-bar buttons, detail_group for game rail items. */
    lv_group_t *nav_group, *detail_group;

    /* Style applied to top-bar action buttons (icon-only). */
    lv_style_t topbar_btn_style;

    bool pane_initialized;
    bool first_created;
    /* Used by apps.controller while it (re)builds the rail to suppress spurious focus
     * callbacks; preserved from the previous architecture. */
    bool detail_changing;

    const app_launch_params_t *launch_params;
    bool def_host_selected;
    bool def_app_requested;

    /* Full-screen overlay over the game area (below the top bar). Hosts embedded Settings. */
    lv_obj_t *settings_layer;
    lv_fragment_t *settings_fragment;
} launcher_fragment_t;


lv_obj_t *launcher_win_create(lv_fragment_t *self, lv_obj_t *parent);

launcher_fragment_t *launcher_instance();

void launcher_select_server(launcher_fragment_t *controller, const uuidstr_t *uuid);

/* Refresh the visible server-button label from the currently selected PC (or fall
 * back to a generic "Select server" placeholder when nothing is selected). */
void launcher_refresh_server_label(launcher_fragment_t *controller);

/** After closing launcher-embedded UI, point keypad/gamepad focus back at the main bar. */
void launcher_restore_nav_focus(launcher_fragment_t *controller);

extern const lv_fragment_class_t launcher_controller_class;
