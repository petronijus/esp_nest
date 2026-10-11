#pragma once

#include <cstdint>

#include "esp_err.h"

namespace esphome::esp_nest {

/*
 * The LED ring: 13x WS2812 (GRB) on GPIO 4, driven through
 * espressif/led_strip over RMT (10 MHz, no DMA), by its own nest_led task
 * (architecture sections 2, 3 and 8). The logic is main/led_ring.c:
 *
 *   - 20 ms tick; menu: 3 s breathing between 5 % and 45 % in the page
 *     accent colour;
 *   - Lights page in detail mode: intensity = UI brightness;
 *   - long-press feedback (600 ms, overrides the above): a swell when
 *     something was turned on, a double blink when it was turned off;
 *   - disabled: all LEDs black, re-sent every 100 ms.
 *
 * Inputs are atomics only, read on every tick: the UI's page, detail mode
 * and brightness (nest_ui.h getters), the enabled flag and the feedback
 * word below (hand-offs 3, 5 and 6). The task owns the strip; loopTask
 * reports its state, errors and stack use.
 *
 * Logging and stack: the task's own code never logs, but the led_strip and
 * RMT driver calls it makes log every failure with ESP_LOGE, which the
 * ESPHome logger (esp_log_set_vprintf) formats on the caller's stack, i.e.
 * on this 3 KB stack (native ran the same driver path on 2 KB). After a
 * failed frame the task holds writes back for 1 s, so a persistent fault
 * logs about once a second instead of every tick. The success-path stack
 * high-water mark is logged 10 s after start; every write-error report
 * repeats it, which covers the error path. To measure that path on
 * hardware, build once with `esphome: build_flags:
 * [-DNEST_LED_INJECT_WRITE_FAULT]`: every frame then also writes one
 * out-of-range pixel, which the driver rejects with an ESP_LOGE (the ring
 * keeps animating at the 1 s retry rate).
 *
 * Placement: native parity is priority 3 / unpinned, with the strip created
 * from app_main on core 0, so the RMT interrupt (allocated on the core that
 * creates the channel) sat on core 0, away from the RGB panel's bounce
 * buffer interrupt on core 1. Under ESPHome loopTask runs on core 1, so the
 * task is pinned to core 0 and creates the strip itself: the RMT interrupt
 * stays on core 0 deterministically.
 */

static constexpr int LED_TASK_CORE = 0;
static constexpr unsigned LED_TASK_PRIORITY = 3;
static constexpr uint32_t LED_TASK_STACK_BYTES = 3 * 1024;
static constexpr int LED_RING_GPIO = 4;
static constexpr uint32_t LED_RING_COUNT = 13;

/// Start-up progress of the nest_led task, published to loopTask.
enum class LedRingState : uint8_t {
  NOT_STARTED,  ///< led_ring_start() not called yet
  STARTING,     ///< task created, creating the strip
  RUNNING,      ///< strip cleared, animation running
  FAILED,       ///< the task or the strip could not be created; LEDs untouched
};

/// Creates the nest_led task, which creates and clears the strip and then
/// animates it. loopTask only, once. The outcome is reported through
/// led_ring_state() and led_ring_error().
void led_ring_start();

/// Current start-up state (any task).
LedRingState led_ring_state();

/// Why the start failed (ESP_OK unless led_ring_state() is FAILED).
esp_err_t led_ring_error();

/// LED ring on/off (hand-off 3). Any task; nest_led applies it on its next
/// tick (at most 20 ms later). On at boot (native default).
void led_ring_set_enabled(bool enabled);
bool led_ring_enabled();

/// Strip writes (set_pixel / refresh) that failed since boot (any task).
uint32_t led_ring_write_errors();

/// Minimum free stack of nest_led since it started, in bytes; 0 unless
/// led_ring_state() is RUNNING (the task never ends once running).
uint32_t led_ring_stack_high_water_mark();

}  // namespace esphome::esp_nest
