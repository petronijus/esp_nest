#pragma once

/*
 * ZX2D10GE01R-V4848 Board Pin Definitions
 * Panlee 2.1" Round IPS-TFT (480x480) with ESP32-S3
 * Display controller: ST7701S (RGB interface + SPI for init commands)
 * Rotary encoder: MT8901
 */

/* ---------- SPI interface (used for display controller initialization) ---------- */
#define LCD_SPI_DATA0       41      /* MOSI */
#define LCD_SPI_CLK         47
#define LCD_SPI_CS          21
#define LCD_SPI_DC          (-1)
#define LCD_SPI_RST         (-1)

/* ---------- RGB parallel interface ---------- */
#define LCD_PCLK_GPIO       (45)
#define LCD_VSYNC_GPIO      (48)
#define LCD_HSYNC_GPIO      (40)
#define LCD_DE_GPIO         (39)
#define LCD_DISP_EN_GPIO    (-1)

/* Blue channel (5 bits) */
#define RGB_B0              (47)
#define RGB_B1              (41)
#define RGB_B2              (0)
#define RGB_B3              (42)
#define RGB_B4              (14)

/* Green channel (6 bits) */
#define RGB_G0              (8)
#define RGB_G1              (13)
#define RGB_G2              (18)
#define RGB_G3              (12)
#define RGB_G4              (11)
#define RGB_G5              (17)

/* Red channel (5 bits) */
#define RGB_R0              (10)
#define RGB_R1              (16)
#define RGB_R2              (9)
#define RGB_R3              (15)
#define RGB_R4              (46)

/* RGB data GPIOs mapped in order for esp_lcd_rgb_panel_config_t */
#define LCD_DATA0_GPIO      (RGB_B0)
#define LCD_DATA1_GPIO      (RGB_B1)
#define LCD_DATA2_GPIO      (RGB_B2)
#define LCD_DATA3_GPIO      (RGB_B3)
#define LCD_DATA4_GPIO      (RGB_B4)
#define LCD_DATA5_GPIO      (RGB_G0)
#define LCD_DATA6_GPIO      (RGB_G1)
#define LCD_DATA7_GPIO      (RGB_G2)
#define LCD_DATA8_GPIO      (RGB_G3)
#define LCD_DATA9_GPIO      (RGB_G4)
#define LCD_DATA10_GPIO     (RGB_G5)
#define LCD_DATA11_GPIO     (RGB_R0)
#define LCD_DATA12_GPIO     (RGB_R1)
#define LCD_DATA13_GPIO     (RGB_R2)
#define LCD_DATA14_GPIO     (RGB_R3)
#define LCD_DATA15_GPIO     (RGB_R4)

/* ---------- Backlight ---------- */
#define LCD_PIN_BK_LIGHT            38
#define LCD_BK_LIGHT_ON_LEVEL       1
#define LCD_BK_LIGHT_OFF_LEVEL      (!LCD_BK_LIGHT_ON_LEVEL)

/* ---------- Display timing ---------- */
#define LCD_H_RES           480
#define LCD_V_RES           480
#define LCD_PIXEL_CLK_HZ    (15 * 1000 * 1000)

/* ---------- Rotary Encoder (MT8901) ---------- */
#define ENCODER_A_PIN       (5)
#define ENCODER_B_PIN       (6)

/* ---------- Push Button ---------- */
#define BTN_PIN             (3)

/* ---------- Peripherals ---------- */
#define MOTOR_PIN           (7)
#define LED_PIN             (4)

/* ---------- External pins ---------- */
#define EXT_PIN_0           (1)
#define EXT_PIN_1           (2)
#define EXT_PIN_2           (20)
#define EXT_PIN_3           (19)
