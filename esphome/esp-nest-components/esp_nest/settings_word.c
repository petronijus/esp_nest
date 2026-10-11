#include "settings_word.h"

#include "backlight_policy.h"

#define PCT_BITS      7
#define TIMEOUT_BITS  10
#define BACKLIGHT_SHIFT 0
#define DIM_SHIFT       PCT_BITS
#define TIMEOUT_SHIFT   (2 * PCT_BITS)
#define PCT_MASK        ((1u << PCT_BITS) - 1u)
#define TIMEOUT_MASK    ((1u << TIMEOUT_BITS) - 1u)

_Static_assert(SETTINGS_SCREEN_BRIGHTNESS_MAX <= PCT_MASK &&
               SETTINGS_DIM_BRIGHTNESS_MAX <= PCT_MASK,
               "percent fields are 7 bits");
_Static_assert(SETTINGS_IDLE_TIMEOUT_MAX <= TIMEOUT_MASK,
               "the timeout field is 10 bits");
_Static_assert(TIMEOUT_SHIFT + TIMEOUT_BITS <= 32, "the word is 32 bits");
_Static_assert(SETTINGS_SCREEN_BRIGHTNESS_STORE_MIN <= SETTINGS_SCREEN_BRIGHTNESS_MIN,
               "the stored range contains the entity range");
/* The entities and backlight_policy share one set of limits and defaults. */
_Static_assert(SETTINGS_IDLE_TIMEOUT_MAX == BACKLIGHT_POLICY_TIMEOUT_MAX_S,
               "the entity range is the policy range");
_Static_assert(SETTINGS_SCREEN_BRIGHTNESS_DEFAULT == BACKLIGHT_POLICY_DEFAULT_ACTIVE_PCT,
               "screen brightness default matches backlight_policy");
_Static_assert(SETTINGS_DIM_BRIGHTNESS_DEFAULT == BACKLIGHT_POLICY_DEFAULT_DIM_PCT,
               "dim brightness default matches backlight_policy");
_Static_assert(SETTINGS_IDLE_TIMEOUT_DEFAULT == BACKLIGHT_POLICY_DEFAULT_TIMEOUT_S,
               "idle timeout default matches backlight_policy");

static int stored_min(settings_id_t id)
{
    switch (id) {
    case SETTINGS_ID_SCREEN_BRIGHTNESS: return SETTINGS_SCREEN_BRIGHTNESS_STORE_MIN;
    case SETTINGS_ID_DIM_BRIGHTNESS:    return SETTINGS_DIM_BRIGHTNESS_MIN;
    case SETTINGS_ID_IDLE_TIMEOUT:      return SETTINGS_IDLE_TIMEOUT_MIN;
    }
    return 0;
}

static int stored_max(settings_id_t id)
{
    switch (id) {
    case SETTINGS_ID_SCREEN_BRIGHTNESS: return SETTINGS_SCREEN_BRIGHTNESS_MAX;
    case SETTINGS_ID_DIM_BRIGHTNESS:    return SETTINGS_DIM_BRIGHTNESS_MAX;
    case SETTINGS_ID_IDLE_TIMEOUT:      return SETTINGS_IDLE_TIMEOUT_MAX;
    }
    return 0;
}

int settings_clamp(settings_id_t id, int value)
{
    const int lo = stored_min(id), hi = stored_max(id);
    return value < lo ? lo : value > hi ? hi : value;
}

int settings_round_clamp(settings_id_t id, float value, int fallback)
{
    if (value != value)                       /* NaN */
        return settings_clamp(id, fallback);
    const float lo = (float)stored_min(id), hi = (float)stored_max(id);
    value = value < lo ? lo : value > hi ? hi : value;
    /* value >= lo >= 0: adding one half and truncating rounds half up,
     * which is lround() for non-negative numbers. */
    return (int)(value + 0.5f);
}

uint32_t settings_word_pack(int screen_brightness, int dim_brightness,
                            int idle_timeout)
{
    return ((uint32_t)settings_clamp(SETTINGS_ID_SCREEN_BRIGHTNESS, screen_brightness) << BACKLIGHT_SHIFT) |
           ((uint32_t)settings_clamp(SETTINGS_ID_DIM_BRIGHTNESS, dim_brightness) << DIM_SHIFT) |
           ((uint32_t)settings_clamp(SETTINGS_ID_IDLE_TIMEOUT, idle_timeout) << TIMEOUT_SHIFT);
}

void settings_word_unpack(uint32_t word, int *screen_brightness,
                          int *dim_brightness, int *idle_timeout)
{
    *screen_brightness = (int)((word >> BACKLIGHT_SHIFT) & PCT_MASK);
    *dim_brightness = (int)((word >> DIM_SHIFT) & PCT_MASK);
    *idle_timeout = (int)((word >> TIMEOUT_SHIFT) & TIMEOUT_MASK);
}

uint32_t settings_word_with(uint32_t word, settings_id_t id, int value)
{
    int b, d, t;
    settings_word_unpack(word, &b, &d, &t);
    switch (id) {
    case SETTINGS_ID_SCREEN_BRIGHTNESS: b = value; break;
    case SETTINGS_ID_DIM_BRIGHTNESS:    d = value; break;
    case SETTINGS_ID_IDLE_TIMEOUT:      t = value; break;
    }
    return settings_word_pack(b, d, t);
}

uint32_t settings_word_default(void)
{
    return settings_word_pack(SETTINGS_SCREEN_BRIGHTNESS_DEFAULT,
                              SETTINGS_DIM_BRIGHTNESS_DEFAULT,
                              SETTINGS_IDLE_TIMEOUT_DEFAULT);
}
