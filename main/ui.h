#pragma once

#include "lvgl.h"

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
 *        diff > 0: next screen, diff < 0: previous screen.
 */
void ui_encoder_navigate(int diff);
