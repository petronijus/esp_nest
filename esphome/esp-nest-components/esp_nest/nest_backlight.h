#pragma once

#include <cstdint>

#include "esp_err.h"

namespace esphome::esp_nest {

/*
 * Backlight PWM (LEDC, GPIO 38) driven by the pure backlight_policy module.
 * nest_ui task only: every entry point asserts it.
 *
 * Hardware parity with main/screen.c: LEDC low-speed timer 0 / channel 0,
 * 5 kHz, 13-bit duty, duty = 8191 * percent / 100, 1 s idle fade. Unlike
 * native, every direct duty set first stops a fade still in progress
 * (ledc_fade_stop), and only the thread-safe LEDC fade APIs are used.
 *
 * Time is int64 milliseconds from ui_now_ms(), never lv_tick_get() or
 * millis().
 */

/// LEDC timer, channel and fade service, duty 0 (backlight off while the
/// panel initialises, so no garbage frame is ever visible).
esp_err_t backlight_init();

/// Starts the idle policy with the given settings and lights the panel at
/// the active level. Call once, after the first frame has been flushed.
void backlight_start(int64_t now_ms, int active_pct, int dim_pct, int timeout_s);

/// One indev read: restarts the idle timer on activity (instant wake from
/// dim), otherwise fades to the dim level once the idle timeout elapsed.
void backlight_update(int64_t now_ms, bool activity);

/// Settings, with the native rules: the active level applies now unless
/// dimmed, the dim level applies now only when dimmed, a timeout change
/// does not restart the idle timer. Values are clamped by the policy.
/// backlight_update() and the setters are refused (logged) before
/// backlight_start(), which receives the initial settings.
void backlight_set_active(int pct);
void backlight_set_dim(int pct);
void backlight_set_timeout(int seconds);

}  // namespace esphome::esp_nest
