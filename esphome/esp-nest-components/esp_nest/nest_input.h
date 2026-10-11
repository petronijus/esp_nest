#pragma once

#include "esp_err.h"
#include "lvgl.h"

namespace esphome::esp_nest {

/*
 * The knob: MT8901 rotary encoder (PCNT, GPIO 5 = signal, GPIO 6 =
 * direction) and the push button (GPIO 3, active low). nest_ui task only:
 * every entry point asserts it.
 *
 * One LVGL encoder indev polls both every INPUT_READ_PERIOD_MS (native
 * cadence). Each read runs the pure input state machine (input_fsm) and
 * dispatches its actions to the UI (ui_encoder_navigate / long_press /
 * press, in that order), then feeds the activity flag of the same read to
 * the backlight idle policy (one backlight_update() per read).
 *
 * Time is int64 milliseconds from ui_now_ms(), never lv_tick_get() or
 * millis() (uint32, wrap after 49.7 days).
 */

/// Indev read period. LVGL's default read period follows the display
/// refresh period (15 ms here); native read the knob every 33 ms, and the
/// menu accumulation (3 counts per page step) and the press timings are
/// tuned to it.
static constexpr uint32_t INPUT_READ_PERIOD_MS = 33;

/// PCNT unit, encoder channel and button GPIO. Call before input_create_indev().
esp_err_t input_init();

/// Creates the LVGL encoder indev (needs the LVGL display) and pins its
/// read timer to INPUT_READ_PERIOD_MS. The backlight must be started before
/// the first read (backlight_start()), i.e. before the next lv_timer_handler().
esp_err_t input_create_indev();

}  // namespace esphome::esp_nest
