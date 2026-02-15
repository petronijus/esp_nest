#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  WIFI_STATUS_DISCONNECTED,
  WIFI_STATUS_CONNECTING,
  WIFI_STATUS_CONNECTED
} wifi_status_t;

void network_init(void);
wifi_status_t network_get_wifi_status(void);
bool network_is_wifi_connected(void);
bool network_is_mqtt_connected(void);

void network_publish_fan(const char *state);
void network_publish_light(int brightness);

#endif // NETWORK_H
