#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ha_mqtt_init(void);
void ha_mqtt_publish_state(void);
void ha_mqtt_publish_light(void);
void ha_mqtt_publish_fan(void);
void ha_mqtt_publish_volume(int dir);   /* +1 = UP, -1 = DOWN (stateless) */
void ha_mqtt_publish_blinds(int target, int step);   /* 0=group A 1=group B, step 0-6 */
void ha_mqtt_process_pending(void);
bool ha_mqtt_is_connected(void);
