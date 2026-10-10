#include "nest_probe.h"

#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

LV_FONT_DECLARE(font_helvetica_140);

namespace esphome::nest_probe {

static const char *const TAG = "nest_probe";

static int64_t s_refr_start_us = 0;
static uint32_t s_frames = 0;
static int64_t s_refr_total_us = 0;
static int64_t s_refr_max_us = 0;
static uint32_t s_window_start_ms = 0;

static void refr_event_cb(lv_event_t *e) {
  const int64_t now = esp_timer_get_time();
  if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
    s_refr_start_us = now;
    return;
  }
  const int64_t dt = now - s_refr_start_us;
  s_frames++;
  s_refr_total_us += dt;
  if (dt > s_refr_max_us)
    s_refr_max_us = dt;
}

static void opa_cb(void *obj, int32_t v) { lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), v, 0); }
static void arc_cb(void *obj, int32_t v) { lv_arc_set_end_angle(static_cast<lv_obj_t *>(obj), v); }

static void digits_timer_cb(lv_timer_t *t) {
  static uint32_t n = 0;
  lv_label_set_text_fmt(static_cast<lv_obj_t *>(lv_timer_get_user_data(t)), "%02lu", (unsigned long) (n++ % 100));
}

void NestProbe::setup() {
  lv_display_t *disp = lv_display_get_default();
  if (disp == nullptr) {
    ESP_LOGE(TAG, "No LVGL display — LVGL not initialised before probe");
    this->mark_failed();
    return;
  }
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Edge glow: 480x200 vertical gradient, faded like the menu page glow.
  lv_obj_t *glow = lv_obj_create(scr);
  lv_obj_remove_style_all(glow);
  lv_obj_set_size(glow, 480, 200);
  lv_obj_align(glow, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_opa(glow, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(glow, lv_color_hex(0xFFE600), 0);
  lv_obj_set_style_bg_grad_color(glow, lv_color_black(), 0);
  lv_obj_set_style_bg_grad_dir(glow, LV_GRAD_DIR_VER, 0);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, glow);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_exec_cb(&a, opa_cb);
  lv_anim_set_duration(&a, 300);
  lv_anim_set_reverse_duration(&a, 300);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);

  // 456 px ring + sweeping arc indicator.
  lv_obj_t *ring = lv_obj_create(scr);
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, 456, 456);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 4, 0);
  lv_obj_set_style_border_color(ring, lv_color_hex(0x00E5CC), 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);

  lv_obj_t *arc = lv_arc_create(scr);
  lv_obj_set_size(arc, 440, 440);
  lv_obj_center(arc);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_start_angle(arc, 270);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0xFFE600), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);

  lv_anim_t b;
  lv_anim_init(&b);
  lv_anim_set_var(&b, arc);
  lv_anim_set_values(&b, 270, 270 + 359);
  lv_anim_set_exec_cb(&b, arc_cb);
  lv_anim_set_duration(&b, 1500);
  lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&b);

  // 140 px label using the project's own C-array font.
  lv_obj_t *label = lv_label_create(scr);
  lv_obj_set_style_text_font(label, &font_helvetica_140, 0);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_center(label);
  lv_timer_create(digits_timer_cb, 100, label);

  lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, nullptr);
  s_window_start_ms = millis();
  ESP_LOGI(TAG, "Stress scene created");
}

void NestProbe::loop() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - s_window_start_ms;
  if (elapsed < 5000)
    return;
  const float fps = s_frames * 1000.0f / elapsed;
  const float avg_ms = s_frames ? s_refr_total_us / 1000.0f / s_frames : 0.0f;
  ESP_LOGI(TAG, "fps=%.1f refresh avg=%.1f ms max=%.1f ms | free internal=%u psram=%u", fps, avg_ms,
           s_refr_max_us / 1000.0f, (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  s_frames = 0;
  s_refr_total_us = 0;
  s_refr_max_us = 0;
  s_window_start_ms = now;
}

}  // namespace esphome::nest_probe
