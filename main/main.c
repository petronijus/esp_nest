#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "network.h"
#include "nvs_flash.h"
#include "rom/gpio.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

#define TAG "MAIN"

static void increase_lvgl_tick(void *arg) { lv_tick_inc(portTICK_PERIOD_MS); }

extern void screen_init(void);

void lvgl_task(void *arg) {
  screen_init();

  // Tick interface for LVGL
  const esp_timer_create_args_t periodic_timer_args = {
      .callback = increase_lvgl_tick, .name = "periodic_gui"};
  esp_timer_handle_t periodic_timer;
  esp_timer_create(&periodic_timer_args, &periodic_timer);
  esp_timer_start_periodic(periodic_timer, portTICK_PERIOD_MS * 1000);

  extern void ui_init(void);
  ui_init();

  for (;;) {
    lv_task_handler();
    update_status_label();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  network_init();

  // Start the LVGL task
  xTaskCreatePinnedToCore(lvgl_task, NULL, 8 * 1024, NULL, 5, NULL, 1);
}
