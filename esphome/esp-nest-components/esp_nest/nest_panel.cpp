#include "nest_panel.h"

#include <cstring>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nest_core.h"

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest.panel";

/* ---- Board pin map (ZX2D10GE01R-V4848, main/board.h) ---- */
static constexpr int PIN_SPI_MOSI = 41;
static constexpr int PIN_SPI_CLK = 47;
static constexpr int PIN_SPI_CS = 21;
static constexpr int PIN_PCLK = 45;
static constexpr int PIN_VSYNC = 48;
static constexpr int PIN_HSYNC = 40;
static constexpr int PIN_DE = 39;
static constexpr int DATA_PINS[16] = {47, 41, 0,  42, 14,      // B0..B4
                                      8,  13, 18, 12, 11, 17,  // G0..G5
                                      10, 16, 9,  15, 46};     // R0..R4

static constexpr int SPI_CLOCK_HZ = 10 * 1000 * 1000;
static constexpr int PIXEL_CLOCK_HZ = 15 * 1000 * 1000;
static constexpr int BOUNCE_BUFFER_LINES = 10;

/* ---- ST7701S commands ---- */
static constexpr uint8_t CMD_SWRESET = 0x01;
static constexpr uint8_t CMD_SLPOUT = 0x11;
static constexpr uint8_t CMD_DISPON = 0x29;
static constexpr uint8_t CMD_MADCTL = 0x36;
static constexpr uint8_t CMD_COLMOD = 0x3A;
static constexpr uint8_t CMD_SDIR = 0xC7;  // command bank 0x10
static constexpr uint8_t CMD_BANK_SELECT = 0xFF;
static constexpr uint8_t SDIR_SOURCE_REVERSE = 0x04;

/* Native init sequence (main/screen.c st7701s_init_sequence), up to the
 * point where SDIR is written; {cmd, len, data...}. */
struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[16];
};
static const InitCmd INIT_SEQ[] = {
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, {0x08}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, {0x3B, 0x00}},
    {0xC1, 2, {0x0B, 0x02}},
    {0xC2, 2, {0x07, 0x02}},
    {0xCC, 1, {0x10}},
    {0xCD, 1, {0x08}},  // RGB565
    // Positive / negative gamma
    {0xB0, 16, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}},
    {0xB1, 16, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, {0x6D}},
    {0xB1, 1, {0x37}},
    {0xB2, 1, {0x81}},
    {0xB3, 1, {0x80}},
    {0xB5, 1, {0x43}},
    {0xB7, 1, {0x85}},
    {0xB8, 1, {0x20}},
    {0xC1, 1, {0x78}},
    {0xC2, 1, {0x78}},
    {0xD0, 1, {0x88}},
    {0xE0, 3, {0x00, 0x00, 0x02}},
    {0xE1, 11, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20}},
    {0xE2, 13, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE3, 4, {0x00, 0x00, 0x11, 0x00}},
    {0xE4, 2, {0x22, 0x00}},
    {0xE5, 16, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE6, 4, {0x00, 0x00, 0x11, 0x00}},
    {0xE7, 2, {0x22, 0x00}},
    {0xE8, 16, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xEB, 7, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}},
    {0xED, 16, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}},
    {0xEF, 6, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, {0x08}},
};

static uint16_t *s_fb = nullptr;
static bool s_hw_mirror_x = true;

/* ---- 9-bit SPI: bit 8 = D/C (0 command, 1 data) ---- */

static esp_err_t spi_write9(spi_device_handle_t spi, uint16_t word) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_CMD;
  t.base.cmd = word;
  t.command_bits = 9;
  return spi_device_transmit(spi, reinterpret_cast<spi_transaction_t *>(&t));
}

static esp_err_t lcd_cmd(spi_device_handle_t spi, uint8_t cmd, const uint8_t *data, size_t len) {
  ESP_RETURN_ON_ERROR(spi_write9(spi, cmd), TAG, "command 0x%02X", cmd);
  for (size_t i = 0; i < len; i++)
    ESP_RETURN_ON_ERROR(spi_write9(spi, 0x100 | data[i]), TAG, "data of command 0x%02X", cmd);
  return ESP_OK;
}

static esp_err_t send_init_sequence(spi_device_handle_t spi, bool hw_mirror_x) {
  // Known register state first: the panel survives MCU resets.
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_SWRESET, nullptr, 0), TAG, "SWRESET");
  vTaskDelay(pdMS_TO_TICKS(120));

  for (const auto &c : INIT_SEQ)
    ESP_RETURN_ON_ERROR(lcd_cmd(spi, c.cmd, c.data, c.len), TAG, "init sequence");

  // SDIR lives in command bank 0x10; it is written in both modes so the
  // result never depends on what an earlier firmware left in the panel.
  const uint8_t bank_10[] = {0x77, 0x01, 0x00, 0x00, 0x10};
  const uint8_t sdir[] = {static_cast<uint8_t>(hw_mirror_x ? SDIR_SOURCE_REVERSE : 0x00)};
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_BANK_SELECT, bank_10, sizeof bank_10), TAG, "bank 0x10");
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_SDIR, sdir, sizeof sdir), TAG, "SDIR");

  const uint8_t bank_off[] = {0x77, 0x01, 0x00, 0x00, 0x00};
  const uint8_t madctl[] = {0x00};
  const uint8_t colmod[] = {0x66};  // RGB666 on the RGB interface (native)
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_BANK_SELECT, bank_off, sizeof bank_off), TAG, "bank off");
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_MADCTL, madctl, sizeof madctl), TAG, "MADCTL");
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_COLMOD, colmod, sizeof colmod), TAG, "COLMOD");
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_SLPOUT, nullptr, 0), TAG, "SLPOUT");
  vTaskDelay(pdMS_TO_TICKS(120));
  ESP_RETURN_ON_ERROR(lcd_cmd(spi, CMD_DISPON, nullptr, 0), TAG, "DISPON");
  vTaskDelay(pdMS_TO_TICKS(20));
  return ESP_OK;
}

static esp_err_t controller_init_over_spi(bool hw_mirror_x) {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_SPI_CLK;
  bus.mosi_io_num = PIN_SPI_MOSI;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 64;
  ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED), TAG, "SPI bus");

  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = SPI_CLOCK_HZ;
  dev.mode = 0;
  dev.spics_io_num = PIN_SPI_CS;
  dev.queue_size = 1;
  spi_device_handle_t spi = nullptr;
  esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &spi);
  if (err == ESP_OK) {
    err = send_init_sequence(spi, hw_mirror_x);
    spi_bus_remove_device(spi);
  } else {
    ESP_LOGE(TAG, "SPI device: %s", esp_err_to_name(err));
  }
  // The init pins double as RGB data pins: always release the bus.
  spi_bus_free(SPI2_HOST);
  return err;
}

static esp_err_t rgb_panel_init() {
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
  cfg.timings.pclk_hz = PIXEL_CLOCK_HZ;
  cfg.timings.h_res = PANEL_H_RES;
  cfg.timings.v_res = PANEL_V_RES;
  cfg.timings.hsync_pulse_width = 10;
  cfg.timings.hsync_back_porch = 10;
  cfg.timings.hsync_front_porch = 10;
  cfg.timings.vsync_pulse_width = 2;
  cfg.timings.vsync_back_porch = 12;
  cfg.timings.vsync_front_porch = 14;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = BOUNCE_BUFFER_LINES * PANEL_H_RES;
  cfg.flags.fb_in_psram = 1;

  esp_lcd_panel_handle_t panel = nullptr;
  ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &panel), TAG, "RGB panel");

  esp_err_t ret = ESP_OK;
  void *fb = nullptr;
  ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), fail, TAG, "RGB panel reset");
  ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), fail, TAG, "RGB panel init");
  ESP_GOTO_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb), fail, TAG, "frame buffer");
  s_fb = static_cast<uint16_t *>(fb);
  return ESP_OK;

fail:
  esp_lcd_panel_del(panel);
  return ret;
}

esp_err_t panel_init(bool hw_mirror_x) {
  NEST_ASSERT_UI_TASK(ESP_ERR_INVALID_STATE);
  s_hw_mirror_x = hw_mirror_x;
  ESP_RETURN_ON_ERROR(controller_init_over_spi(hw_mirror_x), TAG, "ST7701S init");
  ESP_RETURN_ON_ERROR(rgb_panel_init(), TAG, "RGB panel");
  ESP_LOGI(TAG, "ST7701S %dx%d up, 180 degrees via %s", static_cast<int>(PANEL_H_RES), static_cast<int>(PANEL_V_RES),
           hw_mirror_x ? "SDIR + row flip" : "per-pixel copy");
  return ESP_OK;
}

void panel_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const int32_t w = lv_area_get_width(area);
  const int32_t h = lv_area_get_height(area);
  const auto *src = reinterpret_cast<const uint16_t *>(px_map);
  for (int32_t y = 0; y < h; y++) {
    uint16_t *dst_row = s_fb + (PANEL_V_RES - 1 - (area->y1 + y)) * PANEL_H_RES;
    const uint16_t *src_row = src + y * w;
    if (s_hw_mirror_x) {
      // The panel mirrors X itself: copy the row as is, rows bottom-up.
      std::memcpy(dst_row + area->x1, src_row, w * sizeof(uint16_t));
    } else {
      const int32_t dst_x = PANEL_H_RES - 1 - area->x1;
      for (int32_t x = 0; x < w; x++)
        dst_row[dst_x - x] = src_row[x];
    }
  }
  lv_display_flush_ready(disp);
}

}  // namespace esphome::esp_nest
