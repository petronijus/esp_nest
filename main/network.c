#include "network.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "secrets.h"
#include "ui.h"
#include <string.h>

static const char *TAG = "NETWORK";

static esp_mqtt_client_handle_t client = NULL;
static wifi_status_t current_wifi_status = WIFI_STATUS_DISCONNECTED;
static bool mqtt_connected = false;
static int s_retry_num = 0;
#define MAXIMUM_RETRY 10

static void heartbeat_task(void *pvParameters) {
  while (1) {
    ESP_LOGI(TAG, "HEARTBEAT - WiFi Status: %d, Retry: %d", current_wifi_status,
             s_retry_num);
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

static const char *wifi_event_to_str(int32_t id) {
  switch (id) {
  case WIFI_EVENT_STA_START:
    return "STA_START";
  case WIFI_EVENT_STA_STOP:
    return "STA_STOP";
  case WIFI_EVENT_STA_CONNECTED:
    return "STA_CONNECTED";
  case WIFI_EVENT_STA_DISCONNECTED:
    return "STA_DISCONNECTED";
  case WIFI_EVENT_SCAN_DONE:
    return "SCAN_DONE";
  case WIFI_EVENT_WIFI_READY:
    return "WIFI_READY";
  default:
    return "UNKNOWN";
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT) {
    ESP_LOGI(TAG, "WiFi Event: %ld (%s)", event_id,
             wifi_event_to_str(event_id));
    if (event_id == WIFI_EVENT_STA_START) {
      ESP_LOGI(TAG, "WIFI_EVENT_STA_START. Connecting...");
      current_wifi_status = WIFI_STATUS_CONNECTING;
      esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
      ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED. Waiting for IP...");
      current_wifi_status = WIFI_STATUS_CONNECTING;
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
      wifi_event_sta_disconnected_t *event =
          (wifi_event_sta_disconnected_t *)event_data;
      ESP_LOGE(TAG, "WIFI_EVENT_STA_DISCONNECTED. Reason: %d", event->reason);

      if (s_retry_num < MAXIMUM_RETRY) {
        current_wifi_status = WIFI_STATUS_CONNECTING;
        esp_wifi_connect();
        s_retry_num++;
        ESP_LOGI(TAG, "Retry to connect (%d/%d)", s_retry_num, MAXIMUM_RETRY);
      } else {
        current_wifi_status = WIFI_STATUS_DISCONNECTED;
        ESP_LOGE(TAG, "Connection failed.");
      }
    }
  } else if (event_base == IP_EVENT) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      ESP_LOGI(TAG, "GOT IP: " IPSTR, IP2STR(&event->ip_info.ip));
      s_retry_num = 0;
      current_wifi_status = WIFI_STATUS_CONNECTED;
    }
  }
}

wifi_status_t network_get_wifi_status(void) { return current_wifi_status; }

bool network_is_wifi_connected(void) {
  return current_wifi_status == WIFI_STATUS_CONNECTED;
}

void network_init(void) {
  esp_log_level_set("wifi", ESP_LOG_VERBOSE);
  ESP_LOGI(TAG, "Initializing networking (Standard)...");

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Use RAM for WiFi config to avoid NVS issues
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
      &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
      &instance_got_ip));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASS,
              .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,
          },
  };

  ESP_LOGI(TAG, "SSID: %s", (char *)wifi_config.sta.ssid);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  // Disable power save before start to avoid "failed to change power status" during scan
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());

  xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL);

  ESP_LOGI(TAG, "network_init DONE");
}

bool network_is_mqtt_connected(void) { return mqtt_connected; }

void network_publish_fan(const char *state) {
  if (mqtt_connected) {
    esp_mqtt_client_publish(client, "bashouse/livingroom-fan", state, 0, 1, 1);
  }
}

void network_publish_light(int level) {
  if (mqtt_connected) {
    char payload[64];
    const char *state = (level == 0) ? "OFF" : "ON";
    int br = (level * 255 + 50) / 100;
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"brightness\":%d}",
             state, br);
    esp_mqtt_client_publish(client, "zigbee2mqtt/livingroom_lights/set",
                            payload, 0, 0, 0);
  }
}
