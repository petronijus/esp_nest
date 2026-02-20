#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "screen.h"
#include "ui.h"
#include "wifi.h"
#include "ha_mqtt.h"
#include "ota_server.h"
#include "led_ring.h"

static const char *TAG = "main";

static void ui_change_handler(ui_event_type_t type, int value)
{
    if (type == UI_EVT_BRIGHTNESS) {
        ha_mqtt_publish_light();
    } else if (type == UI_EVT_FAN) {
        ha_mqtt_publish_fan();
    }
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(2);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Initializing screen...");
    screen_init();

    const esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));

    ui_init();
    ui_register_change_cb(ui_change_handler);
    ui_set_status("Starting...");

    ESP_LOGI(TAG, "Entering LVGL main loop");
    for (;;) {
        uint32_t time_till_next = lv_timer_handler();
        if (time_till_next < 1) {
            time_till_next = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(time_till_next));
    }
}

static void mqtt_telemetry_task(void *arg)
{
    for (;;) {
        if (ha_mqtt_is_connected()) {
            ha_mqtt_publish_state();
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP Nest starting...");

    /* Start LVGL on core 1 */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16 * 1024, NULL, 5, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* --- WiFi --- */
    ESP_LOGI(TAG, "Initializing WiFi...");
    ui_set_status("Connecting WiFi...");

    esp_err_t wifi_ret = wifi_init_sta();
    if (wifi_ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connection failed");
        ui_set_status("WiFi Failed!");
        return;
    }

    ESP_LOGI(TAG, "WiFi ready: %s", wifi_get_ip_str());
    char buf[128];
    snprintf(buf, sizeof(buf), "IP: %s\nConnecting MQTT...", wifi_get_ip_str());
    ui_set_status(buf);

    /* --- OTA Server --- */
    ESP_LOGI(TAG, "Starting OTA server...");
    ota_server_start();

    /* --- MQTT --- */
    ESP_LOGI(TAG, "Connecting MQTT...");
    esp_err_t mqtt_ret = ha_mqtt_init();

    if (mqtt_ret == ESP_OK) {
        snprintf(buf, sizeof(buf), "IP: %s\nMQTT: OK\nOTA: http://%s/",
                 wifi_get_ip_str(), wifi_get_ip_str());
        ESP_LOGI(TAG, "MQTT connected");
    } else {
        snprintf(buf, sizeof(buf), "IP: %s\nMQTT: Fail\nOTA: http://%s/",
                 wifi_get_ip_str(), wifi_get_ip_str());
        ESP_LOGW(TAG, "MQTT connection failed");
    }
    ui_set_status(buf);
    ha_mqtt_publish_state();

    /* LED ring breathing animation */
    led_ring_start();

    /* Periodic telemetry */
    xTaskCreate(mqtt_telemetry_task, "telemetry", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "All services started");
}
