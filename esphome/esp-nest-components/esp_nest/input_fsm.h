#pragma once

/*
 * Input state machine: rotary encoder counts + push button -> UI actions.
 *
 * Behaviour of the native v2.0.1 firmware (main/screen.c encoder_read_cb),
 * kept as is. Called once per LVGL indev read with the encoder count
 * delta since the previous read and the raw button level:
 *
 *   - Rotation while a cooldown runs is discarded (in both modes) and the
 *     menu accumulator is cleared.
 *   - Detail mode passes the raw count delta through as one navigation.
 *   - Menu mode accumulates counts; reaching +/-INPUT_FSM_MENU_STEP_COUNTS
 *     emits one page step (+1 / -1), clears the accumulator (any surplus
 *     counts are dropped) and starts a INPUT_FSM_MENU_COOLDOWN_MS cooldown.
 *   - A long press fires once, as soon as the button has been held for
 *     more than INPUT_FSM_LONG_PRESS_MS.
 *   - A short press fires on release, when the button was held for more
 *     than INPUT_FSM_PRESS_MIN_MS and no long press fired during the hold.
 *   - Every release (short, long or too short) clears the menu
 *     accumulator and starts a INPUT_FSM_RELEASE_COOLDOWN_MS cooldown; it
 *     replaces any cooldown still running, even a longer menu one.
 *   - Activity (for idle dimming) is any non-zero count delta, including
 *     counts discarded by a cooldown, or the button being held.
 *
 * The detail flag is sampled by the caller before the update, as native
 * did; actions emitted by this update do not change it.
 *
 * Pure C, no ESP-IDF / LVGL / ESPHome dependencies: time is passed in by
 * the caller as int64 milliseconds (never a wrapping uint32) and the
 * module is host-tested in test/host/. The caller takes it from
 * millis_64() / esp_timer_get_time() / 1000, never from lv_tick_get() or
 * millis(): both are uint32 and wrap after 49.7 days, and the implicit
 * widening to int64_t would hide that from the compiler.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_FSM_MENU_STEP_COUNTS     3     /* counts per menu page step */
#define INPUT_FSM_MENU_COOLDOWN_MS     600   /* after a menu page step */
#define INPUT_FSM_RELEASE_COOLDOWN_MS  300   /* after every button release */
#define INPUT_FSM_PRESS_MIN_MS         50    /* short press: held longer than this */
#define INPUT_FSM_LONG_PRESS_MS        1000  /* long press: held longer than this */

typedef struct {
    int     accum;              /* menu-mode count accumulator */
    int64_t cooldown_until_ms;  /* rotation discarded while now < this */
    bool    was_pressed;        /* button level at the previous update */
    int64_t press_start_ms;     /* time the current / last hold began */
    bool    long_press_fired;   /* long press already emitted for this hold */
} input_fsm_t;

/* Actions of one update. The caller dispatches them in field order:
 * navigate, then long_press, then press (long_press and press are never
 * both set by the same update). */
typedef struct {
    int  navigate;    /* ui_encoder_navigate() argument, 0 = no navigation */
    bool long_press;  /* ui_encoder_long_press() */
    bool press;       /* ui_encoder_press() */
    bool activity;    /* user activity, resets the idle-dimming timer */
} input_fsm_out_t;

void input_fsm_init(input_fsm_t *fsm);

/* One indev read. `count_diff` is the encoder count change since the
 * previous update, `pressed` the raw button level (true = held), `detail`
 * whether the UI shows a detail screen. Fills *out (always fully). */
void input_fsm_update(input_fsm_t *fsm, int64_t now_ms,
                      int count_diff, bool pressed, bool detail,
                      input_fsm_out_t *out);

#ifdef __cplusplus
}
#endif
