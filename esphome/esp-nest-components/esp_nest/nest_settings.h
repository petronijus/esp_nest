#pragma once

#include <cstdint>

#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/preferences.h"

#include "settings_word.h"

namespace esphome::esp_nest {

/*
 * The device settings exposed to Home Assistant through the native API
 * (architecture sections 3 and 5): three numbers (screen brightness, dim
 * brightness, idle timeout) and the LED ring switch. Native exposed the
 * same four through MQTT discovery (main/ha_mqtt.c); the ranges, steps,
 * units, icons, category and boot defaults below are those.
 *
 * Threading: the entities live on loopTask (API and web_server controls
 * land there). The numbers hand their values to nest_ui through one packed
 * atomic word (hand-off 3), which nest_ui applies through backlight_policy;
 * the switch writes the LED ring's enabled atomic (nest_led_ring.h). The
 * three numbers share one word on purpose: nest_ui always reads a
 * consistent snapshot, and a change is detected by comparing it with the
 * last applied one, so there is no separate sequence counter to tear.
 */

/// The three number settings.
/// The values are settings_id_t's (settings_word.h, which also owns the
/// ranges, steps and defaults).
enum class SettingId : uint8_t {
  BACKLIGHT_PCT = SETTINGS_ID_SCREEN_BRIGHTNESS,  ///< screen brightness while in use, %
  DIM_PCT = SETTINGS_ID_DIM_BRIGHTNESS,           ///< screen brightness while idle, %
  IDLE_TIMEOUT_S = SETTINGS_ID_IDLE_TIMEOUT,      ///< idle time before dimming, s (0 = never)
};

/// A consistent snapshot of the three number settings.
struct Settings {
  int backlight_pct;
  int dim_pct;
  int idle_timeout_s;

  bool operator==(const Settings &other) const {
    return this->backlight_pct == other.backlight_pct && this->dim_pct == other.dim_pct &&
           this->idle_timeout_s == other.idle_timeout_s;
  }
  bool operator!=(const Settings &other) const { return !(*this == other); }
};

/// The current settings (any task). Boot defaults (native: 100 / 20 / 30)
/// until the entities resolve their initial values in EspNest::setup().
Settings settings_load();

/// Stores one setting (loopTask), clamped to its stored range
/// (settings_word.h: the screen brightness is stored 0..100 like native).
void settings_store(SettingId id, int value);

/// One of the three numbers. loopTask only.
class SettingNumber : public number::Number {
 public:
  explicit SettingNumber(SettingId id) : id_(id) {}

  /// Value used when nothing is restored (the schema's `initial_value`).
  void set_initial_value(float initial_value) { this->initial_value_ = initial_value; }
  /// Persist the value in flash and restore it at boot (native: RAM only,
  /// so the YAML default is off).
  void set_restore_value(bool restore_value) { this->restore_value_ = restore_value; }
  bool restore_value() const { return this->restore_value_; }

  /// Resolves the boot value (restored, else initial), hands it to nest_ui
  /// and publishes it, so HA shows it at once instead of Unknown.
  /// EspNest::setup(), before the nest_ui task starts.
  void setup_state();

 protected:
  /// API / web_server / automation write: clamp like native, store the
  /// value for nest_ui, publish it, persist it when restoring.
  void control(float value) override;

  /// The value rounded to a whole number and clamped to the stored range.
  int clamp_(float value) const;

  SettingId id_;
  float initial_value_{0};
  bool restore_value_{false};
  ESPPreferenceObject pref_;
};

/// The LED ring switch. loopTask only.
class LedRingSwitch : public switch_::Switch {
 public:
  /// Resolves the boot state (`restore_mode`, native: always on) and
  /// applies and publishes it. EspNest::setup(), before nest_led starts.
  void setup_state();

 protected:
  void write_state(bool state) override;
};

}  // namespace esphome::esp_nest
