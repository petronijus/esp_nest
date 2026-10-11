/*
 * Host-side tests for the fan sync state machine
 * (esphome/esp-nest-components/esp_nest/fan_sync.c).
 *
 * Build & run:  make -C test/host run
 *
 * The expectations are the native v2.0.1 behaviour (main/ha_mqtt.c):
 * publish on change, 1 s own-echo guard, retained value wins on connect,
 * and the rapid-stepping jump-back that native exhibits.
 */

#include "check.h"
#include "fan_sync.h"
#include "mqtt_payload.h"

/* Incoming fan payload, parsed exactly like the bridge does */
static int msg(fan_sync_t *fs, int64_t now, const char *payload, int current)
{
    return fan_sync_on_message(fs, now,
                               mqtt_payload_parse_fan(payload, strlen(payload)),
                               current);
}

/* ====================================================================== */

static void test_publish_on_change(void)
{
    SCENARIO("local: publish only when the level differs from the last published one");
    fan_sync_t fs;
    fan_sync_init(&fs);
    CHECK_EQ(fs.last_pub, -1);
    CHECK_EQ(fs.last_pub_ms, 0);

    CHECK(fan_sync_on_local(&fs, 1000, 1));
    CHECK_EQ(fs.last_pub, 1);
    CHECK_EQ(fs.last_pub_ms, 1000);

    /* Same level again: nothing (and the time stays at the publish) */
    CHECK(!fan_sync_on_local(&fs, 1100, 1));
    CHECK_EQ(fs.last_pub_ms, 1000);

    /* Not throttled: every distinct level publishes immediately */
    CHECK(fan_sync_on_local(&fs, 1101, 2));
    CHECK(fan_sync_on_local(&fs, 1102, 3));
    CHECK(fan_sync_on_local(&fs, 1103, 0));
    CHECK_EQ(fs.last_pub, 0);

    /* Out of range: never publishable, state untouched */
    CHECK(!fan_sync_on_local(&fs, 1200, 4));
    CHECK(!fan_sync_on_local(&fs, 1200, -1));
    CHECK_EQ(fs.last_pub, 0);
    CHECK_EQ(fs.last_pub_ms, 1103);
}

static void test_echo_guard(void)
{
    SCENARIO("inbound: own echo within 1 s is dropped, even while the UI moved on");
    fan_sync_t fs;
    fan_sync_init(&fs);

    /* Published medium at t=1000; the UI is already at high, its event not
     * yet published when the echo of medium arrives */
    CHECK(fan_sync_on_local(&fs, 1000, 2));
    CHECK_EQ(msg(&fs, 1200, "medium", 3), -1);
    CHECK_EQ(msg(&fs, 1999, "Medium", 3), -1);   /* last millisecond of the guard */

    /* Guard over (elapsed == 1000 is not < 1000): applied, becomes last_pub */
    CHECK_EQ(msg(&fs, 2000, "medium", 3), 2);
    CHECK_EQ(fs.last_pub, 2);
    /* Accepting does not move the publish time */
    CHECK_EQ(fs.last_pub_ms, 1000);

    /* A different value inside the guard window is not an echo */
    fan_sync_init(&fs);
    CHECK(fan_sync_on_local(&fs, 5000, 2));
    CHECK_EQ(msg(&fs, 5100, "low", 2), 1);
    CHECK_EQ(fs.last_pub, 1);
}

static void test_echo_matching_ui_is_noop(void)
{
    SCENARIO("inbound: echo of the value the UI shows changes nothing");
    fan_sync_t fs;
    fan_sync_init(&fs);

    CHECK(fan_sync_on_local(&fs, 1000, 1));
    CHECK_EQ(msg(&fs, 1300, "low", 1), -1);
    CHECK_EQ(msg(&fs, 9000, "low", 1), -1);      /* also outside the guard */
    CHECK_EQ(fs.last_pub, 1);
}

static void test_rapid_stepping_jump_back(void)
{
    SCENARIO("native quirk preserved: rapid low->medium->high, stale echoes jump back");
    fan_sync_t fs;
    fan_sync_init(&fs);

    CHECK(fan_sync_on_local(&fs, 1000, 1));
    CHECK(fan_sync_on_local(&fs, 1100, 2));
    CHECK(fan_sync_on_local(&fs, 1200, 3));

    /* Echo of "low" is not the last published value -> applied: jump back */
    CHECK_EQ(msg(&fs, 1300, "low", 3), 1);
    CHECK_EQ(fs.last_pub, 1);
    /* Echo of "medium" moves forward again */
    CHECK_EQ(msg(&fs, 1400, "medium", 1), 2);
    /* Echo of "high" ends where the user stopped */
    CHECK_EQ(msg(&fs, 1500, "high", 2), 3);
    CHECK_EQ(fs.last_pub, 3);

    /* last_pub tracks the accepted value: no republish of it */
    CHECK(!fan_sync_on_local(&fs, 1600, 3));
}

static void test_unknown_payload(void)
{
    SCENARIO("inbound: an unrecognised payload maps to the current level (no-op)");
    fan_sync_t fs;
    fan_sync_init(&fs);

    CHECK_EQ(msg(&fs, 1000, "turbo", 2), -1);
    CHECK_EQ(msg(&fs, 1000, "", 0), -1);
    CHECK_EQ(msg(&fs, 1000, "2", 1), -1);
    CHECK_EQ(fs.last_pub, -1);

    /* Even inside the guard with current == last_pub: dropped, untouched */
    CHECK(fan_sync_on_local(&fs, 2000, 2));
    CHECK_EQ(msg(&fs, 2100, "turbo", 2), -1);
    CHECK_EQ(fs.last_pub, 2);
}

static void test_retained_wins_on_connect(void)
{
    SCENARIO("connect: retained value overrides the UI; stepping back to it publishes nothing");
    fan_sync_t fs;
    fan_sync_init(&fs);

    CHECK(fan_sync_on_local(&fs, 1000, 3));
    fan_sync_on_connect(&fs);
    CHECK_EQ(fs.last_pub, -1);
    CHECK_EQ(fs.last_pub_ms, 0);

    /* Retained "low" delivered on subscribe while the UI shows high */
    CHECK_EQ(msg(&fs, 1050, "low", 3), 1);
    CHECK_EQ(fs.last_pub, 1);
    CHECK(!fan_sync_on_local(&fs, 1100, 1));
    CHECK(fan_sync_on_local(&fs, 1200, 2));
}

static void test_connect_disarms_guard(void)
{
    SCENARIO("connect: the echo guard is disarmed after a reconnect");
    fan_sync_t fs;
    fan_sync_init(&fs);

    CHECK(fan_sync_on_local(&fs, 1000, 2));
    fan_sync_on_connect(&fs);
    /* Same value, inside what was the guard window: applied now */
    CHECK_EQ(msg(&fs, 1200, "medium", 3), 2);

    /* After a reconnect the same level publishes again */
    fan_sync_on_connect(&fs);
    CHECK(fan_sync_on_local(&fs, 1300, 2));
}

static void test_publish_at_time_zero(void)
{
    SCENARIO("native quirk preserved: a publish at t=0 does not arm the guard");
    fan_sync_t fs;
    fan_sync_init(&fs);

    CHECK(fan_sync_on_local(&fs, 0, 2));
    CHECK_EQ(msg(&fs, 500, "medium", 3), 2);
}

static void test_time_base_crosses_2_pow_32(void)
{
    SCENARIO("time base: echo guard across now = 2^32 ms (49.7 days)");
    const int64_t wrap = (int64_t)1 << 32;
    fan_sync_t fs;
    fan_sync_init(&fs);

    CHECK(fan_sync_on_local(&fs, wrap - 300, 1));
    CHECK_EQ(msg(&fs, wrap - 1, "low", 2), -1);
    CHECK_EQ(msg(&fs, wrap + 699, "low", 2), -1);
    CHECK_EQ(msg(&fs, wrap + 700, "low", 2), 1);

    /* Publish exactly at 2^32: a truncated uint32 time would read 0 and
     * disarm the guard */
    CHECK(fan_sync_on_local(&fs, wrap, 3));
    CHECK_EQ(msg(&fs, wrap + 500, "high", 1), -1);
    CHECK_EQ(msg(&fs, wrap + 1000, "high", 1), 3);
}

/* ====================================================================== */

int main(void)
{
    test_publish_on_change();
    test_echo_guard();
    test_echo_matching_ui_is_noop();
    test_rapid_stepping_jump_back();
    test_unknown_payload();
    test_retained_wins_on_connect();
    test_connect_disarms_guard();
    test_publish_at_time_zero();
    test_time_base_crosses_2_pow_32();

    return CHECK_SUMMARY();
}
