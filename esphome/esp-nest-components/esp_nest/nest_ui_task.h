#pragma once

#include <cstdint>

namespace esphome::esp_nest {

/*
 * The nest_ui task: owns LVGL, the panel, the knob (nest_input), the UI
 * (nest_ui.c) and the backlight (architecture sections 2 and 3). It
 * initialises the hardware with the backlight off, builds the UI, renders
 * the first frame, lights the backlight and then loops: apply the values
 * handed over by loopTask (ui_process_pending), then run LVGL's timers
 * (animations, the indev read, rendering). LVGL draws with its own two
 * software draw threads; nothing outside this task touches LVGL or LEDC.
 *
 * Placement is native parity (main/main.c: core 1, prio 5, 16 KB): step 13
 * confirms it by measuring the loopTask gaps during the heaviest animation.
 */

static constexpr int UI_TASK_CORE = 1;
static constexpr unsigned UI_TASK_PRIORITY = 5;
static constexpr uint32_t UI_TASK_STACK_BYTES = 16 * 1024;

/// Start-up progress of the nest_ui task, published to loopTask.
enum class UiTaskState : uint8_t {
  NOT_STARTED,  ///< ui_task_start() not called yet
  STARTING,     ///< task created, hardware / LVGL initialising
  RUNNING,      ///< first frame shown, backlight on, LVGL loop running
  FAILED,       ///< initialisation failed; the task has ended, backlight off
};

struct UiTaskConfig {
  /// 180° rotation: true = panel mirrors X (SDIR) + row flip in flush;
  /// false = per-pixel copy like the native firmware.
  bool hw_mirror_x;
};

/// Creates the nest_ui task. loopTask only, once. The outcome is reported
/// through ui_task_state(): FAILED if the task could not be created or its
/// initialisation failed.
void ui_task_start(const UiTaskConfig &config);

/// Current start-up state (readable from any task).
UiTaskState ui_task_state();

/// The connection state shown on the status line (hand-off 4).
struct UiStatus {
  bool wifi_connected;  ///< station associated and holding an IPv4 address
  uint32_t ip_v4;       ///< station address, host byte order (a.b.c.d = a<<24 | ... | d)
  bool mqtt_connected;  ///< the MQTT client is connected to the broker
};

/// Publishes the connection state for the status line (hand-off 4,
/// loopTask only). nest_ui formats and shows the line itself, so no string
/// crosses threads. The line reads "Starting..." until the first call.
void ui_status_publish(const UiStatus &status);

/// The UI-side clock: int64 milliseconds since boot from esp_timer. Every
/// pure module called on nest_ui takes its time from here, never from
/// lv_tick_get() or millis() (uint32, wrap after 49.7 days).
int64_t ui_now_ms();

}  // namespace esphome::esp_nest
