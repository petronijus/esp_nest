#pragma once

/**
 * @brief Initialize the WS2812 LED ring and start the breathing animation task.
 *        Color follows the active UI page (yellow for Lights, teal for Fan).
 */
void led_ring_start(void);
