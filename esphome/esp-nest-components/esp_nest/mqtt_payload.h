#pragma once

/*
 * MQTT payload parsing and formatting for the external devices
 * (zigbee2mqtt lights, fan, stereo volume, ESPHome blinds bridge).
 *
 * The parsers reproduce the native v2.0.1 firmware (main/ha_mqtt.c)
 * byte for byte, including its quirks — they are part of the observable
 * behaviour, so do not "fix" them:
 *
 *   - Every payload is first truncated to a fixed buffer (255 / 31 / 15
 *     chars) and then treated as a C string, so an embedded NUL ends it.
 *   - JSON keys are found with strstr on the quoted key name: the FIRST
 *     occurrence anywhere wins, nesting is ignored.
 *   - A JSON string value is copied up to its closing quote or the end of
 *     the payload and silently cut to 7 chars.
 *   - Numbers go through strtol base 10, so "127.5" reads as 127 and
 *     `null` (or any non-number) reads as absent.
 *
 * Integers are saturated to the int range, which is exactly what strtol
 * does on the 32-bit-long ESP32; on a 64-bit-long host it keeps the
 * results identical instead of implementation-defined.
 *
 * Pure C, no ESP-IDF / ESPHome dependencies: host-tested in test/host/.
 */

#include <stdbool.h>
#include <stddef.h>

#include "blinds_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_PAYLOAD_LIGHT_MAX   255  /* lights state payload truncation */
#define MQTT_PAYLOAD_FAN_MAX     31   /* fan state payload truncation */
#define MQTT_PAYLOAD_INT_MAX     15   /* blinds position payload truncation */
#define MQTT_PAYLOAD_STATE_MAX   7    /* quoted "state" value truncation */

/* Buffer large enough for every payload the format helpers produce. */
#define MQTT_PAYLOAD_OUT_SIZE    64

typedef struct {
    bool have_state;                        /* "state" key with a quoted value */
    bool state_on;                          /* state == "ON", case-insensitive */
    char state[MQTT_PAYLOAD_STATE_MAX + 1]; /* state as received (for logging) */
    bool have_brightness;                   /* "brightness" key with a number */
    int  brightness;                        /* raw 0-255 as sent, -1 if absent */
} mqtt_light_state_t;

/* zigbee2mqtt light state, e.g. {"state":"ON","brightness":128,...}.
 * `data` need not be NUL-terminated. Always fills *out. */
void mqtt_payload_parse_light(const char *data, size_t len,
                              mqtt_light_state_t *out);

/* Fan state: exact case-insensitive "Off" / "low" / "medium" / "high"
 * -> 0..3, anything else -> -1 (the caller keeps its current level). */
int mqtt_payload_parse_fan(const char *data, size_t len);

/* Plain integer (blinds member position). Returns false, leaving *out
 * untouched, when the payload does not start with a number. */
bool mqtt_payload_parse_int(const char *data, size_t len, int *out);

/* Fan level 0..3 -> retained state label; NULL for any other level. */
const char *mqtt_payload_fan_label(int level);

/* Lights /set payload for a 0-100 % brightness:
 * {"state":"ON","brightness":<raw>}, or {"state":"OFF","brightness":0}
 * for 0. Returns the payload length (snprintf semantics). */
int mqtt_payload_light_set(int pct, char *buf, size_t bufsz);

/* Lights /get request: Z2M answers on the state topic with the full
 * state including brightness. */
const char *mqtt_payload_light_get(void);

/* Stereo volume /set pulse for a UI bump direction: "UP" (dir > 0),
 * "DOWN" (dir < 0), NULL for 0 (nothing to send). */
const char *mqtt_payload_volume(int dir);

/* Blinds command payload: "OPEN" / "CLOSE" for the group broadcast,
 * the decimal position for the member commands. Returns the payload
 * length (snprintf semantics), or -1 for BLINDS_SYNC_CMD_NONE. */
int mqtt_payload_blinds_command(const blinds_sync_cmd_t *cmd,
                                char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif
