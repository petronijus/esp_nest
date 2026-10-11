#include "nest_led_ring.h"

#include <atomic>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "nest_core.h"
#include "nest_ui.h"

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest.led";

// Animation constants, main/led_ring.c verbatim.
static constexpr uint32_t TICK_MS = 20;
static constexpr uint32_t DISABLED_TICK_MS = 100;
static constexpr uint32_t BREATH_PERIOD_MS = 3000;
static constexpr float MIN_BRIGHTNESS = 0.05f;
static constexpr float MAX_BRIGHTNESS = 0.45f;
static constexpr uint32_t FX_DURATION_MS = 600;

// After a failed frame the next write waits this long. A failing strip makes
// the led_strip / RMT driver log an error per failed call (through the
// ESPHome logger, on this task's stack); without the back-off a persistent
// fault would log 50 times a second. Successful frames keep the native
// 20 ms cadence.
static constexpr uint32_t WRITE_RETRY_MS = 1000;
static_assert(WRITE_RETRY_MS % TICK_MS == 0 && WRITE_RETRY_MS % DISABLED_TICK_MS == 0,
              "the write retry delay must be a whole number of ticks");

// RMT resolution of the WS2812 encoder (native: 10 MHz).
static constexpr uint32_t RMT_RESOLUTION_HZ = 10 * 1000 * 1000;

// Page accent colours, must match PAGES[] in nest_ui.c:
// Lights #FFE600, Fiiuuu #00E5CC, Blinds #FB4A04, Sound #DE04FF.
static constexpr uint8_t PAGE_RGB[][3] = {
    {255, 230, 0},
    {0, 229, 204},
    {251, 74, 4},
    {222, 4, 255},
};
static constexpr int PAGE_COUNT = sizeof(PAGE_RGB) / sizeof(PAGE_RGB[0]);

// Phases of the breathing period advance by whole ticks, so the running
// position wraps exactly where native's `tick % BREATH_PERIOD_MS` did,
// without native's uint32 tick counter (which jumps after 49.7 days).
static_assert(BREATH_PERIOD_MS % TICK_MS == 0 && BREATH_PERIOD_MS % DISABLED_TICK_MS == 0,
              "the breathing period must be a whole number of ticks");

static std::atomic<LedRingState> s_state{LedRingState::NOT_STARTED};
static std::atomic<esp_err_t> s_error{ESP_OK};
static std::atomic<TaskHandle_t> s_task{nullptr};
static std::atomic<bool> s_enabled{true};
static std::atomic<uint32_t> s_write_errors{0};

// Hand-off 6: (seq << 1) | turned_on, seq = feedback requests since boot
// (0 = none yet). One word, so the flavour can never be torn from its
// request (native kept them in two volatiles). Written by ui_led_feedback()
// on nest_ui, latched by nest_led on every change of seq.
static std::atomic<uint32_t> s_feedback{0};

LedRingState led_ring_state() { return s_state.load(std::memory_order_acquire); }

esp_err_t led_ring_error() { return s_error.load(std::memory_order_acquire); }

void led_ring_set_enabled(bool enabled) { s_enabled.store(enabled, std::memory_order_relaxed); }

bool led_ring_enabled() { return s_enabled.load(std::memory_order_relaxed); }

uint32_t led_ring_write_errors() { return s_write_errors.load(std::memory_order_relaxed); }

uint32_t led_ring_stack_high_water_mark() {
  if (led_ring_state() != LedRingState::RUNNING)
    return 0;
  // ESP-IDF FreeRTOS counts stack in bytes.
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task.load(std::memory_order_relaxed)));
}

/// Brightness envelope 0..1 of the feedback effect at `t` ms (native fx_level).
static float fx_level(bool turned_on, uint32_t t) {
  if (turned_on) {
    // A single bright swell: half a sine over the duration.
    return sinf(static_cast<float>(t) / FX_DURATION_MS * static_cast<float>(M_PI));
  }
  // Turned off: two crisp blinks.
  if (t < 150)
    return 0.8f;
  if (t < 300)
    return 0.0f;
  if (t < 450)
    return 0.8f;
  return 0.0f;
}

/// Sets every LED to one colour and sends the frame; failures are counted
/// for loopTask to report (this task's own code does not log). Returns
/// whether the frame was written.
static bool show(led_strip_handle_t strip, uint8_t r, uint8_t g, uint8_t b) {
  bool ok = true;
  for (uint32_t i = 0; i < LED_RING_COUNT; i++)
    ok = led_strip_set_pixel(strip, i, r, g, b) == ESP_OK && ok;
#ifdef NEST_LED_INJECT_WRITE_FAULT
  // Test builds only (nest_led_ring.h): one out-of-range pixel per frame.
  // The driver rejects it with ESP_LOGE, exercising the error and logging
  // path on this stack while the real pixels still go out.
  ok = led_strip_set_pixel(strip, LED_RING_COUNT, r, g, b) == ESP_OK && ok;
#endif
  ok = led_strip_refresh(strip) == ESP_OK && ok;
  if (!ok)
    s_write_errors.fetch_add(1, std::memory_order_relaxed);
  return ok;
}

/// One frame, unless a recent failure holds writes back (WRITE_RETRY_MS).
/// `hold_ms` is the remaining hold, `tick_ms` the period this frame covers.
static void show_throttled(led_strip_handle_t strip, uint8_t r, uint8_t g, uint8_t b, uint32_t *hold_ms,
                           uint32_t tick_ms) {
  if (*hold_ms == 0 && !show(strip, r, g, b))
    *hold_ms = WRITE_RETRY_MS;
  *hold_ms = *hold_ms > tick_ms ? *hold_ms - tick_ms : 0;
}

/// The RMT channel (and with it its interrupt) is created here, on core 0.
static esp_err_t create_strip(led_strip_handle_t *strip) {
  led_strip_config_t strip_config{};
  strip_config.strip_gpio_num = LED_RING_GPIO;
  strip_config.max_leds = LED_RING_COUNT;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

  led_strip_rmt_config_t rmt_config{};
  rmt_config.resolution_hz = RMT_RESOLUTION_HZ;
  rmt_config.flags.with_dma = false;

  esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, strip);
  if (err != ESP_OK)
    return err;
  // All LEDs off (clear also sends the frame).
  err = led_strip_clear(*strip);
  if (err != ESP_OK) {
    led_strip_del(*strip);
    *strip = nullptr;
  }
  return err;
}

static void led_task_main(void * /*arg*/) {
  led_strip_handle_t strip = nullptr;
  const esp_err_t err = create_strip(&strip);
  if (err != ESP_OK) {
    s_error.store(err, std::memory_order_relaxed);
    s_state.store(LedRingState::FAILED, std::memory_order_release);
    vTaskDelete(nullptr);
    return;
  }
  // Published before RUNNING: led_ring_stack_high_water_mark() reads the
  // handle only after it has seen RUNNING.
  s_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_relaxed);
  s_state.store(LedRingState::RUNNING, std::memory_order_release);

  uint32_t breath_ms = 0;                // position in the breathing period
  uint32_t fx_seen = 0;                  // last latched feedback seq
  uint32_t fx_elapsed = FX_DURATION_MS;  // inactive
  bool fx_on = false;
  uint32_t write_hold_ms = 0;            // remaining back-off after a failed frame

  for (;;) {
    const uint32_t feedback = s_feedback.load(std::memory_order_acquire);
    if ((feedback >> 1) != fx_seen) {  // new feedback request
      fx_seen = feedback >> 1;
      fx_on = (feedback & 1u) != 0;
      fx_elapsed = 0;
    }

    if (!s_enabled.load(std::memory_order_relaxed)) {
      show_throttled(strip, 0, 0, 0, &write_hold_ms, DISABLED_TICK_MS);
      vTaskDelay(pdMS_TO_TICKS(DISABLED_TICK_MS));
      breath_ms = (breath_ms + DISABLED_TICK_MS) % BREATH_PERIOD_MS;
      continue;
    }

    int page = ui_get_current_page();
    if (page < 0 || page >= PAGE_COUNT)
      page = 0;
    const uint8_t *c = PAGE_RGB[page];

    float level;
    if (fx_elapsed < FX_DURATION_MS) {
      // The feedback effect overrides the regular animation.
      level = fx_level(fx_on, fx_elapsed);
      fx_elapsed += TICK_MS;
    } else if (page == 0 && ui_is_detail_mode()) {
      // Brightness detail: the intensity tracks the brightness value.
      level = static_cast<float>(ui_get_brightness()) / 100.0f;
    } else {
      // Menu: breathing in the page accent colour.
      const float phase = static_cast<float>(breath_ms) / static_cast<float>(BREATH_PERIOD_MS);
      level = MIN_BRIGHTNESS +
              (MAX_BRIGHTNESS - MIN_BRIGHTNESS) * (0.5f + 0.5f * sinf(phase * 2.0f * static_cast<float>(M_PI)));
    }
    show_throttled(strip, static_cast<uint8_t>(c[0] * level), static_cast<uint8_t>(c[1] * level),
                   static_cast<uint8_t>(c[2] * level), &write_hold_ms, TICK_MS);

    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    breath_ms = (breath_ms + TICK_MS) % BREATH_PERIOD_MS;
  }
}

void led_ring_start() {
  NEST_ASSERT_LOOP_TASK();
  if (led_ring_state() != LedRingState::NOT_STARTED) {
    ESP_LOGE(TAG, "nest_led already started");
    return;
  }

  s_state.store(LedRingState::STARTING, std::memory_order_release);
  // Internal-RAM stack (the default for xTaskCreate*), size in bytes on ESP-IDF.
  if (xTaskCreatePinnedToCore(led_task_main, "nest_led", LED_TASK_STACK_BYTES, nullptr, LED_TASK_PRIORITY, nullptr,
                              LED_TASK_CORE) != pdPASS) {
    s_error.store(ESP_ERR_NO_MEM, std::memory_order_relaxed);
    s_state.store(LedRingState::FAILED, std::memory_order_release);
  }
}

}  // namespace esphome::esp_nest

// The UI's LED feedback hook (nest_ui.h). Single writer (nest_ui), so the
// sequence number is advanced with a plain load + store.
extern "C" void ui_led_feedback(bool turned_on) {
  using esphome::esp_nest::s_feedback;
  const uint32_t seq = (s_feedback.load(std::memory_order_relaxed) >> 1) + 1;
  s_feedback.store((seq << 1) | (turned_on ? 1u : 0u), std::memory_order_release);
}
