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

/* Embedded PNG assets (linked via CMake EMBED_FILES) */
extern const uint8_t lights_png_start[] asm("_binary_lights_png_start");
extern const uint8_t lights_png_end[]   asm("_binary_lights_png_end");
extern const uint8_t fiiuuu_png_start[] asm("_binary_fiiuuu_png_start");
extern const uint8_t fiiuuu_png_end[]   asm("_binary_fiiuuu_png_end");

static lv_image_dsc_t img_lights_dsc;
static lv_image_dsc_t img_fiiuuu_dsc;

/* Single screen with two page containers */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_pages[SCREEN_COUNT];
static int s_current = 0;
static bool s_animating = false;
static lv_obj_t *s_status_label = NULL;

/* ========================================================================== */
/*  PNG image descriptor setup                                                */
/* ========================================================================== */

static void init_image_descriptors(void)
{
    /* Leave header.magic = 0 so the bin_decoder skips these,
       allowing lodepng to handle them via the PNG magic in data */
    memset(&img_lights_dsc, 0, sizeof(img_lights_dsc));
    img_lights_dsc.data_size = lights_png_end - lights_png_start;
    img_lights_dsc.data      = lights_png_start;

    memset(&img_fiiuuu_dsc, 0, sizeof(img_fiiuuu_dsc));
    img_fiiuuu_dsc.data_size = fiiuuu_png_end - fiiuuu_png_start;
    img_fiiuuu_dsc.data      = fiiuuu_png_start;

    ESP_LOGI(TAG, "PNG lights: %u bytes, fiiuuu: %u bytes",
             (unsigned)img_lights_dsc.data_size,
             (unsigned)img_fiiuuu_dsc.data_size);
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
    lv_obj_center(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
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

static lv_obj_t *create_title_image(lv_obj_t *parent, const lv_image_dsc_t *src)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return img;
}

/* ========================================================================== */
/*  Page builders (children of the single screen)                             */
/* ========================================================================== */

static lv_obj_t *build_lights_page(lv_obj_t *parent)
{
    lv_obj_t *page = create_page_container(parent);

    create_circle_outline(page);
    create_dot(page, true, COL_YELLOW, LV_ALIGN_TOP_MID, 77);
    create_dot(page, false, COL_CIRCLE, LV_ALIGN_BOTTOM_MID, -77);
    create_title_image(page, &img_lights_dsc);

    return page;
}

static lv_obj_t *build_fan_page(lv_obj_t *parent)
{
    lv_obj_t *page = create_page_container(parent);

    create_circle_outline(page);
    create_dot(page, false, COL_CIRCLE, LV_ALIGN_TOP_MID, 77);
    create_dot(page, true, COL_TEAL, LV_ALIGN_BOTTOM_MID, -77);
    create_title_image(page, &img_fiiuuu_dsc);

    return page;
}

/* ========================================================================== */
/*  Fade transition (opacity animation on page containers – no screen swap)   */
/* ========================================================================== */

static void fade_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void fade_in_done_cb(lv_anim_t *a)
{
    s_animating = false;
}

static void switch_page(int new_idx)
{
    if (new_idx < 0) new_idx = SCREEN_COUNT - 1;
    if (new_idx >= SCREEN_COUNT) new_idx = 0;
    if (new_idx == s_current) return;
    if (s_animating) return;

    s_animating = true;

    lv_obj_t *old_page = s_pages[s_current];
    lv_obj_t *new_page = s_pages[new_idx];

    /* Fade out the old page */
    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, old_page);
    lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a_out, fade_opa_cb);
    lv_anim_set_duration(&a_out, FADE_TIME_MS);
    lv_anim_start(&a_out);

    /* Fade in the new page */
    lv_obj_set_style_opa(new_page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(new_page, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, new_page);
    lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_in, fade_opa_cb);
    lv_anim_set_duration(&a_in, FADE_TIME_MS);
    lv_anim_set_completed_cb(&a_in, fade_in_done_cb);
    lv_anim_start(&a_in);

    s_current = new_idx;
}

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

void ui_init(void)
{
    init_image_descriptors();

    /* Single screen – never switched */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);

    /* Build both pages as children of the same screen */
    s_pages[0] = build_lights_page(s_screen);
    s_pages[1] = build_fan_page(s_screen);

    /* Start with page 0 visible, page 1 hidden */
    lv_obj_set_style_opa(s_pages[0], LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_pages[1], LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_pages[1], LV_OBJ_FLAG_HIDDEN);

    /* Status label (on the screen itself, above both pages) */
    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status_label, 440);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -38);

    lv_screen_load(s_screen);
    s_current = 0;
    s_animating = false;

    ESP_LOGI(TAG, "UI initialized with %d pages (single screen)", SCREEN_COUNT);
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
    int next = s_current + (diff > 0 ? 1 : -1);
    switch_page(next);
}
