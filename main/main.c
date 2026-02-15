#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "screen.h"
#include "wifi.h"

static const char *TAG = "main";

static lv_obj_t *s_status_label = NULL;

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(2);
}

static void update_status_label(const char *text)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
}

static void demo_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x003a57), LV_PART_MAIN);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "ZX2D10GE01R\nESP32-S3\nStarting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Initializing screen...");
    screen_init();

    /* LVGL tick timer (2 ms period) */
    const esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));

    /* Create the UI */
    demo_ui();

    ESP_LOGI(TAG, "Entering LVGL main loop");
    for (;;) {
        uint32_t time_till_next = lv_timer_handler();
        if (time_till_next < 1) {
            time_till_next = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(time_till_next));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ZX2D10GE01R-V4848 starting...");

    /* Start LVGL on core 1 */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8 * 1024, NULL, 5, NULL, 1);

    /* Give LVGL a moment to init the display */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Initialize WiFi (runs on core 0) */
    ESP_LOGI(TAG, "Initializing WiFi...");
    update_status_label("Connecting\nWiFi...");

    esp_err_t wifi_ret = wifi_init_sta();
    if (wifi_ret == ESP_OK) {
        char buf[80];
        snprintf(buf, sizeof(buf), "WiFi Connected!\nIP: %s", wifi_get_ip_str());
        ESP_LOGI(TAG, "WiFi ready: %s", wifi_get_ip_str());
        update_status_label(buf);
    } else {
        ESP_LOGW(TAG, "WiFi connection failed");
        update_status_label("WiFi Failed!\nCheck config");
    }
}
