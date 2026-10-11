/*
 * Host-side tests for the loop -> UI status hand-off (a seqlock,
 * esphome/esp-nest-components/esp_nest/status_handoff.c).
 *
 * Build & run:  make -C test/host run
 */

#include <pthread.h>

#include "check.h"
#include "status_handoff.h"

static status_snapshot_t snap(bool wifi, uint32_t ip, bool mqtt)
{
    status_snapshot_t s = { wifi, ip, mqtt };
    return s;
}

static void test_nothing_published(void)
{
    SCENARIO("before the first publish there is nothing to take");
    status_handoff_t h = STATUS_HANDOFF_INIT;
    status_snapshot_t out = snap(true, 7, true);

    CHECK(!status_handoff_take(&h, &out));
    CHECK_EQ(out.ip_v4, 7); /* untouched */
}

static void test_applied_once(void)
{
    SCENARIO("a published state is taken exactly once");
    status_handoff_t h = STATUS_HANDOFF_INIT;
    status_snapshot_t out;

    status_handoff_publish(&h, snap(true, 0xC0A81401u, false));
    CHECK(status_handoff_take(&h, &out));
    CHECK(out.wifi_connected);
    CHECK_EQ(out.ip_v4, 0xC0A81401u);
    CHECK(!out.mqtt_connected);
    CHECK(!status_handoff_take(&h, &out));
    CHECK(!status_handoff_take(&h, &out));
}

static void test_latest_wins(void)
{
    SCENARIO("several publishes between two takes deliver only the latest");
    status_handoff_t h = STATUS_HANDOFF_INIT;
    status_snapshot_t out;

    status_handoff_publish(&h, snap(false, 0, false));
    status_handoff_publish(&h, snap(true, 11, false));
    status_handoff_publish(&h, snap(true, 11, true));
    CHECK(status_handoff_take(&h, &out));
    CHECK(out.wifi_connected && out.mqtt_connected);
    CHECK_EQ(out.ip_v4, 11);
    CHECK(!status_handoff_take(&h, &out));

    status_handoff_publish(&h, snap(false, 0, false));
    CHECK(status_handoff_take(&h, &out));
    CHECK(!out.wifi_connected && !out.mqtt_connected);
}

static void test_identical_republish_is_new(void)
{
    SCENARIO("a republish of identical values still moves the sequence");
    status_handoff_t h = STATUS_HANDOFF_INIT;
    status_snapshot_t out;

    status_handoff_publish(&h, snap(true, 5, true));
    CHECK(status_handoff_take(&h, &out));
    status_handoff_publish(&h, snap(true, 5, true));
    CHECK(status_handoff_take(&h, &out));
}

static void test_odd_seq_skipped(void)
{
    SCENARIO("a publish in progress (odd seq) is left for the next pass");
    status_handoff_t h = STATUS_HANDOFF_INIT;
    status_snapshot_t out = snap(false, 0, false);

    status_handoff_publish(&h, snap(true, 1, true));
    CHECK(status_handoff_take(&h, &out));
    CHECK_EQ(out.ip_v4, 1);

    /* The writer is half way through the next publish. */
    const uint32_t seq = atomic_load(&h.seq);
    atomic_store(&h.seq, seq + 1);
    atomic_store(&h.ip_v4, 2);
    CHECK(!status_handoff_take(&h, &out));
    CHECK_EQ(out.ip_v4, 1); /* out untouched, nothing consumed */
    CHECK_EQ(h.taken_seq, seq);

    /* ... and completes: the next pass sees it. */
    atomic_store(&h.seq, seq + 2);
    CHECK(status_handoff_take(&h, &out));
    CHECK_EQ(out.ip_v4, 2);
}

static void test_first_publish_in_progress(void)
{
    SCENARIO("an odd seq at the very first publish is skipped too");
    status_handoff_t h = STATUS_HANDOFF_INIT;
    status_snapshot_t out;

    atomic_store(&h.seq, 1);
    CHECK(!status_handoff_take(&h, &out));
    CHECK_EQ(h.taken_seq, 0);
    atomic_store(&h.seq, 2);
    CHECK(status_handoff_take(&h, &out));
}

/* The writer encodes the publish counter n in all three fields, so any
 * torn mix of two states breaks the invariant checked by the reader. */
#define STRESS_PUBLISHES 2000000u

static status_handoff_t g_stress = STATUS_HANDOFF_INIT;

static status_snapshot_t encode(uint32_t n)
{
    return snap(n & 1u, n, (n >> 1) & 1u);
}

static void *stress_writer(void *arg)
{
    (void)arg;
    for (uint32_t n = 1; n <= STRESS_PUBLISHES; n++)
        status_handoff_publish(&g_stress, encode(n));
    return NULL;
}

static void test_stress_no_torn_reads(void)
{
    SCENARIO("two threads: every taken state is consistent and monotonic");
    pthread_t writer;
    status_snapshot_t out;
    uint32_t last = 0;
    long torn = 0, backwards = 0, takes = 0;

    pthread_create(&writer, NULL, stress_writer, NULL);
    while (last != STRESS_PUBLISHES) {
        if (!status_handoff_take(&g_stress, &out))
            continue;
        takes++;
        const status_snapshot_t want = encode(out.ip_v4);
        if (out.wifi_connected != want.wifi_connected || out.mqtt_connected != want.mqtt_connected)
            torn++;
        if (out.ip_v4 <= last)
            backwards++;
        last = out.ip_v4;
    }
    pthread_join(writer, NULL);

    CHECK_EQ(torn, 0);
    CHECK_EQ(backwards, 0);
    CHECK(takes > 0);
    CHECK_EQ(last, STRESS_PUBLISHES);
}

int main(void)
{
    test_nothing_published();
    test_applied_once();
    test_latest_wins();
    test_identical_republish_is_new();
    test_odd_seq_skipped();
    test_first_publish_in_progress();
    test_stress_no_torn_reads();
    return CHECK_SUMMARY();
}
