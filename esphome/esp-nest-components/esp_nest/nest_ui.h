#pragma once

/*
 * The ESP Nest UI: one LVGL screen with four menu pages (Lights / Fiiuuu /
 * Blinds / Sound) and the brightness, fan, blinds and sound detail screens.
 * A port of the native firmware's main/ui.c (v2.0.1) to LVGL 9.6, with the
 * same layout, animations, encoder mapping and timings.
 *
 * Threads (docs/esphome-migration-plan.md, section 3):
 *
 *   - nest_ui task only: ui_init(), ui_set_status(), the ui_encoder_*()
 *     input handlers and ui_process_pending(). These are the only
 *     functions that touch LVGL; each one checks the calling task and
 *     refuses (logs an error, does nothing) a call from any other.
 *   - Any task: the getters, which read C11 atomics, and the ui_set_*()
 *     setters, which only store into atomic pending slots (last writer
 *     wins). ui_process_pending() applies the pending values on nest_ui
 *     WITHOUT notifying the change callback: a value that came from MQTT
 *     must never be published back (the anti-loop rule).
 *   - Before the nest_ui task starts (whose creation publishes them):
 *     nest_ui_set_blinds_labels() and ui_register_change_cb().
 *
 * Local user changes are reported through the change callback, on the
 * nest_ui task; the callback must not block (it hands the event to
 * loopTask).
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_EVT_BRIGHTNESS,  /* value: brightness 0-100 % */
    UI_EVT_FAN,         /* value: fan level 0=off 1=low 2=medium 3=high */
    UI_EVT_VOLUME,      /* value: +1 = up, -1 = down (stateless, relative) */
    UI_EVT_BLINDS,      /* value: (target << 8) | step; target 0=group A 1=group B,
                           step 0=open .. 6=closed */
} ui_event_type_t;

/* Local change hook, called on the nest_ui task. */
typedef void (*ui_change_cb_t)(ui_event_type_t type, int value);

/* Blinds group labels shown on the blinds detail screen (native Kconfig
 * TOPIC_BLINDS_LABEL_A/B). The strings are not copied: they must stay
 * valid for the lifetime of the program (string literals). Every glyph
 * must exist in font_helvetica_100 (the component's config validation
 * enforces it). Call before ui_init(); NULL keeps the default "A" / "B". */
void nest_ui_set_blinds_labels(const char *label_a, const char *label_b);

void ui_register_change_cb(ui_change_cb_t cb);

/* Builds the screen and loads it. nest_ui task, once, after the LVGL
 * display exists. */
void ui_init(void);

/* Bottom status line. nest_ui task only (sets the label directly). */
void ui_set_status(const char *text);

/* Input, from the encoder indev read callback (nest_ui task). */
void ui_encoder_navigate(int diff);
void ui_encoder_press(void);
void ui_encoder_long_press(void);

/* Applies the pending ui_set_*() values and runs the detail / menu idle
 * timeouts. nest_ui task, once per LVGL loop iteration. */
void ui_process_pending(void);

/* Snapshots of the applied UI state (any task). */
int  ui_get_current_page(void);   /* 0 Lights, 1 Fiiuuu, 2 Blinds, 3 Sound */
bool ui_is_detail_mode(void);
int  ui_get_brightness(void);     /* 0-100 % */
int  ui_get_fan_level(void);      /* 0-3 */

/* Values from outside (MQTT), applied by ui_process_pending() without a
 * change notification. Any task; clamped like native. */
void ui_set_brightness(int brightness);
void ui_set_fan_level(int level);
void ui_set_blinds_pos(int target, int step);   /* target 0=group A 1=group B, step 0-6 */

/* LED ring feedback for a long-press toggle in the menu (the only
 * confirmation there, the screen does not change): turned_on = swell,
 * turned off = double blink. Called by the UI on the nest_ui task;
 * implemented in nest_led_ring.cpp (an atomic store), never blocks. */
void ui_led_feedback(bool turned_on);

#ifdef __cplusplus
}
#endif
