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
#include "lvgl.h"

#include "board.h"
#include "button.h"
#include "mt8901.h"
#include "ui.h"

static const char *TAG = "screen";

#define ECO_O(y)    ((y) > 0 ? -1 : 1)
#define ECO_STEP(x) ((x) ? ECO_O(x) : 0)

static esp_lcd_panel_handle_t g_panel_handle = NULL;
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
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;

    /* Wait for previous VSYNC before swapping buffers */
    xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100));

    esp_lcd_panel_draw_bitmap(g_panel_handle, x1, y1, x2 + 1, y2 + 1, px_map);
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
        .flags.double_fb = 1,
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
    ESP_LOGI(TAG, "RGB panel initialized (%dx%d) with bounce buffer", LCD_H_RES, LCD_V_RES);

    /* Get the double frame buffers allocated in PSRAM */
    void *buf1 = NULL;
    void *buf2 = NULL;
    esp_lcd_rgb_panel_get_frame_buffer(g_panel_handle, 2, &buf1, &buf2);

    /* Initialize LVGL */
    lv_init();

    /* Create LVGL display with full-frame double buffering */
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, buf1, buf2,
                           LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    ESP_LOGI(TAG, "LVGL display driver registered");
}

/* ========================================================================== */
/*  Encoder input device                                                      */
/* ========================================================================== */

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int last_count = 0;
    static int accum = 0;

    int now = mt8901_get_count();
    int diff = now - last_count;
    last_count = now;

    data->enc_diff = 0;

    if (diff != 0) {
        accum += diff;

        /* Require 3 accumulated encoder ticks to trigger a screen switch */
        if (accum >= 3) {
            ui_encoder_navigate(1);
            accum = 0;
        } else if (accum <= -3) {
            ui_encoder_navigate(-1);
            accum = 0;
        }
    }

    if (button_is_pressed(g_btn)) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void encoder_init(void)
{
    g_btn = button_attach(BTN_PIN, 1, 10);
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
    /* Configure backlight GPIO */
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_PIN_BK_LIGHT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL));

    /* Initialize the display controller via SPI */
    spi_init_display_controller();

    /* Initialize the RGB panel and LVGL */
    rgb_panel_init();

    /* Initialize encoder input */
    encoder_init();

    /* Turn on backlight */
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL));
    ESP_LOGI(TAG, "Screen initialization complete");
}
