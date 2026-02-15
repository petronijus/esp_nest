#include "ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "network.h"
#include <stdio.h>
#include <string.h>

#define TAG "UI"

static lv_obj_t *lv_btn_1;
static lv_obj_t *lv_btn_2;
static lv_obj_t *label_version = NULL;
static lv_obj_t *label_status = NULL;

lv_obj_t *lv_btns[2];
int num_buttons = 2;
int selection_focus = 0;

static int light_level = 0;
static int last_nonzero_brightness = 100;
static uint32_t last_ui_update = 0;
static bool ui_update_pending = false;

typedef enum { FAN_OFF, FAN_LOW, FAN_MEDIUM, FAN_HIGH } FanState;
static FanState fan_state = FAN_OFF;

typedef enum { UI_MODE_NONE, UI_MODE_FAN, UI_MODE_LIGHT } UiMode;
UiMode activeMode = UI_MODE_NONE;

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

void update_status_label(void) {
  if (!label_status)
    return;
  static uint32_t last_status_update = 0;
  uint32_t now = millis();
  if (now - last_status_update < 500)
    return;

  wifi_status_t wifi_status = network_get_wifi_status();
  bool mqtt_ok = network_is_mqtt_connected();

  const char *wifi_text = "X";
  if (wifi_status == WIFI_STATUS_CONNECTED) {
    wifi_text = "OK";
  } else if (wifi_status == WIFI_STATUS_CONNECTING) {
    wifi_text = "Retrying";
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "WiFi: %s  MQTT: %s", wifi_text,
           mqtt_ok ? "OK" : "X");
  lv_label_set_text(label_status, buf);

  last_status_update = now;
}

void update_light_button_ui(void) {
  uint32_t now = millis();
  if (now - last_ui_update < 50) {
    ui_update_pending = true;
    return;
  }

  lv_obj_t *label = lv_obj_get_child(lv_btn_2, 0);
  if (light_level == 0) {
    lv_label_set_text(label, "Light: Off");
    lv_obj_set_style_bg_color(lv_btn_2, lv_palette_main(LV_PALETTE_GREY),
                              LV_PART_MAIN);
  } else {
    lv_label_set_text_fmt(label, "Light: %d%%", light_level);
    // Calculate yellow shade based on brightness
    uint8_t min_r = 51, min_g = 34, min_b = 0;
    uint8_t max_r = 255, max_g = 255, max_b = 0;
    float t = light_level / 100.0f;
    uint8_t r = min_r + (uint8_t)((max_r - min_r) * t);
    uint8_t g = min_g + (uint8_t)((max_g - min_g) * t);
    uint8_t b = min_b + (uint8_t)((max_b - min_b) * t);
    lv_obj_set_style_bg_color(lv_btn_2, lv_color_make(r, g, b), LV_PART_MAIN);
  }

  last_ui_update = now;
  ui_update_pending = false;
}

static void fan_btn_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    if (activeMode == UI_MODE_FAN) {
      activeMode = UI_MODE_NONE;
      lv_obj_clear_state(lv_btn_1, LV_STATE_CHECKED);
    } else {
      activeMode = UI_MODE_FAN;
      lv_obj_add_state(lv_btn_1, LV_STATE_CHECKED);
      lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
    }
  }
}

static void light_btn_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    if (activeMode == UI_MODE_LIGHT) {
      activeMode = UI_MODE_NONE;
      lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
    } else {
      activeMode = UI_MODE_LIGHT;
      lv_obj_add_state(lv_btn_2, LV_STATE_CHECKED);
      lv_obj_clear_state(lv_btn_1, LV_STATE_CHECKED);
    }
  }
}

void ui_init(void) {
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);

  static lv_style_t style_button_focused;
  lv_style_init(&style_button_focused);
  lv_style_set_border_color(&style_button_focused, lv_color_hex(0x2196F3));
  lv_style_set_border_width(&style_button_focused, 6);
  lv_style_set_outline_color(&style_button_focused, lv_color_hex(0xFF4081));
  lv_style_set_outline_width(&style_button_focused, 8);
  lv_style_set_outline_pad(&style_button_focused, 4);

  static lv_style_t style_active_darken;
  lv_style_init(&style_active_darken);
  lv_style_set_bg_opa(&style_active_darken, LV_OPA_80);
  lv_style_set_bg_color(&style_active_darken, lv_color_black());

  lv_btn_1 = lv_btn_create(lv_scr_act());
  lv_obj_set_style_bg_color(lv_btn_1, lv_palette_main(LV_PALETTE_GREY),
                            LV_PART_MAIN);
  lv_obj_set_size(lv_btn_1, 180, 90);
  lv_obj_align(lv_btn_1, LV_ALIGN_CENTER, 0, -60);
  lv_obj_add_event_cb(lv_btn_1, fan_btn_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_style(lv_btn_1, &style_button_focused, LV_STATE_FOCUSED);
  lv_obj_add_style(lv_btn_1, &style_active_darken, LV_STATE_CHECKED);

  lv_obj_t *label1 = lv_label_create(lv_btn_1);
  lv_label_set_text(label1, "Fan: Off");
  lv_obj_center(label1);

  lv_btn_2 = lv_btn_create(lv_scr_act());
  lv_obj_set_style_bg_color(lv_btn_2, lv_palette_main(LV_PALETTE_GREY),
                            LV_PART_MAIN);
  lv_obj_set_size(lv_btn_2, 180, 90);
  lv_obj_align(lv_btn_2, LV_ALIGN_CENTER, 0, 60);
  lv_obj_add_event_cb(lv_btn_2, light_btn_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_style(lv_btn_2, &style_button_focused, LV_STATE_FOCUSED);
  lv_obj_add_style(lv_btn_2, &style_active_darken, LV_STATE_CHECKED);

  lv_obj_t *label2 = lv_label_create(lv_btn_2);
  lv_label_set_text(label2, "Light: Off");
  lv_obj_center(label2);

  lv_btns[0] = lv_btn_1;
  lv_btns[1] = lv_btn_2;

  label_version = lv_label_create(lv_scr_act());
  lv_label_set_text(label_version, "v1.4");
  lv_obj_set_style_text_color(label_version, lv_color_white(), 0);
  lv_obj_align(label_version, LV_ALIGN_BOTTOM_MID, 0, -24);

  label_status = lv_label_create(lv_scr_act());
  lv_label_set_text(label_status, "WiFi: ...  MQTT: ...");
  lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
  lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -40);

  update_status_label();
}

// These will be called from the modified encoder_read in screen.c
void ui_handle_encoder(int8_t diff, bool pressed) {
  static bool last_pressed = false;
  static uint32_t last_activity = 0;
  uint32_t now = millis();

  if (diff != 0 || pressed != last_pressed) {
    last_activity = now;
  }

  if (activeMode == UI_MODE_NONE) {
    if (diff > 0 && selection_focus < num_buttons - 1) {
      selection_focus++;
      lv_group_focus_obj(lv_btns[selection_focus]);
    } else if (diff < 0 && selection_focus > 0) {
      selection_focus--;
      lv_group_focus_obj(lv_btns[selection_focus]);
    }

    if (pressed && !last_pressed) {
      lv_event_send(lv_btns[selection_focus], LV_EVENT_CLICKED, NULL);
    }
  } else if (activeMode == UI_MODE_FAN) {
    if (diff != 0) {
      if (diff > 0 && fan_state < FAN_HIGH)
        fan_state++;
      else if (diff < 0 && fan_state > FAN_OFF)
        fan_state--;

      const char *fan_labels[] = {"Off", "low", "medium", "high"};
      lv_label_set_text_fmt(lv_obj_get_child(lv_btn_1, 0), "Fan: %s",
                            fan_labels[fan_state]);
    }
    if (pressed && !last_pressed) {
      lv_event_send(lv_btn_1, LV_EVENT_CLICKED, NULL);
      const char *fan_labels[] = {"Off", "low", "medium", "high"};
      network_publish_fan(fan_labels[fan_state]);
    }
  } else if (activeMode == UI_MODE_LIGHT) {
    if (diff != 0) {
      int step = 3;
      if (diff > 0 && light_level < 100)
        light_level = (light_level + step > 100) ? 100 : light_level + step;
      else if (diff < 0 && light_level > 0)
        light_level = (light_level - step < 0) ? 0 : light_level - step;

      if (light_level > 0)
        last_nonzero_brightness = light_level;
      update_light_button_ui();
    }
    if (pressed && !last_pressed) {
      lv_event_send(lv_btn_2, LV_EVENT_CLICKED, NULL);
      network_publish_light(light_level);
    }
  }

  // Auto-exit after 10s idle
  if (activeMode != UI_MODE_NONE && (now - last_activity > 10000)) {
    activeMode = UI_MODE_NONE;
    lv_obj_clear_state(lv_btn_1, LV_STATE_CHECKED);
    lv_obj_clear_state(lv_btn_2, LV_STATE_CHECKED);
  }

  last_pressed = pressed;
}

void ui_set_fan_state(const char *state) {
  if (strcmp(state, "Off") == 0)
    fan_state = FAN_OFF;
  else if (strcmp(state, "low") == 0)
    fan_state = FAN_LOW;
  else if (strcmp(state, "medium") == 0)
    fan_state = FAN_MEDIUM;
  else if (strcmp(state, "high") == 0)
    fan_state = FAN_HIGH;

  const char *fan_labels[] = {"Off", "low", "medium", "high"};
  lv_label_set_text_fmt(lv_obj_get_child(lv_btn_1, 0), "Fan: %s",
                        fan_labels[fan_state]);
}

void ui_set_light_level(int level) {
  light_level = level;
  if (light_level > 0)
    last_nonzero_brightness = light_level;
  update_light_button_ui();
}
