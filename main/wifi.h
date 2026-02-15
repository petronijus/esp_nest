#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize WiFi in station mode and connect to the configured AP.
 *
 * Blocks until connected (with IP) or connection fails after max retries.
 *
 * @return ESP_OK on successful connection, ESP_FAIL otherwise.
 */
esp_err_t wifi_init_sta(void);

/**
 * @brief Get the current IP address as a string.
 *
 * @return Null-terminated IP string, or "0.0.0.0" if not connected.
 */
const char *wifi_get_ip_str(void);

/**
 * @brief Check if WiFi is currently connected.
 *
 * @return true if connected with IP, false otherwise.
 */
bool wifi_is_connected(void);
