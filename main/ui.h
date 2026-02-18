#pragma once

#include "lvgl.h"
#include <stdbool.h>

/**
 * @brief Initialize the UI with the two menu screens (Lights / Fan).
 *        Encoder rotation switches between screens with a fade effect.
 */
void ui_init(void);

/**
 * @brief Update the small status bar at the bottom of the active screen.
 */
void ui_set_status(const char *text);

/**
 * @brief Handle encoder rotation (call from encoder read callback or main).
 *        In menu mode: diff > 0 next screen, diff < 0 previous screen.
 *        In detail mode: adjusts brightness.
 */
void ui_encoder_navigate(int diff);

/**
 * @brief Handle encoder short press.
 *        In menu mode: enters the detail screen for the current page.
 *        In detail mode: goes back to menu.
 */
void ui_encoder_press(void);

/**
 * @brief Handle encoder long press.
 *        In detail mode: sets brightness to 0 (turn off) and goes back.
 */
void ui_encoder_long_press(void);

/**
 * @brief Return the index of the currently active page (0 = Lights, 1 = Fan).
 */
int ui_get_current_page(void);

/**
 * @brief Return true if the UI is in a detail/editing screen.
 *        Used by encoder driver to choose sensitivity.
 */
bool ui_is_detail_mode(void);

/**
 * @brief Return the current brightness value (0-100).
 */
int ui_get_brightness(void);

/**
 * @brief Return the current fan level (0=off, 1=low, 2=medium, 3=high).
 */
int ui_get_fan_level(void);
