#include "led_ring.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "board.h"
#include "ui.h"

static const char *TAG = "led_ring";

#define NUM_LEDS        13
#define BREATH_PERIOD_MS 3000
#define TICK_MS          20
#define MIN_BRIGHTNESS   0.05f
#define MAX_BRIGHTNESS   0.45f

static led_strip_handle_t s_strip = NULL;
static volatile bool s_led_enabled = true;

static void led_ring_task(void *arg)
{
    uint32_t tick = 0;

    for (;;) {
        uint8_t r = 0, g = 0, b = 0;

        if (!s_led_enabled) {
            for (int i = 0; i < NUM_LEDS; i++)
                led_strip_set_pixel(s_strip, i, 0, 0, 0);
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(100));
            tick += 100;
            continue;
        }

        /* Page accent colors, must match PAGES[] in ui.c:
         * Lights #FFE600, Fiiuuu #00E5CC, Blinds #FB4A04, Sound #DE04FF */
        static const uint8_t page_rgb[][3] = {
            {255, 230,   0},
            {  0, 229, 204},
            {251,  74,   4},
            {222,   4, 255},
        };
        int page = ui_get_current_page();
        if (page < 0 || page > 3) page = 0;
        const uint8_t *c = page_rgb[page];

        if (page == 0 && ui_is_detail_mode()) {
            /* Brightness detail: intensity tracks brightness value directly */
            float level = (float)ui_get_brightness() / 100.0f;
            r = (uint8_t)(c[0] * level);
            g = (uint8_t)(c[1] * level);
            b = (uint8_t)(c[2] * level);
        } else {
            /* Menu mode: breathing animation in the page accent color */
            float phase = (float)(tick % BREATH_PERIOD_MS) / (float)BREATH_PERIOD_MS;
            float breath = MIN_BRIGHTNESS +
                            (MAX_BRIGHTNESS - MIN_BRIGHTNESS) *
                            (0.5f + 0.5f * sinf(phase * 2.0f * (float)M_PI));
            r = (uint8_t)(c[0] * breath);
            g = (uint8_t)(c[1] * breath);
            b = (uint8_t)(c[2] * breath);
        }

        for (int i = 0; i < NUM_LEDS; i++) {
            led_strip_set_pixel(s_strip, i, r, g, b);
        }
        led_strip_refresh(s_strip);

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        tick += TICK_MS;
    }
}

void led_ring_start(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_PIN,
        .max_leds = NUM_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(err));
        return;
    }

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    ESP_LOGI(TAG, "LED ring initialized (%d LEDs on GPIO %d)", NUM_LEDS, LED_PIN);

    xTaskCreate(led_ring_task, "led_ring", 2048, NULL, 3, NULL);
}

bool led_ring_get_enabled(void) { return s_led_enabled; }

void led_ring_set_enabled(bool enabled) { s_led_enabled = enabled; }
