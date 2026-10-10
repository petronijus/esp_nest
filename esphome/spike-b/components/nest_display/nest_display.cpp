#include "nest_display.h"

#include <cstring>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_timer.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

LV_FONT_DECLARE(font_helvetica_140);

namespace esphome::nest_display {

static const char *const TAG = "nest_display";

/* ---- Board pin map (ZX2D10GE01R-V4848, see main/board.h) ---- */
static constexpr int H_RES = 480, V_RES = 480;
static constexpr int PIN_SPI_MOSI = 41, PIN_SPI_CLK = 47, PIN_SPI_CS = 21;
static constexpr int PIN_PCLK = 45, PIN_VSYNC = 48, PIN_HSYNC = 40, PIN_DE = 39;
static constexpr int PIN_BACKLIGHT = 38, PIN_ENC_A = 5, PIN_ENC_B = 6, PIN_BUTTON = 3;
static constexpr int DATA_PINS[16] = {47, 41, 0, 42, 14,       // B0..B4
                                      8, 13, 18, 12, 11, 17,   // G0..G5
                                      10, 16, 9, 15, 46};      // R0..R4
static constexpr int DRAW_BUF_LINES = 40;

/* ---- ST7701S init sequence (main/screen.c), {cmd, len, data...} ---- */
struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[16];
};
static const InitCmd INIT_SEQ[] = {
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}}, {0xEF, 1, {0x08}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}}, {0xC0, 2, {0x3B, 0x00}},
    {0xC1, 2, {0x0B, 0x02}}, {0xC2, 2, {0x07, 0x02}}, {0xCC, 1, {0x10}}, {0xCD, 1, {0x08}},
    {0xB0, 16, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}},
    {0xB1, 16, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x11}}, {0xB0, 1, {0x6D}}, {0xB1, 1, {0x37}}, {0xB2, 1, {0x81}},
    {0xB3, 1, {0x80}}, {0xB5, 1, {0x43}}, {0xB7, 1, {0x85}}, {0xB8, 1, {0x20}}, {0xC1, 1, {0x78}},
    {0xC2, 1, {0x78}}, {0xD0, 1, {0x88}}, {0xE0, 3, {0x00, 0x00, 0x02}},
    {0xE1, 11, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20}},
    {0xE2, 13, {0}}, {0xE3, 4, {0x00, 0x00, 0x11, 0x00}}, {0xE4, 2, {0x22, 0x00}},
    {0xE5, 16, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0}},
    {0xE6, 4, {0x00, 0x00, 0x11, 0x00}}, {0xE7, 2, {0x22, 0x00}},
    {0xE8, 16, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0}},
    {0xEB, 7, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}},
    {0xED, 16, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}},
    {0xEF, 6, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}}, {0xEF, 1, {0x08}},
};

static spi_device_handle_t s_spi;
static uint16_t *s_fb;
static bool s_hw_mirror_x;
static pcnt_unit_handle_t s_pcnt;

static void spi_write9(uint16_t word) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_CMD;
  t.base.cmd = word;
  t.command_bits = 9;
  ESP_ERROR_CHECK(spi_device_transmit(s_spi, reinterpret_cast<spi_transaction_t *>(&t)));
}
static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
  spi_write9(cmd);
  for (size_t i = 0; i < len; i++)
    spi_write9(0x100 | data[i]);
}

static void panel_init_over_spi() {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_SPI_CLK;
  bus.mosi_io_num = PIN_SPI_MOSI;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 64;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 10 * 1000 * 1000;
  dev.spics_io_num = PIN_SPI_CS;
  dev.queue_size = 1;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

  // The panel has no reset line and keeps its registers across MCU resets
  // (an earlier firmware's SDIR survived an OTA reboot): always start from a
  // software reset so every register below is in a known state.
  lcd_cmd(0x01, nullptr, 0);
  vTaskDelay(pdMS_TO_TICKS(120));

  for (const auto &c : INIT_SEQ)
    lcd_cmd(c.cmd, c.data, c.len);
  // SDIR (0xC7) lives in command bank 0x10; bit 2 reverses the source
  // (column) direction, so the panel itself performs the X half of 180°.
  // Written in both modes so the result never depends on prior panel state.
  const uint8_t bk0[] = {0x77, 0x01, 0x00, 0x00, 0x10};
  const uint8_t sdir[] = {static_cast<uint8_t>(s_hw_mirror_x ? 0x04 : 0x00)};
  lcd_cmd(0xFF, bk0, sizeof bk0);
  lcd_cmd(0xC7, sdir, sizeof sdir);
  const uint8_t bk_off[] = {0x77, 0x01, 0x00, 0x00, 0x00}, madctl[] = {0x00}, colmod[] = {0x66};
  lcd_cmd(0xFF, bk_off, sizeof bk_off);
  lcd_cmd(0x36, madctl, sizeof madctl);
  lcd_cmd(0x3A, colmod, sizeof colmod);
  lcd_cmd(0x11, nullptr, 0);
  vTaskDelay(pdMS_TO_TICKS(120));
  lcd_cmd(0x29, nullptr, 0);
  vTaskDelay(pdMS_TO_TICKS(20));

  // The init pins double as RGB data pins: release the bus before the RGB panel claims them.
  spi_bus_remove_device(s_spi);
  spi_bus_free(SPI2_HOST);
}

static void rgb_panel_init() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.data_width = 16;
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  cfg.pclk_gpio_num = PIN_PCLK;
  cfg.vsync_gpio_num = PIN_VSYNC;
  cfg.hsync_gpio_num = PIN_HSYNC;
  cfg.de_gpio_num = PIN_DE;
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < 16; i++)
    cfg.data_gpio_nums[i] = DATA_PINS[i];
  cfg.timings.pclk_hz = 15 * 1000 * 1000;
  cfg.timings.h_res = H_RES;
  cfg.timings.v_res = V_RES;
  cfg.timings.hsync_pulse_width = 10;
  cfg.timings.hsync_back_porch = 10;
  cfg.timings.hsync_front_porch = 10;
  cfg.timings.vsync_pulse_width = 2;
  cfg.timings.vsync_back_porch = 12;
  cfg.timings.vsync_front_porch = 14;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = 10 * H_RES;
  cfg.flags.fb_in_psram = 1;
  esp_lcd_panel_handle_t panel;
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  void *fb = nullptr;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb));
  s_fb = static_cast<uint16_t *>(fb);
}

static void backlight_init() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.timer_num = LEDC_TIMER_0;
  t.duty_resolution = LEDC_TIMER_13_BIT;
  t.freq_hz = 5000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));
  ledc_channel_config_t c = {};
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_0;
  c.timer_sel = LEDC_TIMER_0;
  c.gpio_num = PIN_BACKLIGHT;
  c.duty = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void backlight_set(int percent) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ((1u << 13) - 1) * percent / 100);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* 180°: either the panel mirrors X and we only reverse row order (one memcpy
 * per row), or we reverse every pixel ourselves like the native firmware. */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const int32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
  const auto *src = reinterpret_cast<const uint16_t *>(px_map);
  for (int32_t y = 0; y < h; y++) {
    uint16_t *dst_row = s_fb + (V_RES - 1 - (area->y1 + y)) * H_RES;
    const uint16_t *src_row = src + y * w;
    if (s_hw_mirror_x) {
      std::memcpy(dst_row + area->x1, src_row, w * sizeof(uint16_t));
    } else {
      const int32_t dst_x = H_RES - 1 - area->x1;
      for (int32_t x = 0; x < w; x++)
        dst_row[dst_x - x] = src_row[x];
    }
  }
  lv_display_flush_ready(disp);
}

static void encoder_init() {
  pcnt_unit_config_t unit = {};
  unit.high_limit = INT16_MAX;
  unit.low_limit = INT16_MIN;
  ESP_ERROR_CHECK(pcnt_new_unit(&unit, &s_pcnt));
  pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
  ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &filter));
  pcnt_chan_config_t chan = {};
  chan.edge_gpio_num = PIN_ENC_A;
  chan.level_gpio_num = PIN_ENC_B;
  pcnt_channel_handle_t ch;
  ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &chan, &ch));
  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                               PCNT_CHANNEL_EDGE_ACTION_INCREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
  ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
  ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
  ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));

  gpio_reset_pin(static_cast<gpio_num_t>(PIN_BUTTON));
  gpio_set_direction(static_cast<gpio_num_t>(PIN_BUTTON), GPIO_MODE_INPUT);
  gpio_set_pull_mode(static_cast<gpio_num_t>(PIN_BUTTON), GPIO_PULLUP_ONLY);
}

static int s_enc_total;
static void encoder_read_cb(lv_indev_t *, lv_indev_data_t *data) {
  static int last = 0;
  int now = 0;
  pcnt_unit_get_count(s_pcnt, &now);
  data->enc_diff = static_cast<int16_t>(now - last);
  s_enc_total += now - last;
  last = now;
  data->state = gpio_get_level(static_cast<gpio_num_t>(PIN_BUTTON)) ? LV_INDEV_STATE_RELEASED
                                                                      : LV_INDEV_STATE_PRESSED;
}

/* ---- Stress scene comparable to the real UI + refresh statistics ---- */
static int64_t s_refr_start_us, s_refr_total_us, s_refr_max_us;
static uint32_t s_frames;

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

static void stats_timer_cb(lv_timer_t *) {
  static int64_t window_start = esp_timer_get_time();
  const int64_t now = esp_timer_get_time();
  const float secs = (now - window_start) / 1e6f;
  ESP_LOGI(TAG, "fps=%.1f refresh avg=%.1f ms max=%.1f ms enc=%d | free internal=%u psram=%u",
           s_frames / secs, s_frames ? s_refr_total_us / 1000.0f / s_frames : 0.0f, s_refr_max_us / 1000.0f,
           s_enc_total, (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  s_frames = 0;
  s_refr_total_us = 0;
  s_refr_max_us = 0;
  window_start = now;
}

static void build_scene(lv_display_t *disp) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

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

  lv_obj_t *label = lv_label_create(scr);
  lv_obj_set_style_text_font(label, &font_helvetica_140, 0);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_center(label);
  lv_timer_create(digits_timer_cb, 100, label);

  lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, nullptr);
  lv_timer_create(stats_timer_cb, 5000, nullptr);
}

static uint32_t tick_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void NestDisplay::ui_task(void *arg) {
  backlight_init();
  panel_init_over_spi();
  rgb_panel_init();

  lv_init();
  lv_tick_set_cb(tick_ms);
  lv_display_t *disp = lv_display_create(H_RES, V_RES);
  const size_t buf_bytes = H_RES * DRAW_BUF_LINES * sizeof(uint16_t);
  void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  encoder_init();
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
  lv_indev_set_read_cb(indev, encoder_read_cb);

  build_scene(disp);
  backlight_set(100);
  ESP_LOGI(TAG, "UI task running on core %d", xPortGetCoreID());

  for (;;) {
    uint32_t next = lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(next < 1 ? 1 : next));
  }
}

void NestDisplay::setup() {
  s_hw_mirror_x = this->hw_mirror_x_;
  xTaskCreatePinnedToCore(ui_task, "nest_ui", 8192, this, 5, nullptr, 1);
}

void NestDisplay::dump_config() {
  ESP_LOGCONFIG(TAG, "Nest display: ST7701S 480x480 RGB, LVGL %d.%d.%d, 180° via %s", LVGL_VERSION_MAJOR,
                LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, s_hw_mirror_x ? "SDIR + row flip" : "per-pixel copy");
}

}  // namespace esphome::nest_display
