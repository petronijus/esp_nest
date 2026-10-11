#pragma once

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

namespace esphome::esp_nest {

/*
 * ST7701S 2.1" round 480x480 panel of the Panlee ZX2D10GE01R-V4848
 * (pin map: main/board.h). nest_ui task only.
 *
 * The controller is configured once over 9-bit SPI, then driven as a
 * 16-bit parallel RGB565 panel by esp_lcd. The SPI pins double as RGB data
 * pins, so the SPI bus is released before the RGB panel claims them.
 *
 * The panel has no reset line and keeps its registers across MCU resets
 * (an earlier firmware's SDIR survived an OTA reboot on hardware), so the
 * init always starts with a software reset and writes SDIR explicitly.
 *
 * The image is rotated by 180° (the board is mounted upside down):
 *   - hw_mirror_x = true:  SDIR reverses the source direction in the panel
 *                          (X half), flush only reverses the row order;
 *   - hw_mirror_x = false: flush reverses every pixel like the native
 *                          firmware (main/screen.c).
 */

static constexpr int32_t PANEL_H_RES = 480;
static constexpr int32_t PANEL_V_RES = 480;

/// SPI init sequence + RGB panel with one PSRAM framebuffer. On error
/// everything acquired so far is released again.
esp_err_t panel_init(bool hw_mirror_x);

/// LVGL flush callback (RGB565 partial area -> rotated framebuffer).
/// Microseconds spent in panel_flush_cb since the last call (nest_ui only).
int64_t panel_take_flush_time_us();
void panel_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);

}  // namespace esphome::esp_nest
