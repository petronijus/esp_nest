#include "ui.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ui";

#define SCREEN_COUNT    2
#define CIRCLE_DIAM     308
#define DOT_SIZE        19
#define FADE_TIME_MS    300

/* Colors matching Figma design */
#define COL_BLACK       lv_color_hex(0x000000)
#define COL_YELLOW      lv_color_hex(0xFFE600)
#define COL_TEAL        lv_color_hex(0x00E5CC)
#define COL_CIRCLE      lv_color_hex(0x888888)
#define COL_WHITE       lv_color_hex(0xFFFFFF)

/* Brightness detail screen constants */
#define BRIGHT_OUTER_DIAM   456
#define BRIGHT_CIRCLE_MAX   420
#define BRIGHT_CIRCLE_MIN   10

/* ========================================================================== */
/*  LVGL C-array image descriptors (defined in assets)                        */
/* ========================================================================== */

extern const lv_image_dsc_t bg_glow_yellow;
extern const lv_image_dsc_t bg_glow_teal;
extern const lv_image_dsc_t circle_glow_yellow;
extern const lv_image_dsc_t circle_glow_teal;
extern const lv_image_dsc_t lights;
extern const lv_image_dsc_t fiiuuu;

/* ========================================================================== */
/*  UI mode state                                                             */
/* ========================================================================== */

typedef enum {
    UI_MODE_MENU,
    UI_MODE_BRIGHTNESS,
    UI_MODE_FAN,
} ui_mode_t;

static ui_mode_t s_mode = UI_MODE_MENU;
static int s_brightness = 30;

/* Fan detail state: 0=off, 1=low, 2=medium, 3=high */
static int s_fan_level = 0;
static int s_fan_accum = 0;

/* Single screen with page containers */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_pages[SCREEN_COUNT];
static int s_current = 0;
static bool s_animating = false;
static lv_obj_t *s_status_label = NULL;

/* Brightness detail page widgets */
static lv_obj_t *s_bright_page = NULL;
static lv_obj_t *s_bright_outer = NULL;
static lv_obj_t *s_bright_circle = NULL;
static lv_obj_t *s_bright_label = NULL;
static lv_obj_t *s_bright_hint = NULL;

/* Lights page widgets for brightness transition */
static lv_obj_t *s_lights_circle = NULL;

/* Fan detail page widgets */
static lv_obj_t *s_fan_page = NULL;
static lv_obj_t *s_fan_circles[3];   /* [0]=outer(456), [1]=mid(281), [2]=small(126) */
static lv_obj_t *s_fan_label = NULL;
static lv_obj_t *s_fan_hint = NULL;

/* ========================================================================== */
/*  Image helpers                                                             */
/* ========================================================================== */

static lv_obj_t *create_image(lv_obj_t *parent, const lv_image_dsc_t *dsc,
                               lv_align_t align, int x_ofs, int y_ofs)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    lv_obj_align(img, align, x_ofs, y_ofs);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_FLOATING);
    return img;
}

/* ========================================================================== */
/*  Drawing helpers                                                           */
/* ========================================================================== */

static lv_obj_t *create_page_container(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 480, 480);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(page, 0, 0);
    return page;
}

static lv_obj_t *create_circle_outline(lv_obj_t *parent)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, CIRCLE_DIAM, CIRCLE_DIAM);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(circle, 1, 0);
    lv_obj_set_style_border_color(circle, COL_CIRCLE, 0);
    lv_obj_set_style_border_opa(circle, LV_OPA_60, 0);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return circle;
}

static lv_obj_t *create_dot(lv_obj_t *parent, bool filled, lv_color_t color,
                             lv_align_t align, int y_offset)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

    if (filled) {
        lv_obj_set_style_bg_color(dot, color, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(dot, color, 0);
        lv_obj_set_style_shadow_width(dot, 12, 0);
        lv_obj_set_style_shadow_opa(dot, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
    }

    lv_obj_set_style_border_width(dot, filled ? 0 : 2, 0);
    if (!filled) {
        lv_obj_set_style_border_color(dot, COL_CIRCLE, 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_70, 0);
    }

    lv_obj_align(dot, align, 0, y_offset);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static lv_obj_t *create_title(lv_obj_t *parent, const char *fallback_text,
                               const lv_image_dsc_t *img_dsc)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, img_dsc);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ESP_LOGI(TAG, "Title image '%s': %ldx%ld", fallback_text,
             (long)img_dsc->header.w, (long)img_dsc->header.h);
    return img;
}

/* ========================================================================== */
/*  Page builders (children of the single screen)                             */
/* ========================================================================== */

static lv_obj_t *build_lights_page(lv_obj_t *parent)
{
    lv_obj_t *page = create_page_container(parent);

    /* Background glow -- yellow at top */
    create_image(page, &bg_glow_yellow, LV_ALIGN_TOP_MID, 0, 0);

    s_lights_circle = create_circle_outline(page);

    /* Circle glow behind the outline */
    create_image(page, &circle_glow_yellow, LV_ALIGN_CENTER, 0, 0);

    create_dot(page, true, COL_YELLOW, LV_ALIGN_TOP_MID, 77);
    create_dot(page, false, COL_CIRCLE, LV_ALIGN_BOTTOM_MID, -77);
    create_title(page, "Lights", &lights);

    return page;
}

static lv_obj_t *build_fan_page(lv_obj_t *parent)
{
    lv_obj_t *page = create_page_container(parent);

    /* Background glow -- teal at bottom */
    create_image(page, &bg_glow_teal, LV_ALIGN_BOTTOM_MID, 0, 0);

    create_circle_outline(page);

    /* Circle glow behind the outline */
    create_image(page, &circle_glow_teal, LV_ALIGN_CENTER, 0, 0);

    create_dot(page, false, COL_CIRCLE, LV_ALIGN_TOP_MID, 77);
    create_dot(page, true, COL_TEAL, LV_ALIGN_BOTTOM_MID, -77);
    create_title(page, "Fiiuuu", &fiiuuu);

    return page;
}

/* ========================================================================== */
/*  Brightness detail page                                                    */
/* ========================================================================== */

static void update_brightness_display(void)
{
    if (!s_bright_label || !s_bright_circle) return;

    /* Update number */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_brightness);
    lv_label_set_text(s_bright_label, buf);

    /* Update yellow circle size: scale linearly from MIN to MAX */
    if (s_brightness <= 0) {
        lv_obj_add_flag(s_bright_circle, LV_OBJ_FLAG_HIDDEN);
    } else {
        int diam = BRIGHT_CIRCLE_MIN +
                   (BRIGHT_CIRCLE_MAX - BRIGHT_CIRCLE_MIN) * s_brightness / 100;
        lv_obj_clear_flag(s_bright_circle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_bright_circle, diam, diam);
        lv_obj_center(s_bright_circle);
    }
}

static lv_obj_t *build_brightness_page(lv_obj_t *parent)
{
    lv_obj_t *page = create_page_container(parent);

    /* Outer circle outline */
    s_bright_outer = lv_obj_create(page);
    lv_obj_remove_style_all(s_bright_outer);
    lv_obj_set_size(s_bright_outer, BRIGHT_OUTER_DIAM, BRIGHT_OUTER_DIAM);
    lv_obj_set_style_radius(s_bright_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_bright_outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bright_outer, 1, 0);
    lv_obj_set_style_border_color(s_bright_outer, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_opa(s_bright_outer, LV_OPA_COVER, 0);
    lv_obj_align(s_bright_outer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_bright_outer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Yellow filled circle (dynamic size based on brightness) */
    s_bright_circle = lv_obj_create(page);
    lv_obj_remove_style_all(s_bright_circle);
    lv_obj_set_style_radius(s_bright_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bright_circle, COL_YELLOW, 0);
    lv_obj_set_style_bg_opa(s_bright_circle, LV_OPA_COVER, 0);
    lv_obj_align(s_bright_circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_bright_circle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Brightness number */
    s_bright_label = lv_label_create(page);
    lv_label_set_text(s_bright_label, "30");
    lv_obj_set_style_text_color(s_bright_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_bright_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_bright_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_bright_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_bright_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Hint text */
    s_bright_hint = lv_label_create(page);
    lv_label_set_text(s_bright_hint, "long press to turn off");
    lv_obj_set_style_text_color(s_bright_hint, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_bright_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(s_bright_hint, 3, 0);
    lv_obj_set_style_text_align(s_bright_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_bright_hint, LV_ALIGN_CENTER, 0, 90);
    lv_obj_clear_flag(s_bright_hint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Set initial display */
    update_brightness_display();

    return page;
}

/* ========================================================================== */
/*  Fan detail page                                                           */
/* ========================================================================== */

#define FAN_OUTER_DIAM  456
#define FAN_MID_DIAM    281
#define FAN_SMALL_DIAM  126

static const char *fan_level_names[] = {"off", "low", "medium", "high"};

static void update_fan_display(void)
{
    if (!s_fan_label) return;

    lv_label_set_text(s_fan_label, fan_level_names[s_fan_level]);

    for (int i = 0; i < 3; i++) {
        /* Circle index 0 = outer (fills at level 3),
         * 1 = mid (fills at level >= 2), 2 = small (fills at level >= 1).
         * So circle i is filled when s_fan_level >= (3 - i). */
        bool filled = (s_fan_level >= (3 - i));

        if (filled) {
            lv_obj_set_style_bg_color(s_fan_circles[i], COL_TEAL, 0);
            lv_obj_set_style_bg_opa(s_fan_circles[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_fan_circles[i], 0, 0);
        } else {
            lv_obj_set_style_bg_opa(s_fan_circles[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(s_fan_circles[i], 1, 0);
            lv_obj_set_style_border_color(s_fan_circles[i], lv_color_hex(0x444444), 0);
            lv_obj_set_style_border_opa(s_fan_circles[i], LV_OPA_COVER, 0);
        }
    }
}

static lv_obj_t *build_fan_detail_page(lv_obj_t *parent)
{
    lv_obj_t *page = create_page_container(parent);

    static const int diams[] = {FAN_OUTER_DIAM, FAN_MID_DIAM, FAN_SMALL_DIAM};

    for (int i = 0; i < 3; i++) {
        s_fan_circles[i] = lv_obj_create(page);
        lv_obj_remove_style_all(s_fan_circles[i]);
        lv_obj_set_size(s_fan_circles[i], diams[i], diams[i]);
        lv_obj_set_style_radius(s_fan_circles[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_fan_circles[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_fan_circles[i], 1, 0);
        lv_obj_set_style_border_color(s_fan_circles[i], lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_opa(s_fan_circles[i], LV_OPA_COVER, 0);
        lv_obj_align(s_fan_circles[i], LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(s_fan_circles[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    s_fan_label = lv_label_create(page);
    lv_label_set_text(s_fan_label, "off");
    lv_obj_set_style_text_color(s_fan_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_fan_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_fan_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_fan_label, LV_ALIGN_CENTER, 0, 19);
    lv_obj_clear_flag(s_fan_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_fan_hint = lv_label_create(page);
    lv_label_set_text(s_fan_hint, "long press to turn off");
    lv_obj_set_style_text_color(s_fan_hint, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_fan_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(s_fan_hint, 3, 0);
    lv_obj_set_style_text_align(s_fan_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_fan_hint, LV_ALIGN_CENTER, 0, 90);
    lv_obj_clear_flag(s_fan_hint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    update_fan_display();

    return page;
}

/* ========================================================================== */
/*  Fade transition (opacity animation on page containers)                    */
/* ========================================================================== */

static void fade_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static lv_obj_t *s_fade_out_target = NULL;

static void fade_out_done_cb(lv_anim_t *a)
{
    if (s_fade_out_target) {
        lv_obj_add_flag(s_fade_out_target, LV_OBJ_FLAG_HIDDEN);
        s_fade_out_target = NULL;
    }
}

static void fade_in_done_cb(lv_anim_t *a)
{
    s_animating = false;
}

static void fade_transition(lv_obj_t *from, lv_obj_t *to)
{
    if (s_animating) return;
    s_animating = true;

    s_fade_out_target = from;

    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, from);
    lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a_out, fade_opa_cb);
    lv_anim_set_duration(&a_out, FADE_TIME_MS);
    lv_anim_set_completed_cb(&a_out, fade_out_done_cb);
    lv_anim_start(&a_out);

    lv_obj_set_style_opa(to, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(to, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, to);
    lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_in, fade_opa_cb);
    lv_anim_set_duration(&a_in, FADE_TIME_MS);
    lv_anim_set_completed_cb(&a_in, fade_in_done_cb);
    lv_anim_start(&a_in);
}

/* ========================================================================== */
/*  Brightness transition: circle grows 308->456, yellow circle scales up,    */
/*  text fades in, lights page fades out.                                     */
/* ========================================================================== */

#define BRIGHT_ANIM_MS 400

static void circle_size_cb(void *obj, int32_t value)
{
    lv_obj_set_size((lv_obj_t *)obj, value, value);
    lv_obj_center((lv_obj_t *)obj);
}

static void enter_bright_done_cb(lv_anim_t *a)
{
    if (s_fade_out_target) {
        lv_obj_add_flag(s_fade_out_target, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_fade_out_target, LV_OPA_COVER, 0);
        s_fade_out_target = NULL;
    }
    s_animating = false;
}

static void exit_bright_done_cb(lv_anim_t *a)
{
    lv_obj_add_flag(s_bright_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_bright_outer, BRIGHT_OUTER_DIAM, BRIGHT_OUTER_DIAM);
    lv_obj_center(s_bright_outer);
    lv_obj_set_style_opa(s_bright_label, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_bright_hint, LV_OPA_COVER, 0);
    update_brightness_display();
    s_animating = false;
}

static void switch_page(int new_idx)
{
    if (new_idx < 0) new_idx = SCREEN_COUNT - 1;
    if (new_idx >= SCREEN_COUNT) new_idx = 0;
    if (new_idx == s_current) return;
    if (s_animating) return;

    fade_transition(s_pages[s_current], s_pages[new_idx]);
    s_current = new_idx;
}

/* ========================================================================== */
/*  Mode transitions                                                          */
/* ========================================================================== */

static void enter_brightness_mode(void)
{
    if (s_mode != UI_MODE_MENU || s_animating) return;
    s_mode = UI_MODE_BRIGHTNESS;
    s_animating = true;

    update_brightness_display();

    int target_diam = s_brightness > 0
        ? BRIGHT_CIRCLE_MIN + (BRIGHT_CIRCLE_MAX - BRIGHT_CIRCLE_MIN) * s_brightness / 100
        : 0;

    /* Prepare brightness page children at their starting state */
    lv_obj_set_size(s_bright_outer, CIRCLE_DIAM, CIRCLE_DIAM);
    lv_obj_center(s_bright_outer);
    if (target_diam > 0) {
        lv_obj_clear_flag(s_bright_circle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_bright_circle, 1, 1);
        lv_obj_center(s_bright_circle);
    }
    lv_obj_set_style_opa(s_bright_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_bright_hint, LV_OPA_TRANSP, 0);

    /* Show brightness page at full opacity (children control their own visibility) */
    lv_obj_set_style_opa(s_bright_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_bright_page, LV_OBJ_FLAG_HIDDEN);

    s_fade_out_target = s_pages[s_current];

    /* Anim 1: Fade out lights page */
    lv_anim_t a1;
    lv_anim_init(&a1);
    lv_anim_set_var(&a1, s_pages[s_current]);
    lv_anim_set_values(&a1, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a1, fade_opa_cb);
    lv_anim_set_duration(&a1, BRIGHT_ANIM_MS);
    lv_anim_start(&a1);

    /* Anim 2: Grow outer circle outline 308 -> 456 */
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, s_bright_outer);
    lv_anim_set_values(&a2, CIRCLE_DIAM, BRIGHT_OUTER_DIAM);
    lv_anim_set_exec_cb(&a2, circle_size_cb);
    lv_anim_set_duration(&a2, BRIGHT_ANIM_MS);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_out);
    lv_anim_start(&a2);

    /* Anim 3: Grow yellow filled circle 1 -> target */
    if (target_diam > 0) {
        lv_anim_t a3;
        lv_anim_init(&a3);
        lv_anim_set_var(&a3, s_bright_circle);
        lv_anim_set_values(&a3, 1, target_diam);
        lv_anim_set_exec_cb(&a3, circle_size_cb);
        lv_anim_set_duration(&a3, BRIGHT_ANIM_MS);
        lv_anim_set_path_cb(&a3, lv_anim_path_ease_out);
        lv_anim_start(&a3);
    }

    /* Anim 4: Fade in brightness number */
    lv_anim_t a4;
    lv_anim_init(&a4);
    lv_anim_set_var(&a4, s_bright_label);
    lv_anim_set_values(&a4, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a4, fade_opa_cb);
    lv_anim_set_duration(&a4, BRIGHT_ANIM_MS);
    lv_anim_start(&a4);

    /* Anim 5: Fade in hint text (carries the done callback) */
    lv_anim_t a5;
    lv_anim_init(&a5);
    lv_anim_set_var(&a5, s_bright_hint);
    lv_anim_set_values(&a5, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a5, fade_opa_cb);
    lv_anim_set_duration(&a5, BRIGHT_ANIM_MS);
    lv_anim_set_completed_cb(&a5, enter_bright_done_cb);
    lv_anim_start(&a5);

    ESP_LOGI(TAG, "Entered brightness mode (brightness=%d)", s_brightness);
}

static void exit_brightness_mode(void)
{
    if (s_mode != UI_MODE_BRIGHTNESS || s_animating) return;
    s_mode = UI_MODE_MENU;
    s_animating = true;

    int current_diam = s_brightness > 0
        ? BRIGHT_CIRCLE_MIN + (BRIGHT_CIRCLE_MAX - BRIGHT_CIRCLE_MIN) * s_brightness / 100
        : 1;

    /* Show lights page underneath, starting transparent */
    lv_obj_set_style_opa(s_pages[s_current], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_pages[s_current], LV_OBJ_FLAG_HIDDEN);

    /* Anim 1: Fade in lights page */
    lv_anim_t a1;
    lv_anim_init(&a1);
    lv_anim_set_var(&a1, s_pages[s_current]);
    lv_anim_set_values(&a1, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a1, fade_opa_cb);
    lv_anim_set_duration(&a1, BRIGHT_ANIM_MS);
    lv_anim_start(&a1);

    /* Anim 2: Shrink outer circle 456 -> 308 */
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, s_bright_outer);
    lv_anim_set_values(&a2, BRIGHT_OUTER_DIAM, CIRCLE_DIAM);
    lv_anim_set_exec_cb(&a2, circle_size_cb);
    lv_anim_set_duration(&a2, BRIGHT_ANIM_MS);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_start(&a2);

    /* Anim 3: Shrink yellow circle -> 1 */
    lv_anim_t a3;
    lv_anim_init(&a3);
    lv_anim_set_var(&a3, s_bright_circle);
    lv_anim_set_values(&a3, current_diam, 1);
    lv_anim_set_exec_cb(&a3, circle_size_cb);
    lv_anim_set_duration(&a3, BRIGHT_ANIM_MS);
    lv_anim_set_path_cb(&a3, lv_anim_path_ease_in);
    lv_anim_start(&a3);

    /* Anim 4: Fade out brightness number */
    lv_anim_t a4;
    lv_anim_init(&a4);
    lv_anim_set_var(&a4, s_bright_label);
    lv_anim_set_values(&a4, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a4, fade_opa_cb);
    lv_anim_set_duration(&a4, BRIGHT_ANIM_MS);
    lv_anim_start(&a4);

    /* Anim 5: Fade out hint (carries done callback) */
    lv_anim_t a5;
    lv_anim_init(&a5);
    lv_anim_set_var(&a5, s_bright_hint);
    lv_anim_set_values(&a5, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a5, fade_opa_cb);
    lv_anim_set_duration(&a5, BRIGHT_ANIM_MS);
    lv_anim_set_completed_cb(&a5, exit_bright_done_cb);
    lv_anim_start(&a5);

    ESP_LOGI(TAG, "Exited brightness mode (brightness=%d)", s_brightness);
}

/* Fan transition: all 3 circles scale up proportionally from menu circle size */
#define FAN_ANIM_MS 400
static const int fan_diams[3] = {FAN_OUTER_DIAM, FAN_MID_DIAM, FAN_SMALL_DIAM};
static const int fan_start_diams[3] = {
    CIRCLE_DIAM,
    FAN_MID_DIAM * CIRCLE_DIAM / FAN_OUTER_DIAM,
    FAN_SMALL_DIAM * CIRCLE_DIAM / FAN_OUTER_DIAM,
};

static void enter_fan_done_cb(lv_anim_t *a)
{
    if (s_fade_out_target) {
        lv_obj_add_flag(s_fade_out_target, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_fade_out_target, LV_OPA_COVER, 0);
        s_fade_out_target = NULL;
    }
    s_animating = false;
}

static void exit_fan_done_cb(lv_anim_t *a)
{
    lv_obj_add_flag(s_fan_page, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        lv_obj_set_size(s_fan_circles[i], fan_diams[i], fan_diams[i]);
        lv_obj_center(s_fan_circles[i]);
    }
    lv_obj_set_style_opa(s_fan_label, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_fan_hint, LV_OPA_COVER, 0);
    update_fan_display();
    s_animating = false;
}

static void enter_fan_mode(void)
{
    if (s_mode != UI_MODE_MENU || s_animating) return;
    s_mode = UI_MODE_FAN;
    s_animating = true;
    s_fan_accum = 0;

    update_fan_display();

    /* Set circles to starting sizes (proportional to menu circle) */
    for (int i = 0; i < 3; i++) {
        lv_obj_set_size(s_fan_circles[i], fan_start_diams[i], fan_start_diams[i]);
        lv_obj_center(s_fan_circles[i]);
    }
    lv_obj_set_style_opa(s_fan_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_fan_hint, LV_OPA_TRANSP, 0);

    lv_obj_set_style_opa(s_fan_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_fan_page, LV_OBJ_FLAG_HIDDEN);

    s_fade_out_target = s_pages[s_current];

    /* Fade out menu page */
    lv_anim_t a_menu;
    lv_anim_init(&a_menu);
    lv_anim_set_var(&a_menu, s_pages[s_current]);
    lv_anim_set_values(&a_menu, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a_menu, fade_opa_cb);
    lv_anim_set_duration(&a_menu, FAN_ANIM_MS);
    lv_anim_start(&a_menu);

    /* Grow all 3 circles from start to final size */
    for (int i = 0; i < 3; i++) {
        lv_anim_t ac;
        lv_anim_init(&ac);
        lv_anim_set_var(&ac, s_fan_circles[i]);
        lv_anim_set_values(&ac, fan_start_diams[i], fan_diams[i]);
        lv_anim_set_exec_cb(&ac, circle_size_cb);
        lv_anim_set_duration(&ac, FAN_ANIM_MS);
        lv_anim_set_path_cb(&ac, lv_anim_path_ease_out);
        lv_anim_start(&ac);
    }

    /* Fade in fan label */
    lv_anim_t a_lbl;
    lv_anim_init(&a_lbl);
    lv_anim_set_var(&a_lbl, s_fan_label);
    lv_anim_set_values(&a_lbl, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_lbl, fade_opa_cb);
    lv_anim_set_duration(&a_lbl, FAN_ANIM_MS);
    lv_anim_start(&a_lbl);

    /* Fade in hint (carries done callback) */
    lv_anim_t a_hint;
    lv_anim_init(&a_hint);
    lv_anim_set_var(&a_hint, s_fan_hint);
    lv_anim_set_values(&a_hint, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_hint, fade_opa_cb);
    lv_anim_set_duration(&a_hint, FAN_ANIM_MS);
    lv_anim_set_completed_cb(&a_hint, enter_fan_done_cb);
    lv_anim_start(&a_hint);

    ESP_LOGI(TAG, "Entered fan mode (level=%d)", s_fan_level);
}

static void exit_fan_mode(void)
{
    if (s_mode != UI_MODE_FAN || s_animating) return;
    s_mode = UI_MODE_MENU;
    s_animating = true;

    /* Show menu page underneath, starting transparent */
    lv_obj_set_style_opa(s_pages[s_current], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_pages[s_current], LV_OBJ_FLAG_HIDDEN);

    /* Fade in menu page */
    lv_anim_t a_menu;
    lv_anim_init(&a_menu);
    lv_anim_set_var(&a_menu, s_pages[s_current]);
    lv_anim_set_values(&a_menu, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_menu, fade_opa_cb);
    lv_anim_set_duration(&a_menu, FAN_ANIM_MS);
    lv_anim_start(&a_menu);

    /* Shrink all 3 circles back to start sizes */
    for (int i = 0; i < 3; i++) {
        lv_anim_t ac;
        lv_anim_init(&ac);
        lv_anim_set_var(&ac, s_fan_circles[i]);
        lv_anim_set_values(&ac, fan_diams[i], fan_start_diams[i]);
        lv_anim_set_exec_cb(&ac, circle_size_cb);
        lv_anim_set_duration(&ac, FAN_ANIM_MS);
        lv_anim_set_path_cb(&ac, lv_anim_path_ease_in);
        lv_anim_start(&ac);
    }

    /* Fade out fan label */
    lv_anim_t a_lbl;
    lv_anim_init(&a_lbl);
    lv_anim_set_var(&a_lbl, s_fan_label);
    lv_anim_set_values(&a_lbl, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a_lbl, fade_opa_cb);
    lv_anim_set_duration(&a_lbl, FAN_ANIM_MS);
    lv_anim_start(&a_lbl);

    /* Fade out hint (carries done callback) */
    lv_anim_t a_hint;
    lv_anim_init(&a_hint);
    lv_anim_set_var(&a_hint, s_fan_hint);
    lv_anim_set_values(&a_hint, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a_hint, fade_opa_cb);
    lv_anim_set_duration(&a_hint, FAN_ANIM_MS);
    lv_anim_set_completed_cb(&a_hint, exit_fan_done_cb);
    lv_anim_start(&a_hint);

    ESP_LOGI(TAG, "Exited fan mode (level=%d)", s_fan_level);
}

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

void ui_init(void)
{
    ESP_LOGI(TAG, "Images: lights=%ldx%ld bg_yellow=%ldx%ld bg_teal=%ldx%ld",
             (long)lights.header.w, (long)lights.header.h,
             (long)bg_glow_yellow.header.w, (long)bg_glow_yellow.header.h,
             (long)bg_glow_teal.header.w, (long)bg_glow_teal.header.h);

    /* Single screen -- never switched */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);

    /* Build menu pages */
    s_pages[0] = build_lights_page(s_screen);
    s_pages[1] = build_fan_page(s_screen);

    /* Build brightness detail page (initially hidden) */
    s_bright_page = build_brightness_page(s_screen);
    lv_obj_set_style_opa(s_bright_page, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_bright_page, LV_OBJ_FLAG_HIDDEN);

    /* Build fan detail page (initially hidden) */
    s_fan_page = build_fan_detail_page(s_screen);
    lv_obj_set_style_opa(s_fan_page, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_fan_page, LV_OBJ_FLAG_HIDDEN);

    /* Start with page 0 visible, page 1 hidden */
    lv_obj_set_style_opa(s_pages[0], LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_pages[1], LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_pages[1], LV_OBJ_FLAG_HIDDEN);

    /* Status label (on the screen itself, above everything) */
    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status_label, 440);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -38);

    lv_screen_load(s_screen);
    s_current = 0;
    s_mode = UI_MODE_MENU;
    s_animating = false;

    ESP_LOGI(TAG, "UI initialized with %d pages + brightness/fan detail", SCREEN_COUNT);
}

void ui_set_status(const char *text)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
}

void ui_encoder_navigate(int diff)
{
    if (diff == 0) return;

    if (s_mode == UI_MODE_BRIGHTNESS) {
        s_brightness -= diff;  /* opposite rotation to encoder */
        if (s_brightness < 0) s_brightness = 0;
        if (s_brightness > 100) s_brightness = 100;
        update_brightness_display();
    } else if (s_mode == UI_MODE_FAN) {
        s_fan_accum -= diff;  /* reversed so increase is opposite direction */
        if (s_fan_accum >= 3) {
            s_fan_level++;
            if (s_fan_level > 3) s_fan_level = 3;
            s_fan_accum = 0;
            update_fan_display();
        } else if (s_fan_accum <= -3) {
            s_fan_level--;
            if (s_fan_level < 0) s_fan_level = 0;
            s_fan_accum = 0;
            update_fan_display();
        }
    } else {
        int next = s_current + (diff > 0 ? 1 : -1);
        switch_page(next);
    }
}

void ui_encoder_press(void)
{
    if (s_mode == UI_MODE_MENU) {
        if (s_current == 0) {
            enter_brightness_mode();
        } else if (s_current == 1) {
            enter_fan_mode();
        }
    } else if (s_mode == UI_MODE_BRIGHTNESS) {
        exit_brightness_mode();
    } else if (s_mode == UI_MODE_FAN) {
        exit_fan_mode();
    }
}

void ui_encoder_long_press(void)
{
    if (s_mode == UI_MODE_BRIGHTNESS) {
        s_brightness = 0;
        update_brightness_display();
        ESP_LOGI(TAG, "Long press: brightness set to 0");
        exit_brightness_mode();
    } else if (s_mode == UI_MODE_FAN) {
        s_fan_level = 0;
        update_fan_display();
        ESP_LOGI(TAG, "Long press: fan set to off");
        exit_fan_mode();
    }
}

int ui_get_current_page(void)
{
    return s_current;
}

bool ui_is_detail_mode(void)
{
    return s_mode != UI_MODE_MENU;
}

int ui_get_brightness(void)
{
    return s_brightness;
}

int ui_get_fan_level(void)
{
    return s_fan_level;
}
