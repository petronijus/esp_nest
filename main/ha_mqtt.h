#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize MQTT client and connect to broker.
 *        Publishes Home Assistant MQTT discovery messages once connected.
 *
 * @return ESP_OK on success
 */
esp_err_t ha_mqtt_init(void);

/**
 * @brief Publish a state update to Home Assistant.
 *        Call periodically to keep HA updated with device telemetry.
 */
void ha_mqtt_publish_state(void);

/**
 * @brief Check if MQTT is currently connected.
 */
bool ha_mqtt_is_connected(void);
