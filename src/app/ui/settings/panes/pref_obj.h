#pragma once

#include <lvgl.h>

/** Row-bound controls are not separate D-pad stops (parent row owns focus). */
#define PREF_ROW_BOUND_FLAG LV_OBJ_FLAG_USER_1

typedef struct pref_dropdown_int_entry_t {
    const char *name;
    int value;
    bool fallback;
} pref_dropdown_int_entry_t;

typedef struct pref_dropdown_int_pair_entry_t {
    const char *name;
    int value_a, value_b;
    bool fallback;
} pref_dropdown_int_pair_entry_t;

typedef struct pref_dropdown_string_entry_t {
    const char *name;
    const char *value;
    bool fallback;
} pref_dropdown_string_entry_t;

typedef bool(*pref_int_pair_write_predicate)(lv_obj_t *, int, int);

lv_obj_t *pref_pane_container(lv_obj_t *parent);

/**
 * punktfunk-style focus row: full-width horizontal strip with a left label.
 * The row is the sole D-pad focus target; bind the control with pref_row_bind_control().
 */
lv_obj_t *pref_focus_row(lv_obj_t *parent, const char *title);

bool pref_obj_is_focus_row(const lv_obj_t *obj);

lv_obj_t *pref_row_get_control(const lv_obj_t *row);

void pref_row_bind_control(lv_obj_t *row, lv_obj_t *control);

lv_obj_t *pref_checkbox(lv_obj_t *parent, const char *title, bool *value, bool reverse);

/** Toggle checkbox state and write back (Enter / explicit click only). */
void pref_checkbox_toggle(lv_obj_t *checkbox);

/** Disable LVGL arrow-key toggling; use click/Enter to change state. */
void pref_checkbox_prepare_for_dpad(lv_obj_t *checkbox);

lv_obj_t *pref_dropdown_int(lv_obj_t *parent, const pref_dropdown_int_entry_t *entries, size_t num_entries, int *value,
                            bool(*write_predicate)(int));

lv_obj_t *pref_dropdown_int_pair(lv_obj_t *parent, const pref_dropdown_int_pair_entry_t *entries, size_t num_entries,
                                 int *value_a, int *value_b, pref_int_pair_write_predicate write_predicate);

lv_obj_t *pref_dropdown_string(lv_obj_t *parent, const pref_dropdown_string_entry_t *entries, size_t num_entries,
                               char **value);

lv_obj_t *pref_slider(lv_obj_t *parent, int *value, int min, int max, int step);

lv_obj_t *pref_slider_set_value(lv_obj_t *parent, int value);

lv_obj_t *pref_title_label(lv_obj_t *parent, const char *title);

lv_obj_t *pref_desc_label(lv_obj_t *parent, const char *title, bool focusable);

lv_obj_t *pref_header(lv_obj_t *parent, const char *title);