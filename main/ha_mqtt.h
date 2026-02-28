#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ha_mqtt_init(void);
void ha_mqtt_publish_state(void);
void ha_mqtt_publish_light(void);
void ha_mqtt_publish_fan(void);
void ha_mqtt_process_pending(void);
bool ha_mqtt_is_connected(void);
