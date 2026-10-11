#include "nest_core.h"

#include <atomic>
#include <cstdarg>

#include "lwip/sockets.h"

#include "esp_timer.h"
#include "esphome/components/network/ip_address.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "freertos/task.h"

#include "nest_c.h"
#include "nest_ui.h"

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest";

// Written once by the owning task before any cross-task use; read from any
// task by the assertion macros.
static std::atomic<TaskHandle_t> s_loop_task{nullptr};
static std::atomic<TaskHandle_t> s_ui_task{nullptr};

void register_loop_task() { s_loop_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release); }

void register_ui_task() { s_ui_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release); }

bool on_loop_task() {
  TaskHandle_t loop_task = s_loop_task.load(std::memory_order_acquire);
  return loop_task != nullptr && loop_task == xTaskGetCurrentTaskHandle();
}

bool on_ui_task() {
  TaskHandle_t ui_task = s_ui_task.load(std::memory_order_acquire);
  return ui_task != nullptr && ui_task == xTaskGetCurrentTaskHandle();
}

}  // namespace esphome::esp_nest

// ---------------------------------------------------------------------------
// C interface (nest_c.h)
// ---------------------------------------------------------------------------

// nest_c.h repeats log.h's level values for C; they must not drift.
static_assert(ESPHOME_LOG_LEVEL_NONE == 0 && ESPHOME_LOG_LEVEL_ERROR == 1 && ESPHOME_LOG_LEVEL_WARN == 2 &&
                  ESPHOME_LOG_LEVEL_INFO == 3 && ESPHOME_LOG_LEVEL_CONFIG == 4 && ESPHOME_LOG_LEVEL_DEBUG == 5 &&
                  ESPHOME_LOG_LEVEL_VERBOSE == 6 && ESPHOME_LOG_LEVEL_VERY_VERBOSE == 7,
              "nest_c.h: ESPHOME_LOG_LEVEL_* values differ from esphome/core/log.h");

extern "C" void nest_log_c(int level, const char *tag, int line, const char *format, ...) {
  va_list args;
  va_start(args, format);
  ::esphome::esp_log_vprintf_(level, tag, line, format, args);
  va_end(args);
}

extern "C" bool nest_on_ui_task(void) { return ::esphome::esp_nest::on_ui_task(); }

namespace esphome::esp_nest {

// ---------------------------------------------------------------------------
// Hand-off 1: UI -> loop events
// ---------------------------------------------------------------------------

// Statically allocated queue (no heap): created in EspNest::setup() before
// nest_ui starts, never deleted.
static StaticQueue_t s_ui_event_queue_buf;
static uint8_t s_ui_event_queue_storage[UI_EVENT_QUEUE_DEPTH * sizeof(UiEvent)];
static QueueHandle_t s_ui_events = nullptr;
static std::atomic<uint32_t> s_ui_event_drops{0};

/// The UI's change callback, on nest_ui: copy the event into the queue
/// without blocking, then wake loopTask so it drains it this pass instead
/// of after its loop interval.
static void on_ui_change(ui_event_type_t type, int value) {
  NEST_ASSERT_UI_TASK();
  const UiEvent event{static_cast<uint8_t>(type), static_cast<int32_t>(value)};
  if (xQueueSend(s_ui_events, &event, 0) != pdTRUE)
    s_ui_event_drops.fetch_add(1, std::memory_order_relaxed);
  App.wake_loop_threadsafe();
}

// ---------------------------------------------------------------------------
// EspNest
// ---------------------------------------------------------------------------

static const char *const FAN_LEVEL_NAMES[] = {"off", "low", "medium", "high"};

/// Delay from the LED ring start to the one-time stack high-water mark
/// report: by then every code path of the task (breathing, refresh, the
/// detail tracking and, if used, the feedback effect) has run many times.
static constexpr uint32_t LED_STACK_REPORT_DELAY_MS = 10000;

/// LED strip write errors: the first one is logged at once, later ones at
/// most once per interval as a summary (the task retries a failing strip
/// once a second, see nest_led_ring.h).
static constexpr int64_t LED_ERROR_REPORT_INTERVAL_MS = 10000;
/// The write-error warning status clears after this long without a new error.
static constexpr int64_t LED_ERROR_CLEAR_MS = 10000;

/// The station's IPv4 address in host byte order (a.b.c.d = a<<24 | ... | d),
/// 0 while there is none. Native "connected" meant "has an IP" (the
/// IP_EVENT_STA_GOT_IP event), so the status line needs the address too.
static uint32_t wifi_station_ipv4() {
  if (wifi::global_wifi_component == nullptr || !wifi::global_wifi_component->is_connected())
    return 0;
  for (const network::IPAddress &address : wifi::global_wifi_component->get_ip_addresses()) {
    if (!address.is_set() || !address.is_ip4())
      continue;
    char text[network::IP_ADDRESS_BUFFER_SIZE];
    in_addr parsed;
    if (inet_pton(AF_INET, address.str_to(text), &parsed) == 1)
      return ntohl(parsed.s_addr);
  }
  return 0;
}

void EspNest::setup() {
  register_loop_task();

  s_ui_events = xQueueCreateStatic(UI_EVENT_QUEUE_DEPTH, sizeof(UiEvent), s_ui_event_queue_storage,
                                   &s_ui_event_queue_buf);

  // Everything nest_ui and nest_led read at start-up is set before their
  // tasks exist: creating them publishes these writes to them. The settings
  // entities resolve their boot values (restored or initial), hand them over
  // and publish them, so HA shows 100 / 20 / 30 / ON right after boot.
  this->screen_brightness_->setup_state();
  this->dim_brightness_->setup_state();
  this->idle_timeout_->setup_state();
  this->led_ring_->setup_state();
  nest_ui_set_blinds_labels(this->blinds_label_a_, this->blinds_label_b_);
  ui_register_change_cb(on_ui_change);

  // The UI task initialises asynchronously (native: the lvgl task); loop()
  // reports its outcome.
  ui_task_start(UiTaskConfig{.hw_mirror_x = this->hw_mirror_x_});

  // Subscriptions are registered now and sent by ESPHome's MQTT client on
  // every (re)connect; the connect edge itself is seen by the bridge tick.
  this->mqtt_bridge_.setup();

  // Only the relative order is native: the LED ring after the UI (it reads
  // the UI's atomics, which hold the boot defaults until the UI is up).
  // Native started the ring last in app_main, after the WiFi connect
  // attempt (up to 30 s), the OTA server and MQTT, so it stayed dark during
  // network bring-up; here it breathes from setup() on, before any network
  // (a deliberate deviation, plan S09).
  led_ring_start();
}

void EspNest::loop() {
  this->report_ui_state_();
  this->report_led_ring_();

  // Drain in order; every local change is handled exactly once.
  UiEvent event;
  while (xQueueReceive(s_ui_events, &event, 0) == pdTRUE)
    this->handle_ui_event_(event);

  this->mqtt_bridge_.tick();
  // After the bridge tick: the MQTT state is the one the bridge just saw.
  this->refresh_status_();

  const uint32_t drops = s_ui_event_drops.load(std::memory_order_relaxed);
  if (drops != this->reported_ui_event_drops_) {
    ESP_LOGW(TAG, "UI event queue full: %u event(s) dropped since boot", static_cast<unsigned>(drops));
    this->reported_ui_event_drops_ = drops;
  }
}

void EspNest::handle_ui_event_(const UiEvent &event) {
  switch (static_cast<ui_event_type_t>(event.type)) {
    case UI_EVT_BRIGHTNESS:
      ESP_LOGD(TAG, "UI event: brightness %d%%", static_cast<int>(event.value));
      break;
    case UI_EVT_FAN:
      if (event.value >= 0 && event.value <= 3) {
        ESP_LOGD(TAG, "UI event: fan %s", FAN_LEVEL_NAMES[event.value]);
      } else {
        ESP_LOGW(TAG, "UI event: fan level %d out of range", static_cast<int>(event.value));
      }
      break;
    case UI_EVT_VOLUME:
      ESP_LOGD(TAG, "UI event: volume %s", event.value > 0 ? "up" : "down");
      break;
    case UI_EVT_BLINDS: {
      const int target = static_cast<int>(event.value >> 8);
      const int step = static_cast<int>(event.value & 0xFF);
      if (target == 0 || target == 1) {
        ESP_LOGD(TAG, "UI event: blinds %s step %d", target == 0 ? this->blinds_label_a_ : this->blinds_label_b_,
                 step);
      } else {
        ESP_LOGW(TAG, "UI event: blinds target %d out of range", target);
      }
      break;
    }
    default:
      ESP_LOGW(TAG, "UI event: unknown type %u", static_cast<unsigned>(event.type));
      break;
  }

  this->mqtt_bridge_.on_ui_event(static_cast<ui_event_type_t>(event.type), static_cast<int>(event.value));
}

void EspNest::refresh_status_() {
  const uint32_t ip_v4 = wifi_station_ipv4();
  const UiStatus status{.wifi_connected = ip_v4 != 0, .ip_v4 = ip_v4, .mqtt_connected = this->mqtt_bridge_.connected()};
  const UiStatus &last = this->published_status_;

  // The first pass always publishes: the line leaves "Starting..." even when
  // nothing is up yet. The transitions are those of the native monitor task
  // (which started from "down"), logged here without its 10 s polling delay.
  if (this->status_published_ && status.wifi_connected == last.wifi_connected && status.ip_v4 == last.ip_v4 &&
      status.mqtt_connected == last.mqtt_connected)
    return;

  const bool was_wifi = this->status_published_ && last.wifi_connected;
  const bool was_mqtt = this->status_published_ && last.mqtt_connected;
  if (status.wifi_connected && !was_wifi)
    ESP_LOGI(TAG, "WiFi restored");
  if (!status.wifi_connected && was_wifi)
    ESP_LOGW(TAG, "WiFi lost");
  if (status.mqtt_connected && !was_mqtt)
    ESP_LOGI(TAG, "MQTT restored");
  if (!status.mqtt_connected && was_mqtt)
    ESP_LOGW(TAG, "MQTT lost");

  ui_status_publish(status);
  this->published_status_ = status;
  this->status_published_ = true;
}

void EspNest::report_ui_state_() {
  // The UI task cannot use ESPHome APIs itself, so the status flag is set
  // here, on loopTask.
  const UiTaskState state = ui_task_state();
  if (state == this->reported_ui_state_)
    return;
  this->reported_ui_state_ = state;

  if (state == UiTaskState::RUNNING) {
    ESP_LOGI(TAG, "UI running");
  } else if (state == UiTaskState::FAILED) {
    ESP_LOGE(TAG, "UI failed to start; the backlight stays off");
    this->status_set_error(LOG_STR("UI failed to start"));
  }
}

void EspNest::report_led_ring_() {
  const LedRingState state = led_ring_state();
  if (state != this->reported_led_state_) {
    this->reported_led_state_ = state;
    if (state == LedRingState::RUNNING) {
      ESP_LOGI(TAG, "LED ring initialized (%u LEDs on GPIO %d)", static_cast<unsigned>(LED_RING_COUNT),
               LED_RING_GPIO);
      this->set_timeout(LED_STACK_REPORT_DELAY_MS, []() {
        ESP_LOGI(TAG, "nest_led stack high-water mark: %u of %u B free",
                 static_cast<unsigned>(led_ring_stack_high_water_mark()), static_cast<unsigned>(LED_TASK_STACK_BYTES));
      });
    } else if (state == LedRingState::FAILED) {
      // Not fatal: the knob works without its ring.
      ESP_LOGE(TAG, "LED ring failed to start: %s", esp_err_to_name(led_ring_error()));
      this->status_set_warning(LOG_STR("LED ring failed to start"));
    }
  }

  // Write errors: rate-limited, so a persistent strip fault (the task then
  // retries once a second) cannot flood the log.
  const uint32_t errors = led_ring_write_errors();
  const int64_t now_ms = esp_timer_get_time() / 1000;
  if (errors != this->seen_led_write_errors_) {
    this->seen_led_write_errors_ = errors;
    this->led_write_error_at_ms_ = now_ms;
  }
  if (errors != this->reported_led_write_errors_ &&
      (this->reported_led_write_errors_ == 0 ||
       now_ms - this->led_write_report_at_ms_ >= LED_ERROR_REPORT_INTERVAL_MS)) {
    // Every report repeats the stack high-water mark: the driver logs each
    // failure on the nest_led stack, so this covers the error path.
    ESP_LOGW(TAG,
             "LED ring: %u strip write(s) failed since the last report, %u since boot; "
             "nest_led stack: %u of %u B free",
             static_cast<unsigned>(errors - this->reported_led_write_errors_), static_cast<unsigned>(errors),
             static_cast<unsigned>(led_ring_stack_high_water_mark()), static_cast<unsigned>(LED_TASK_STACK_BYTES));
    this->reported_led_write_errors_ = errors;
    this->led_write_report_at_ms_ = now_ms;
    if (!this->led_write_warning_) {
      this->led_write_warning_ = true;
      this->status_set_warning(LOG_STR("LED ring strip writes failing"));
    }
  }
  if (this->led_write_warning_ && now_ms - this->led_write_error_at_ms_ >= LED_ERROR_CLEAR_MS) {
    // Only reachable with the ring RUNNING, so this cannot clear the
    // start-up failure warning above.
    this->led_write_warning_ = false;
    this->status_clear_warning();
    ESP_LOGI(TAG, "LED ring: strip writes recovered (%u failed since boot)", static_cast<unsigned>(errors));
  }
}

void EspNest::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP Nest:\n"
                "  Display: ST7701S 480x480 RGB565, 180 degrees via %s\n"
                "  UI task: core %d, priority %u, stack %u bytes\n"
                "  UI event queue: %u events\n"
                "  LED ring: %u x WS2812 on GPIO %d, task core %d, priority %u, stack %u bytes\n"
                "  Blinds labels: '%s' / '%s'",
                this->hw_mirror_x_ ? "SDIR + row flip" : "per-pixel copy", UI_TASK_CORE, UI_TASK_PRIORITY,
                static_cast<unsigned>(UI_TASK_STACK_BYTES), static_cast<unsigned>(UI_EVENT_QUEUE_DEPTH),
                static_cast<unsigned>(LED_RING_COUNT), LED_RING_GPIO, LED_TASK_CORE, LED_TASK_PRIORITY,
                static_cast<unsigned>(LED_TASK_STACK_BYTES), this->blinds_label_a_, this->blinds_label_b_);
  LOG_NUMBER("  ", "Screen Brightness", this->screen_brightness_);
  ESP_LOGCONFIG(TAG, "    Restore value: %s", YESNO(this->screen_brightness_->restore_value()));
  LOG_NUMBER("  ", "Dim Brightness", this->dim_brightness_);
  ESP_LOGCONFIG(TAG, "    Restore value: %s", YESNO(this->dim_brightness_->restore_value()));
  LOG_NUMBER("  ", "Idle Timeout", this->idle_timeout_);
  ESP_LOGCONFIG(TAG, "    Restore value: %s", YESNO(this->idle_timeout_->restore_value()));
  LOG_SWITCH("  ", "LED Ring", this->led_ring_);
  this->mqtt_bridge_.dump_config();
}

}  // namespace esphome::esp_nest
