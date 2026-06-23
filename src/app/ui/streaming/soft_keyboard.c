/*
 * Soft keyboard overlay (Windows 11 / Xbox OSK layout) - sends keys to streaming host.
 * Layers: alphabetic (abc) and symbols (&123). F1-F12 row. Aurora theme colors.
 */

#include "soft_keyboard.h"
#include "app.h"
#include "config.h"
#include "stream/input/keyboard_input.h"
#include "stream/input/session_input.h"
#include "stream/input/vk.h"
#include "stream/session.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"

#include <Limelight.h>
#include <stdlib.h>
#include <string.h>

#define KBD_CLICK_DEDUPE_MS 80
#define KBD_BTN_W(w) (LV_BTNMATRIX_CTRL_POPOVER | (w))

#define KBD_ARROW_LEFT  "<"
#define KBD_ARROW_RIGHT ">"
#define KBD_ARROW_UP    "^"
#define KBD_ARROW_DOWN  "v"

typedef enum {
    KBD_LAYER_ALPHA,
    KBD_LAYER_SYMBOLS,
} kbd_layer_t;

typedef struct {
    lv_obj_t *container;
    lv_obj_t *btnm;
    lv_group_t *group;
    stream_input_t *input;
    keyboard_modifier_state_t mods;
    kbd_layer_t layer;
    bool upper_case;
    uint16_t last_click_btn_id;
    uint32_t last_click_tick;
    void (*on_close)(void *);
    void *on_close_userdata;
} soft_kbd_t;

/* --- Alphabetic lower --- */
static const char *alpha_map_lower[] = {
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "\n",
    "Esc","q","w","e","r","t","y","u","i","o","p","Bksp",
    "\n",
    "Tab","a","s","d","f","g","h","j","k","l",";","Enter",
    "\n",
    "Shift","z","x","c","v","b","n","m",",",".","/","Shift",
    "\n",
    "&123","Ctrl","Win","Alt","Space","Mic",KBD_ARROW_LEFT,KBD_ARROW_RIGHT,
    ""
};

static const char *alpha_map_upper[] = {
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "\n",
    "Esc","Q","W","E","R","T","Y","U","I","O","P","Bksp",
    "\n",
    "Tab","A","S","D","F","G","H","J","K","L",":","Enter",
    "\n",
    "Shift","Z","X","C","V","B","N","M",",",".","/","Shift",
    "\n",
    "&123","Ctrl","Win","Alt","Space","Mic",KBD_ARROW_LEFT,KBD_ARROW_RIGHT,
    ""
};

static const lv_btnmatrix_ctrl_t alpha_ctrl[] = {
    /* F-row: 12 equal */
    1,1,1,1,1,1,1,1,1,1,1,1,
    /* row 1 */
    1,1,1,1,1,1,1,1,1,1,1,KBD_BTN_W(2),
    /* row 2 */
    KBD_BTN_W(2),1,1,1,1,1,1,1,1,1,1,KBD_BTN_W(2),
    /* row 3 */
    KBD_BTN_W(2),1,1,1,1,1,1,1,1,1,1,KBD_BTN_W(2),
    /* row 4 */
    KBD_BTN_W(1),1,1,1,KBD_BTN_W(5),1,1,1,
};

static const short alpha_vks[] = {
    VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
    VK_ESCAPE,VK_Q,VK_W,VK_E,VK_R,VK_T,VK_Y,VK_U,VK_I,VK_O,VK_P,VK_BACK,
    VK_TAB,VK_A,VK_S,VK_D,VK_F,VK_G,VK_H,VK_J,VK_K,VK_L,VK_OEM_1,VK_RETURN,
    VK_SHIFT,VK_Z,VK_X,VK_C,VK_V,VK_B,VK_N,VK_M,VK_OEM_COMMA,VK_OEM_PERIOD,VK_OEM_2,VK_SHIFT,
    0,VK_LCONTROL,VK_LWIN,VK_LMENU,VK_SPACE,0,VK_LEFT,VK_RIGHT,
};

/* --- Symbols (&123) --- */
static const char *sym_map[] = {
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "\n",
    "Esc","1","2","3","4","5","6","7","8","9","0","Bksp",
    "\n",
    "Tab","!","@","#","$","^","&","-","_","=","+","Enter",
    "\n",
    KBD_ARROW_LEFT,KBD_ARROW_RIGHT,";",":","(",")","/","'","\"","?","Home",KBD_ARROW_UP,"End",
    "\n",
    "abc","Ctrl","Win","Alt","Space",",",".",KBD_ARROW_LEFT,KBD_ARROW_DOWN,KBD_ARROW_RIGHT,
    ""
};

static const lv_btnmatrix_ctrl_t sym_ctrl[] = {
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,KBD_BTN_W(2),
    KBD_BTN_W(2),1,1,1,1,1,1,1,1,1,1,KBD_BTN_W(2),
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    KBD_BTN_W(1),1,1,1,KBD_BTN_W(3),1,1,1,1,1,
};

/* true = apply MODIFIER_SHIFT when sending this sym_vks entry */
static const bool sym_shift[] = {
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,1,1,1,1,1,1,0,1,0,1,0,
    0,0,1,1,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
};

static const short sym_vks[] = {
    VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
    VK_ESCAPE,VK_1,VK_2,VK_3,VK_4,VK_5,VK_6,VK_7,VK_8,VK_9,VK_0,VK_BACK,
    VK_TAB,VK_1,VK_2,VK_3,VK_4,VK_6,VK_7,VK_OEM_MINUS,VK_OEM_MINUS,VK_OEM_PLUS,VK_OEM_PLUS,VK_RETURN,
    VK_LEFT,VK_RIGHT,VK_OEM_1,VK_OEM_1,VK_9,VK_0,VK_OEM_2,VK_OEM_7,VK_OEM_7,VK_OEM_2,VK_HOME,VK_UP,VK_END,
    0,VK_LCONTROL,VK_LWIN,VK_LMENU,VK_SPACE,VK_OEM_COMMA,VK_OEM_PERIOD,VK_LEFT,VK_DOWN,VK_RIGHT,
};

#define ALPHA_VK_COUNT ((int)(sizeof(alpha_vks) / sizeof(alpha_vks[0])))
#define SYM_VK_COUNT   ((int)(sizeof(sym_vks) / sizeof(sym_vks[0])))

#define BTN_LAYER_123  48
#define BTN_LAYER_ABC  49
#define BTN_SHIFTL     36
#define BTN_SHIFTR     47
#define BTN_CTRLL_A    49
#define BTN_WINL_A     50
#define BTN_ALTL_A     51
#define BTN_CTRLL_S    49
#define BTN_WINL_S     50
#define BTN_ALTL_S     51
#define BTN_MIC_A      53
#define BTN_MIC_S      53

static void kbd_set_btn_checked(lv_obj_t *btnm, uint16_t idx, bool checked) {
    if (checked) {
        lv_btnmatrix_set_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    } else {
        lv_btnmatrix_clear_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    }
}

static uint16_t kbd_modifier_indices(const soft_kbd_t *kbd, uint16_t *ctrl, uint16_t *win, uint16_t *alt) {
    if (kbd->layer == KBD_LAYER_ALPHA) {
        *ctrl = BTN_CTRLL_A;
        *win = BTN_WINL_A;
        *alt = BTN_ALTL_A;
        return BTN_SHIFTL;
    }
    *ctrl = BTN_CTRLL_S;
    *win = BTN_WINL_S;
    *alt = BTN_ALTL_S;
    return 0;
}

static void kbd_update_modifier_visuals(soft_kbd_t *kbd) {
    lv_obj_t *btnm = kbd->btnm;
    if (!btnm) {
        return;
    }
    uint16_t ctrl, win, alt;
    uint16_t shiftl = kbd_modifier_indices(kbd, &ctrl, &win, &alt);
    if (kbd->layer == KBD_LAYER_ALPHA) {
        kbd_set_btn_checked(btnm, BTN_SHIFTL, kbd->mods.shift);
        kbd_set_btn_checked(btnm, BTN_SHIFTR, kbd->mods.shift);
    }
    kbd_set_btn_checked(btnm, ctrl, kbd->mods.ctrl);
    kbd_set_btn_checked(btnm, win, kbd->mods.win);
    kbd_set_btn_checked(btnm, alt, kbd->mods.alt);
    (void) shiftl;
}

static void kbd_apply_controls(soft_kbd_t *kbd) {
    lv_obj_t *btnm = kbd->btnm;
    const lv_btnmatrix_ctrl_t *ctrl_map = (kbd->layer == KBD_LAYER_ALPHA) ? alpha_ctrl : sym_ctrl;
    size_t ctrl_len = (kbd->layer == KBD_LAYER_ALPHA) ? sizeof(alpha_ctrl) : sizeof(sym_ctrl);
    size_t ctrl_count = ctrl_len / sizeof(lv_btnmatrix_ctrl_t);
    lv_btnmatrix_set_ctrl_map(btnm, ctrl_map);
    lv_btnmatrix_set_btn_ctrl_all(btnm, LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT);
    for (size_t i = 0; i < ctrl_count; i++) {
        if (ctrl_map[i] & LV_BTNMATRIX_CTRL_POPOVER) {
            lv_btnmatrix_set_btn_ctrl(btnm, (uint16_t) i, LV_BTNMATRIX_CTRL_POPOVER);
        }
    }
    kbd_update_modifier_visuals(kbd);
}

static void kbd_refresh_map(soft_kbd_t *kbd) {
    if (!kbd->btnm) {
        return;
    }
    if (kbd->layer == KBD_LAYER_SYMBOLS) {
        lv_btnmatrix_set_map(kbd->btnm, sym_map);
    } else {
        lv_btnmatrix_set_map(kbd->btnm, kbd->upper_case ? alpha_map_upper : alpha_map_lower);
    }
    kbd_apply_controls(kbd);
}

typedef struct {
    soft_kbd_t *kbd;
    const short *vks;
    int vk_count;
} kbd_data_t;

static void kbd_send_key(soft_kbd_t *kbd, short vk, bool down) {
    keyboard_input_send_key(kbd->input, vk, down, &kbd->mods, app_configuration->syskey_capture);
}

static void kbd_send_key_shift(soft_kbd_t *kbd, short vk, bool down) {
    keyboard_modifier_state_t mods = kbd->mods;
    mods.shift = true;
    keyboard_input_send_key(kbd->input, vk, down, &mods, app_configuration->syskey_capture);
}

static void kbd_force_release(soft_kbd_t *kbd) {
    keyboard_input_force_release_all_modifiers(kbd->input, app_configuration->syskey_capture);
    memset(&kbd->mods, 0, sizeof(kbd->mods));
}

static void on_keyboard_click(lv_event_t *e) {
    kbd_data_t *kd = lv_event_get_user_data(e);
    uint32_t *btn_id_ptr = lv_event_get_param(e);
    uint16_t btn_id = (btn_id_ptr != NULL) ? (uint16_t) *btn_id_ptr
                                           : lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (btn_id == LV_BTNMATRIX_BTN_NONE) {
        return;
    }
    if (btn_id >= (uint16_t) kd->vk_count) {
        return;
    }

    soft_kbd_t *kbd = kd->kbd;
    short vk = kd->vks[btn_id];
    uint32_t now = lv_tick_get();
    if (btn_id == kbd->last_click_btn_id && (now - kbd->last_click_tick) < KBD_CLICK_DEDUPE_MS) {
        return;
    }
    kbd->last_click_btn_id = btn_id;
    kbd->last_click_tick = now;

  if (kbd->layer == KBD_LAYER_ALPHA && btn_id == BTN_LAYER_123) {
        kbd->layer = KBD_LAYER_SYMBOLS;
        kbd->upper_case = false;
        kd->vks = sym_vks;
        kd->vk_count = SYM_VK_COUNT;
        kbd_refresh_map(kbd);
        return;
    }
    if (kbd->layer == KBD_LAYER_SYMBOLS && btn_id == BTN_LAYER_ABC) {
        kbd->layer = KBD_LAYER_ALPHA;
        kd->vks = alpha_vks;
        kd->vk_count = ALPHA_VK_COUNT;
        kbd_refresh_map(kbd);
        return;
    }
    if (vk == 0) {
        return;
    }

    if (vk == VK_SHIFT) {
        kbd->mods.shift = !kbd->mods.shift;
        keyboard_input_send_modifier(kbd->input, VK_SHIFT, kbd->mods.shift);
        kbd->upper_case = kbd->mods.shift;
        kbd_refresh_map(kbd);
        return;
    }
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) {
        kbd->mods.ctrl = !kbd->mods.ctrl;
        keyboard_input_send_modifier(kbd->input, VK_LCONTROL, kbd->mods.ctrl);
        kbd_update_modifier_visuals(kbd);
        return;
    }
    if (vk == VK_LMENU || vk == VK_RMENU) {
        kbd->mods.alt = !kbd->mods.alt;
        keyboard_input_send_modifier(kbd->input, VK_LMENU, kbd->mods.alt);
        kbd_update_modifier_visuals(kbd);
        return;
    }
    if ((vk == VK_LWIN || vk == VK_RWIN) && app_configuration->syskey_capture) {
        kbd->mods.win = !kbd->mods.win;
        keyboard_input_send_modifier(kbd->input, VK_LWIN, kbd->mods.win);
        kbd_update_modifier_visuals(kbd);
        return;
    }

    if (kbd->layer == KBD_LAYER_SYMBOLS && btn_id < (uint16_t) SYM_VK_COUNT && sym_shift[btn_id]) {
        kbd_send_key_shift(kbd, vk, true);
        kbd_send_key_shift(kbd, vk, false);
    } else {
        kbd_send_key(kbd, vk, true);
        kbd_send_key(kbd, vk, false);
    }
    keyboard_input_release_toggles(kbd->input, &kbd->mods, app_configuration->syskey_capture);
    if (kbd->layer == KBD_LAYER_ALPHA) {
        kbd->upper_case = kbd->mods.shift;
        if (kbd->btnm) {
            lv_btnmatrix_set_map(kbd->btnm, kbd->upper_case ? alpha_map_upper : alpha_map_lower);
            kbd_apply_controls(kbd);
        }
    }
}

static void kbd_data_delete_cb(lv_event_t *e) {
    kbd_data_t *kd = lv_event_get_user_data(e);
    free(kd);
}

static void container_delete_cb(lv_event_t *e) {
    soft_kbd_t *kbd = lv_event_get_user_data(e);
    if (kbd) {
        kbd->btnm = NULL;
        kbd_force_release(kbd);
        stream_input_flush_pressed_keys(kbd->input);
        if (kbd->group) {
            lv_group_del(kbd->group);
        }
        free(kbd);
    }
}

lv_group_t *soft_keyboard_get_group(lv_obj_t *keyboard_container) {
    soft_kbd_t *kbd = lv_obj_get_user_data(keyboard_container);
    return kbd ? kbd->group : NULL;
}

void soft_keyboard_focus_keys(lv_obj_t *keyboard_container) {
    soft_kbd_t *kbd = keyboard_container ? lv_obj_get_user_data(keyboard_container) : NULL;
    if (kbd && kbd->btnm) {
        lv_group_focus_obj(kbd->btnm);
    }
}

static void kbd_apply_theme(lv_obj_t *btnm) {
    lv_obj_set_style_pad_all(btnm, LV_DPX(3), LV_PART_ITEMS);
    lv_obj_set_style_pad_gap(btnm, LV_DPX(5), 0);
    lv_obj_set_style_radius(btnm, LV_DPX(6), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(btnm, ml_color_hex(ML_COLOR_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(btnm, ml_color_hex(ML_COLOR_BORDER), LV_PART_ITEMS);
    lv_obj_set_style_border_width(btnm, LV_DPX(1), LV_PART_ITEMS);
    lv_obj_set_style_text_color(btnm, ml_color_hex(ML_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(btnm, ml_color_hex(ML_COLOR_SURFACE_HI), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btnm, ml_color_hex(ML_COLOR_PRIMARY), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btnm, ml_color_hex(ML_COLOR_PRIMARY), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0xd4820a), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0xd4820a),
                              LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0xd4820a),
                              LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(btnm, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btnm, 0, LV_STATE_FOCUS_KEY);
}

lv_obj_t *soft_keyboard_create(lv_obj_t *parent, session_t *session,
                               void (*on_close_cb)(void *userdata), void *userdata) {
    stream_input_t *input = session ? session_get_input(session) : NULL;

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_70, 0);

    soft_kbd_t *kbd = malloc(sizeof(soft_kbd_t));
    if (!kbd) {
        return cont;
    }
    memset(kbd, 0, sizeof(*kbd));
    kbd->container = cont;
    kbd->input = input;
    kbd->on_close = on_close_cb;
    kbd->on_close_userdata = userdata;
    kbd->layer = KBD_LAYER_ALPHA;
    kbd->group = lv_group_create();
    lv_obj_set_user_data(cont, kbd);

    lv_obj_t *kbd_content = lv_obj_create(cont);
    lv_obj_remove_style_all(kbd_content);
    lv_obj_set_width(kbd_content, LV_PCT(100));
    lv_obj_set_height(kbd_content, lv_pct(34));
    lv_obj_set_style_bg_color(kbd_content, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
    lv_obj_set_style_bg_opa(kbd_content, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kbd_content, LV_DPX(10), 0);
    lv_obj_set_style_pad_gap(kbd_content, LV_DPX(4), 0);
    lv_obj_clear_flag(kbd_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(kbd_content, LV_ALIGN_BOTTOM_MID, 0, -LV_DPX(12));

    kbd_data_t *kd = malloc(sizeof(kbd_data_t));
    if (!kd) {
        lv_obj_del(cont);
        return cont;
    }
    kd->kbd = kbd;
    kd->vks = alpha_vks;
    kd->vk_count = ALPHA_VK_COUNT;

    lv_obj_t *btnm = lv_btnmatrix_create(kbd_content);
    kbd->btnm = btnm;
    lv_group_add_obj(kbd->group, btnm);
    lv_btnmatrix_set_map(btnm, alpha_map_lower);
    lv_obj_set_width(btnm, LV_PCT(100));
    lv_obj_set_height(btnm, LV_PCT(100));
    lv_obj_set_style_bg_opa(btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(btnm, lv_theme_get_font_small(parent), 0);
    kbd_apply_theme(btnm);
    kbd_apply_controls(kbd);

    lv_obj_add_event_cb(btnm, on_keyboard_click, LV_EVENT_VALUE_CHANGED, kd);
    lv_obj_add_event_cb(btnm, kbd_data_delete_cb, LV_EVENT_DELETE, kd);
    lv_group_focus_obj(btnm);
    lv_obj_add_event_cb(cont, container_delete_cb, LV_EVENT_DELETE, kbd);

    return cont;
}
