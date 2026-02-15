#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "button.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "button";

static QueueHandle_t btn_queue = NULL;
static bool isr_service_installed = false;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    button_t *btn = (button_t *)arg;
    xQueueSendFromISR(btn_queue, &btn, NULL);
}

static void button_update_task(void *arg)
{
    button_t *btn = NULL;
    for (;;) {
        xQueueReceive(btn_queue, &btn, portMAX_DELAY);
        uint32_t time = xTaskGetTickCount();
        if (time - btn->last_press_time < btn->filter_time) {
            continue;
        }
        if (time - btn->last_release_time < btn->filter_time) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(btn->filter_time));
        if (gpio_get_level(btn->pin_io) == btn->pressed_value) {
            btn->pressed = 1;
            btn->last_press_time = time;
        } else {
            btn->released = 1;
            btn->last_release_time = time;
        }
    }
}

bool button_is_pressed(button_t *button)
{
    return gpio_get_level(button->pin_io) == button->pressed_value;
}

bool button_is_released(button_t *button)
{
    return gpio_get_level(button->pin_io) != button->pressed_value;
}

bool button_was_pressed(button_t *button)
{
    uint32_t time = xTaskGetTickCount();
    uint8_t pressed = button->pressed;
    button->pressed = 0;
    return pressed && ((time - button->last_press_time) < 300);
}

bool button_was_released(button_t *button)
{
    uint32_t time = xTaskGetTickCount();
    uint8_t released = button->released;
    button->released = 0;
    return released && ((time - button->last_release_time) < 300);
}

bool button_was_pressed_for(button_t *button, uint32_t press_time)
{
    uint32_t time_now = xTaskGetTickCount();
    if (button->pressed &&
        ((time_now - button->last_press_time) > press_time) &&
        button->last_press_time > button->last_release_time) {
        button->pressed = 0;
        return true;
    }
    return false;
}

button_t *button_attach(uint16_t gpio_pin, uint8_t press_value, uint16_t filter_time)
{
    if (btn_queue == NULL) {
        btn_queue = xQueueCreate(10, sizeof(button_t *));
        xTaskCreatePinnedToCore(button_update_task, "btn_task", 2 * 1024, NULL, 4, NULL, 1);
    }

    if (!isr_service_installed) {
        gpio_install_isr_service(0);
        isr_service_installed = true;
    }

    button_t *button = (button_t *)calloc(1, sizeof(button_t));
    if (button == NULL) {
        ESP_LOGE(TAG, "Failed to allocate button");
        return NULL;
    }

    button->pin_io = gpio_pin;
    button->pressed_value = press_value;
    button->filter_time = filter_time;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
    gpio_isr_handler_add(gpio_pin, gpio_isr_handler, button);

    ESP_LOGI(TAG, "Button attached on GPIO %d", gpio_pin);
    return button;
}
