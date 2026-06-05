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
    } else if (type == UI_EVT_VOLUME) {
        ha_mqtt_publish_volume(value);
    } else if (type == UI_EVT_BLINDS) {
        ha_mqtt_publish_blinds(value >> 8, value & 0xFF);
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
        ui_process_pending();
        ha_mqtt_process_pending();
        uint32_t time_till_next = lv_timer_handler();
        if (time_till_next < 1) {
            time_till_next = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(time_till_next));
    }
}

static void update_status_line(void)
{
    char buf[128];
    bool wifi = wifi_is_connected();
    bool mqtt = ha_mqtt_is_connected();

    if (!wifi) {
        snprintf(buf, sizeof(buf), "WiFi: disconnected");
    } else if (!mqtt) {
        snprintf(buf, sizeof(buf), "IP: %s | MQTT: offline", wifi_get_ip_str());
    } else {
        snprintf(buf, sizeof(buf), "IP: %s | MQTT: ok", wifi_get_ip_str());
    }
    ui_set_status(buf);
}

static void monitor_task(void *arg)
{
    bool was_wifi = false;
    bool was_mqtt = false;

    for (;;) {
        bool wifi_now = wifi_is_connected();
        bool mqtt_now = ha_mqtt_is_connected();

        if (wifi_now != was_wifi || mqtt_now != was_mqtt) {
            update_status_line();
            was_wifi = wifi_now;
            was_mqtt = mqtt_now;

            if (wifi_now && !was_wifi)
                ESP_LOGI(TAG, "WiFi restored");
            if (!wifi_now && was_wifi)
                ESP_LOGW(TAG, "WiFi lost");
            if (mqtt_now && !was_mqtt)
                ESP_LOGI(TAG, "MQTT restored");
            if (!mqtt_now && was_mqtt)
                ESP_LOGW(TAG, "MQTT lost");
        }

        if (mqtt_now) {
            ha_mqtt_publish_state();
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
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
        ESP_LOGW(TAG, "WiFi initial connection failed, will keep retrying");
        ui_set_status("WiFi: connecting...");
    } else {
        ESP_LOGI(TAG, "WiFi ready: %s", wifi_get_ip_str());
        ui_set_status("WiFi connected, starting services...");
    }

    /* --- OTA Server (start even without WiFi, will work once connected) --- */
    if (wifi_ret == ESP_OK) {
        ESP_LOGI(TAG, "Starting OTA server...");
        ota_server_start();
    }

    /* --- MQTT --- */
    ESP_LOGI(TAG, "Initializing MQTT...");
    ha_mqtt_init();

    /* LED ring breathing animation */
    led_ring_start();

    /* Update status display with current state */
    update_status_line();

    /* Periodic monitoring & telemetry */
    xTaskCreate(monitor_task, "monitor", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "All services started");
}
