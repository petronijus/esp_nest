#pragma once

/**
 * @brief Initialize the RGB LCD display, LVGL display driver, backlight,
 *        and the rotary encoder input device.
 *
 * Must be called from a task with sufficient stack (>= 8 KB).
 */
void screen_init(void);

int  screen_get_backlight(void);
void screen_set_backlight(int percent);

int  screen_get_dim_level(void);
void screen_set_dim_level(int percent);

int  screen_get_idle_timeout(void);
void screen_set_idle_timeout(int seconds);
