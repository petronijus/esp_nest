#pragma once

/*
 * Light sync state machine: ESP <-> zigbee2mqtt brightness synchronization.
 *
 * Design ("local authority window" instead of echo detection):
 *
 *   - Local changes are throttled to one /set publish per
 *     LIGHT_SYNC_THROTTLE_MS; the newest value is kept pending and
 *     flushed from the main loop tick.
 *
 *   - Every publish (re)opens a LIGHT_SYNC_BLOCK_MS authority window.
 *     While the window is open, ALL incoming state messages are dropped:
 *     echoes of our own /set publishes are indistinguishable from
 *     external commands, so we don't even try to tell them apart.
 *
 *   - When the window closes, one /get resync request is emitted.
 *     From then on incoming state is applied unconditionally — Z2M is
 *     the single source of truth and the device is eventually
 *     consistent even if an external change raced our window.
 *
 * Pure C, no ESP-IDF dependencies: time is passed in by the caller so
 * the whole state machine is host-testable (see test/host/).
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIGHT_SYNC_THROTTLE_MS  300   /* min spacing between /set publishes */
#define LIGHT_SYNC_BLOCK_MS     1500  /* authority window after a publish */

typedef enum {
    LIGHT_SYNC_ACT_NONE = 0,
    LIGHT_SYNC_ACT_PUBLISH,   /* caller publishes brightness to /set now */
    LIGHT_SYNC_ACT_RESYNC,    /* caller publishes a /get request now */
} light_sync_action_t;

typedef struct {
    int64_t last_pub_ms;        /* time of last /set publish */
    int64_t block_until_ms;     /* authority window end */
    int     synced_brightness;  /* 0-100 value Z2M and we agree on, -1 unknown */
    int     pending_brightness; /* throttled local value awaiting publish, -1 none */
    bool    resync_pending;     /* /get due once the window closes */
} light_sync_t;

void light_sync_init(light_sync_t *ls);

/* Local change (encoder). Returns ACT_PUBLISH if the caller should
 * publish `brightness` to /set right away; throttled values are kept
 * pending and surface later via light_sync_tick(). */
light_sync_action_t light_sync_on_local_change(light_sync_t *ls,
                                               int64_t now_ms,
                                               int brightness);

/* Main-loop tick. Returns ACT_PUBLISH with *out_brightness set (flush of
 * a throttled pending value), ACT_RESYNC (send /get), or ACT_NONE. */
light_sync_action_t light_sync_tick(light_sync_t *ls,
                                    int64_t now_ms,
                                    int *out_brightness);

/* Incoming state message from the Z2M state topic. Returns the new
 * 0-100 brightness to apply to the UI, or -1 when the message should
 * be ignored / changes nothing. Z2M reports the stored brightness even
 * when state is OFF, so `state` always wins over `brightness`. */
int light_sync_on_state_msg(light_sync_t *ls, int64_t now_ms,
                            bool have_state, bool state_on,
                            bool have_br, int raw,
                            int current_brightness);

/* 0-100 % <-> 0-255 raw conversions (round half up, stable roundtrip) */
int light_sync_pct_to_raw(int pct);
int light_sync_raw_to_pct(int raw);

#ifdef __cplusplus
}
#endif
