#pragma once

/*
 * Backlight policy: active level, idle dimming and the settings rules.
 *
 * Behaviour of the native v2.0.1 firmware (main/screen.c: the idle part of
 * encoder_read_cb and the screen_set_* setters), kept as is:
 *
 *   - User activity restarts the idle timer; when the screen is dimmed it
 *     returns to the active level instantly (no fade).
 *   - Without activity, once the idle timeout has elapsed the screen fades
 *     to the dim level over BACKLIGHT_POLICY_FADE_MS. A timeout of 0 means
 *     never dim.
 *   - Active level change: stored; applied instantly only when not dimmed,
 *     otherwise it takes effect on the next wake.
 *   - Dim level change: stored; applied instantly only when dimmed.
 *   - Timeout change: stored; the idle timer is NOT restarted, so a shorter
 *     (or newly enabled) timeout can dim on the very next tick.
 *   - Levels are clamped to 0..100 %, the timeout to
 *     0..BACKLIGHT_POLICY_TIMEOUT_MAX_S seconds.
 *
 * The module only decides; the caller drives the LEDC hardware from the
 * returned action (a SET must stop a fade still in progress first).
 *
 * Pure C, no ESP-IDF / ESPHome dependencies: time is passed in by the
 * caller as int64 milliseconds (never a wrapping uint32) and the module is
 * host-tested in test/host/. The caller takes it from millis_64() /
 * esp_timer_get_time() / 1000, never from lv_tick_get() or millis(): both
 * are uint32 and wrap after 49.7 days, and the implicit widening to
 * int64_t would hide that from the compiler.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKLIGHT_POLICY_FADE_MS            1000  /* idle fade duration */
#define BACKLIGHT_POLICY_TIMEOUT_MAX_S      600   /* idle timeout clamp */
#define BACKLIGHT_POLICY_DEFAULT_ACTIVE_PCT 100   /* native boot defaults */
#define BACKLIGHT_POLICY_DEFAULT_DIM_PCT    20
#define BACKLIGHT_POLICY_DEFAULT_TIMEOUT_S  30

typedef enum {
    BACKLIGHT_POLICY_ACT_NONE = 0,
    BACKLIGHT_POLICY_ACT_SET,    /* set *out_pct instantly */
    BACKLIGHT_POLICY_ACT_FADE,   /* fade to *out_pct over BACKLIGHT_POLICY_FADE_MS */
} backlight_policy_action_t;

typedef struct {
    int     active_pct;        /* 0-100 level while in use */
    int     dim_pct;           /* 0-100 level while idle */
    int     timeout_ms;        /* idle time before dimming, 0 = never */
    bool    dimmed;            /* at (or fading to) the dim level */
    int64_t last_activity_ms;  /* time of the last user activity */
} backlight_policy_t;

/* Initial state: the given settings (clamped like the setters), not
 * dimmed, idle timer started at now_ms. Returns ACT_SET with *out_pct =
 * the active level, which the caller applies once the panel is up. */
backlight_policy_action_t backlight_policy_init(backlight_policy_t *bp,
                                                int64_t now_ms,
                                                int active_pct, int dim_pct,
                                                int timeout_s, int *out_pct);

/* One indev read: the single entry point for idle dimming, called once per
 * read with `activity` = input_fsm_out_t.activity of the same read. As in
 * native, activity and the idle check are mutually exclusive within a read:
 *
 *   - activity:    restart the idle timer; returns ACT_SET with the active
 *                  level when waking from dim, else ACT_NONE. Never FADE,
 *                  even when the timeout elapsed before this read.
 *   - no activity: returns ACT_FADE with the dim level when not dimmed, the
 *                  timeout is non-zero and it has elapsed, else ACT_NONE.
 *
 * `now_ms` must be the int64 millis_64() / esp_timer_get_time() / 1000
 * clock, never lv_tick_get() or millis() (uint32, wraps after 49.7 days). */
backlight_policy_action_t backlight_policy_update(backlight_policy_t *bp,
                                                  int64_t now_ms,
                                                  bool activity,
                                                  int *out_pct);

/* Settings. The level setters return ACT_SET with the clamped level when
 * it must be applied now, else ACT_NONE (stored for later). */
backlight_policy_action_t backlight_policy_set_active(backlight_policy_t *bp,
                                                      int pct, int *out_pct);
backlight_policy_action_t backlight_policy_set_dim(backlight_policy_t *bp,
                                                   int pct, int *out_pct);
void backlight_policy_set_timeout(backlight_policy_t *bp, int seconds);

#ifdef __cplusplus
}
#endif
