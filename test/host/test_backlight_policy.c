/*
 * Host-side tests for the backlight policy
 * (esphome/esp-nest-components/esp_nest/backlight_policy.c).
 *
 * Build & run:  make -C test/host run
 *
 * The expectations are the native v2.0.1 behaviour (main/screen.c): idle
 * fade to the dim level after the timeout, instant wake on activity,
 * activity and the idle check mutually exclusive within one indev read,
 * timeout 0 = never dim, and the setter rules (active level stored only
 * while dimmed, dim level applied at once while dimmed, a timeout change
 * never restarts the idle timer).
 */

#include "check.h"
#include "backlight_policy.h"

#define NONE BACKLIGHT_POLICY_ACT_NONE
#define SET  BACKLIGHT_POLICY_ACT_SET
#define FADE BACKLIGHT_POLICY_ACT_FADE

/* Native boot state at time `now` (100 / 20 / 30 s) */
static void boot(backlight_policy_t *bp, int64_t now)
{
    int pct = -1;
    CHECK_EQ(backlight_policy_init(bp, now,
                                   BACKLIGHT_POLICY_DEFAULT_ACTIVE_PCT,
                                   BACKLIGHT_POLICY_DEFAULT_DIM_PCT,
                                   BACKLIGHT_POLICY_DEFAULT_TIMEOUT_S, &pct),
             SET);
    CHECK_EQ(pct, 100);
}

/* Wrappers returning the action and storing the level in *pct; *pct is
 * poisoned first so a NONE that writes the level is caught too */
static int indev_read(backlight_policy_t *bp, int64_t now, bool act, int *pct)
{
    *pct = -999;
    return backlight_policy_update(bp, now, act, pct);
}

/* An indev read without activity (the idle check) */
static int tick(backlight_policy_t *bp, int64_t now, int *pct)
{
    return indev_read(bp, now, false, pct);
}

/* An indev read with activity (encoder count or button held) */
static int activity(backlight_policy_t *bp, int64_t now, int *pct)
{
    return indev_read(bp, now, true, pct);
}

static int set_active(backlight_policy_t *bp, int value, int *pct)
{
    *pct = -999;
    return backlight_policy_set_active(bp, value, pct);
}

static int set_dim(backlight_policy_t *bp, int value, int *pct)
{
    *pct = -999;
    return backlight_policy_set_dim(bp, value, pct);
}

/* ====================================================================== */

static void test_init(void)
{
    SCENARIO("init: native defaults 100 / 20 / 30 s, not dimmed, SET active");
    backlight_policy_t bp;
    boot(&bp, 5000);
    CHECK_EQ(bp.active_pct, 100);
    CHECK_EQ(bp.dim_pct, 20);
    CHECK_EQ(bp.timeout_ms, 30000);
    CHECK(!bp.dimmed);
    CHECK_EQ(bp.last_activity_ms, 5000);

    /* Restored settings are clamped like the setters */
    int pct = -1;
    CHECK_EQ(backlight_policy_init(&bp, 0, 140, -3, 900, &pct), SET);
    CHECK_EQ(pct, 100);
    CHECK_EQ(bp.dim_pct, 0);
    CHECK_EQ(bp.timeout_ms, 600000);
    CHECK_EQ(backlight_policy_init(&bp, 0, 45, 5, -1, &pct), SET);
    CHECK_EQ(pct, 45);
    CHECK_EQ(bp.timeout_ms, 0);
}

static void test_idle_dim(void)
{
    SCENARIO("idle: fade to the dim level once the timeout has elapsed, once");
    backlight_policy_t bp;
    int pct;
    boot(&bp, 1000);

    CHECK_EQ(tick(&bp, 1000, &pct), NONE);
    CHECK_EQ(pct, -999);
    CHECK_EQ(tick(&bp, 30999, &pct), NONE);       /* 29 999 ms idle */
    CHECK(!bp.dimmed);

    CHECK_EQ(tick(&bp, 31000, &pct), FADE);       /* elapsed == timeout */
    CHECK_EQ(pct, 20);
    CHECK(bp.dimmed);

    /* Already dimmed: no further action however long it stays idle */
    CHECK_EQ(tick(&bp, 31033, &pct), NONE);
    CHECK_EQ(tick(&bp, 999999, &pct), NONE);
}

static void test_wake_on_activity(void)
{
    SCENARIO("activity: instant SET to the active level when dimmed, timer restart");
    backlight_policy_t bp;
    int pct;
    boot(&bp, 0);

    CHECK_EQ(tick(&bp, 30000, &pct), FADE);
    CHECK_EQ(activity(&bp, 30500, &pct), SET);    /* mid-fade: instant */
    CHECK_EQ(pct, 100);
    CHECK(!bp.dimmed);
    CHECK_EQ(bp.last_activity_ms, 30500);

    /* Activity while not dimmed: no action, but the timer restarts */
    CHECK_EQ(activity(&bp, 50000, &pct), NONE);
    CHECK_EQ(pct, -999);
    CHECK_EQ(tick(&bp, 60500, &pct), NONE);       /* old deadline */
    CHECK_EQ(tick(&bp, 79999, &pct), NONE);
    CHECK_EQ(tick(&bp, 80000, &pct), FADE);
    CHECK_EQ(pct, 20);

    /* Continuous activity (every 33 ms indev read) never dims */
    boot(&bp, 0);
    int actions = 0;
    for (int64_t t = 0; t <= 120000; t += 33)
        actions += activity(&bp, t, &pct) != NONE;
    CHECK_EQ(actions, 0);
    CHECK(!bp.dimmed);
}

static void test_activity_suppresses_idle_check(void)
{
    SCENARIO("one read: activity suppresses the idle check, never FADE");
    backlight_policy_t bp;
    int pct;

    /* Not dimmed, the timeout elapsed between two reads, the next read has
     * activity: native's if/else never starts the fade -> NONE, not
     * dimmed, the idle timer restarts from this read */
    boot(&bp, 0);
    CHECK_EQ(tick(&bp, 29990, &pct), NONE);
    CHECK_EQ(activity(&bp, 30023, &pct), NONE);
    CHECK_EQ(pct, -999);
    CHECK(!bp.dimmed);
    CHECK_EQ(bp.last_activity_ms, 30023);
    CHECK_EQ(tick(&bp, 60022, &pct), NONE);
    CHECK_EQ(tick(&bp, 60023, &pct), FADE);
    CHECK_EQ(pct, 20);

    /* Long idle with no reads at all (first read after a long gap) that
     * carries activity: still NONE, never a fade followed by a wake */
    boot(&bp, 0);
    CHECK_EQ(activity(&bp, 600000, &pct), NONE);
    CHECK(!bp.dimmed);

    /* Already dimmed, a read with activity: SET to the active level only */
    boot(&bp, 0);
    CHECK_EQ(tick(&bp, 30000, &pct), FADE);
    CHECK_EQ(activity(&bp, 90000, &pct), SET);
    CHECK_EQ(pct, 100);
    CHECK(!bp.dimmed);

    /* Activity exactly on every idle deadline: each such read is NONE and
     * pushes the deadline on, so the screen never dims */
    boot(&bp, 0);
    for (int64_t t = 30000; t <= 300000; t += 30000) {
        CHECK_EQ(activity(&bp, t, &pct), NONE);
        CHECK(!bp.dimmed);
    }
}

static void test_timeout_zero_never_dims(void)
{
    SCENARIO("timeout 0 means never dim");
    backlight_policy_t bp;
    int pct;
    boot(&bp, 0);
    backlight_policy_set_timeout(&bp, 0);
    CHECK_EQ(bp.timeout_ms, 0);

    CHECK_EQ(tick(&bp, 30000, &pct), NONE);
    CHECK_EQ(tick(&bp, (int64_t)7 * 24 * 3600 * 1000, &pct), NONE);
    CHECK(!bp.dimmed);

    /* Set to 0 while dimmed: stays dimmed until activity (native) */
    boot(&bp, 0);
    CHECK_EQ(tick(&bp, 30000, &pct), FADE);
    backlight_policy_set_timeout(&bp, 0);
    CHECK(bp.dimmed);
    CHECK_EQ(tick(&bp, 40000, &pct), NONE);
    CHECK_EQ(activity(&bp, 50000, &pct), SET);
    CHECK_EQ(pct, 100);
    CHECK_EQ(tick(&bp, 999999, &pct), NONE);
}

static void test_timeout_change_keeps_timer(void)
{
    SCENARIO("timeout change never restarts the idle timer");
    backlight_policy_t bp;
    int pct;

    /* Idle for 100 s with dimming off, then 30 s enabled: dims at once */
    boot(&bp, 0);
    backlight_policy_set_timeout(&bp, 0);
    CHECK_EQ(tick(&bp, 100000, &pct), NONE);
    backlight_policy_set_timeout(&bp, 30);
    CHECK_EQ(bp.last_activity_ms, 0);
    CHECK_EQ(tick(&bp, 100033, &pct), FADE);
    CHECK_EQ(pct, 20);

    /* Longer timeout at 25 s: deadline counts from the last activity */
    boot(&bp, 0);
    CHECK_EQ(tick(&bp, 25000, &pct), NONE);
    backlight_policy_set_timeout(&bp, 60);
    CHECK_EQ(tick(&bp, 30000, &pct), NONE);
    CHECK_EQ(tick(&bp, 59999, &pct), NONE);
    CHECK_EQ(tick(&bp, 60000, &pct), FADE);

    /* Shorter timeout below the elapsed idle time: next tick dims */
    boot(&bp, 0);
    CHECK_EQ(tick(&bp, 20000, &pct), NONE);
    backlight_policy_set_timeout(&bp, 10);
    CHECK_EQ(tick(&bp, 20033, &pct), FADE);

    /* Change while dimmed: nothing happens, still dimmed */
    boot(&bp, 0);
    CHECK_EQ(tick(&bp, 30000, &pct), FADE);
    backlight_policy_set_timeout(&bp, 300);
    CHECK(bp.dimmed);
    CHECK_EQ(tick(&bp, 30033, &pct), NONE);
}

static void test_set_active(void)
{
    SCENARIO("active level: SET now when not dimmed, stored only while dimmed");
    backlight_policy_t bp;
    int pct;
    boot(&bp, 0);

    CHECK_EQ(set_active(&bp, 60, &pct), SET);
    CHECK_EQ(pct, 60);
    CHECK_EQ(bp.active_pct, 60);

    /* While dimmed: stored, the screen stays at the dim level */
    CHECK_EQ(tick(&bp, 30000, &pct), FADE);
    CHECK_EQ(set_active(&bp, 80, &pct), NONE);
    CHECK_EQ(pct, -999);
    CHECK_EQ(bp.active_pct, 80);
    CHECK(bp.dimmed);

    /* ... and applied on wake */
    CHECK_EQ(activity(&bp, 31000, &pct), SET);
    CHECK_EQ(pct, 80);

    /* Setting does not count as activity */
    CHECK_EQ(bp.last_activity_ms, 31000);
    CHECK_EQ(set_active(&bp, 90, &pct), SET);
    CHECK_EQ(bp.last_activity_ms, 31000);
    CHECK_EQ(tick(&bp, 61000, &pct), FADE);
}

static void test_set_dim(void)
{
    SCENARIO("dim level: stored while active, SET at once while dimmed");
    backlight_policy_t bp;
    int pct;
    boot(&bp, 0);

    CHECK_EQ(set_dim(&bp, 35, &pct), NONE);
    CHECK_EQ(pct, -999);
    CHECK_EQ(bp.dim_pct, 35);

    /* The idle fade uses the new level */
    CHECK_EQ(tick(&bp, 30000, &pct), FADE);
    CHECK_EQ(pct, 35);

    /* While dimmed (also mid-fade): applied instantly */
    CHECK_EQ(set_dim(&bp, 5, &pct), SET);
    CHECK_EQ(pct, 5);
    CHECK(bp.dimmed);

    /* Waking returns to active; the dim level is kept for next time */
    CHECK_EQ(activity(&bp, 40000, &pct), SET);
    CHECK_EQ(pct, 100);
    CHECK_EQ(tick(&bp, 70000, &pct), FADE);
    CHECK_EQ(pct, 5);
}

static void test_clamping(void)
{
    SCENARIO("clamping: levels 0..100 %, timeout 0..600 s");
    backlight_policy_t bp;
    int pct;
    boot(&bp, 0);

    CHECK_EQ(set_active(&bp, 150, &pct), SET);
    CHECK_EQ(pct, 100);
    CHECK_EQ(set_active(&bp, -20, &pct), SET);
    CHECK_EQ(pct, 0);
    CHECK_EQ(bp.active_pct, 0);
    CHECK_EQ(set_active(&bp, 0, &pct), SET);      /* 0 is a valid level */
    CHECK_EQ(pct, 0);

    CHECK_EQ(set_dim(&bp, 101, &pct), NONE);
    CHECK_EQ(bp.dim_pct, 100);
    CHECK_EQ(set_dim(&bp, -1, &pct), NONE);
    CHECK_EQ(bp.dim_pct, 0);

    backlight_policy_set_timeout(&bp, 601);
    CHECK_EQ(bp.timeout_ms, 600000);
    backlight_policy_set_timeout(&bp, 600);
    CHECK_EQ(bp.timeout_ms, 600000);
    backlight_policy_set_timeout(&bp, -5);
    CHECK_EQ(bp.timeout_ms, 0);
    backlight_policy_set_timeout(&bp, 1);
    CHECK_EQ(bp.timeout_ms, 1000);

    /* Dim level 0 fades the screen fully off; wake restores it */
    boot(&bp, 0);
    CHECK_EQ(set_dim(&bp, 0, &pct), NONE);
    CHECK_EQ(tick(&bp, 30000, &pct), FADE);
    CHECK_EQ(pct, 0);
    CHECK_EQ(activity(&bp, 30100, &pct), SET);
    CHECK_EQ(pct, 100);
}

static void test_time_base_crosses_2_pow_32(void)
{
    SCENARIO("time base: idle timer across now = 2^32 ms (49.7 days)");
    const int64_t wrap = (int64_t)1 << 32;
    backlight_policy_t bp;
    int pct;

    /* Activity 10 s before the boundary, 30 s timeout: dims 20 s after it.
     * A uint32 time base would see now < last_activity past the boundary */
    boot(&bp, 0);
    CHECK_EQ(activity(&bp, wrap - 10000, &pct), NONE);
    CHECK_EQ(tick(&bp, wrap - 1, &pct), NONE);
    CHECK_EQ(tick(&bp, wrap, &pct), NONE);
    CHECK_EQ(tick(&bp, wrap + 19999, &pct), NONE);
    CHECK_EQ(tick(&bp, wrap + 20000, &pct), FADE);
    CHECK_EQ(pct, 20);
    CHECK_EQ(activity(&bp, wrap + 25000, &pct), SET);
    CHECK_EQ(tick(&bp, wrap + 54999, &pct), NONE);
    CHECK_EQ(tick(&bp, wrap + 55000, &pct), FADE);

    /* Booted just before the boundary with the maximum timeout */
    boot(&bp, wrap - 1);
    backlight_policy_set_timeout(&bp, 600);
    CHECK_EQ(tick(&bp, wrap + 599998, &pct), NONE);
    CHECK_EQ(tick(&bp, wrap + 599999, &pct), FADE);

    /* Dimming off and untouched for more than 2^32 ms, then enabled: the
     * idle time is 49.7 days + 1 s, so it dims on the next tick. Wrap-safe
     * uint32 subtraction would see 1 s idle and keep the screen lit */
    boot(&bp, 0);
    backlight_policy_set_timeout(&bp, 0);
    CHECK_EQ(tick(&bp, wrap + 1000, &pct), NONE);
    backlight_policy_set_timeout(&bp, 30);
    CHECK_EQ(tick(&bp, wrap + 1000, &pct), FADE);
    CHECK_EQ(pct, 20);
}

/* ====================================================================== */

int main(void)
{
    test_init();
    test_idle_dim();
    test_wake_on_activity();
    test_activity_suppresses_idle_check();
    test_timeout_zero_never_dims();
    test_timeout_change_keeps_timer();
    test_set_active();
    test_set_dim();
    test_clamping();
    test_time_base_crosses_2_pow_32();

    return CHECK_SUMMARY();
}
