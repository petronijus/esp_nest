#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t pin_io;
    uint8_t  pressed_value;
    uint8_t  pressed;
    uint8_t  released;
    uint32_t last_press_time;
    uint32_t last_release_time;
    uint16_t filter_time;
} button_t;

/**
 * @brief Create and attach a button on the given GPIO pin.
 *
 * @param gpio_pin    GPIO number
 * @param press_value GPIO level when the button is pressed (0 or 1)
 * @param filter_time Debounce filter time in ms
 * @return Pointer to the created button handle
 */
button_t *button_attach(uint16_t gpio_pin, uint8_t press_value, uint16_t filter_time);

/** @brief Returns true if button is currently pressed */
bool button_is_pressed(button_t *button);

/** @brief Returns true if button is currently released */
bool button_is_released(button_t *button);

/** @brief Returns true if button was pressed (edge-triggered, auto-clears) */
bool button_was_pressed(button_t *button);

/** @brief Returns true if button was released (edge-triggered, auto-clears) */
bool button_was_released(button_t *button);

/** @brief Returns true if button has been held for at least press_time ms */
bool button_was_pressed_for(button_t *button, uint32_t press_time);
