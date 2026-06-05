#pragma once

#include <stdbool.h>

/**
 * @brief Initialize the WS2812 LED ring and start the breathing animation task.
 *        Color follows the active UI page (yellow for Lights, teal for Fan).
 */
void led_ring_start(void);

/* One-shot feedback effect in the current page accent color:
 * turned_on = bright swell, turned_off = double blink. */
void led_ring_feedback(bool turned_on);

bool led_ring_get_enabled(void);
void led_ring_set_enabled(bool enabled);
