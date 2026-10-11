#pragma once

/*
 * Fan synchronization: UI fan level <-> the retained fan state topic.
 *
 * Behaviour of the native v2.0.1 firmware (main/ha_mqtt.c), kept as is:
 *
 *   - A local change publishes the level (retained, not throttled) only
 *     when it differs from the last published level.
 *   - An incoming state equal to the last published level within
 *     FAN_SYNC_ECHO_MS of that publish is our own echo and is dropped.
 *   - Any other incoming state is applied when it differs from the level
 *     the UI shows, and becomes the last published level, so stepping the
 *     knob back to it does not republish. A stale echo of an older value
 *     during rapid stepping is therefore applied (the UI jumps back and the
 *     next echo moves it forward again) — native behaviour, preserved.
 *   - On every MQTT (re)connect the last published level is forgotten, so
 *     the retained value delivered on subscribe always wins over the UI.
 *
 * Pure C, no ESP-IDF / ESPHome dependencies: time is passed in by the
 * caller as int64 milliseconds (never a wrapping uint32) and the module is
 * host-tested in test/host/.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAN_SYNC_ECHO_MS    1000  /* own-echo window after a publish */
#define FAN_SYNC_LEVEL_MAX  3     /* levels 0 (Off) .. 3 (high) */

typedef struct {
    int     last_pub;     /* last level published or accepted, -1 none */
    int64_t last_pub_ms;  /* time of the last publish, 0 none */
} fan_sync_t;

void fan_sync_init(fan_sync_t *fs);

/* MQTT (re)connected: forget the last published level. */
void fan_sync_on_connect(fan_sync_t *fs);

/* Local level change while connected. Returns true when the caller must
 * publish `level` now (the state is updated as published); false when it
 * equals the last published level or is outside 0..FAN_SYNC_LEVEL_MAX. */
bool fan_sync_on_local(fan_sync_t *fs, int64_t now_ms, int level);

/* Incoming fan state, `parsed_level` as returned by
 * mqtt_payload_parse_fan() (-1 = unrecognised payload, which maps to the
 * current UI level). Returns the level to apply to the UI, or -1 when the
 * message is an own echo or changes nothing. */
int fan_sync_on_message(fan_sync_t *fs, int64_t now_ms,
                        int parsed_level, int current_level);

#ifdef __cplusplus
}
#endif
