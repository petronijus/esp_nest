#pragma once

#include <stdint.h>

/**
 * @brief Initialize the MT8901 rotary encoder using the PCNT peripheral.
 *
 * @param sig_pin GPIO for pulse signal (encoder A)
 * @param dir_pin GPIO for direction control (encoder B)
 */
void mt8901_init(int16_t sig_pin, int16_t dir_pin);

/**
 * @brief Get the current accumulated encoder count.
 *
 * @return Current count value
 */
int mt8901_get_count(void);
