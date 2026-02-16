#pragma once

#include "esp_err.h"

/**
 * @brief Start an HTTP server that accepts OTA firmware uploads.
 *
 * POST a .bin file to http://<device-ip>/update to flash OTA.
 * GET  http://<device-ip>/ returns a simple upload form.
 *
 * @return ESP_OK on success
 */
esp_err_t ota_server_start(void);
