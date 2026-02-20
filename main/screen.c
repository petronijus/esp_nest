#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "board.h"
#include "button.h"
#include "mt8901.h"
#include "ui.h"

static const char *TAG = "screen";

#define ECO_O(y)    ((y) > 0 ? -1 : 1)
#define ECO_STEP(x) ((x) ? ECO_O(x) : 0)

/* Backlight PWM */
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ     5000
#define BL_LEDC_RESOLUTION  LEDC_TIMER_13_BIT
#define BL_MAX_DUTY         ((1 << 13) - 1)

#define BL_FADE_MS          1000

static int s_bl_active_percent = 100;
static int s_bl_dim_percent = 20;
static int s_bl_idle_timeout_ms = 30000;

static uint32_t s_last_activity_ms = 0;
static bool s_bl_dimmed = false;

static esp_lcd_panel_handle_t g_panel_handle = NULL;
static uint16_t *s_fb = NULL;
static button_t *g_btn = NULL;

/* ========================================================================== */
/*  LVGL display flush callback with VSYNC sync                               */
/* ========================================================================== */

static SemaphoreHandle_t s_vsync_sem = NULL;
static lv_display_t *s_disp = NULL;

static bool IRAM_ATTR vsync_callback(esp_lcd_panel_handle_t panel,
                                      const esp_lcd_rgb_panel_event_data_t *edata,
                                      void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &high_task_wakeup);
    }
    return (high_task_wakeup == pdTRUE);
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    const uint16_t *src = (const uint16_t *)px_map;

    for (int32_t y = 0; y < h; y++) {
        int32_t dst_y = (LCD_V_RES - 1 - area->y1) - y;
        int32_t dst_x_start = LCD_H_RES - 1 - area->x1;
        const uint16_t *src_row = src + y * w;
        uint16_t *dst_row = s_fb + dst_y * LCD_H_RES;
        for (int32_t x = 0; x < w; x++) {
            dst_row[dst_x_start - x] = src_row[x];
        }
    }

    lv_display_flush_ready(disp);
}

/* ========================================================================== */
/*  ST7701S SPI initialization commands                                       */
/* ========================================================================== */

static spi_device_handle_t g_screen_spi;

static void lcd_cmd(spi_device_handle_t spi, const uint16_t data)
{
    uint16_t tmp_cmd = (data | 0x0000);
    spi_transaction_ext_t trans = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD,
            .cmd = tmp_cmd,
        },
        .command_bits = 9,
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi, (spi_transaction_t *)&trans));
}

static void lcd_data(spi_device_handle_t spi, const uint16_t data)
{
    uint16_t tmp_cmd = (data | 0x0100);
    spi_transaction_ext_t trans = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD,
            .cmd = tmp_cmd,
        },
        .command_bits = 9,
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi, (spi_transaction_t *)&trans));
}

static void SPI_WriteComm(uint16_t cmd)
{
    lcd_cmd(g_screen_spi, cmd);
}

static void SPI_WriteData(uint16_t data)
{
    lcd_data(g_screen_spi, data);
}

static void st7701s_init_sequence(void)
{
    SPI_WriteComm(0xFF);
    SPI_WriteData(0x77);
    SPI_WriteData(0x01);
    SPI_WriteData(0x00);
    SPI_WriteData(0x00);
    SPI_WriteData(0x13);
    SPI_WriteComm(0xEF);
    SPI_WriteData(0x08);

    SPI_WriteComm(0xFF);
    SPI_WriteData(0x77);
    SPI_WriteData(0x01);
    SPI_WriteData(0x00);
    SPI_WriteData(0x00);
    SPI_WriteData(0x10);

    SPI_WriteComm(0xC0);
    SPI_WriteData(0x3B);
    SPI_WriteData(0x00);

    SPI_WriteComm(0xC1);
    SPI_WriteData(0x0B);
    SPI_WriteData(0x02);

    SPI_WriteComm(0xC2);
    SPI_WriteData(0x07);
    SPI_WriteData(0x02);

    SPI_WriteComm(0xCC);
    SPI_WriteData(0x10);

    SPI_WriteComm(0xCD);
    SPI_WriteData(0x08);  /* RGB565 */

    /* Positive Gamma */
    SPI_WriteComm(0xB0);
    SPI_WriteData(0x00); SPI_WriteData(0x11); SPI_WriteData(0x16);
    SPI_WriteData(0x0e); SPI_WriteData(0x11); SPI_WriteData(0x06);
    SPI_WriteData(0x05); SPI_WriteData(0x09); SPI_WriteData(0x08);
    SPI_WriteData(0x21); SPI_WriteData(0x06); SPI_WriteData(0x13);
    SPI_WriteData(0x10); SPI_WriteData(0x29); SPI_WriteData(0x31);
    SPI_WriteData(0x18);

    /* Negative Gamma */
    SPI_WriteComm(0xB1);
    SPI_WriteData(0x00); SPI_WriteData(0x11); SPI_WriteData(0x16);
    SPI_WriteData(0x0e); SPI_WriteData(0x11); SPI_WriteData(0x07);
    SPI_WriteData(0x05); SPI_WriteData(0x09); SPI_WriteData(0x09);
    SPI_WriteData(0x21); SPI_WriteData(0x05); SPI_WriteData(0x13);
    SPI_WriteData(0x11); SPI_WriteData(0x2a); SPI_WriteData(0x31);
    SPI_WriteData(0x18);

    SPI_WriteComm(0xFF);
    SPI_WriteData(0x77);
    SPI_WriteData(0x01);
    SPI_WriteData(0x00);
    SPI_WriteData(0x00);
    SPI_WriteData(0x11);

    SPI_WriteComm(0xB0);
    SPI_WriteData(0x6d);

    SPI_WriteComm(0xB1);
    SPI_WriteData(0x37);

    SPI_WriteComm(0xB2);
    SPI_WriteData(0x81);

    SPI_WriteComm(0xB3);
    SPI_WriteData(0x80);

    SPI_WriteComm(0xB5);
    SPI_WriteData(0x43);

    SPI_WriteComm(0xB7);
    SPI_WriteData(0x85);

    SPI_WriteComm(0xB8);
    SPI_WriteData(0x20);

    SPI_WriteComm(0xC1);
    SPI_WriteData(0x78);

    SPI_WriteComm(0xC2);
    SPI_WriteData(0x78);

    SPI_WriteComm(0xD0);
    SPI_WriteData(0x88);

    SPI_WriteComm(0xE0);
    SPI_WriteData(0x00);
    SPI_WriteData(0x00);
    SPI_WriteData(0x02);

    SPI_WriteComm(0xE1);
    SPI_WriteData(0x03); SPI_WriteData(0xA0);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x04); SPI_WriteData(0xA0);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00);
    SPI_WriteData(0x20); SPI_WriteData(0x20);

    SPI_WriteComm(0xE2);
    SPI_WriteData(0x00); SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00);

    SPI_WriteComm(0xE3);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x11); SPI_WriteData(0x00);

    SPI_WriteComm(0xE4);
    SPI_WriteData(0x22); SPI_WriteData(0x00);

    SPI_WriteComm(0xE5);
    SPI_WriteData(0x05); SPI_WriteData(0xEC);
    SPI_WriteData(0xA0); SPI_WriteData(0xA0);
    SPI_WriteData(0x07); SPI_WriteData(0xEE);
    SPI_WriteData(0xA0); SPI_WriteData(0xA0);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00);

    SPI_WriteComm(0xE6);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x11); SPI_WriteData(0x00);

    SPI_WriteComm(0xE7);
    SPI_WriteData(0x22); SPI_WriteData(0x00);

    SPI_WriteComm(0xE8);
    SPI_WriteData(0x06); SPI_WriteData(0xED);
    SPI_WriteData(0xA0); SPI_WriteData(0xA0);
    SPI_WriteData(0x08); SPI_WriteData(0xEF);
    SPI_WriteData(0xA0); SPI_WriteData(0xA0);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00); SPI_WriteData(0x00);

    SPI_WriteComm(0xEB);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x40); SPI_WriteData(0x40);
    SPI_WriteData(0x00); SPI_WriteData(0x00);
    SPI_WriteData(0x00);

    SPI_WriteComm(0xED);
    SPI_WriteData(0xFF); SPI_WriteData(0xFF);
    SPI_WriteData(0xFF); SPI_WriteData(0xBA);
    SPI_WriteData(0x0A); SPI_WriteData(0xBF);
    SPI_WriteData(0x45); SPI_WriteData(0xFF);
    SPI_WriteData(0xFF); SPI_WriteData(0x54);
    SPI_WriteData(0xFB); SPI_WriteData(0xA0);
    SPI_WriteData(0xAB); SPI_WriteData(0xFF);
    SPI_WriteData(0xFF); SPI_WriteData(0xFF);

    SPI_WriteComm(0xEF);
    SPI_WriteData(0x10); SPI_WriteData(0x0D);
    SPI_WriteData(0x04); SPI_WriteData(0x08);
    SPI_WriteData(0x3F); SPI_WriteData(0x1F);

    SPI_WriteComm(0xFF);
    SPI_WriteData(0x77);
    SPI_WriteData(0x01);
    SPI_WriteData(0x00);
    SPI_WriteData(0x00);
    SPI_WriteData(0x13);

    SPI_WriteComm(0xEF);
    SPI_WriteData(0x08);

    SPI_WriteComm(0xFF);
    SPI_WriteData(0x77);
    SPI_WriteData(0x01);
    SPI_WriteData(0x00);
    SPI_WriteData(0x00);
    SPI_WriteData(0x00);

    SPI_WriteComm(0x36);
    SPI_WriteData(0x00);

    SPI_WriteComm(0x3A);
    SPI_WriteData(0x66);  /* 18-bit color (RGB666) for RGB interface */

    SPI_WriteComm(0x11);  /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));

    SPI_WriteComm(0x29);  /* Display on */
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void spi_init_display_controller(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SPI_CLK,
        .mosi_io_num = LCD_SPI_DATA0,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 10 * 1024,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_MASTER_FREQ_10M,
        .mode = 0,
        .spics_io_num = LCD_SPI_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &g_screen_spi));

    st7701s_init_sequence();

    /* SPI is only needed for init commands; free it so RGB pins can take over */
    spi_bus_remove_device(g_screen_spi);
    spi_bus_free(SPI2_HOST);
    ESP_LOGI(TAG, "ST7701S init via SPI complete");
}

/* ========================================================================== */
/*  Backlight (LEDC PWM)                                                      */
/* ========================================================================== */

static void backlight_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RESOLUTION,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t channel_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .gpio_num   = LCD_PIN_BK_LIGHT,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));
    ESP_ERROR_CHECK(ledc_fade_func_install(0));
}

static void backlight_set(int percent)
{
    uint32_t duty = ((uint32_t)BL_MAX_DUTY * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

static void backlight_fade_to(int percent, int fade_ms)
{
    uint32_t duty = ((uint32_t)BL_MAX_DUTY * percent) / 100;
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty, fade_ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
}

/* ========================================================================== */
/*  RGB panel + LVGL display setup                                            */
/* ========================================================================== */

static void rgb_panel_init(void)
{
    s_vsync_sem = xSemaphoreCreateBinary();

    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16,
        .psram_trans_align = 64,
        .clk_src = LCD_CLK_SRC_PLL160M,
        .pclk_gpio_num = LCD_PCLK_GPIO,
        .vsync_gpio_num = LCD_VSYNC_GPIO,
        .hsync_gpio_num = LCD_HSYNC_GPIO,
        .de_gpio_num = LCD_DE_GPIO,
        .disp_gpio_num = LCD_DISP_EN_GPIO,
        .data_gpio_nums = {
            LCD_DATA0_GPIO,  LCD_DATA1_GPIO,  LCD_DATA2_GPIO,  LCD_DATA3_GPIO,
            LCD_DATA4_GPIO,  LCD_DATA5_GPIO,  LCD_DATA6_GPIO,  LCD_DATA7_GPIO,
            LCD_DATA8_GPIO,  LCD_DATA9_GPIO,  LCD_DATA10_GPIO, LCD_DATA11_GPIO,
            LCD_DATA12_GPIO, LCD_DATA13_GPIO, LCD_DATA14_GPIO, LCD_DATA15_GPIO,
        },
        .timings = {
            .pclk_hz = LCD_PIXEL_CLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 10,
            .hsync_back_porch = 10,
            .hsync_front_porch = 10,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 12,
            .vsync_front_porch = 14,
        },
        .flags.fb_in_psram = 1,
        .flags.double_fb = 0,
        .flags.refresh_on_demand = 0,
        .bounce_buffer_size_px = 10 * LCD_H_RES,
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &g_panel_handle));

    /* Register VSYNC callback for frame synchronization */
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = vsync_callback,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(g_panel_handle, &cbs, NULL));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel_handle));
    ESP_LOGI(TAG, "RGB panel initialized (%dx%d) rotated 180°", LCD_H_RES, LCD_V_RES);

    /* Get the single framebuffer for direct pixel writes */
    void *fb = NULL;
    esp_lcd_rgb_panel_get_frame_buffer(g_panel_handle, 1, &fb);
    s_fb = (uint16_t *)fb;

    /* Initialize LVGL */
    lv_init();

    /* PARTIAL mode: small render buffer, flush writes rotated pixels to FB */
    static lv_color_t lvgl_buf[LCD_H_RES * 60];
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, lvgl_buf, NULL, sizeof(lvgl_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "LVGL display driver registered");
}

/* ========================================================================== */
/*  Encoder input device                                                      */
/* ========================================================================== */

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int last_count = 0;
    static int accum = 0;
    static uint32_t cooldown_until = 0;
    static bool was_pressed = false;
    static uint32_t press_start = 0;
    static bool long_press_fired = false;

    int now = mt8901_get_count();
    int diff = now - last_count;
    last_count = now;

    data->enc_diff = 0;

    uint32_t now_ms = lv_tick_get();
    bool detail = ui_is_detail_mode();

    /* --- Rotation handling --- */
    if (now_ms < cooldown_until) {
        accum = 0;
    } else if (diff != 0) {
        if (detail) {
            ui_encoder_navigate(diff);
        } else {
            accum += diff;
            if (accum >= 3) {
                ui_encoder_navigate(1);
                accum = 0;
                cooldown_until = now_ms + 600;
            } else if (accum <= -3) {
                ui_encoder_navigate(-1);
                accum = 0;
                cooldown_until = now_ms + 600;
            }
        }
    }

    /* --- Button press / long-press detection --- */
    bool pressed = button_is_pressed(g_btn);

    if (pressed && !was_pressed) {
        press_start = now_ms;
        long_press_fired = false;
    }

    if (pressed && !long_press_fired) {
        if ((now_ms - press_start) > 1000) {
            ui_encoder_long_press();
            long_press_fired = true;
        }
    }

    if (!pressed && was_pressed) {
        if (!long_press_fired && (now_ms - press_start) > 50) {
            ui_encoder_press();
        }
        accum = 0;
        cooldown_until = now_ms + 300;
    }

    /* --- Backlight idle dimming --- */
    bool activity = (diff != 0) || pressed;
    if (activity) {
        s_last_activity_ms = now_ms;
        if (s_bl_dimmed) {
            backlight_set(s_bl_active_percent);
            s_bl_dimmed = false;
        }
    } else if (!s_bl_dimmed && s_bl_idle_timeout_ms > 0 &&
               now_ms - s_last_activity_ms >= (uint32_t)s_bl_idle_timeout_ms) {
        backlight_fade_to(s_bl_dim_percent, BL_FADE_MS);
        s_bl_dimmed = true;
    }

    was_pressed = pressed;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void encoder_init(void)
{
    /* Reset GPIO 3 first to clear any strapping-pin configuration */
    gpio_reset_pin(BTN_PIN);

    g_btn = button_attach(BTN_PIN, 0, 10);
    mt8901_init(ENCODER_A_PIN, ENCODER_B_PIN);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, encoder_read_cb);

    ESP_LOGI(TAG, "Encoder input device registered");
}

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

void screen_init(void)
{
    /* Configure backlight PWM (off during init to hide garbage frames) */
    backlight_init();

    /* Initialize the display controller via SPI */
    spi_init_display_controller();

    /* Initialize the RGB panel and LVGL */
    rgb_panel_init();

    /* Initialize encoder input */
    encoder_init();

    /* Turn on backlight at full brightness */
    backlight_set(s_bl_active_percent);
    s_bl_dimmed = false;
    ESP_LOGI(TAG, "Screen initialization complete");
}

int screen_get_backlight(void) { return s_bl_active_percent; }

void screen_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_bl_active_percent = percent;
    if (!s_bl_dimmed) backlight_set(percent);
}

int screen_get_dim_level(void) { return s_bl_dim_percent; }

void screen_set_dim_level(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_bl_dim_percent = percent;
    if (s_bl_dimmed) backlight_set(percent);
}

int screen_get_idle_timeout(void) { return s_bl_idle_timeout_ms / 1000; }

void screen_set_idle_timeout(int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds > 600) seconds = 600;
    s_bl_idle_timeout_ms = seconds * 1000;
}
