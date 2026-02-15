/*
 * Project ZX2D10GE01R-V4848 for Arduino
 * Description: The base project that can be used in the Arduino environment
 * using the ZX2D10GE01R-V4848 device Author: Eric Nam Date: 03-18-2023
 */

// ** Prerequisites **
// ESP32 Arduino 2.0.7 based on ESP-IDF 4.4.4
// https://github.com/espressif/arduino-esp32

// LVGL version 8.x
// https://github.com/lvgl/lvgl

// GFX Library for Arduino 1.3.2
// https://github.com/moononournation/Arduino_GFX

// ZX2D10GE01R-V4848 for ESP-IDF
// https://github.com/wireless-tag-com/ZX2D10GE01R-V4848

#include "Freenove_WS2812_Lib_for_ESP32.h" // WS2812 LED library
#include "button.hpp"
#include "mt8901.hpp"
#include "secrets.h"
#include <Arduino.h> // for ledc functions
#include <Arduino_GFX_Library.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ctype.h>
#include <lvgl.h>

// Use Montserrat 22 font enabled in lv_conf.h
extern const lv_font_t lv_font_montserrat_22;

#define ECO_O(y) (y > 0) ? -1 : 1
#define ECO_STEP(x) x ? ECO_O(x) : 0
#define GFX_BL 38

// WS2812 LED Strip Configuration
#define LEDS_COUNT 13 // Number of LEDs in your strip
#define LEDS_PIN 4    // GPIO pin connected to LED strip data line
#define CHANNEL 0     // RMT channel (0-7)

Arduino_DataBus *bus =
    new Arduino_SWSPI(GFX_NOT_DEFINED /* DC */, 21 /* CS */, 47 /* SCK */,
                      41 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    39 /* DE */, 48 /* VSYNC */, 40 /* HSYNC */, 45 /* PCLK */, 10 /* R0 */,
    16 /* R1 */, 9 /* R2 */, 15 /* R3 */, 46 /* R4 */, 8 /* G0 */, 13 /* G1 */,
    18 /* G2 */, 12 /* G3 */, 11 /* G4 */, 17 /* G5 */, 47 /* B0 */,
    41 /* B1 */, 0 /* B2 */, 42 /* B3 */, 14 /* B4 */, 1 /* hsync_polarity */,
    10 /* hsync_front_porch */, 10 /* hsync_pulse_width */,
    10 /* hsync_back_porch */, 1 /* vsync_polarity */,
    14 /* vsync_front_porch */, 2 /* vsync_pulse_width */,
    12 /* vsync_back_porch */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480 /* width */, 480 /* height */, rgbpanel, 0 /* rotation */,
    true /* auto_flush */, bus, GFX_NOT_DEFINED /* RST */,
    st7701_type7_init_operations, sizeof(st7701_type7_init_operations));

static button_t *g_btn;
static uint32_t screenWidth;
static uint32_t screenHeight;
static uint16_t *disp_draw_buf;
static lv_group_t *lv_group;
static lv_obj_t *lv_btn_1;
static lv_obj_t *lv_btn_2;
static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t draw_buf;
static lv_indev_t *indev_encoder = NULL;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// WS2812 LED Strip object
Freenove_ESP32_WS2812 strip =
    Freenove_ESP32_WS2812(LEDS_COUNT, LEDS_PIN, CHANNEL, TYPE_GRB);

// Global variables for button selection
lv_obj_t *lv_btns[2];
int num_buttons = 2;
int selection_focus = 0;

// Add flag to prevent rapid UI updates
static bool ui_update_pending = false;
static unsigned long last_ui_update = 0;

static enum ActiveButton { NONE, FAN, LIGHT } activeButton = NONE;
typedef enum { FAN_OFF, FAN_LOW, FAN_MEDIUM, FAN_HIGH } FanState;
static FanState fan_state = FAN_OFF;
// removed unused light_cnt
static int light_level = 0; // Ensure default is OFF
// Remove unused color/temp tracking and last_light_level (replaced with
// last_nonzero_brightness)

static const char *FAN_TOPIC = "bashouse/livingroom-fan"; // keep fan as-is
// Zigbee2MQTT light topics
static const char *Z2M_LIGHT_TOPIC =
    "zigbee2mqtt/livingroom_lights"; // state updates
static const char *Z2M_LIGHT_SET_TOPIC =
    "zigbee2mqtt/livingroom_lights/set"; // commands
static const char *Z2M_LIGHT_GET_TOPIC =
    "zigbee2mqtt/livingroom_lights/get"; // request current state

// Add these static variables to track last published values
static int last_published_light_level = -1;
static FanState last_published_fan_state = (FanState)-1;

// Add global pointers for version and status labels
static lv_obj_t *label_version = NULL;
static lv_obj_t *label_status = NULL;

// Add global variable for inactivity timeout
unsigned long last_activity_time = 0;

// Add global variable for last nonzero brightness
static int last_nonzero_brightness = 100; // Default to 100 if never set
// Track last time user adjusted brightness via rotation to gate blue animation
static unsigned long last_light_adjust_time = 0;
// Throttle outgoing light MQTT publishes
static unsigned long last_light_publish_ms = 0;
static bool light_publish_pending = false;
// Suppress applying incoming light updates while user/local change is in
// progress
static unsigned long ignore_incoming_light_until_ms = 0;

// Helper to update the status label (throttled to reduce redraws)
void update_status_label() {
  static unsigned long last_status_update = 0;
  static char last_text[32] = "";
  unsigned long now = millis();
  if (now - last_status_update < 500)
    return; // update at most every 500ms

  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  bool mqtt_ok = mqttClient.connected();
  char buf[32];
  snprintf(buf, sizeof(buf), "WiFi: %s  MQTT: %s", wifi_ok ? "OK" : "X",
           mqtt_ok ? "OK" : "X");
  if (strcmp(buf, last_text) != 0) {
    if (label_status)
      lv_label_set_text(label_status, buf);
    strncpy(last_text, buf, sizeof(last_text) - 1);
    last_text[sizeof(last_text) - 1] = '\0';
  }
  last_status_update = now;
}

// Forward declarations
void publish_fan();
void publish_light();
void init_ws2812_lights();
void update_ws2812_lights();
void run_selected_led_cycle();

void mqtt_callback(char *topic, byte *payload, unsigned int length) {
  Serial.print("[mqtt_callback] topic: ");
  Serial.println(topic);
  char msg[256] = {0};
  unsigned int copy_len = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
  strncpy(msg, (char *)payload, copy_len);
  msg[copy_len] = '\0';
  Serial.print("[mqtt_callback] payload: ");
  Serial.println(msg);
  if (strcmp(topic, FAN_TOPIC) == 0) {
    Serial.println("[mqtt_callback] FAN_TOPIC");
    FanState new_state = fan_state;
    if (strcmp(msg, "Off") == 0)
      new_state = FAN_OFF;
    else if (strcmp(msg, "low") == 0)
      new_state = FAN_LOW;
    else if (strcmp(msg, "medium") == 0)
      new_state = FAN_MEDIUM;
    else if (strcmp(msg, "high") == 0)
      new_state = FAN_HIGH;
    if (fan_state != new_state) {
      fan_state = new_state;
      const char *fan_labels[] = {"Off", "low", "medium", "high"};
      lv_obj_t *label = lv_obj_get_child(lv_btn_1, 0);
      lv_label_set_text_fmt(label, "Fan: %s", fan_labels[fan_state]);
      Serial.printf("[mqtt_callback] Fan state changed to %d\n", fan_state);
    }
  } else if (strcmp(topic, Z2M_LIGHT_TOPIC) == 0) {
    unsigned long nowms = millis();
    if (nowms < ignore_incoming_light_until_ms || light_publish_pending) {
      Serial.println("[mqtt_callback] Z2M light update ignored (local change "
                     "in progress)");
      return;
    }
    // Zigbee2MQTT state JSON
    auto skip_ws = [](const char *p) {
      while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        ++p;
      return p;
    };
    auto find_key = [](const char *json, const char *key) {
      static char needle[64];
      snprintf(needle, sizeof(needle), "\"%s\"", key);
      return strstr(json, needle);
    };
    auto parse_string = [&](const char *json, const char *key, char *out,
                            size_t outsz) -> bool {
      const char *p = find_key(json, key);
      if (!p)
        return false;
      p = strchr(p, ':');
      if (!p)
        return false;
      p++;
      p = skip_ws(p);
      if (*p != '"')
        return false;
      p++;
      size_t i = 0;
      while (*p && *p != '"' && i < outsz - 1) {
        out[i++] = *p++;
      }
      out[i] = '\0';
      return true;
    };
    auto parse_int = [&](const char *json, const char *key, int *out) -> bool {
      const char *p = find_key(json, key);
      if (!p)
        return false;
      p = strchr(p, ':');
      if (!p)
        return false;
      p++;
      p = skip_ws(p);
      char *endp = nullptr;
      long v = strtol(p, &endp, 10);
      if (endp == p)
        return false;
      *out = (int)v;
      return true;
    };

    char stateStr[8] = {0};
    int br = -1;
    bool haveState = parse_string(msg, "state", stateStr, sizeof(stateStr));
    bool haveBrightness = parse_int(msg, "brightness", &br);

    int new_level = light_level;
    if (haveBrightness) {
      br = constrain(br, 0, 255);
      new_level = (br == 0) ? 0 : (int)((br * 100 + 127) / 255);
    }
    auto eq_icase = [](const char *a, const char *b) {
      while (*a && *b) {
        if (tolower(*a) != tolower(*b))
          return false;
        ++a;
        ++b;
      }
      return *a == '\0' && *b == '\0';
    };

    if (haveState) {
      if (eq_icase(stateStr, "OFF")) {
        new_level = 0;
      } else if (eq_icase(stateStr, "ON")) {
        if (!haveBrightness && new_level == 0) {
          new_level =
              last_nonzero_brightness > 0 ? last_nonzero_brightness : 100;
        }
      }
    }

    if (new_level != light_level) {
      light_level = new_level;
      if (light_level > 0)
        last_nonzero_brightness = light_level;
      ui_update_pending = true;
      update_ws2812_lights(); // Update WS2812 LEDs
      // Do not publish here to avoid loops; UI will reflect state
      Serial.printf(
          "[mqtt_callback] Z2M light level -> %d%% (br=%d state=%s)\n",
          light_level, br, stateStr);
    }
  }
}

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area,
                   lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w,
                            h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  lv_disp_flush_ready(disp);
}

static void encoder_read_common(lv_indev_data_t *data) {
  static bool long_press_handled = false;
  static int16_t cont_last = 0;
  static int8_t encoder_accum = 0; // NEW: accumulator for sensitivity
  static bool block_until_release = false;
  int16_t cont_now = mt8901_get_count();
  int8_t diff = ECO_STEP(cont_now - cont_last);
  cont_last = cont_now;

  int sensitivity = 4; // default for selection mode
  if (activeButton == FAN) {
    sensitivity = 3; // for example, more sensitive for fan
  } else if (activeButton == LIGHT) {
    sensitivity = 1; // for example, less sensitive for light
  }

  // Only accumulate if not blocked
  if (diff != 0) {
    last_activity_time =
        millis(); // Update last activity time on encoder rotation
    encoder_accum += diff;
    if (encoder_accum >= sensitivity) {
      diff = 1;
      encoder_accum = 0;
    } else if (encoder_accum <= -sensitivity) {
      diff = -1;
      encoder_accum = 0;
    } else {
      diff = 0;
    }
  }

  // --- New: Long press in selection mode to toggle light if light button is
  // focused ---
  if (activeButton == NONE && selection_focus == 1 && button_isPressed(g_btn) &&
      !long_press_handled) {
    if (button_wasPressFor(g_btn, 1000)) { // 1s long press
      if (light_level == 0) {
        light_level =
            last_nonzero_brightness > 0 ? last_nonzero_brightness : 100;
        update_light_button_ui();
        update_ws2812_lights(); // Update WS2812 LEDs
        ignore_incoming_light_until_ms = millis() + 800;
        publish_light();
        Serial.println(
            "[encoder_read] Long press in selection mode: Light turned ON");
      } else {
        light_level = 0;
        update_light_button_ui();
        update_ws2812_lights(); // Update WS2812 LEDs
        ignore_incoming_light_until_ms = millis() + 800;
        publish_light();
        Serial.println(
            "[encoder_read] Long press in selection mode: Light turned OFF");
      }
      // Always return to selection mode after long press toggle
      activeButton = NONE;
      lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
      lv_group_focus_obj(lv_btn_2);
      data->state = LV_INDEV_STATE_REL; // Simulate encoder release
      long_press_handled = true;
      last_activity_time =
          millis(); // Update last activity time after long press toggle
      block_until_release = true; // <--- Add this line
      return;
    }
  }
  // --- End new logic ---

  if (block_until_release) {
    if (!button_isPressed(g_btn)) {
      block_until_release = false; // Wait for release
    }
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  if (button_isPressed(g_btn)) {
    last_activity_time = millis(); // Update last activity time on button press
    data->state = LV_INDEV_STATE_PR;
    if (activeButton == LIGHT && !long_press_handled) {
      if (button_wasPressFor(g_btn, 1000)) { // 1s long press
        if (light_level == 0) {
          light_level =
              last_nonzero_brightness > 0 ? last_nonzero_brightness : 100;
        } else {
          light_level = 0;
        }
        ignore_incoming_light_until_ms = millis() + 800;
        update_light_button_ui();
        update_ws2812_lights(); // Update WS2812 LEDs
        publish_light();
        long_press_handled = true;
      }
    }
  } else {
    data->state = LV_INDEV_STATE_REL;
    if (activeButton == LIGHT && long_press_handled) {
      // On release after long press, return to selection
      activeButton = NONE;
      lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
      long_press_handled = false;
      last_activity_time =
          millis(); // Update last activity time after long press toggle
      return;
    }
    long_press_handled = false;
  }

  // No color/LED update here
  if (activeButton != NONE && diff != 0) {
    if (activeButton == FAN) {
      int prev = fan_state;
      if (diff > 0 && fan_state < FAN_HIGH)
        fan_state = (FanState)(fan_state + 1);
      else if (diff < 0 && fan_state > FAN_OFF)
        fan_state = (FanState)(fan_state - 1);
      if (prev != fan_state) {
        const char *fan_labels[] = {"Off", "low", "medium", "high"};
        lv_obj_t *label = lv_obj_get_child(lv_btn_1, 0);
        lv_label_set_text_fmt(label, "Fan: %s", fan_labels[fan_state]);
        publish_fan();
      }
    } else if (activeButton == LIGHT && !long_press_handled) {
      int prev = light_level;
      int step = 3; // 3x faster adjustment
      if (diff > 0 && light_level < 100)
        light_level = min(100, light_level + step);
      else if (diff < 0 && light_level > 0)
        light_level = max(0, light_level - step);
      if (prev != light_level) {
        if (light_level > 0)
          last_nonzero_brightness = light_level;
        last_light_adjust_time = millis();
        ignore_incoming_light_until_ms =
            last_light_adjust_time + 800; // ignore Z2M echoes briefly
        update_light_button_ui();
        update_ws2812_lights(); // Update WS2812 LEDs
        publish_light();
      }
    }
    data->enc_diff = 0;
  } else {
    data->enc_diff = diff;
  }

  // At the top of encoder_read (after static variables):
  // Use the global variables directly in the selection logic.

  if (activeButton == NONE && diff != 0) {
    if (diff > 0 && selection_focus < num_buttons - 1) {
      selection_focus++;
      lv_group_focus_obj(lv_btns[selection_focus]);
      encoder_accum = 0;
    } else if (diff < 0 && selection_focus > 0) {
      selection_focus--;
      lv_group_focus_obj(lv_btns[selection_focus]);
      encoder_accum = 0;
    }
    diff = 0;
  }
}

void encoder_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  encoder_read_common(data);
}

void connectToWiFi() {
  Serial.println("[connectToWiFi] called");
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void publish_fan() {
  if (fan_state != last_published_fan_state) {
    Serial.println("[publish_fan] called");
    const char *fan_labels[] = {"Off", "low", "medium", "high"};
    mqttClient.publish(FAN_TOPIC, fan_labels[fan_state], true);
    last_published_fan_state = fan_state;
    Serial.printf("[publish_fan] Published fan state: %d\n", fan_state);
  } else {
    Serial.println("[publish_fan] No change, not publishing");
  }
}

// Publish Zigbee2MQTT JSON command on /set when level changes from UI
#if 0
void publish_light() {
  if (light_level != last_published_light_level) {
    Serial.println("[publish_light] called");
    const char* stateStr       = (light_level == 0) ? "OFF" : "ON";
    int         raw_brightness = (light_level * 255 + 50) / 100;
    raw_brightness = constrain(raw_brightness, 0, 255);
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"brightness\":%d}", stateStr, raw_brightness);
    Serial.printf("[publish_light] %s -> %s\n", Z2M_LIGHT_SET_TOPIC, payload);
    mqttClient.publish(Z2M_LIGHT_SET_TOPIC, payload, false); // retain=false

    // // --- Sync the set topic if turning OFF from device ---
    // if (light_level == 0) {
    //   mqttClient.publish(LIGHT_SET_TOPIC, "OFF", true); // retain = true
    // } else if (light_level > 0) {
    //   mqttClient.publish(LIGHT_SET_TOPIC, "ON", true); // Optionally sync ON as well
    // }

    last_published_light_level = light_level;
    Serial.printf("[publish_light] Published light level: %d\n", light_level);
  } else {
    Serial.println("[publish_light] No change, not publishing");
  }
}
#endif

// Internal: actually publish current light level now (bypassing throttle)
static void publish_light_now() {
  if (light_level == last_published_light_level)
    return;
  const char *stateStr = (light_level == 0) ? "OFF" : "ON";
  int raw_brightness = (light_level * 255 + 50) / 100;
  raw_brightness = constrain(raw_brightness, 0, 255);
  char payload[64];
  snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"brightness\":%d}",
           stateStr, raw_brightness);
  Serial.printf("[publish_light NOW] %s -> %s\n", Z2M_LIGHT_SET_TOPIC, payload);
  mqttClient.publish(Z2M_LIGHT_SET_TOPIC, payload, false);
  last_published_light_level = light_level;
  last_light_publish_ms = millis();
  ignore_incoming_light_until_ms = last_light_publish_ms + 800;
  light_publish_pending = false;
}

// Throttled publish: schedule immediately, send at most once per 300 ms
void publish_light() {
  if (light_level == last_published_light_level) {
    light_publish_pending = false;
    return;
  }
  light_publish_pending = true;
  unsigned long now = millis();
  if (now - last_light_publish_ms >= 300) {
    publish_light_now();
  } else {
    Serial.println("[publish_light] Throttled, will publish later");
  }
}

// Add at the top, after mqttClient declaration
#define MQTT_RECONNECT_INTERVAL 5000 // ms (initial)
unsigned long lastMqttReconnectAttempt = 0;
unsigned long mqttReconnectIntervalMs = MQTT_RECONNECT_INTERVAL;
const unsigned long mqttReconnectIntervalMax = 60000; // cap at 60s
bool mqttWasConnected = false;

// WiFi reconnect management
unsigned long lastWifiReconnectAttempt = 0;
const unsigned long wifiReconnectIntervalMs = 5000;

void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWifiReconnectAttempt > wifiReconnectIntervalMs) {
      Serial.println("[ensureWiFi] WiFi disconnected, attempting reconnect...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      lastWifiReconnectAttempt = now;
    }
  }
}

void mqtt_subscribe_topics() {
  mqttClient.subscribe(FAN_TOPIC);
  mqttClient.subscribe(Z2M_LIGHT_TOPIC);
}

void handleMQTT() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    // Wait for WiFi before attempting MQTT
    return;
  }
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt > mqttReconnectIntervalMs) {
      Serial.println(
          "[handleMQTT] MQTT not connected, attempting reconnect...");
      lastMqttReconnectAttempt = now;
      if (mqttClient.connect("ZX2D10GE01R-V4848_Client", MQTT_USER,
                             MQTT_PASS)) {
        Serial.println("[handleMQTT] MQTT reconnected!");
        mqtt_subscribe_topics();
        mqttReconnectIntervalMs = MQTT_RECONNECT_INTERVAL; // reset backoff
        // Request latest Zigbee2MQTT state (if not retained)
        mqttClient.publish(Z2M_LIGHT_GET_TOPIC, "{\"state\":\"\"}", false);
        mqttClient.publish(Z2M_LIGHT_GET_TOPIC, "{\"brightness\":\"\"}", false);
        mqttWasConnected = true;
      } else {
        Serial.print("[handleMQTT] MQTT connect failed, rc=");
        Serial.println(mqttClient.state());
        // Exponential backoff up to max
        if (mqttReconnectIntervalMs < mqttReconnectIntervalMax) {
          mqttReconnectIntervalMs = mqttReconnectIntervalMs * 2;
          if (mqttReconnectIntervalMs > mqttReconnectIntervalMax)
            mqttReconnectIntervalMs = mqttReconnectIntervalMax;
        }
      }
    }
  } else {
    if (!mqttWasConnected) {
      Serial.println("[handleMQTT] MQTT connected!");
      mqtt_subscribe_topics();
      mqttReconnectIntervalMs = MQTT_RECONNECT_INTERVAL; // reset backoff
      // Request latest Zigbee2MQTT state (if not retained)
      mqttClient.publish(Z2M_LIGHT_GET_TOPIC, "{\"state\":\"\"}", false);
      mqttClient.publish(Z2M_LIGHT_GET_TOPIC, "{\"brightness\":\"\"}", false);
      mqttWasConnected = true;
    }
    mqttClient.loop();
  }
  if (mqttWasConnected && !mqttClient.connected()) {
    Serial.println("[handleMQTT] MQTT connection lost!");
    mqttWasConnected = false;
  }
}

// Use default LVGL font configured in lv_conf.h

// Initialize WS2812 LED strip and turn on startup lights
void init_ws2812_lights() {
  Serial.println("[init_ws2812_lights] Initializing WS2812 LED strip");
  strip.begin();
  strip.setBrightness(100); // Set brightness (0-255)

  // Turn on all LEDs with a warm white color at startup
  for (int i = 0; i < LEDS_COUNT; i++) {
    strip.setLedColorData(i, 255, 200, 100); // Warm white (R, G, B)
  }
  strip.show();
  Serial.println("[init_ws2812_lights] WS2812 LEDs turned ON at startup");
}

// Update WS2812 LEDs based on light_level
void update_ws2812_lights() {
  if (light_level == 0) {
    // Turn off all LEDs
    for (int i = 0; i < LEDS_COUNT; i++) {
      strip.setLedColorData(i, 0, 0, 0);
    }
  } else {
    // Calculate brightness based on light_level (0-100)
    uint8_t brightness =
        map(light_level, 1, 100, 20, 255); // Map 1-100% to 20-255 brightness
    uint8_t r = map(brightness, 20, 255, 51, 255); // Red component
    uint8_t g = map(brightness, 20, 255, 34,
                    200); // Green component (less for warm white)
    uint8_t b = map(brightness, 20, 255, 0,
                    100); // Blue component (minimal for warm white)

    for (int i = 0; i < LEDS_COUNT; i++) {
      strip.setLedColorData(i, r, g, b);
    }
  }
  strip.show();
  Serial.printf("[update_ws2812_lights] Updated WS2812 LEDs to level: %d%%\n",
                light_level);
}

// While LIGHT is selected, smoothly cycle LEDs in blue as a visual cue
void run_selected_led_cycle() {
  static unsigned long last_tick = 0;
  static int step = 4;   // brightness delta per tick
  static int value = 40; // current brightness 0-255
  const int min_v = 20;  // avoid too dim
  const int max_v = 255;
  unsigned long now = millis();
  if (activeButton != LIGHT)
    return;
  // Skip animation shortly after user changes brightness by rotation
  if (now - last_light_adjust_time < 1000)
    return;
  if (now - last_tick < 20)
    return; // ~50 FPS
  last_tick = now;
  value += step;
  if (value >= max_v) {
    value = max_v;
    step = -step;
  }
  if (value <= min_v) {
    value = min_v;
    step = -step;
  }
  for (int i = 0; i < LEDS_COUNT; i++) {
    strip.setLedColorData(i, 0, 0, (uint8_t)value); // blue channel only
  }
  strip.show();
}

void setup() {
  Serial.println("[setup] called");
  Serial.begin(115200);
  connectToWiFi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqtt_callback);
  gfx->begin();
  gfx->setRotation(2); // 0=0°, 1=90°, 2=180°, 3=270°
  gfx->fillScreen(BLACK);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  lv_init();
  // Set LVGL tick callback so timers/animations advance
  // lv_tick_set_cb(lvgl_tick_cb); // Not needed with LV_TICK_CUSTOM
  g_btn = button_attch(3, 0, 10);
  mt8901_init(5, 6);

  // Initialize WS2812 LED strip and turn on lights at startup
  init_ws2812_lights();
  screenWidth = gfx->width();
  screenHeight = gfx->height();
  // FULL FRAME buffering in PSRAM only (no partial fallback)
  size_t full_bytes = (size_t)screenWidth * screenHeight * sizeof(uint16_t);
  uint16_t *disp_draw_buf1 = nullptr;
  disp_draw_buf = (uint16_t *)heap_caps_malloc(full_bytes, MALLOC_CAP_SPIRAM |
                                                               MALLOC_CAP_8BIT);
  if (disp_draw_buf) {
    disp_draw_buf1 = (uint16_t *)heap_caps_malloc(
        full_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  bool have_double = disp_draw_buf && disp_draw_buf1;

  if (!disp_draw_buf) {
    Serial.println("LVGL draw buffer allocation failed (FULL mode)!");
  } else {
    Serial.printf("[LVGL] FULL %s buffering: %dx%d, %u bytes each\n",
                  have_double ? "double" : "single", (int)screenWidth,
                  (int)screenHeight, (unsigned)full_bytes);

    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf,
                          have_double ? disp_draw_buf1 : NULL,
                          screenWidth * screenHeight);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1; // Correspond to LV_DISPLAY_RENDER_MODE_FULL ?
                               // No, full_refresh is simpler.
    // Actually LV_DISPLAY_RENDER_MODE_FULL in v9 means we have 2 full buffers
    // and we swap them or just copy? If we have 2 full buffers, we can use
    // straight buffering. But GFX library draw16bitRGBBitmap usually works best
    // with full refresh if we passed the whole buffer.
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_ENCODER;
    indev_drv.read_cb = encoder_read;
    indev_encoder = lv_indev_drv_register(&indev_drv);

    init_lv_group();
    lv_example_get_started_1();
  }
}

void loop() {
  static unsigned long last_lvgl = 0;
  unsigned long now = millis();

  // Handle MQTT every 10ms
  handleMQTT();

  // Handle LVGL roughly at LV_DEF_REFR_PERIOD (~33ms)
  if (now - last_lvgl >= 33) {
    lv_timer_handler();
    last_lvgl = now;
  }

  // Handle pending UI updates
  if (ui_update_pending && (now - last_ui_update >= 50)) {
    update_light_button_ui();
  }

  // LED cycling when LIGHT is selected
  run_selected_led_cycle();

  // Deferred/throttled light publish
  if (light_publish_pending && (now - last_light_publish_ms >= 300)) {
    publish_light_now();
  }

  // Small delay to prevent watchdog issues
  delay(1);
  update_status_label();

  // Check for inactivity timeout (auto-exit from selection after 10s idle)
  if (activeButton != NONE && (millis() - last_activity_time > 10000)) {
    activeButton = NONE;
    lv_obj_clear_state(lv_btn_1, LV_STATE_CHECKED);
    lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
  }

  // If we just left LIGHT selection, restore LEDs to actual light level
  static ActiveButton prev_active = NONE;
  if (prev_active == LIGHT && activeButton != LIGHT) {
    update_ws2812_lights();
  }
  prev_active = activeButton;
}

void init_lv_group() {
  Serial.println("[init_lv_group] called");
  lv_group = lv_group_create();
  lv_group_set_default(lv_group);
  lv_group_set_wrap(lv_group, false);
  if (indev_encoder) {
    lv_indev_set_group(indev_encoder, lv_group);
  }
}

static void fan_btn_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    Serial.println("[fan_btn_event_cb] CLICKED");
    if (activeButton == FAN) {
      activeButton = NONE;
      lv_obj_clear_state(lv_btn_1, LV_STATE_CHECKED);
    } else {
      activeButton = FAN;
      lv_obj_add_state(lv_btn_1, LV_STATE_CHECKED);
      lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
    }
  }
}

static void light_btn_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    Serial.println("[light_btn_event_cb] CLICKED");
    if (activeButton == LIGHT) {
      activeButton = NONE;
      lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
    } else {
      activeButton = LIGHT;
      lv_obj_add_state(lv_btn_2, LV_STATE_CHECKED);
      lv_obj_clear_state(lv_btn_1, LV_STATE_CHECKED);
      // lv_obj_set_style_bg_color(lv_btn_1, lv_palette_main(LV_PALETTE_BLUE),
      // LV_PART_MAIN);
    }
  }
}

/**
 * Create a button with a label and react on click event.
 */
void lv_example_get_started_1(void) {
  Serial.println("[lv_example_get_started_1] called");
  // Set the screen background to black
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);

  // --- Custom style for button focused ---
  static lv_style_t style_button_focused;
  lv_style_init(&style_button_focused);
  // lv_style_set_bg_color(&style_button_focused, lv_color_hex(0x2196F3)); //
  // blue background
  lv_style_set_border_color(&style_button_focused,
                            lv_color_hex(0x2196F3));   // yellow border
  lv_style_set_border_width(&style_button_focused, 6); // thick border
  lv_style_set_outline_color(&style_button_focused,
                             lv_color_hex(0xFF4081));   // pink outline
  lv_style_set_outline_width(&style_button_focused, 8); // thick outline
  lv_style_set_outline_pad(&style_button_focused,
                           4); // extra space around outline
  // --- Custom style for button checked ---
  static lv_style_t style_button_checked;
  lv_style_init(&style_button_checked);
  // lv_style_set_bg_color(&style_button_checked, lv_color_hex(0x0D47A1)); //
  // dark blue
  //  --- Custom style for light ON (yellow) ---
  static lv_style_t style_light_on;
  lv_style_init(&style_light_on);
  lv_style_set_bg_color(&style_light_on, lv_color_hex(0xFB8C00)); // yeallow

  // --- Custom style for active (checked) state, darken with black overlay ---
  static lv_style_t style_active_darken;
  lv_style_init(&style_active_darken);
  lv_style_set_bg_opa(&style_active_darken,
                      LV_OPA_80); // 80% opacity black overlay
  lv_style_set_bg_color(&style_active_darken, lv_color_black());

  lv_btn_1 = lv_btn_create(lv_scr_act());
  lv_obj_set_style_bg_color(lv_btn_1, lv_palette_main(LV_PALETTE_GREY),
                            LV_PART_MAIN); // Set to gray
  lv_obj_set_size(lv_btn_1, 180, 90);
  lv_obj_align(lv_btn_1, LV_ALIGN_CENTER, 0, -60);
  lv_obj_add_event_cb(lv_btn_1, fan_btn_event_cb, LV_EVENT_CLICKED, NULL);
  // Apply button focused style for focused/checked state
  lv_obj_add_style(lv_btn_1, &style_button_focused, LV_STATE_FOCUSED);
  lv_obj_add_style(lv_btn_1, &style_button_checked,
                   (lv_style_selector_t)LV_STATE_FOCUSED |
                       (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_add_style(lv_btn_1, &style_active_darken, LV_STATE_CHECKED);
  // For fan label
  lv_obj_t *label = lv_label_create(lv_btn_1);
  lv_label_set_text(label, "Fan: Off");
  lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
  lv_obj_center(label);

  lv_btn_2 = lv_btn_create(lv_scr_act());
  lv_obj_set_style_bg_color(lv_btn_2, lv_palette_main(LV_PALETTE_GREY),
                            LV_PART_MAIN); // Set to gray
  lv_obj_set_size(lv_btn_2, 180, 90);
  lv_obj_align(lv_btn_2, LV_ALIGN_CENTER, 0, 60);
  lv_obj_add_event_cb(lv_btn_2, light_btn_event_cb, LV_EVENT_CLICKED, NULL);
  // Apply focused and checked styles
  lv_obj_add_style(lv_btn_2, &style_button_focused, LV_STATE_FOCUSED);
  lv_obj_add_style(lv_btn_2, &style_button_checked,
                   (lv_style_selector_t)LV_STATE_FOCUSED |
                       (lv_style_selector_t)LV_STATE_CHECKED);
  // Add yellow style for all combinations with LV_STATE_USER_1 (must be added
  // last for highest priority) lv_obj_add_style(lv_btn_2, &style_light_on,
  // LV_STATE_USER_1); // Removed static yellow style
  lv_obj_add_style(lv_btn_2, &style_light_on,
                   (lv_style_selector_t)LV_STATE_USER_1 |
                       (lv_style_selector_t)LV_STATE_FOCUSED);
  lv_obj_add_style(lv_btn_2, &style_light_on,
                   (lv_style_selector_t)LV_STATE_USER_1 |
                       (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_add_style(lv_btn_2, &style_light_on,
                   (lv_style_selector_t)LV_STATE_USER_1 |
                       (lv_style_selector_t)LV_STATE_FOCUSED |
                       (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_add_style(lv_btn_2, &style_active_darken, LV_STATE_CHECKED);
  // For light label
  lv_obj_t *label2 = lv_label_create(lv_btn_2);
  lv_label_set_text(label2, "Light: Off");
  lv_obj_set_style_text_font(label2, &lv_font_montserrat_22, 0);
  lv_obj_center(label2);

  // Set yellow if light is ON at startup
  if (light_level > 0) {
    lv_obj_add_state(lv_btn_2, LV_STATE_USER_1);
  } else {
    lv_obj_clear_state(lv_btn_2, LV_STATE_USER_1);
  }
  // Set label and color at startup
  update_light_button_ui();

  // Initialize the button array after buttons are created
  lv_btns[0] = lv_btn_1;
  lv_btns[1] = lv_btn_2;
  num_buttons = 2;
  selection_focus = 0;

  // Version label
  label_version = lv_label_create(lv_scr_act());
  lv_label_set_text(label_version, "v1.4");
  lv_obj_set_style_text_font(label_version, LV_FONT_DEFAULT, 0);
  lv_obj_set_style_text_color(label_version, lv_color_white(), 0);
  lv_obj_align(label_version, LV_ALIGN_BOTTOM_MID, 0, -24);

  // Status label
  label_status = lv_label_create(lv_scr_act());
  lv_label_set_text(label_status, "WiFi: ...  MQTT: ...");
  lv_obj_set_style_text_font(label_status, LV_FONT_DEFAULT, 0);
  lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
  lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -40);
  update_status_label();
}

/**
 * Update the light button label and color
 */
void update_light_button_ui() {
  // Prevent rapid successive updates
  unsigned long now = millis();
  if (now - last_ui_update < 50) { // Minimum 50ms between updates
    ui_update_pending = true;
    return;
  }

  lv_obj_t *label = lv_obj_get_child(lv_btn_2, 0);
  if (light_level == 0) {
    lv_label_set_text(label, "Light: Off");
  } else {
    lv_label_set_text_fmt(label, "Light: %d%%", light_level);
  }
  // Update color
  if (light_level == 0) {
    lv_obj_set_style_bg_color(lv_btn_2, lv_palette_main(LV_PALETTE_GREY),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        lv_btn_2, lv_palette_main(LV_PALETTE_GREY),
        (lv_style_selector_t)((uint32_t)LV_PART_MAIN |
                              (uint32_t)LV_STATE_CHECKED));
  } else {
    // Calculate yellow shade based on brightness
    uint8_t min_r = 51, min_g = 34, min_b = 0;   // Very dark yellow
    uint8_t max_r = 255, max_g = 255, max_b = 0; // Bright yellow
    float t = light_level / 100.0f;
    uint8_t r = min_r + (uint8_t)((max_r - min_r) * t);
    uint8_t g = min_g + (uint8_t)((max_g - min_g) * t);
    uint8_t b = min_b + (uint8_t)((max_b - min_b) * t);
    lv_color_t color = lv_color_make(r, g, b);
    lv_obj_set_style_bg_color(lv_btn_2, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        lv_btn_2, color,
        (lv_style_selector_t)((uint32_t)LV_PART_MAIN |
                              (uint32_t)LV_STATE_CHECKED));
  }

  last_ui_update = now;
  ui_update_pending = false;
}
