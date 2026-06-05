/*
 * Host-side tests for the light_sync state machine (main/light_sync.c).
 *
 * Build & run:  make -C test/host run
 *
 * Each scenario simulates the real message flow between the ESP encoder,
 * the MQTT broker and zigbee2mqtt, including the failure modes that
 * motivated the redesign (stale echoes after throttled publishes,
 * external OFF with unchanged stored brightness).
 */

#include <stdio.h>
#include "light_sync.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                              \
    g_checks++;                                                       \
    if (!(cond)) {                                                    \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#define CHECK_EQ(a, b) do {                                           \
    g_checks++;                                                       \
    long long _a = (long long)(a), _b = (long long)(b);               \
    if (_a != _b) {                                                   \
        printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n",             \
               __FILE__, __LINE__, #a, #b, _a, _b);                   \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#define SCENARIO(name) printf("--- %s\n", name)

/* Shorthand: incoming Z2M state message {"state":..., "brightness":...} */
static int msg(light_sync_t *ls, int64_t now, const char *state, int raw, int current)
{
    bool have_state = state != NULL;
    bool state_on = have_state && state[0] == 'O' && state[1] == 'N';
    bool have_br = raw >= 0;
    return light_sync_on_state_msg(ls, now, have_state, state_on, have_br, raw, current);
}

/* ====================================================================== */

static void test_conversion_roundtrip(void)
{
    SCENARIO("conversion: pct->raw->pct roundtrip is stable for all 0-100");
    for (int pct = 0; pct <= 100; pct++)
        CHECK_EQ(light_sync_raw_to_pct(light_sync_pct_to_raw(pct)), pct);

    CHECK_EQ(light_sync_pct_to_raw(100), 255);
    CHECK_EQ(light_sync_pct_to_raw(0), 0);
    CHECK_EQ(light_sync_pct_to_raw(-5), 0);      /* clamp */
    CHECK_EQ(light_sync_pct_to_raw(150), 255);   /* clamp */
    CHECK_EQ(light_sync_raw_to_pct(300), 100);   /* clamp */
    CHECK_EQ(light_sync_raw_to_pct(-1), 0);      /* clamp */
}

static void test_throttle(void)
{
    SCENARIO("throttle: rapid changes -> publishes spaced >= THROTTLE, last value flushed");
    light_sync_t ls;
    light_sync_init(&ls);
    int out = -1;

    /* First change publishes immediately */
    CHECK_EQ(light_sync_on_local_change(&ls, 1000, 30), LIGHT_SYNC_ACT_PUBLISH);

    /* Changes within the throttle window queue up */
    CHECK_EQ(light_sync_on_local_change(&ls, 1100, 34), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_on_local_change(&ls, 1200, 38), LIGHT_SYNC_ACT_NONE);

    /* Tick before the window elapses: nothing */
    CHECK_EQ(light_sync_tick(&ls, 1250, &out), LIGHT_SYNC_ACT_NONE);

    /* Tick after: flush of the NEWEST pending value */
    CHECK_EQ(light_sync_tick(&ls, 1300, &out), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(out, 38);

    /* Nothing left pending */
    CHECK_EQ(light_sync_tick(&ls, 1350, &out), LIGHT_SYNC_ACT_NONE);
}

static void test_dial_back_cancels_pending(void)
{
    SCENARIO("throttle: dialing back to the published value cancels the pending publish");
    light_sync_t ls;
    light_sync_init(&ls);
    int out = -1;

    CHECK_EQ(light_sync_on_local_change(&ls, 1000, 50), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(light_sync_on_local_change(&ls, 1100, 52), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_on_local_change(&ls, 1200, 50), LIGHT_SYNC_ACT_NONE);
    /* No publish ever happens; window (extended by the pending change at
     * t=1100 to 1100+1500) closes -> straight to resync. */
    CHECK_EQ(light_sync_tick(&ls, 2000, &out), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, 2700, &out), LIGHT_SYNC_ACT_RESYNC);
}

static void test_echo_storm_no_jumping(void)
{
    SCENARIO("BUG 1 regression: stale echoes of older publishes never jump the UI back");
    light_sync_t ls;
    light_sync_init(&ls);
    int out = -1;

    /* User rotates: publishes at t=0 (30%) and t=320 (38%) */
    CHECK_EQ(light_sync_on_local_change(&ls, 0, 30), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(light_sync_on_local_change(&ls, 100, 34), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, 320, &out), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(out, 34);
    CHECK_EQ(light_sync_on_local_change(&ls, 400, 38), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, 700, &out), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(out, 38);

    /* Slow Zigbee echoes of the OLD values arrive late — with the old
     * two-phase logic the t=1400 echo (raw of 30%) was accepted as an
     * "external command" and the UI jumped 38 -> 30. */
    CHECK_EQ(msg(&ls, 900,  "ON", light_sync_pct_to_raw(30), 38), -1);
    CHECK_EQ(msg(&ls, 1400, "ON", light_sync_pct_to_raw(34), 38), -1);
    CHECK_EQ(msg(&ls, 2100, "ON", light_sync_pct_to_raw(38), 38), -1); /* echo of final value: no-op */

    /* Window closed at 700+1500=2200 -> resync /get exactly once */
    CHECK_EQ(light_sync_tick(&ls, 2150, &out), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, 2250, &out), LIGHT_SYNC_ACT_RESYNC);
    CHECK_EQ(light_sync_tick(&ls, 2300, &out), LIGHT_SYNC_ACT_NONE);

    /* Resync response confirms what we show -> nothing to apply */
    CHECK_EQ(msg(&ls, 2500, "ON", light_sync_pct_to_raw(38), 38), -1);
}

static void test_external_off_after_window(void)
{
    SCENARIO("BUG 2 regression: external OFF with stored brightness is applied (was: suppressed)");
    light_sync_t ls;
    light_sync_init(&ls);

    /* ESP published 50% (raw 128) long ago */
    CHECK_EQ(light_sync_on_local_change(&ls, 0, 50), LIGHT_SYNC_ACT_PUBLISH);

    /* 5s later HA turns the light off; Z2M reports OFF with the STORED
     * brightness still 128 — the old rounding guard dropped this. */
    CHECK_EQ(msg(&ls, 5000, "OFF", 128, 50), 0);
}

static void test_external_off_inside_window_repaired_by_resync(void)
{
    SCENARIO("external OFF racing the window is repaired by the /get resync");
    light_sync_t ls;
    light_sync_init(&ls);
    int out = -1;

    CHECK_EQ(light_sync_on_local_change(&ls, 0, 50), LIGHT_SYNC_ACT_PUBLISH);

    /* External OFF arrives inside the window -> dropped by design */
    CHECK_EQ(msg(&ls, 800, "OFF", 128, 50), -1);

    /* Window closes -> resync requested */
    CHECK_EQ(light_sync_tick(&ls, 1600, &out), LIGHT_SYNC_ACT_RESYNC);

    /* Z2M answers the /get with the truth -> OFF is applied */
    CHECK_EQ(msg(&ls, 1900, "OFF", 128, 50), 0);
}

static void test_external_brightness_change(void)
{
    SCENARIO("external brightness change while idle is applied directly");
    light_sync_t ls;
    light_sync_init(&ls);

    CHECK_EQ(msg(&ls, 1000, "ON", 204, 38), 80);   /* 204 raw = 80% */
}

static void test_bare_on(void)
{
    SCENARIO("bare ON without brightness: restores 100 when off, no-op when on");
    light_sync_t ls;
    light_sync_init(&ls);

    CHECK_EQ(msg(&ls, 1000, "ON", -1, 0), 100);
    CHECK_EQ(msg(&ls, 2000, "ON", -1, 60), -1);
}

static void test_accepted_state_updates_synced(void)
{
    SCENARIO("applied external value becomes 'synced': no republish echo loop");
    light_sync_t ls;
    light_sync_init(&ls);

    /* External change to 80% applied... */
    CHECK_EQ(msg(&ls, 1000, "ON", 204, 38), 80);

    /* ...so a local change reporting the same 80% must NOT publish
     * (this is what closes the door on infinite echo ping-pong). */
    CHECK_EQ(light_sync_on_local_change(&ls, 1100, 80), LIGHT_SYNC_ACT_NONE);

    /* A real local change publishes again */
    CHECK_EQ(light_sync_on_local_change(&ls, 1500, 90), LIGHT_SYNC_ACT_PUBLISH);
}

static void test_device_quantization_no_oscillation(void)
{
    SCENARIO("bulb quantization (raw 128 -> reports 127) cannot oscillate");
    light_sync_t ls;
    light_sync_init(&ls);

    /* We published 50% (raw 128); bulb stores & reports 127 = 50% too */
    CHECK_EQ(light_sync_on_local_change(&ls, 0, 50), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(msg(&ls, 2000, "ON", 127, 50), -1);     /* 127 -> 50%, no change */

    /* Even a 1% display correction (raw 126 -> 49%) is applied once and
     * settles: applying it never triggers a publish back. */
    int applied = msg(&ls, 3000, "ON", 126, 50);
    CHECK_EQ(applied, 49);
    CHECK_EQ(light_sync_on_local_change(&ls, 3100, 49), LIGHT_SYNC_ACT_NONE);
}

static void test_initial_retained_state(void)
{
    SCENARIO("fresh connect: retained Z2M state is applied immediately");
    light_sync_t ls;
    light_sync_init(&ls);

    /* No window open after init -> retained message accepted */
    CHECK_EQ(msg(&ls, 50, "ON", 255, 30), 100);
    CHECK_EQ(msg(&ls, 60, "OFF", 255, 100), 0);
}

static void test_no_resync_without_publish(void)
{
    SCENARIO("idle device never emits spurious /get requests");
    light_sync_t ls;
    light_sync_init(&ls);
    int out = -1;

    for (int64_t t = 0; t < 10000; t += 100)
        CHECK_EQ(light_sync_tick(&ls, t, &out), LIGHT_SYNC_ACT_NONE);
}

/* ====================================================================== */

int main(void)
{
    test_conversion_roundtrip();
    test_throttle();
    test_dial_back_cancels_pending();
    test_echo_storm_no_jumping();
    test_external_off_after_window();
    test_external_off_inside_window_repaired_by_resync();
    test_external_brightness_change();
    test_bare_on();
    test_accepted_state_updates_synced();
    test_device_quantization_no_oscillation();
    test_initial_retained_state();
    test_no_resync_without_publish();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
