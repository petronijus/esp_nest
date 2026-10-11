#pragma once

/*
 * The three number settings (screen brightness, dim brightness, idle
 * timeout) as one packed 32-bit word, plus their ranges, steps and boot
 * defaults. Pure C, no ESP-IDF / ESPHome dependencies, host-tested in
 * test/host/.
 *
 * This header is the single source of the numbers: nest_settings.cpp builds
 * the hand-off word from it, and esp_nest/__init__.py parses the
 * SETTINGS_* defines below into the YAML schema (entity min / max / step /
 * initial_value), so the entities, the UI hand-off and backlight_policy
 * cannot drift apart. Keep each define on one line as
 * `#define SETTINGS_<NAME> <decimal>`: the parser reads them by that shape.
 *
 * Layout (hand-off 3, loopTask -> nest_ui):
 *   bits  0- 6  screen brightness, %
 *   bits  7-13  dim brightness, %
 *   bits 14-23  idle timeout, s
 *
 * All fields are stored clamped, so a field can never spill into its
 * neighbour. Updating one field leaves the others as they were.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Entity ranges (native discovery: min / max / step) and boot defaults. */
#define SETTINGS_SCREEN_BRIGHTNESS_MIN     1
#define SETTINGS_SCREEN_BRIGHTNESS_MAX     100
#define SETTINGS_SCREEN_BRIGHTNESS_STEP    5
#define SETTINGS_SCREEN_BRIGHTNESS_DEFAULT 100
#define SETTINGS_DIM_BRIGHTNESS_MIN        0
#define SETTINGS_DIM_BRIGHTNESS_MAX        100
#define SETTINGS_DIM_BRIGHTNESS_STEP       5
#define SETTINGS_DIM_BRIGHTNESS_DEFAULT    20
#define SETTINGS_IDLE_TIMEOUT_MIN          0
#define SETTINGS_IDLE_TIMEOUT_MAX          600
#define SETTINGS_IDLE_TIMEOUT_STEP         5
#define SETTINGS_IDLE_TIMEOUT_DEFAULT      30

/* Stored range of the screen brightness. Native advertised 1..100 to HA but
 * clamped screen_set_backlight() to 0..100, so a direct API write of 0 is
 * stored as 0 (black screen), as in native. The entity range above is what
 * HA's slider offers. */
#define SETTINGS_SCREEN_BRIGHTNESS_STORE_MIN 0

typedef enum {
    SETTINGS_ID_SCREEN_BRIGHTNESS = 0,
    SETTINGS_ID_DIM_BRIGHTNESS,
    SETTINGS_ID_IDLE_TIMEOUT,
} settings_id_t;

/* `value` limited to the stored range of setting `id`. */
int settings_clamp(settings_id_t id, int value);

/* A float written to an entity: NaN -> `fallback` (then clamped), otherwise
 * limited to the stored range and rounded to a whole number. */
int settings_round_clamp(settings_id_t id, float value, int fallback);

/* The word holding the three values, each clamped. */
uint32_t settings_word_pack(int screen_brightness, int dim_brightness,
                            int idle_timeout);

/* Splits a word into its three fields. */
void settings_word_unpack(uint32_t word, int *screen_brightness,
                          int *dim_brightness, int *idle_timeout);

/* `word` with setting `id` replaced by `value` (clamped), the other two
 * fields untouched. */
uint32_t settings_word_with(uint32_t word, settings_id_t id, int value);

/* The boot defaults (native: 100 / 20 / 30) as a word. */
uint32_t settings_word_default(void);

#ifdef __cplusplus
}
#endif
