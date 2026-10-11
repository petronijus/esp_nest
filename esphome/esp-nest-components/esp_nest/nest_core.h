#pragma once

#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "nest_led_ring.h"
#include "nest_mqtt_bridge.h"
#include "nest_settings.h"
#include "nest_ui_task.h"

namespace esphome::esp_nest {

/*
 * Thread ownership (docs/esphome-migration-plan.md, section 3):
 *
 *   - loopTask (ESPHome) owns every ESPHome API: MQTT, entity
 *     publish_state, preferences.
 *   - nest_ui owns every LVGL and LEDC call: panel flush, indev, backlight.
 *
 * Each side records its FreeRTOS task handle once; the assertion macros
 * below compare against it, log an error and refuse the call when a
 * function is entered from the wrong task. Nothing but the hand-off
 * primitives of section 3 crosses between the two tasks.
 */

/// Records the calling task as loopTask. EspNest::setup() calls it first.
void register_loop_task();
/// Records the calling task as nest_ui. The UI task calls it first.
void register_ui_task();
/// True on the recorded loopTask (false before it is recorded).
bool on_loop_task();
/// True on the recorded nest_ui task (false before it is recorded).
bool on_ui_task();

/// A local user change, handed from nest_ui to loopTask (hand-off 1):
/// `type` is a ui_event_type_t, `value` its payload as documented in
/// nest_ui.h. Copied by value through a FreeRTOS queue.
struct UiEvent {
  uint8_t type;
  int32_t value;
};

/// Depth of the UI -> loop event queue. A full queue drops the event and
/// counts it; the loop logs the count.
static constexpr UBaseType_t UI_EVENT_QUEUE_DEPTH = 64;

/// Owner of everything device-specific (architecture section 2). Runs on
/// loopTask: records the loop task handle, creates the UI event queue,
/// starts the nest_ui and nest_led tasks, drains the UI's events and
/// reports what the tasks cannot log themselves.
class EspNest : public Component {
 public:
  void set_hw_mirror_x(bool hw_mirror_x) { this->hw_mirror_x_ = hw_mirror_x; }
  /// Blinds group labels; string literals from the generated code (they
  /// are not copied). Every glyph is in font_helvetica_100 (validated).
  void set_blinds_labels(const char *label_a, const char *label_b) {
    this->blinds_label_a_ = label_a;
    this->blinds_label_b_ = label_b;
  }

  /// The external-device MQTT bridge: ESPHome's `mqtt:` client and the
  /// device topics (native: the Kconfig TOPIC_* options).
  void set_mqtt(mqtt::MQTTClientComponent *client, const std::string &lights, const std::string &fan,
                const std::string &stereo_volume, const std::string &blinds_prefix,
                const std::string &blinds_group_a, const std::string &blinds_group_b) {
    this->mqtt_bridge_.configure(client, MqttBridgeConfig{.lights = lights,
                                                          .fan = fan,
                                                          .stereo_volume = stereo_volume,
                                                          .blinds_prefix = blinds_prefix,
                                                          .blinds_groups = {blinds_group_a, blinds_group_b}});
  }

  /// The settings entities (created by the generated code, owned for the
  /// life of the device). Native: the four HA entities of main/ha_mqtt.c.
  void set_screen_brightness(SettingNumber *number) { this->screen_brightness_ = number; }
  void set_dim_brightness(SettingNumber *number) { this->dim_brightness_ = number; }
  void set_idle_timeout(SettingNumber *number) { this->idle_timeout_ = number; }
  void set_led_ring(LedRingSwitch *led_ring) { this->led_ring_ = led_ring; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  /// One drained UI event (loopTask): logged, then handed to the MQTT bridge.
  void handle_ui_event_(const UiEvent &event);
  /// Hands the connection state (WiFi, IPv4, MQTT) to the UI's status line
  /// when it changed, and logs the transitions like the native monitor.
  void refresh_status_();
  /// Logs the UI start-up outcome once it settles.
  void report_ui_state_();
  /// Logs the LED ring start-up outcome once it settles, then its write
  /// errors: the first at once, later ones as rate-limited summaries, with
  /// a warning status while they continue.
  void report_led_ring_();

  bool hw_mirror_x_{true};
  MqttBridge mqtt_bridge_;
  SettingNumber *screen_brightness_{nullptr};
  SettingNumber *dim_brightness_{nullptr};
  SettingNumber *idle_timeout_{nullptr};
  LedRingSwitch *led_ring_{nullptr};
  const char *blinds_label_a_{"A"};
  const char *blinds_label_b_{"B"};
  /// The connection state last handed to the status line; valid once
  /// `status_published_` is set (the first pass always publishes).
  UiStatus published_status_{};
  bool status_published_{false};
  /// Last nest_ui start-up state seen by loop(), for edge-triggered reporting.
  UiTaskState reported_ui_state_{UiTaskState::NOT_STARTED};
  /// UI events dropped on a full queue, as last logged.
  uint32_t reported_ui_event_drops_{0};
  /// Last nest_led start-up state seen by loop(), for edge-triggered reporting.
  LedRingState reported_led_state_{LedRingState::NOT_STARTED};
  /// LED strip write errors, as last logged.
  uint32_t reported_led_write_errors_{0};
  /// LED strip write errors, as last seen by loop().
  uint32_t seen_led_write_errors_{0};
  /// When the last write-error report was logged (esp_timer ms).
  int64_t led_write_report_at_ms_{0};
  /// When loop() last saw the write-error count grow (esp_timer ms).
  int64_t led_write_error_at_ms_{0};
  /// The write-error warning status is set.
  bool led_write_warning_{false};
};

}  // namespace esphome::esp_nest

/*
 * Refuse a call made from the wrong task. Expands inside a function that
 * has a `TAG` in scope; the optional argument is the value returned on
 * refusal (empty for void functions), e.g.
 *
 *   NEST_ASSERT_LOOP_TASK();        // void function
 *   NEST_ASSERT_UI_TASK(ESP_FAIL);  // esp_err_t function
 */
#define NEST_ASSERT_LOOP_TASK(...) \
  do { \
    if (!::esphome::esp_nest::on_loop_task()) { \
      ESP_LOGE(TAG, "%s: called outside loopTask, refused", __func__); \
      return __VA_ARGS__; \
    } \
  } while (0)

#define NEST_ASSERT_UI_TASK(...) \
  do { \
    if (!::esphome::esp_nest::on_ui_task()) { \
      ESP_LOGE(TAG, "%s: called outside the nest_ui task, refused", __func__); \
      return __VA_ARGS__; \
    } \
  } while (0)
