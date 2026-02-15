#pragma once

/**
 * @brief Initialize the RGB LCD display, LVGL display driver, backlight,
 *        and the rotary encoder input device.
 *
 * Must be called from a task with sufficient stack (>= 8 KB).
 */
void screen_init(void);
