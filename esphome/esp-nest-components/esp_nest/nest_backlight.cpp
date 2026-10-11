#include "nest_backlight.h"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esphome/core/log.h"
#include "soc/soc_caps.h"

#include "backlight_policy.h"
#include "nest_core.h"

static_assert(SOC_LEDC_SUPPORT_FADE_STOP, "the backlight needs ledc_fade_stop() before direct duty sets");

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest.backlight";

static constexpr int PIN_BACKLIGHT = 38;
static constexpr ledc_mode_t BL_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t BL_TIMER = LEDC_TIMER_0;
static constexpr ledc_channel_t BL_CHANNEL = LEDC_CHANNEL_0;
static constexpr uint32_t BL_FREQ_HZ = 5000;
static constexpr ledc_timer_bit_t BL_RESOLUTION = LEDC_TIMER_13_BIT;
static constexpr uint32_t BL_MAX_DUTY = (1u << 13) - 1;

static backlight_policy_t s_policy;
static bool s_started = false;

static uint32_t duty_for(int pct) { return BL_MAX_DUTY * static_cast<uint32_t>(pct) / 100u; }

/// Drives the hardware from one policy decision.
static void apply(backlight_policy_action_t action, int pct) {
  esp_err_t err = ESP_OK;
  switch (action) {
    case BACKLIGHT_POLICY_ACT_NONE:
      return;
    case BACKLIGHT_POLICY_ACT_SET:
      // A direct set must not race a running fade (the idle dim fade).
      err = ledc_fade_stop(BL_MODE, BL_CHANNEL);
      if (err == ESP_OK)
        err = ledc_set_duty_and_update(BL_MODE, BL_CHANNEL, duty_for(pct), 0);
      break;
    case BACKLIGHT_POLICY_ACT_FADE:
      err = ledc_set_fade_time_and_start(BL_MODE, BL_CHANNEL, duty_for(pct), BACKLIGHT_POLICY_FADE_MS,
                                         LEDC_FADE_NO_WAIT);
      break;
  }
  if (err != ESP_OK)
    ESP_LOGW(TAG, "Backlight %s to %d%% failed: %s", action == BACKLIGHT_POLICY_ACT_FADE ? "fade" : "set", pct,
             esp_err_to_name(err));
}

/// The policy only exists once backlight_start() ran: the initial settings
/// are passed to it, so nothing may change them before.
static bool require_started(const char *caller) {
  if (s_started)
    return true;
  ESP_LOGW(TAG, "%s before backlight_start(), ignored", caller);
  return false;
}

esp_err_t backlight_init() {
  NEST_ASSERT_UI_TASK(ESP_ERR_INVALID_STATE);

  ledc_timer_config_t timer = {};
  timer.speed_mode = BL_MODE;
  timer.timer_num = BL_TIMER;
  timer.duty_resolution = BL_RESOLUTION;
  timer.freq_hz = BL_FREQ_HZ;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC timer");

  ledc_channel_config_t channel = {};
  channel.speed_mode = BL_MODE;
  channel.channel = BL_CHANNEL;
  channel.timer_sel = BL_TIMER;
  channel.gpio_num = PIN_BACKLIGHT;
  channel.duty = 0;
  channel.hpoint = 0;
  ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "LEDC channel");
  ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "LEDC fade service");
  return ESP_OK;
}

void backlight_start(int64_t now_ms, int active_pct, int dim_pct, int timeout_s) {
  NEST_ASSERT_UI_TASK();
  int pct = 0;
  apply(backlight_policy_init(&s_policy, now_ms, active_pct, dim_pct, timeout_s, &pct), pct);
  s_started = true;
  ESP_LOGI(TAG, "Backlight %d%%, dim %d%% after %d s idle", s_policy.active_pct, s_policy.dim_pct,
           s_policy.timeout_ms / 1000);
}

void backlight_update(int64_t now_ms, bool activity) {
  NEST_ASSERT_UI_TASK();
  if (!require_started(__func__))
    return;
  int pct = 0;
  apply(backlight_policy_update(&s_policy, now_ms, activity, &pct), pct);
}

void backlight_set_active(int pct) {
  NEST_ASSERT_UI_TASK();
  if (!require_started(__func__))
    return;
  int out = 0;
  apply(backlight_policy_set_active(&s_policy, pct, &out), out);
}

void backlight_set_dim(int pct) {
  NEST_ASSERT_UI_TASK();
  if (!require_started(__func__))
    return;
  int out = 0;
  apply(backlight_policy_set_dim(&s_policy, pct, &out), out);
}

void backlight_set_timeout(int seconds) {
  NEST_ASSERT_UI_TASK();
  if (!require_started(__func__))
    return;
  backlight_policy_set_timeout(&s_policy, seconds);
}

}  // namespace esphome::esp_nest
