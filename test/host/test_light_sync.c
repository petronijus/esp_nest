/*
 * Host-side tests for the light_sync state machine
 * (esphome/esp-nest-components/esp_nest/light_sync.c).
 *
 * Build & run:  make -C test/host run
 *
 * Each scenario simulates the real message flow between the ESP encoder,
 * the MQTT broker and zigbee2mqtt, including the failure modes that
 * motivated the redesign (stale echoes after throttled publishes,
 * external OFF with unchanged stored brightness).
 */

#include "check.h"
#include "light_sync.h"

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

static void test_time_base_crosses_2_pow_32(void)
{
    SCENARIO("time base: throttle, window, resync and apply across now = 2^32 ms (49.7 days)");
    /* The ESPHome bridge feeds int64 millis_64(); a uint32 millis() would
     * wrap here. Every interval below straddles or lands just past 2^32. */
    const int64_t wrap = (int64_t)1 << 32;
    const int64_t t0 = wrap - 1000;
    light_sync_t ls;
    light_sync_init(&ls);
    int out = -1;

    /* Throttle well before the boundary */
    CHECK_EQ(light_sync_on_local_change(&ls, t0, 30), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(light_sync_on_local_change(&ls, t0 + 100, 34), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, t0 + 299, &out), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, t0 + 300, &out), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(out, 34);

    /* Publish 100 ms before 2^32, queue a change exactly at 2^32: the
     * 300 ms throttle spans the boundary and flushes at 2^32 + 200. */
    CHECK(t0 + 900 < wrap && t0 + 1200 > wrap);
    CHECK_EQ(light_sync_on_local_change(&ls, t0 + 900, 38), LIGHT_SYNC_ACT_PUBLISH);

    /* The window opened at 2^32 - 100 ends past 2^32. Just before the
     * boundary it must still be closed to state messages and must not emit
     * an early /get (a truncated uint32 window end would read as elapsed). */
    CHECK_EQ(msg(&ls, wrap - 50, "OFF", 0, 38), -1);
    CHECK_EQ(light_sync_tick(&ls, wrap - 50, &out), LIGHT_SYNC_ACT_NONE);

    CHECK_EQ(light_sync_on_local_change(&ls, wrap, 42), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, wrap + 199, &out), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, wrap + 200, &out), LIGHT_SYNC_ACT_PUBLISH);
    CHECK_EQ(out, 42);

    /* Authority window: 1500 ms from the last publish (2^32 + 200), so it
     * closes at 2^32 + 1700. Everything inside it is dropped — an external
     * OFF and a stale echo on the window's last millisecond alike. */
    CHECK_EQ(msg(&ls, wrap + 500,  "OFF", 0, 42), -1);
    CHECK_EQ(msg(&ls, wrap + 1699, "ON", light_sync_pct_to_raw(30), 42), -1);

    /* Exactly one /get when the window closes */
    CHECK_EQ(light_sync_tick(&ls, wrap + 1699, &out), LIGHT_SYNC_ACT_NONE);
    CHECK_EQ(light_sync_tick(&ls, wrap + 1700, &out), LIGHT_SYNC_ACT_RESYNC);
    CHECK_EQ(light_sync_tick(&ls, wrap + 1800, &out), LIGHT_SYNC_ACT_NONE);

    /* Resync answer is applied unconditionally; OFF wins over the stored
     * brightness (raw of the 42 % we published). */
    CHECK_EQ(light_sync_pct_to_raw(42), 107);
    CHECK_EQ(msg(&ls, wrap + 1900, "OFF", light_sync_pct_to_raw(42), 42), 0);

    /* Idle past the boundary: no spurious publishes or /get requests */
    for (int64_t t = wrap + 2000; t < wrap + 12000; t += 100)
        CHECK_EQ(light_sync_tick(&ls, t, &out), LIGHT_SYNC_ACT_NONE);

    /* Throttle state survived the boundary: a fresh change publishes */
    CHECK_EQ(light_sync_on_local_change(&ls, wrap + 12000, 60), LIGHT_SYNC_ACT_PUBLISH);
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
    test_time_base_crosses_2_pow_32();

    return CHECK_SUMMARY();
}
