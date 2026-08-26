#include "streaming.controller.h"
#include "app.h"

#include <string.h>

#include "util/i18n.h"
#include "util/font.h"
#include "util/log_overlay.h"
#include "hints.h"

#include "lvgl/ext/lv_child_group.h"
#include "lvgl/theme/lv_theme_moonlight.h"

static lv_obj_t *stat_label(lv_obj_t *parent, const char *title);

static lv_obj_t *overlay_title(lv_obj_t *parent, const char *title, streaming_controller_t *controller);

static lv_obj_t *gfn_metric_box(lv_obj_t *parent, const char *unit, const char *caption);

static lv_obj_t *gfn_section_title(lv_obj_t *parent, const char *title);

static lv_obj_t *gfn_kv_row(lv_obj_t *parent, const char *key);

lv_obj_t *streaming_scene_create(lv_fragment_t *self, lv_obj_t *parent) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    controller->detached_root = obj;

    lv_obj_t *hint = lv_label_create(obj);
    lv_obj_set_style_pad_all(hint, LV_DPX(20), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text_fmt(hint, locstr("Hint: %s"), hints_obtain());
    controller->hint = hint;

    lv_obj_t *overlay = lv_obj_create(obj);
    lv_obj_remove_style_all(overlay);
    controller->overlay = overlay;

    controller->group = lv_group_create();
    lv_obj_add_event_cb(overlay, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->group);

    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));

    /* GFN-style panel sits over the video (left). Don't shrink the video pane. */
    int stats_w = 0;
    lv_disp_t *disp = lv_disp_get_default();
    int video_w = lv_disp_get_hor_res(disp) - LV_DPX(20) * 2 - (stats_w ? LV_DPX(30) + stats_w : 0);
    int video_h_pct = video_w * 100 / lv_disp_get_hor_res(disp);

    lv_obj_t *video = lv_obj_create(overlay);
    lv_obj_remove_style_all(video);
    lv_obj_set_size(video, video_w, LV_PCT(video_h_pct));
    int video_top = app_configuration->show_stats_compact ? LV_DPX(36) : LV_DPX(20);
    lv_obj_align(video, LV_ALIGN_TOP_LEFT, LV_DPX(20), video_top);
    lv_obj_clear_flag(video, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bottom_stack = lv_obj_create(overlay);
    lv_obj_remove_style_all(bottom_stack);
    lv_obj_set_width(bottom_stack, LV_PCT(100));
    lv_obj_set_height(bottom_stack, LV_DPX(260));
    lv_obj_align(bottom_stack, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_layout(bottom_stack, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bottom_stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bottom_stack, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bottom_stack, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bottom_stack, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *actions = lv_obj_create(bottom_stack);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_DPX(200));
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(actions, LV_DPX(15), 0);
    lv_obj_set_style_pad_all(actions, LV_DPX(20), 0);
    static const lv_grad_dsc_t actions_grad = {
            .dir = LV_GRAD_DIR_VER,
            .stops = {
                    {.color = {.ch ={0, 0, 0, 0}}, .frac = 0},
                    {.color = {.ch ={0, 0, 0, 255}}, .frac = 255},
            },
            .stops_count = 2
    };
    lv_obj_set_style_bg_grad(actions, &actions_grad, 0);
    // We need a non-opaque opacity to properly render the elements
    lv_obj_set_style_bg_opa(actions, LV_OPA_COVER, 0);
    lv_obj_add_flag(actions, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(actions, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->group);

    lv_obj_t *kbd_btn = lv_btn_create(actions);
    lv_obj_add_flag(kbd_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(kbd_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(kbd_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(kbd_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t *kbd_label = lv_label_create(kbd_btn);
    lv_obj_add_style(kbd_label, &controller->overlay_button_label_style, 0);
    lv_label_set_text(kbd_label, locstr("Full keyboard"));

    lv_obj_t *vmouse_btn = lv_btn_create(actions);
    lv_obj_add_flag(vmouse_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(vmouse_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(vmouse_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(vmouse_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_t *vmouse_label = lv_label_create(vmouse_btn);
    lv_obj_add_style(vmouse_label, &controller->overlay_button_label_style, 0);
    lv_label_set_text(vmouse_label, locstr("Virtual Mouse"));

    lv_obj_t *actions_spacing = lv_obj_create(actions);
    lv_obj_remove_style_all(actions_spacing);
    lv_obj_set_flex_grow(actions_spacing, 1);

    lv_obj_t *suspend_btn = lv_btn_create(actions);
    lv_obj_add_flag(suspend_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(suspend_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(suspend_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(suspend_btn, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_t *suspend_lbl = lv_label_create(suspend_btn);
    lv_obj_add_style(suspend_lbl, &controller->overlay_button_label_style, 0);
    lv_label_set_text(suspend_lbl, locstr("Disconnect"));

    lv_obj_t *exit_btn = lv_btn_create(actions);
    lv_obj_add_flag(exit_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(exit_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(exit_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(exit_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_obj_add_style(exit_lbl, &controller->overlay_button_label_style, 0);
    lv_label_set_text(exit_lbl, locstr("Quit game"));

    lv_obj_t *stats = lv_obj_create(overlay);
    lv_obj_remove_style_all(stats);
    lv_obj_set_style_text_color(stats, lv_color_white(), 0);
    lv_obj_set_style_pad_gap(stats, LV_DPX(5), 0);
    lv_obj_set_style_bg_color(stats, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(stats, LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(stats, LV_OPA_30, LV_STATE_USER_1);
    lv_obj_set_user_data(stats, controller);

    if (app_configuration->show_stats_compact) {
        /* Artemis lite mode: slim horizontal bar at top, full width, single line */
        lv_obj_set_size(stats, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(stats, LV_DPX(12), 0);
        lv_obj_set_style_pad_ver(stats, LV_DPX(6), 0);
        lv_obj_set_style_pad_bottom(stats, LV_DPX(6), 0);
        lv_obj_align(stats, LV_ALIGN_TOP_LEFT, 0, 0);

        /* Quality indicator as colored label (●): green/yellow/red by latency */
        lv_obj_t *quality_dot = lv_label_create(stats);
        lv_label_set_text(quality_dot, "\u25CF");  /* ● U+25CF BLACK CIRCLE */
        lv_obj_set_style_text_color(quality_dot, lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_obj_set_style_text_font(quality_dot, lv_theme_get_font_small(stats), 0);
        controller->stats_quality_indicator = quality_dot;

        controller->stats_compact_label = lv_label_create(stats);
        lv_label_set_text(controller->stats_compact_label, "-");
        lv_obj_set_style_text_font(controller->stats_compact_label, lv_theme_get_font_small(stats), 0);
        lv_obj_set_flex_grow(controller->stats_compact_label, 1);

        /* Pin button inline, minimal */
        lv_obj_t *stats_pin = lv_btn_create(stats);
        lv_group_remove_obj(stats_pin);
        lv_obj_add_flag(stats_pin, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_bg_opa(stats_pin, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(stats_pin, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(stats_pin, LV_DPX(4), 0);
        lv_obj_set_style_radius(stats_pin, LV_DPX(4), 0);
        lv_obj_set_style_text_opa(stats_pin, LV_OPA_50, 0);
        lv_obj_set_style_text_opa(stats_pin, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_ext_click_area(stats_pin, LV_DPX(5));
        lv_obj_t *stat_pin_content = lv_img_create(stats_pin);
        lv_obj_set_style_text_font(stat_pin_content, lv_theme_moonlight_get_iconfont_small(stats_pin), 0);
        lv_img_set_src(stat_pin_content, MAT_SYMBOL_PUSH_PIN);
        controller->stats_pin = stats_pin;

        memset(&controller->stats_items, 0, sizeof(controller->stats_items));
    } else {
        lv_obj_set_width(stats, LV_DPX(280));
        lv_obj_set_height(stats, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(stats, LV_DPX(6), 0);
        lv_obj_set_style_pad_gap(stats, LV_DPX(2), 0);
        lv_obj_set_style_radius(stats, LV_DPX(6), 0);
        lv_obj_align(stats, LV_ALIGN_TOP_RIGHT, -LV_DPX(12), LV_DPX(12));
        controller->stats_items.header = overlay_title(stats, locstr("Performance"), controller);
        lv_obj_set_flex_grow(controller->stats_items.header, 0);
        controller->stats_compact_label = NULL;
        controller->stats_quality_indicator = NULL;

        lv_obj_t *metrics = lv_obj_create(stats);
        lv_obj_remove_style_all(metrics);
        lv_obj_set_width(metrics, LV_PCT(100));
        lv_obj_set_height(metrics, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_hor(metrics, LV_DPX(6), 0);
        lv_obj_set_style_pad_gap(metrics, LV_DPX(4), 0);
        controller->stats_items.game_fps = gfn_metric_box(metrics, locstr("FPS"), locstr("Game"));
        controller->stats_items.total_ms = gfn_metric_box(metrics, "ms", locstr("Latency"));
        controller->stats_items.ping = gfn_metric_box(metrics, "ms", locstr("Ping"));

        gfn_section_title(stats, locstr("Network"));
        controller->stats_items.frame_loss = gfn_kv_row(stats, locstr("Frame loss"));
        controller->stats_items.bandwidth = gfn_kv_row(stats, locstr("Bandwidth"));

        lv_obj_t *divider = lv_obj_create(stats);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, LV_PCT(100), LV_DPX(1));
        lv_obj_set_style_bg_color(divider, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_20, 0);
        lv_obj_set_style_pad_ver(divider, LV_DPX(4), 0);

        gfn_section_title(stats, locstr("Stream"));
        controller->stats_items.resolution = gfn_kv_row(stats, locstr("Resolution"));
        controller->stats_items.codec = gfn_kv_row(stats, locstr("Video"));
        controller->stats_items.decoder = gfn_kv_row(stats, locstr("Video backend"));
        controller->stats_items.host_latency = gfn_kv_row(stats, locstr("Host processing latency"));
        controller->stats_items.vdec_latency = gfn_kv_row(stats, locstr("Decoder latency"));
        controller->stats_items.cpu_ram = gfn_kv_row(stats, locstr("CPU / RAM"));
        controller->stats_items.audio = gfn_kv_row(stats, locstr("Audio"));
    }


    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    controller->video = video;
    controller->actions = actions;
    controller->kbd_btn = kbd_btn;
    controller->vmouse_btn = vmouse_btn;
    controller->quit_btn = exit_btn;
    controller->suspend_btn = suspend_btn;
    controller->stats = stats;

    streaming_overlay_resized(controller);

    /* Settings → Show logs ON: force Live when the stream UI mounts. */
    log_overlay_reassert();

    // We return overlay instead of obj, and will delete the obj manually
    return overlay;
}

void streaming_styles_init(streaming_controller_t *controller) {
    lv_theme_t *theme = lv_disp_get_default()->theme;

    lv_style_init(&controller->overlay_button_style);
    lv_style_set_shadow_ofs_y(&controller->overlay_button_style, LV_DPX(3));
    lv_style_set_shadow_width(&controller->overlay_button_style, LV_DPX(4));
    lv_style_set_shadow_color(&controller->overlay_button_style, lv_color_black());
    lv_style_set_shadow_opa(&controller->overlay_button_style, LV_OPA_30);
    lv_style_set_radius(&controller->overlay_button_style, LV_DPX(8));
    lv_style_set_pad_hor(&controller->overlay_button_style, LV_DPX(15));
    lv_style_set_pad_ver(&controller->overlay_button_style, LV_DPX(10));
    lv_style_init(&controller->overlay_button_style_focused);
    lv_style_set_outline_color(&controller->overlay_button_style_focused, lv_palette_lighten(LV_PALETTE_BLUE, 3));

    lv_style_init(&controller->overlay_button_label_style);
    lv_style_set_text_font(&controller->overlay_button_label_style, theme->font_small);
}

void streaming_styles_reset(streaming_controller_t *controller) {
    lv_style_reset(&controller->overlay_button_style);
    lv_style_reset(&controller->overlay_button_style_focused);
    lv_style_reset(&controller->overlay_button_label_style);
}

void streaming_overlay_resized(streaming_controller_t *controller) {
    lv_obj_update_layout(controller->actions);
    lv_obj_update_layout(controller->overlay);
}

static lv_obj_t *gfn_metric_box(lv_obj_t *parent, const char *unit, const char *caption) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_10, 0);
    lv_obj_set_style_radius(box, LV_DPX(4), 0);
    lv_obj_set_style_pad_ver(box, LV_DPX(4), 0);
    lv_obj_set_style_pad_hor(box, LV_DPX(2), 0);
    lv_obj_set_style_pad_gap(box, LV_DPX(0), 0);

    lv_obj_t *value = lv_label_create(box);
    lv_label_set_text(value, "-");
    lv_obj_set_style_text_font(value, lv_theme_get_font_normal(box), 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);

    lv_obj_t *unit_lbl = lv_label_create(box);
    lv_label_set_text_fmt(unit_lbl, "(%s)", unit);
    lv_obj_set_style_text_font(unit_lbl, lv_theme_get_font_small(box), 0);
    lv_obj_set_style_text_opa(unit_lbl, LV_OPA_70, 0);

    lv_obj_t *cap = lv_label_create(box);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, lv_theme_get_font_small(box), 0);
    lv_obj_set_style_text_opa(cap, LV_OPA_80, 0);
    return value;
}

static lv_obj_t *gfn_section_title(lv_obj_t *parent, const char *title) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_hor(label, LV_DPX(8), 0);
    lv_obj_set_style_pad_top(label, LV_DPX(2), 0);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(label), 0);
    return label;
}

static lv_obj_t *gfn_kv_row(lv_obj_t *parent, const char *key) {
    return stat_label(parent, key);
}

static lv_obj_t *stat_label(lv_obj_t *parent, const char *title) {
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(container, LV_DPX(8), 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(container, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_t *label = lv_label_create(container);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(container), 0);
    lv_obj_t *value = lv_label_create(container);
    lv_obj_set_style_text_font(value, lv_theme_get_font_small(container), 0);
    return value;
}

static lv_obj_t *overlay_title(lv_obj_t *parent, const char *title, streaming_controller_t *controller) {
    lv_obj_t *stats_title = lv_label_create(parent);
    lv_label_set_text_static(stats_title, title);
    lv_obj_set_width(stats_title, LV_PCT(100));
    lv_obj_set_flex_grow(stats_title, 0);
    lv_obj_set_style_bg_opa(stats_title, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(stats_title, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(stats_title, LV_DPX(8), 0);
    lv_obj_set_style_pad_ver(stats_title, LV_DPX(4), 0);
    lv_obj_t *stats_pin = lv_btn_create(stats_title);
    lv_group_remove_obj(stats_pin);
    lv_obj_add_flag(stats_pin, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(stats_pin, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(stats_pin, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(stats_pin, 0, 0);
    lv_obj_set_style_radius(stats_pin, LV_DPX(4), 0);
    lv_obj_set_style_transform_width(stats_pin, LV_DPX(5), 0);
    lv_obj_set_style_transform_height(stats_pin, LV_DPX(5), 0);
    lv_obj_set_style_text_opa(stats_pin, LV_OPA_50, 0);

    lv_obj_set_style_transform_width(stats_pin, LV_DPX(5), LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(stats_pin, LV_DPX(5), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(stats_pin, LV_OPA_40, LV_STATE_PRESSED);

    lv_obj_set_style_bg_color(stats_pin, lv_color_black(), LV_STATE_CHECKED);
    lv_obj_set_style_text_opa(stats_pin, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(stats_pin, LV_OPA_10, LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(stats_pin, LV_DPX(5));

    lv_obj_t *stat_pin_content = lv_img_create(stats_pin);
    lv_obj_set_style_text_font(stat_pin_content, lv_theme_moonlight_get_iconfont_small(stat_pin_content), 0);
    lv_img_set_src(stat_pin_content, MAT_SYMBOL_PUSH_PIN);

    lv_obj_align(stats_pin, LV_ALIGN_RIGHT_MID, 0, 0);
    controller->stats_pin = stats_pin;
    return stats_title;
}
