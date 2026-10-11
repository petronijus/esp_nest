#include "nest_settings.h"

#include <atomic>
#include <cmath>

#include "esphome/core/log.h"

#include "nest_core.h"
#include "nest_led_ring.h"

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest.settings";

// ---------------------------------------------------------------------------
// Hand-off 3: loop -> UI settings, one packed word
// ---------------------------------------------------------------------------

// The packing, clamping and defaults are the pure settings_word module
// (host-tested); this file adds the atomic around it.

static std::atomic<uint32_t> s_settings{settings_word_default()};

Settings settings_load() {
  Settings settings;
  settings_word_unpack(s_settings.load(std::memory_order_acquire), &settings.backlight_pct, &settings.dim_pct,
                       &settings.idle_timeout_s);
  return settings;
}

void settings_store(SettingId id, int value) {
  // Compare-and-swap: the other fields are re-read on a lost race, so a
  // concurrent update of another setting is never overwritten.
  uint32_t word = s_settings.load(std::memory_order_relaxed);
  uint32_t updated;
  do {
    updated = settings_word_with(word, static_cast<settings_id_t>(id), value);
  } while (!s_settings.compare_exchange_weak(word, updated, std::memory_order_acq_rel, std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// SettingNumber
// ---------------------------------------------------------------------------

int SettingNumber::clamp_(float value) const {
  return settings_round_clamp(static_cast<settings_id_t>(this->id_), value,
                              static_cast<int>(std::lround(this->initial_value_)));
}

void SettingNumber::setup_state() {
  NEST_ASSERT_LOOP_TASK();
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = this->make_entity_preference<float>();
    float restored;
    if (this->pref_.load(&restored) && !std::isnan(restored))
      value = restored;
  }
  const int resolved = this->clamp_(value);
  settings_store(this->id_, resolved);
  this->publish_state(static_cast<float>(resolved));
  ESP_LOGD(TAG, "'%s' starts at %d", this->get_name().c_str(), resolved);
}

void SettingNumber::control(float value) {
  NEST_ASSERT_LOOP_TASK();
  const int clamped = this->clamp_(value);
  settings_store(this->id_, clamped);
  this->publish_state(static_cast<float>(clamped));
  if (this->restore_value_) {
    float stored = static_cast<float>(clamped);
    this->pref_.save(&stored);
  }
  ESP_LOGI(TAG, "'%s' set to %d", this->get_name().c_str(), clamped);
}

// ---------------------------------------------------------------------------
// LedRingSwitch
// ---------------------------------------------------------------------------

void LedRingSwitch::setup_state() {
  NEST_ASSERT_LOOP_TASK();
  // restore_mode may be "disabled": then the ring keeps its native boot
  // state (on) and the switch is published from it.
  const optional<bool> initial = this->get_initial_state_with_restore_mode();
  const bool enabled = initial.value_or(led_ring_enabled());
  led_ring_set_enabled(enabled);
  this->publish_state(enabled);
}

void LedRingSwitch::write_state(bool state) {
  NEST_ASSERT_LOOP_TASK();
  led_ring_set_enabled(state);
  this->publish_state(state);
  ESP_LOGI(TAG, "LED ring %s", state ? "ON" : "OFF");
}

}  // namespace esphome::esp_nest
