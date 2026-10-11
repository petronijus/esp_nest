/*
 * Host-side tests for the blinds sync state machine
 * (esphome/esp-nest-components/esp_nest/blinds_sync.c).
 *
 * Build & run:  make -C test/host run
 *
 * The expectations are the native v2.0.1 behaviour (main/ha_mqtt.c):
 * 500 ms debounce, OPEN/CLOSE group broadcast vs. position x3, 2.5 s
 * block window, truncating member average, queued across disconnects.
 */

#include "check.h"
#include "blinds_sync.h"
#include "mqtt_payload.h"

#define A 0
#define B 1

/* Member report, parsed exactly like the bridge does */
static int member(blinds_sync_t *bs, int64_t now, int t, int m, const char *payload)
{
    int pos;
    if (!mqtt_payload_parse_int(payload, strlen(payload), &pos))
        return -2;   /* the bridge drops it before blinds_sync sees it */
    return blinds_sync_on_member(bs, now, t, m, pos, NULL);
}

/* ====================================================================== */

static void test_conversions(void)
{
    SCENARIO("conversion: step <-> position, endpoints, clamping, roundtrip");
    static const int pos[7] = {100, 83, 67, 50, 33, 17, 0};
    for (int s = 0; s <= BLINDS_SYNC_MAX_STEP; s++) {
        CHECK_EQ(blinds_sync_step_to_pos(s), pos[s]);
        CHECK_EQ(blinds_sync_pos_to_step(blinds_sync_step_to_pos(s)), s);
    }
    CHECK_EQ(blinds_sync_pos_to_step(100), 0);
    CHECK_EQ(blinds_sync_pos_to_step(0), 6);
    CHECK_EQ(blinds_sync_pos_to_step(50), 3);
    CHECK_EQ(blinds_sync_pos_to_step(92), 0);    /* (8*6+50)/100 = 0 */
    CHECK_EQ(blinds_sync_pos_to_step(91), 1);    /* (9*6+50)/100 = 1 */
    CHECK_EQ(blinds_sync_pos_to_step(-10), 6);   /* clamp */
    CHECK_EQ(blinds_sync_pos_to_step(150), 0);   /* clamp */
}

static void test_settle_and_flush(void)
{
    SCENARIO("local: flushed after >= 500 ms of settle, exactly once");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    CHECK(blinds_sync_on_local(&bs, 1000, A, 3));
    CHECK_EQ(blinds_sync_tick(&bs, 1000, true, out), 0);
    CHECK_EQ(blinds_sync_tick(&bs, 1499, true, out), 0);
    CHECK_EQ(blinds_sync_tick(&bs, 1500, true, out), 1);
    CHECK_EQ(out[0].target, A);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_POSITION);
    CHECK_EQ(out[0].position, 50);
    CHECK_EQ(bs.block_until_ms[A], 1500 + BLINDS_SYNC_BLOCK_MS);

    CHECK_EQ(blinds_sync_tick(&bs, 1600, true, out), 0);
    CHECK_EQ(blinds_sync_tick(&bs, 9000, true, out), 0);
}

static void test_debounce_newest_step(void)
{
    SCENARIO("local: rapid steps restart the settle timer, only the newest is sent");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    CHECK(blinds_sync_on_local(&bs, 1000, B, 1));
    CHECK(blinds_sync_on_local(&bs, 1200, B, 2));
    CHECK(blinds_sync_on_local(&bs, 1400, B, 4));
    CHECK_EQ(blinds_sync_tick(&bs, 1600, true, out), 0);   /* 600 since first */
    CHECK_EQ(blinds_sync_tick(&bs, 1899, true, out), 0);
    CHECK_EQ(blinds_sync_tick(&bs, 1900, true, out), 1);
    CHECK_EQ(out[0].target, B);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_POSITION);
    CHECK_EQ(out[0].position, 33);
}

static void test_open_close_vs_position(void)
{
    SCENARIO("flush: step 0 -> OPEN, step 6 -> CLOSE (group), 1-5 -> position (x3 members)");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];
    char buf[MQTT_PAYLOAD_OUT_SIZE];

    CHECK(blinds_sync_on_local(&bs, 0, A, 0));
    CHECK_EQ(blinds_sync_tick(&bs, 500, true, out), 1);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_OPEN);
    mqtt_payload_blinds_command(&out[0], buf, sizeof(buf));
    CHECK_STR(buf, "OPEN");

    CHECK(blinds_sync_on_local(&bs, 1000, A, 6));
    CHECK_EQ(blinds_sync_tick(&bs, 1500, true, out), 1);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_CLOSE);
    mqtt_payload_blinds_command(&out[0], buf, sizeof(buf));
    CHECK_STR(buf, "CLOSE");

    static const char *const expect[] = {"", "83", "67", "50", "33", "17"};
    for (int s = 1; s <= 5; s++) {
        int64_t t = 2000 + s * 1000;
        CHECK(blinds_sync_on_local(&bs, t, A, s));
        CHECK_EQ(blinds_sync_tick(&bs, t + 500, true, out), 1);
        CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_POSITION);
        mqtt_payload_blinds_command(&out[0], buf, sizeof(buf));
        CHECK_STR(buf, expect[s]);
    }
}

static void test_both_groups(void)
{
    SCENARIO("flush: both groups in one tick, in group order, independently settled");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    CHECK(blinds_sync_on_local(&bs, 1000, B, 6));
    CHECK(blinds_sync_on_local(&bs, 1100, A, 2));
    CHECK_EQ(blinds_sync_tick(&bs, 1550, true, out), 1);   /* only B settled */
    CHECK_EQ(out[0].target, B);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_CLOSE);
    CHECK_EQ(blinds_sync_tick(&bs, 1600, true, out), 1);
    CHECK_EQ(out[0].target, A);

    CHECK(blinds_sync_on_local(&bs, 3000, B, 0));
    CHECK(blinds_sync_on_local(&bs, 3000, A, 5));
    CHECK_EQ(blinds_sync_tick(&bs, 3500, true, out), 2);
    CHECK_EQ(out[0].target, A);
    CHECK_EQ(out[0].position, 17);
    CHECK_EQ(out[1].target, B);
    CHECK_EQ(out[1].kind, BLINDS_SYNC_CMD_OPEN);
}

static void test_queued_across_disconnect(void)
{
    SCENARIO("disconnect: a queued step is kept, not lost, and sent after reconnect");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    /* Queuing has no connected check */
    CHECK(blinds_sync_on_local(&bs, 1000, A, 4));
    CHECK_EQ(blinds_sync_tick(&bs, 2000, false, out), 0);
    CHECK_EQ(blinds_sync_tick(&bs, 60000, false, out), 0);
    CHECK_EQ(bs.pending_step[A], 4);
    CHECK_EQ(bs.block_until_ms[A], 1000 + BLINDS_SYNC_BLOCK_MS);

    /* Reports while queued are stored but never shown */
    CHECK_EQ(member(&bs, 60500, A, 0, "100"), -1);
    CHECK_EQ(bs.member_pos[A][0], 100);

    CHECK_EQ(blinds_sync_tick(&bs, 61000, true, out), 1);
    CHECK_EQ(out[0].position, 33);
    CHECK_EQ(bs.pending_step[A], -1);
    CHECK_EQ(bs.block_until_ms[A], 61000 + BLINDS_SYNC_BLOCK_MS);
}

static void test_block_window(void)
{
    SCENARIO("inbound: positions stored while blocked, shown after the 2.5 s window");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    CHECK(blinds_sync_on_local(&bs, 1000, A, 5));
    CHECK_EQ(blinds_sync_tick(&bs, 1500, true, out), 1);

    /* Travel reports inside the window (until 4000) */
    CHECK_EQ(member(&bs, 2000, A, 0, "60"), -1);
    CHECK_EQ(member(&bs, 2500, A, 1, "40"), -1);
    CHECK_EQ(member(&bs, 3999, A, 2, "20"), -1);
    CHECK_EQ(bs.member_pos[A][0], 60);
    CHECK_EQ(bs.member_pos[A][1], 40);
    CHECK_EQ(bs.member_pos[A][2], 20);

    /* Group B is not blocked by group A's window */
    CHECK_EQ(member(&bs, 2000, B, 0, "0"), 6);

    /* First report after the window uses the stored positions:
     * (17 + 40 + 20) / 3 = 25 -> step ((75*6)+50)/100 = 5 */
    int avg = -1;
    CHECK_EQ(blinds_sync_on_member(&bs, 4000, A, 0, 17, &avg), 5);
    CHECK_EQ(avg, 25);

    /* No resync after the window: nothing is ever emitted by itself */
    for (int64_t t = 4000; t < 20000; t += 100)
        CHECK_EQ(blinds_sync_tick(&bs, t, true, out), 0);
}

static void test_local_opens_block(void)
{
    SCENARIO("local: queuing opens the block window too (pending also blocks)");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    /* Not blocked initially: retained reports apply at once */
    CHECK_EQ(member(&bs, 100, A, 0, "100"), 0);

    CHECK(blinds_sync_on_local(&bs, 1000, A, 3));
    CHECK_EQ(bs.block_until_ms[A], 3500);
    CHECK_EQ(member(&bs, 1200, A, 0, "100"), -1);

    /* Flushed at 1500: window extended to 4000 */
    CHECK_EQ(blinds_sync_tick(&bs, 1500, true, out), 1);
    CHECK_EQ(member(&bs, 3600, A, 0, "50"), -1);
    CHECK_EQ(member(&bs, 4000, A, 0, "50"), 3);
}

static void test_average(void)
{
    SCENARIO("inbound: truncating average of known members, negatives ignored");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    int avg = -1;

    /* Only one member known: that one */
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, 1, 60, &avg), 2);
    CHECK_EQ(avg, 60);

    /* Negative positions are stored but count as unknown */
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, 0, -5, &avg), 2);
    CHECK_EQ(avg, 60);
    CHECK_EQ(bs.member_pos[A][0], -5);

    /* Truncation: (50 + 51 + 51) / 3 = 50 */
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, 0, 50, NULL), 3);
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, 1, 51, NULL), 3);
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, 2, 51, &avg), 3);
    CHECK_EQ(avg, 50);

    /* (0 + 0 + 2) / 3 = 0 -> fully closed */
    CHECK_EQ(blinds_sync_on_member(&bs, 100, B, 0, 0, NULL), 6);
    CHECK_EQ(blinds_sync_on_member(&bs, 100, B, 1, 0, NULL), 6);
    CHECK_EQ(blinds_sync_on_member(&bs, 100, B, 2, 2, &avg), 6);
    CHECK_EQ(avg, 0);

    /* Out-of-range positions are averaged, then clamped by pos_to_step */
    CHECK_EQ(blinds_sync_on_member(&bs, 100, B, 0, 300, NULL), 0);   /* 302/3 = 100 */
    CHECK_EQ(blinds_sync_on_member(&bs, 100, B, 0, 2147483647, &avg), 0);
    CHECK_EQ(avg, 715827883);   /* (INT_MAX + 0 + 2) / 3, no overflow */

    /* No member known at all: nothing to show */
    blinds_sync_init(&bs);
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, 0, -1, &avg), -1);
    CHECK_EQ(member(&bs, 100, A, 0, "-3"), -1);
}

static void test_payload_rejects(void)
{
    SCENARIO("inbound: non-numeric payloads never reach blinds_sync");
    blinds_sync_t bs;
    blinds_sync_init(&bs);

    CHECK_EQ(member(&bs, 100, A, 0, "50"), 3);
    CHECK_EQ(member(&bs, 200, A, 0, "unavailable"), -2);
    CHECK_EQ(bs.member_pos[A][0], 50);
    CHECK_EQ(member(&bs, 300, A, 0, "33.9"), 4);
}

static void test_invalid_arguments(void)
{
    SCENARIO("guards: invalid target / member / step change nothing");
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    CHECK(!blinds_sync_on_local(&bs, 1000, -1, 3));
    CHECK(!blinds_sync_on_local(&bs, 1000, 2, 3));
    CHECK(!blinds_sync_on_local(&bs, 1000, A, -1));
    CHECK(!blinds_sync_on_local(&bs, 1000, A, 7));
    CHECK_EQ(bs.pending_step[A], -1);
    CHECK_EQ(bs.block_until_ms[A], 0);
    CHECK_EQ(blinds_sync_tick(&bs, 5000, true, out), 0);

    CHECK_EQ(blinds_sync_on_member(&bs, 100, 2, 0, 50, NULL), -1);
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, 3, 50, NULL), -1);
    CHECK_EQ(blinds_sync_on_member(&bs, 100, A, -1, 50, NULL), -1);
    for (int t = 0; t < BLINDS_SYNC_GROUPS; t++)
        for (int m = 0; m < BLINDS_SYNC_MEMBERS; m++)
            CHECK_EQ(bs.member_pos[t][m], -1);
}

static void test_time_base_crosses_2_pow_32(void)
{
    SCENARIO("time base: settle and block windows straddling now = 2^32 ms (49.7 days)");
    /* The ESPHome bridge feeds int64 millis_64(); a uint32 millis() would
     * wrap here. One monotonic timeline: both block windows open before
     * 2^32 and end after it, so a window end truncated to uint32 (a small
     * number) would read as already elapsed for the reports just before the
     * boundary and show the travelling covers. */
    const int64_t wrap = (int64_t)1 << 32;
    blinds_sync_t bs;
    blinds_sync_init(&bs);
    blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS];

    /* Group A: flushed 1 s before the boundary, window ends at wrap + 1500 */
    CHECK(blinds_sync_on_local(&bs, wrap - 1600, A, 2));
    CHECK_EQ(blinds_sync_tick(&bs, wrap - 1101, true, out), 0);
    CHECK_EQ(blinds_sync_tick(&bs, wrap - 1000, true, out), 1);
    CHECK_EQ(out[0].target, A);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_POSITION);
    CHECK_EQ(out[0].position, 67);
    CHECK_EQ(bs.block_until_ms[A], wrap + 1500);

    /* Group B: queued across a disconnect, flushed on reconnect 300 ms
     * before the boundary, window ends at wrap + 2200 */
    CHECK(blinds_sync_on_local(&bs, wrap - 900, B, 5));
    CHECK_EQ(member(&bs, wrap - 500, A, 0, "67"), -1);   /* A: inside window */
    CHECK_EQ(blinds_sync_tick(&bs, wrap - 400, false, out), 0);
    CHECK_EQ(bs.pending_step[B], 5);
    CHECK_EQ(member(&bs, wrap - 350, B, 0, "17"), -1);   /* B: still queued */
    CHECK_EQ(blinds_sync_tick(&bs, wrap - 300, true, out), 1);
    CHECK_EQ(out[0].target, B);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_POSITION);
    CHECK_EQ(out[0].position, 17);
    CHECK_EQ(bs.block_until_ms[B], wrap + 2200);
    CHECK_EQ(member(&bs, wrap - 250, B, 0, "17"), -1);   /* B: inside window */

    /* Group B again: a step queued 200 ms before the boundary settles
     * 300 ms after it (a truncated change time would flush it at once) */
    CHECK(blinds_sync_on_local(&bs, wrap - 200, B, 6));
    CHECK_EQ(blinds_sync_tick(&bs, wrap - 1, true, out), 0);
    CHECK_EQ(member(&bs, wrap, A, 0, "67"), -1);
    CHECK_EQ(member(&bs, wrap + 100, B, 0, "17"), -1);   /* B: queued */
    CHECK_EQ(blinds_sync_tick(&bs, wrap + 299, true, out), 0);
    CHECK_EQ(blinds_sync_tick(&bs, wrap + 300, true, out), 1);
    CHECK_EQ(out[0].target, B);
    CHECK_EQ(out[0].kind, BLINDS_SYNC_CMD_CLOSE);
    CHECK_EQ(bs.block_until_ms[B], wrap + 2800);

    /* A's window: blocked up to its last millisecond, then applied */
    CHECK_EQ(member(&bs, wrap + 1499, A, 0, "67"), -1);
    CHECK_EQ(member(&bs, wrap + 1500, A, 0, "67"), 2);

    /* B's window from the CLOSE flush */
    CHECK_EQ(member(&bs, wrap + 2799, B, 0, "0"), -1);
    CHECK_EQ(member(&bs, wrap + 2800, B, 0, "0"), 6);
}

/* ====================================================================== */

int main(void)
{
    test_conversions();
    test_settle_and_flush();
    test_debounce_newest_step();
    test_open_close_vs_position();
    test_both_groups();
    test_queued_across_disconnect();
    test_block_window();
    test_local_opens_block();
    test_average();
    test_payload_rejects();
    test_invalid_arguments();
    test_time_base_crosses_2_pow_32();

    return CHECK_SUMMARY();
}
