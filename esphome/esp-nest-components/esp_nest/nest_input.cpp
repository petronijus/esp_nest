#include "nest_input.h"

#include <climits>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esphome/core/log.h"

#include "input_fsm.h"
#include "nest_backlight.h"
#include "nest_core.h"
#include "nest_ui.h"
#include "nest_ui_task.h"

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest.input";

// Pin map: main/board.h.
static constexpr gpio_num_t PIN_ENCODER_SIGNAL = GPIO_NUM_5;     // MT8901 A
static constexpr gpio_num_t PIN_ENCODER_DIRECTION = GPIO_NUM_6;  // MT8901 B
static constexpr gpio_num_t PIN_BUTTON = GPIO_NUM_3;
static constexpr int BUTTON_PRESSED_LEVEL = 0;

/// Pulses shorter than this are ignored (native: 1000 ns).
static constexpr uint32_t ENCODER_GLITCH_NS = 1000;
/// Hardware counter range; the driver accumulates beyond it (see input_init).
static constexpr int ENCODER_HIGH_LIMIT = SHRT_MAX;
static constexpr int ENCODER_LOW_LIMIT = SHRT_MIN;

static pcnt_unit_handle_t s_encoder = nullptr;
static int s_last_count = 0;
static input_fsm_t s_fsm;

/// Encoder count since boot. pcnt_unit_get_count() only fails for a NULL
/// unit, which input_init() rules out before the indev exists.
static int encoder_count() {
  int count = 0;
  pcnt_unit_get_count(s_encoder, &count);
  return count;
}

/// The native button component's button_is_pressed(): a raw level read,
/// debounced only by the press-duration rules of input_fsm.
static bool button_pressed() { return gpio_get_level(PIN_BUTTON) == BUTTON_PRESSED_LEVEL; }

/// Native screen.c encoder_read_cb, with the logic in input_fsm and
/// backlight_policy.
static void encoder_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
  const int count = encoder_count();
  const int diff = count - s_last_count;
  s_last_count = count;

  const bool pressed = button_pressed();
  const int64_t now_ms = ui_now_ms();

  input_fsm_out_t out;
  input_fsm_update(&s_fsm, now_ms, diff, pressed, ui_is_detail_mode(), &out);
  if (out.navigate != 0)
    ui_encoder_navigate(out.navigate);
  if (out.long_press)
    ui_encoder_long_press();
  if (out.press)
    ui_encoder_press();

  backlight_update(now_ms, out.activity);

  // LVGL itself gets no rotation (no group is attached): the UI handles
  // the knob directly, as native did.
  data->enc_diff = 0;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t input_init() {
  NEST_ASSERT_UI_TASK(ESP_ERR_INVALID_STATE);
  if (s_encoder != nullptr)
    return ESP_ERR_INVALID_STATE;

  // Button: plain input. GPIO 3 is a strapping pin; resetting it first
  // clears any configuration left from boot (native order). Native then
  // disabled the pull (the board pulls the line up); the internal pull-up
  // is enabled here as well, in parallel, so the level is defined even
  // with a marginal external pull-up. It does not change an active-low
  // switch's levels.
  ESP_RETURN_ON_ERROR(gpio_reset_pin(PIN_BUTTON), TAG, "button reset");
  gpio_config_t button = {};
  button.pin_bit_mask = 1ULL << PIN_BUTTON;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  button.pull_down_en = GPIO_PULLDOWN_DISABLE;
  button.intr_type = GPIO_INTR_DISABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&button), TAG, "button GPIO");

  // Encoder (native components/mt8901): count on the signal edges, the
  // direction pin's level selects up / down. accum_count + watch points at
  // both limits make the driver carry the count across the 16-bit hardware
  // range: without them the counter silently restarts at 0 on reaching a
  // limit (native), which a read would see as one huge rotation.
  pcnt_unit_config_t unit_config = {};
  unit_config.high_limit = ENCODER_HIGH_LIMIT;
  unit_config.low_limit = ENCODER_LOW_LIMIT;
  unit_config.flags.accum_count = 1;
  pcnt_unit_handle_t unit = nullptr;
  ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &unit), TAG, "PCNT unit");

  esp_err_t ret = ESP_OK;  // set by ESP_GOTO_ON_ERROR
  pcnt_channel_handle_t channel = nullptr;
  pcnt_glitch_filter_config_t filter = {};
  filter.max_glitch_ns = ENCODER_GLITCH_NS;
  pcnt_chan_config_t chan_config = {};
  chan_config.edge_gpio_num = PIN_ENCODER_SIGNAL;
  chan_config.level_gpio_num = PIN_ENCODER_DIRECTION;

  ESP_GOTO_ON_ERROR(pcnt_unit_set_glitch_filter(unit, &filter), fail_unit, TAG, "PCNT glitch filter");
  ESP_GOTO_ON_ERROR(pcnt_new_channel(unit, &chan_config, &channel), fail_unit, TAG, "PCNT channel");
  // Rising edge: down, falling edge: up; direction high: keep, low: invert.
  ESP_GOTO_ON_ERROR(pcnt_channel_set_edge_action(channel, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE),
                    fail_channel, TAG, "PCNT edge action");
  ESP_GOTO_ON_ERROR(pcnt_channel_set_level_action(channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
                    fail_channel, TAG, "PCNT level action");
  ESP_GOTO_ON_ERROR(pcnt_unit_add_watch_point(unit, ENCODER_HIGH_LIMIT), fail_channel, TAG, "PCNT high watch point");
  ESP_GOTO_ON_ERROR(pcnt_unit_add_watch_point(unit, ENCODER_LOW_LIMIT), fail_channel, TAG, "PCNT low watch point");
  ESP_GOTO_ON_ERROR(pcnt_unit_enable(unit), fail_channel, TAG, "PCNT enable");
  ESP_GOTO_ON_ERROR(pcnt_unit_clear_count(unit), fail_enabled, TAG, "PCNT clear");
  ESP_GOTO_ON_ERROR(pcnt_unit_start(unit), fail_enabled, TAG, "PCNT start");

  s_encoder = unit;
  s_last_count = 0;
  input_fsm_init(&s_fsm);
  ESP_LOGI(TAG, "Encoder on GPIO %d/%d, button on GPIO %d", PIN_ENCODER_SIGNAL, PIN_ENCODER_DIRECTION, PIN_BUTTON);
  return ESP_OK;

fail_enabled:
  pcnt_unit_disable(unit);
fail_channel:
  pcnt_del_channel(channel);
fail_unit:
  pcnt_del_unit(unit);
  return ret;
}

esp_err_t input_create_indev() {
  NEST_ASSERT_UI_TASK(ESP_ERR_INVALID_STATE);
  if (s_encoder == nullptr) {
    ESP_LOGE(TAG, "input_init() must succeed before the indev is created");
    return ESP_ERR_INVALID_STATE;
  }

  lv_indev_t *indev = lv_indev_create();
  if (indev == nullptr)
    return ESP_ERR_NO_MEM;
  lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
  lv_indev_set_read_cb(indev, encoder_read_cb);
  lv_timer_set_period(lv_indev_get_read_timer(indev), INPUT_READ_PERIOD_MS);
  return ESP_OK;
}

}  // namespace esphome::esp_nest
