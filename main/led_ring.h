#pragma once

#include <stdbool.h>

/**
 * @brief Initialize the WS2812 LED ring and start the breathing animation task.
 *        Color follows the active UI page (yellow for Lights, teal for Fan).
 */
void led_ring_start(void);

bool led_ring_get_enabled(void);
void led_ring_set_enabled(bool enabled);
