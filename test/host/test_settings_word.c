/*
 * Host-side tests for the packed settings word
 * (esphome/esp-nest-components/esp_nest/settings_word.c): the 7/7/10-bit
 * layout, clamping, the NaN fallback and independent field updates.
 *
 * Build & run:  make -C test/host run
 */

#include <math.h>
#include <stdint.h>

#include "check.h"
#include "settings_word.h"

#define BRIGHT SETTINGS_ID_SCREEN_BRIGHTNESS
#define DIM    SETTINGS_ID_DIM_BRIGHTNESS
#define IDLE   SETTINGS_ID_IDLE_TIMEOUT

static void fields(uint32_t word, int *b, int *d, int *t)
{
    settings_word_unpack(word, b, d, t);
}

static int field(uint32_t word, settings_id_t id)
{
    int f[3];
    fields(word, &f[0], &f[1], &f[2]);
    return f[id];
}

int main(void)
{
    int b, d, t;

    SCENARIO("boot defaults 100 / 20 / 30 round-trip");
    fields(settings_word_default(), &b, &d, &t);
    CHECK_EQ(b, 100);
    CHECK_EQ(d, 20);
    CHECK_EQ(t, 30);
    CHECK_EQ(settings_word_default(), settings_word_pack(100, 20, 30));
    fields(settings_word_pack(100, 20, 30), &b, &d, &t);
    CHECK_EQ(b, 100);
    CHECK_EQ(d, 20);
    CHECK_EQ(t, 30);

    SCENARIO("bit layout: 0-6 brightness, 7-13 dim, 14-23 timeout");
    CHECK_EQ(settings_word_pack(1, 0, 0), 1u);
    CHECK_EQ(settings_word_pack(0, 1, 0), 1u << 7);
    CHECK_EQ(settings_word_pack(0, 0, 1), 1u << 14);
    CHECK_EQ(settings_word_pack(100, 100, 600),
             100u | (100u << 7) | (600u << 14));
    CHECK_EQ(settings_word_pack(100, 100, 600) >> 24, 0);

    SCENARIO("boundary values survive a round trip");
    {
        static const int cases[][3] = {
            {1, 0, 0}, {100, 100, 600}, {1, 100, 0}, {100, 0, 600},
            {0, 0, 0}, {50, 50, 300}, {5, 95, 595},
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            fields(settings_word_pack(cases[i][0], cases[i][1], cases[i][2]),
                   &b, &d, &t);
            CHECK_EQ(b, cases[i][0]);
            CHECK_EQ(d, cases[i][1]);
            CHECK_EQ(t, cases[i][2]);
        }
    }

    SCENARIO("clamping: brightness 0..100 stored, dim 0..100, timeout 0..600");
    CHECK_EQ(settings_clamp(BRIGHT, -5), 0);
    CHECK_EQ(settings_clamp(BRIGHT, 0), 0);   /* native stored 0 although it advertised 1 */
    CHECK_EQ(settings_clamp(BRIGHT, 1), 1);
    CHECK_EQ(settings_clamp(BRIGHT, 100), 100);
    CHECK_EQ(settings_clamp(BRIGHT, 101), 100);
    CHECK_EQ(settings_clamp(BRIGHT, 100000), 100);
    CHECK_EQ(settings_clamp(DIM, -1), 0);
    CHECK_EQ(settings_clamp(DIM, 100), 100);
    CHECK_EQ(settings_clamp(DIM, 127), 100);
    CHECK_EQ(settings_clamp(IDLE, -1), 0);
    CHECK_EQ(settings_clamp(IDLE, 600), 600);
    CHECK_EQ(settings_clamp(IDLE, 601), 600);
    CHECK_EQ(settings_clamp(IDLE, 1023), 600);
    CHECK_EQ(settings_clamp(IDLE, INT32_MAX), 600);
    CHECK_EQ(settings_clamp(IDLE, INT32_MIN), 0);

    SCENARIO("out-of-range values never spill into a neighbouring field");
    fields(settings_word_pack(1000, 1000, 100000), &b, &d, &t);
    CHECK_EQ(b, 100);
    CHECK_EQ(d, 100);
    CHECK_EQ(t, 600);
    fields(settings_word_pack(-1, -1, -1), &b, &d, &t);
    CHECK_EQ(b, 0);
    CHECK_EQ(d, 0);
    CHECK_EQ(t, 0);

    SCENARIO("a single-field update leaves the other two intact");
    {
        uint32_t w = settings_word_pack(70, 15, 120);
        uint32_t w1 = settings_word_with(w, BRIGHT, 35);
        CHECK_EQ(field(w1, BRIGHT), 35);
        CHECK_EQ(field(w1, DIM), 15);
        CHECK_EQ(field(w1, IDLE), 120);
        uint32_t w2 = settings_word_with(w, DIM, 0);
        CHECK_EQ(field(w2, BRIGHT), 70);
        CHECK_EQ(field(w2, DIM), 0);
        CHECK_EQ(field(w2, IDLE), 120);
        uint32_t w3 = settings_word_with(w, IDLE, 600);
        CHECK_EQ(field(w3, BRIGHT), 70);
        CHECK_EQ(field(w3, DIM), 15);
        CHECK_EQ(field(w3, IDLE), 600);
        /* the update clamps its own field only */
        uint32_t w4 = settings_word_with(w, IDLE, 99999);
        CHECK_EQ(field(w4, BRIGHT), 70);
        CHECK_EQ(field(w4, DIM), 15);
        CHECK_EQ(field(w4, IDLE), 600);
        /* the source word is a value, not modified */
        CHECK_EQ(field(w, BRIGHT), 70);
    }

    SCENARIO("interleaved updates of different fields (CAS retry shape)");
    {
        /* A lost compare-and-swap re-applies the update onto the winner's
         * word: both changes must survive whichever lands first. */
        uint32_t base = settings_word_default();
        uint32_t a_then_b = settings_word_with(settings_word_with(base, BRIGHT, 40), IDLE, 90);
        uint32_t b_then_a = settings_word_with(settings_word_with(base, IDLE, 90), BRIGHT, 40);
        CHECK_EQ(a_then_b, b_then_a);
        CHECK_EQ(field(a_then_b, BRIGHT), 40);
        CHECK_EQ(field(a_then_b, DIM), 20);
        CHECK_EQ(field(a_then_b, IDLE), 90);
    }

    SCENARIO("every value of every field round-trips alone");
    for (int v = 0; v <= 100; v++) {
        uint32_t w = settings_word_with(settings_word_default(), BRIGHT, v);
        CHECK_EQ(field(w, BRIGHT), v);
        CHECK_EQ(field(w, DIM), 20);
        CHECK_EQ(field(w, IDLE), 30);
        w = settings_word_with(settings_word_default(), DIM, v);
        CHECK_EQ(field(w, BRIGHT), 100);
        CHECK_EQ(field(w, DIM), v);
        CHECK_EQ(field(w, IDLE), 30);
    }
    for (int v = 0; v <= 600; v++) {
        uint32_t w = settings_word_with(settings_word_default(), IDLE, v);
        CHECK_EQ(field(w, BRIGHT), 100);
        CHECK_EQ(field(w, DIM), 20);
        CHECK_EQ(field(w, IDLE), v);
    }

    SCENARIO("float writes: rounding, range, NaN fallback");
    CHECK_EQ(settings_round_clamp(BRIGHT, 55.0f, 100), 55);
    CHECK_EQ(settings_round_clamp(BRIGHT, 54.4f, 100), 54);
    CHECK_EQ(settings_round_clamp(BRIGHT, 54.5f, 100), 55);
    CHECK_EQ(settings_round_clamp(BRIGHT, 17.0f, 100), 17);  /* no step enforcement, like native */
    CHECK_EQ(settings_round_clamp(BRIGHT, 0.0f, 100), 0);
    CHECK_EQ(settings_round_clamp(BRIGHT, -3.0f, 100), 0);
    CHECK_EQ(settings_round_clamp(BRIGHT, 100.4f, 100), 100);
    CHECK_EQ(settings_round_clamp(BRIGHT, 250.0f, 100), 100);
    CHECK_EQ(settings_round_clamp(IDLE, 600.0f, 30), 600);
    CHECK_EQ(settings_round_clamp(IDLE, 1e9f, 30), 600);
    CHECK_EQ(settings_round_clamp(IDLE, -1e9f, 30), 0);
    CHECK_EQ(settings_round_clamp(IDLE, INFINITY, 30), 600);
    CHECK_EQ(settings_round_clamp(IDLE, -INFINITY, 30), 0);
    CHECK_EQ(settings_round_clamp(IDLE, NAN, 30), 30);
    CHECK_EQ(settings_round_clamp(DIM, NAN, 20), 20);
    CHECK_EQ(settings_round_clamp(DIM, NAN, 5000), 100);     /* the fallback is clamped too */
    CHECK_EQ(settings_round_clamp(DIM, 20.0f, 0), 20);

    SCENARIO("ranges and defaults are consistent");
    CHECK_EQ(SETTINGS_SCREEN_BRIGHTNESS_MIN, 1);
    CHECK_EQ(SETTINGS_SCREEN_BRIGHTNESS_MAX, 100);
    CHECK_EQ(SETTINGS_DIM_BRIGHTNESS_MIN, 0);
    CHECK_EQ(SETTINGS_DIM_BRIGHTNESS_MAX, 100);
    CHECK_EQ(SETTINGS_IDLE_TIMEOUT_MIN, 0);
    CHECK_EQ(SETTINGS_IDLE_TIMEOUT_MAX, 600);
    CHECK_EQ(SETTINGS_SCREEN_BRIGHTNESS_STEP, 5);
    CHECK_EQ(SETTINGS_DIM_BRIGHTNESS_STEP, 5);
    CHECK_EQ(SETTINGS_IDLE_TIMEOUT_STEP, 5);

    return CHECK_SUMMARY();
}
