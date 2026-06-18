/*
 * Soft keyboard overlay (webOS/Xbox-style) - sends keys directly to streaming host.
 * Toggle modifiers (Shift, Ctrl, Alt, Win) for combos like Alt+F4, Win+G.
 * Close: Back button (remote) or B (gamepad) - no on-screen close button.
 * Quick keys: A/OK = select, Y = Space, X = Backspace, Start = Enter, B = Esc + close.
 */

#include "soft_keyboard.h"
#include "app.h"
#include "config.h"
#include "stream/input/session_input.h"
#include "stream/input/vk.h"
#include "stream/session.h"

#include <Limelight.h>
#include <stdlib.h>
#include <string.h>

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_META  0x08

#define VK_SHIFT  VK_LSHIFT
#define VK_CTRL   VK_LCONTROL
#define VK_ALT    VK_LMENU

typedef struct {
    lv_obj_t *container;
    lv_obj_t *btnm;
    lv_group_t *group;
    stream_input_t *input;
    bool toggle_shift;
    bool toggle_caps;
    bool toggle_ctrl;
    bool toggle_alt;
    bool toggle_win;
    uint16_t last_click_btn_id;
    uint32_t last_click_tick;
    void (*on_close)(void *);
    void *on_close_userdata;
} soft_kbd_t;

#define KBD_CLICK_DEDUPE_MS 80

/*
 * Button indices inside the single btnmatrix (matches kbd_map order).
 * Row 1  (0-12):  Esc, F1-F12
 * Row 2  (13-29): ` 1-0 - = Bksp Ins Home PgUp
 * Row 3  (30-46): Tab q-p [ ] \ Del End PgDn
 * Row 4  (47-59): Caps a-l ; ' Enter
 * Row 5  (60-72): ShiftL z-/ ShiftR Up
 * Row 6  (73-80): CtrlL WinL AltL Space AltGr WinR Menu CtrlR
 * Row 7  (81-83): Left Down Right
 */
#define BTN_IDX_CAPS    47
#define BTN_IDX_SHIFTL  60
#define BTN_IDX_SHIFTR  71
#define BTN_IDX_CTRLL   73
#define BTN_IDX_WINL    74
#define BTN_IDX_ALTL    75
#define BTN_IDX_ALTR    77  /* AltGr = VK_RMENU */
#define BTN_IDX_WINR    78
#define BTN_IDX_CTRLR   80

/*
 * Two map variants swapped when Shift or Caps Lock is toggled.
 * "Bksp" and "Enter" are used as plain text because LV_SYMBOL_BACKSPACE /
 * LV_SYMBOL_NEW_LINE are not present in every build font.
 */
static const char *kbd_map_lower[] = {
    "Esc","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "\n",
    "`","1","2","3","4","5","6","7","8","9","0","-","=","Bksp","Ins","Home","PgUp",
    "\n",
    "Tab","q","w","e","r","t","y","u","i","o","p","[","]","\\","Del","End","PgDn",
    "\n",
    "Caps","a","s","d","f","g","h","j","k","l",";","'","Enter",
    "\n",
    "Shift","z","x","c","v","b","n","m",",",".","/","Shift","\xE2\x86\x91",
    "\n",
    "Ctrl","Win","Alt","Space","AltGr","Win","Menu","Ctrl",
    "\n",
    LV_SYMBOL_LEFT,LV_SYMBOL_DOWN,LV_SYMBOL_RIGHT,
    ""
};

static const char *kbd_map_upper[] = {
    "Esc","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "\n",
    "`","1","2","3","4","5","6","7","8","9","0","-","=","Bksp","Ins","Home","PgUp",
    "\n",
    "Tab","Q","W","E","R","T","Y","U","I","O","P","[","]","\\","Del","End","PgDn",
    "\n",
    "Caps","A","S","D","F","G","H","J","K","L",";","'","Enter",
    "\n",
    "Shift","Z","X","C","V","B","N","M",",",".","/","Shift","\xE2\x86\x91",
    "\n",
    "Ctrl","Win","Alt","Space","AltGr","Win","Menu","Ctrl",
    "\n",
    LV_SYMBOL_LEFT,LV_SYMBOL_DOWN,LV_SYMBOL_RIGHT,
    ""
};

static char mods_to_flags(soft_kbd_t *kbd) {
    char m = 0;
    if (kbd->toggle_shift) m |= MODIFIER_SHIFT;
    if (kbd->toggle_ctrl)  m |= MODIFIER_CTRL;
    if (kbd->toggle_alt)   m |= MODIFIER_ALT;
    if (kbd->toggle_win && app_configuration->syskey_capture) m |= MODIFIER_META;
    return m;
}

static void kbd_set_btn_checked(lv_obj_t *btnm, uint16_t idx, bool checked) {
    if (checked) {
        lv_btnmatrix_set_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    } else {
        lv_btnmatrix_clear_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    }
}

/* Reflect the current toggle state of each modifier as a visual highlight on its key. */
static void kbd_update_modifier_visuals(soft_kbd_t *kbd) {
    lv_obj_t *btnm = kbd->btnm;
    if (!btnm) return;
    kbd_set_btn_checked(btnm, BTN_IDX_CAPS,   kbd->toggle_caps);
    kbd_set_btn_checked(btnm, BTN_IDX_SHIFTL, kbd->toggle_shift);
    kbd_set_btn_checked(btnm, BTN_IDX_SHIFTR, kbd->toggle_shift);
    kbd_set_btn_checked(btnm, BTN_IDX_CTRLL,  kbd->toggle_ctrl);
    kbd_set_btn_checked(btnm, BTN_IDX_CTRLR,  kbd->toggle_ctrl);
    kbd_set_btn_checked(btnm, BTN_IDX_WINL,   kbd->toggle_win);
    kbd_set_btn_checked(btnm, BTN_IDX_WINR,   kbd->toggle_win);
    kbd_set_btn_checked(btnm, BTN_IDX_ALTL,   kbd->toggle_alt);
    kbd_set_btn_checked(btnm, BTN_IDX_ALTR,   kbd->toggle_alt);
}

/*
 * Reapply per-button control flags after lv_btnmatrix_set_map(), which resets
 * the ctrl-bits array.  Called every time the map is swapped for case change.
 */
static void kbd_apply_controls(soft_kbd_t *kbd) {
    lv_obj_t *btnm = kbd->btnm;
    lv_btnmatrix_set_btn_ctrl_all(btnm, LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT);
    kbd_update_modifier_visuals(kbd);
}

/*
 * Switch between lowercase and uppercase map depending on Shift / Caps state.
 * Must reapply button controls because set_map resets them.
 */
static void kbd_refresh_case(soft_kbd_t *kbd) {
    bool upper = kbd->toggle_shift || kbd->toggle_caps;
    lv_btnmatrix_set_map(kbd->btnm, upper ? kbd_map_upper : kbd_map_lower);
    kbd_apply_controls(kbd);
}

/* Send a regular key with any active modifiers reported in the flags byte. */
static void send_key(soft_kbd_t *kbd, short vk, bool down) {
    stream_input_send_key_event(kbd->input, vk, down, mods_to_flags(kbd));
}

/*
 * Send a modifier key itself (Shift, Ctrl, Alt, Win).
 * Use modifiers=0: the key code already conveys which modifier it is;
 * including its own flag in the modifier byte can confuse the host state machine.
 */
static void send_modifier(soft_kbd_t *kbd, short vk, bool down) {
    stream_input_send_key_event(kbd->input, vk, down, 0);
}

static void release_toggles(soft_kbd_t *kbd) {
    bool was_shift = kbd->toggle_shift;
    if (kbd->toggle_shift) {
        kbd->toggle_shift = false;
        /* Row 5 uses VK_SHIFT for both keys; release all shift scancodes so host never stays stuck. */
        send_modifier(kbd, VK_SHIFT, false);
        send_modifier(kbd, VK_LSHIFT, false);
        send_modifier(kbd, VK_RSHIFT, false);
    }
    if (kbd->toggle_ctrl) {
        kbd->toggle_ctrl = false;
        send_modifier(kbd, VK_LCONTROL, false);
        send_modifier(kbd, VK_RCONTROL, false);
    }
    if (kbd->toggle_alt) {
        kbd->toggle_alt = false;
        send_modifier(kbd, VK_LMENU, false);
        send_modifier(kbd, VK_RMENU, false);
    }
    if (kbd->toggle_win && app_configuration->syskey_capture) {
        kbd->toggle_win = false;
        send_modifier(kbd, VK_LWIN, false);
        send_modifier(kbd, VK_RWIN, false);
    }
    if (was_shift) {
        kbd_refresh_case(kbd);  /* switch back to lowercase map if Shift was active */
    } else {
        kbd_update_modifier_visuals(kbd);
    }
}

/*
 * Paranoid cleanup after every key combo: unconditionally send key-up for ALL modifier scancodes
 * (left, right, and generic) so the host's modifier byte and key state can never stay stuck even
 * if a packet drops or the host's state machine desyncs. Cheap on the wire, prevents the gamepad
 * from "going crazy" later because Ctrl/Alt/Shift was still considered held by the host.
 */
static void force_release_all_modifiers(soft_kbd_t *kbd) {
    /* L/R variants hit the GFE modifier-byte fixups in InputStream.c so the host's modifier state
     * is forced back to 0. Generic VK_SHIFT/VK_CONTROL/VK_MENU (0x10/0x11/0x12) cover hosts that
     * track these as separate keys (e.g. some Sunshine builds and webOS guest VMs). */
    send_modifier(kbd, VK_LSHIFT,   false);
    send_modifier(kbd, VK_RSHIFT,   false);
    send_modifier(kbd, 0x10 /* VK_SHIFT (generic) */,   false);
    send_modifier(kbd, VK_LCONTROL, false);
    send_modifier(kbd, VK_RCONTROL, false);
    send_modifier(kbd, 0x11 /* VK_CONTROL (generic) */, false);
    send_modifier(kbd, VK_LMENU,    false);
    send_modifier(kbd, VK_RMENU,    false);
    send_modifier(kbd, 0x12 /* VK_MENU (generic) */,    false);
    if (app_configuration->syskey_capture) {
        send_modifier(kbd, VK_LWIN, false);
        send_modifier(kbd, VK_RWIN, false);
    }
    kbd->toggle_shift = false;
    kbd->toggle_caps  = false;
    kbd->toggle_ctrl  = false;
    kbd->toggle_alt   = false;
    kbd->toggle_win   = false;
    if (kbd->btnm) {
        kbd_refresh_case(kbd);
    }
}

typedef struct {
    soft_kbd_t *kbd;
    short *vks;
    int vk_count;
} kbd_data_t;

static void on_keyboard_click(lv_event_t *e) {
    kbd_data_t *kd = lv_event_get_user_data(e);
    uint32_t *btn_id_ptr = lv_event_get_param(e);
    uint16_t btn_id = (btn_id_ptr != NULL) ? (uint16_t) *btn_id_ptr
                                           : lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (btn_id == LV_BTNMATRIX_BTN_NONE) return;
    if (btn_id >= (uint16_t) kd->vk_count) return;
    short vk = kd->vks[btn_id];
    if (vk == 0) return;

    soft_kbd_t *kbd = kd->kbd;
    uint32_t now = lv_tick_get();
    if (btn_id == kbd->last_click_btn_id && (now - kbd->last_click_tick) < KBD_CLICK_DEDUPE_MS) {
        return;
    }
    kbd->last_click_btn_id = btn_id;
    kbd->last_click_tick = now;

    if (vk == VK_SHIFT || vk == VK_RSHIFT) {
        kbd->toggle_shift = !kbd->toggle_shift;
        send_modifier(kbd, vk, kbd->toggle_shift);
        kbd_refresh_case(kbd);  /* upper/lower map + amber highlight */
        return;
    }
    if (vk == VK_CTRL || vk == VK_RCONTROL) {
        kbd->toggle_ctrl = !kbd->toggle_ctrl;
        send_modifier(kbd, vk, kbd->toggle_ctrl);
        kbd_update_modifier_visuals(kbd);
        return;
    }
    if (vk == VK_ALT || vk == VK_RMENU) {
        kbd->toggle_alt = !kbd->toggle_alt;
        send_modifier(kbd, vk, kbd->toggle_alt);
        kbd_update_modifier_visuals(kbd);
        return;
    }
    if ((vk == VK_LWIN || vk == VK_RWIN) && app_configuration->syskey_capture) {
        kbd->toggle_win = !kbd->toggle_win;
        send_modifier(kbd, vk, kbd->toggle_win);
        kbd_update_modifier_visuals(kbd);
        return;
    }
    if (vk == VK_CAPITAL) {
        kbd->toggle_caps = !kbd->toggle_caps;
        send_key(kbd, VK_CAPITAL, true);
        send_key(kbd, VK_CAPITAL, false);
        kbd_refresh_case(kbd);  /* upper/lower map + Caps amber highlight */
        return;
    }
    send_key(kbd, vk, true);
    send_key(kbd, vk, false);
    release_toggles(kbd);
}

static void kbd_data_delete_cb(lv_event_t *e) {
    kbd_data_t *kd = lv_event_get_user_data(e);
    if (kd && kd->vks) free(kd->vks);
    if (kd) free(kd);
}

static void container_delete_cb(lv_event_t *e) {
    soft_kbd_t *kbd = lv_event_get_user_data(e);
    if (kbd) {
        /* btnm is being torn down by LVGL alongside the container; refresh_case would touch it,
         * so clear the pointer first and let force_release_all_modifiers skip the visual refresh. */
        kbd->btnm = NULL;
        force_release_all_modifiers(kbd);
        stream_input_flush_pressed_keys(kbd->input);
        if (kbd->group) lv_group_del(kbd->group);
        free(kbd);
    }
}

lv_group_t *soft_keyboard_get_group(lv_obj_t *keyboard_container) {
    soft_kbd_t *kbd = lv_obj_get_user_data(keyboard_container);
    return kbd ? kbd->group : NULL;
}

/*
 * Single btnmatrix: all keys in one grid.
 * D-pad/gamepad navigates tecla a tecla (lv_btnmatrix handles UP/DOWN/LEFT/RIGHT internally).
 * Modifiers are sticky: press Alt then Tab = Alt+Tab; press Ctrl then F4 = Ctrl+F4.
 * Row 1 : F-keys           Row 5 : Shift row
 * Row 2 : numbers + nav    Row 6 : modifiers
 * Row 3 : Tab row          Row 7 : arrow keys
 * Row 4 : Caps row
 * kbd_map_lower / kbd_map_upper are declared near the top of this file.
 */
static const short kbd_vks[] = {
    /* Row 1: F-keys (13) */
    VK_ESCAPE,VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
    /* Row 2: numbers + nav (17) */
    VK_OEM_3,VK_1,VK_2,VK_3,VK_4,VK_5,VK_6,VK_7,VK_8,VK_9,VK_0,VK_OEM_MINUS,VK_OEM_PLUS,VK_BACK,VK_INSERT,VK_HOME,VK_PRIOR,
    /* Row 3: Tab row (17) */
    VK_TAB,VK_Q,VK_W,VK_E,VK_R,VK_T,VK_Y,VK_U,VK_I,VK_O,VK_P,VK_OEM_4,VK_OEM_6,VK_OEM_5,VK_DELETE,VK_END,VK_NEXT,
    /* Row 4: Caps row (13) */
    VK_CAPITAL,VK_A,VK_S,VK_D,VK_F,VK_G,VK_H,VK_J,VK_K,VK_L,VK_OEM_1,VK_OEM_7,VK_RETURN,
    /* Row 5: Shift row (13) */
    VK_SHIFT,VK_Z,VK_X,VK_C,VK_V,VK_B,VK_N,VK_M,VK_OEM_COMMA,VK_OEM_PERIOD,VK_OEM_2,VK_SHIFT,VK_UP,
    /* Row 6: modifiers (8) */
    VK_LCONTROL,VK_LWIN,VK_ALT,VK_SPACE,VK_RMENU,VK_RWIN,VK_APPS,VK_RCONTROL,
    /* Row 7: arrows (3) */
    VK_LEFT,VK_DOWN,VK_RIGHT
};

#define KBD_VK_COUNT ((int)(sizeof(kbd_vks)/sizeof(kbd_vks[0])))

lv_obj_t *soft_keyboard_create(lv_obj_t *parent, session_t *session,
                               void (*on_close_cb)(void *userdata), void *userdata) {
    stream_input_t *input = session_get_input(session);

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_70, 0);

    soft_kbd_t *kbd = malloc(sizeof(soft_kbd_t));
    if (!kbd) return cont;
    memset(kbd, 0, sizeof(*kbd));
    kbd->container = cont;
    kbd->input = input;
    kbd->on_close = on_close_cb;
    kbd->on_close_userdata = userdata;
    kbd->group = lv_group_create();
    lv_obj_set_user_data(cont, kbd);

    /* Panel: fixed 35% of screen height, anchored at bottom - scales on 4K */
    lv_obj_t *kbd_content = lv_obj_create(cont);
    lv_obj_remove_style_all(kbd_content);
    lv_obj_set_width(kbd_content, LV_PCT(100));
    lv_obj_set_height(kbd_content, lv_pct(35));
    lv_obj_set_style_pad_all(kbd_content, LV_DPX(10), 0);
    lv_obj_set_style_pad_gap(kbd_content, LV_DPX(4), 0);
    lv_obj_set_flex_flow(kbd_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(kbd_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(kbd_content, LV_ALIGN_BOTTOM_MID, 0, -LV_DPX(24));

    /* Single btnmatrix: D-pad navigates key-by-key across all rows */
    kbd_data_t *kd = malloc(sizeof(kbd_data_t));
    if (!kd) { lv_obj_del(cont); return cont; }
    kd->kbd = kbd;
    kd->vk_count = KBD_VK_COUNT;
    kd->vks = malloc((size_t)KBD_VK_COUNT * sizeof(short));
    if (!kd->vks) { free(kd); lv_obj_del(cont); return cont; }
    memcpy(kd->vks, kbd_vks, (size_t)KBD_VK_COUNT * sizeof(short));

    lv_obj_t *btnm = lv_btnmatrix_create(kbd_content);
    kbd->btnm = btnm;
    lv_group_add_obj(kbd->group, btnm);
    lv_btnmatrix_set_map(btnm, kbd_map_lower);
    lv_obj_set_width(btnm, LV_PCT(100));
    lv_obj_set_flex_grow(btnm, 1);
    lv_obj_set_style_pad_all(btnm, LV_DPX(2), 0);
    lv_obj_set_style_pad_gap(btnm, LV_DPX(2), 0);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(btnm, lv_theme_get_font_small(parent), 0);
    /* Pressed state */
    lv_obj_set_style_bg_opa(btnm, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    /* No focus ring on the whole row; per-key focus via PART_ITEMS */
    lv_obj_set_style_outline_width(btnm, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(btnm, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btnm, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_TRANSP, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_opa(btnm, LV_OPA_TRANSP, LV_STATE_FOCUS_KEY);
    /* Per-key focus highlight (blue) */
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0x1e6fb5), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0x1e6fb5), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    /* Active modifier highlight (amber) */
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0xd4820a), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    /* Amber wins even when the modifier key also has focus */
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0xd4820a), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(0xd4820a), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);

    lv_obj_add_event_cb(btnm, on_keyboard_click, LV_EVENT_VALUE_CHANGED, kd);
    lv_obj_add_event_cb(btnm, kbd_data_delete_cb, LV_EVENT_DELETE, kd);
    kbd_apply_controls(kbd);  /* CLICK_TRIG | NO_REPEAT for all; CHECKED state for modifiers */

    lv_group_focus_obj(btnm);
    lv_obj_add_event_cb(cont, container_delete_cb, LV_EVENT_DELETE, kbd);

    return cont;
}
