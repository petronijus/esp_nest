/*
 * Host-side tests for the input state machine
 * (esphome/esp-nest-components/esp_nest/input_fsm.c).
 *
 * Build & run:  make -C test/host run
 *
 * The expectations are the native v2.0.1 behaviour (main/screen.c
 * encoder_read_cb): menu +/-3 accumulation with a 600 ms cooldown, raw
 * diff in detail mode, short press on release (>50 ms, no long press),
 * long press once at >1000 ms, release resets the accumulator and starts
 * a 300 ms cooldown.
 */

#include "check.h"
#include "input_fsm.h"

#define MENU   false
#define DETAIL true

static input_fsm_out_t upd(input_fsm_t *fsm, int64_t now, int diff,
                           bool pressed, bool detail)
{
    input_fsm_out_t out;
    /* Poison the output: the update must fill every field */
    memset(&out, 0x5a, sizeof out);
    input_fsm_update(fsm, now, diff, pressed, detail, &out);
    return out;
}

/* True when the update produced no action at all (activity aside) */
static bool no_action(input_fsm_out_t o)
{
    return o.navigate == 0 && !o.press && !o.long_press;
}

/* ====================================================================== */

static void test_init(void)
{
    SCENARIO("init: idle state, no cooldown, button released");
    input_fsm_t fsm;
    memset(&fsm, 0x5a, sizeof fsm);
    input_fsm_init(&fsm);
    CHECK_EQ(fsm.accum, 0);
    CHECK_EQ(fsm.cooldown_until_ms, 0);
    CHECK(!fsm.was_pressed);
    CHECK(!fsm.long_press_fired);

    /* Nothing happening: no action, no activity */
    input_fsm_out_t o = upd(&fsm, 0, 0, false, MENU);
    CHECK(no_action(o));
    CHECK(!o.activity);
    /* A count right at boot is not inside any cooldown */
    o = upd(&fsm, 0, 3, false, MENU);
    CHECK_EQ(o.navigate, 1);
}

static void test_menu_accumulation(void)
{
    SCENARIO("menu: 3 counts per page step, accumulated across reads, both directions");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK_EQ(upd(&fsm, 1000, 1, false, MENU).navigate, 0);
    CHECK_EQ(fsm.accum, 1);
    CHECK_EQ(upd(&fsm, 1033, 1, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 1066, 1, false, MENU).navigate, 1);
    CHECK_EQ(fsm.accum, 0);
    CHECK_EQ(fsm.cooldown_until_ms, 1066 + 600);

    /* Counter-clockwise after the cooldown */
    CHECK_EQ(upd(&fsm, 2000, -2, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 2033, -1, false, MENU).navigate, -1);
    CHECK_EQ(fsm.accum, 0);

    /* Direction changes cancel out: +2 -1 +2 = 3 */
    CHECK_EQ(upd(&fsm, 3000, 2, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 3033, -1, false, MENU).navigate, 0);
    CHECK_EQ(fsm.accum, 1);
    CHECK_EQ(upd(&fsm, 3066, 2, false, MENU).navigate, 1);

    /* Zero diff never touches the accumulator */
    CHECK_EQ(upd(&fsm, 4000, 2, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 4033, 0, false, MENU).navigate, 0);
    CHECK_EQ(fsm.accum, 2);
}

static void test_menu_surplus_dropped(void)
{
    SCENARIO("menu: a fast spin is one page step, surplus counts are dropped");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK_EQ(upd(&fsm, 1000, 7, false, MENU).navigate, 1);
    CHECK_EQ(fsm.accum, 0);
    CHECK_EQ(upd(&fsm, 2000, -11, false, MENU).navigate, -1);
    CHECK_EQ(fsm.accum, 0);

    /* 2 accumulated + 4 = 6: still exactly one step */
    CHECK_EQ(upd(&fsm, 3000, 2, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 3033, 4, false, MENU).navigate, 1);
    CHECK_EQ(fsm.accum, 0);
}

static void test_menu_cooldown(void)
{
    SCENARIO("menu: 600 ms cooldown discards counts and keeps the accumulator empty");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK_EQ(upd(&fsm, 1000, 3, false, MENU).navigate, 1);

    /* Inside the cooldown: discarded, nothing accumulates */
    CHECK_EQ(upd(&fsm, 1001, 5, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 1300, 2, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 1599, 2, false, MENU).navigate, 0);   /* last ms */
    CHECK_EQ(fsm.accum, 0);

    /* Cooldown over at exactly now == cooldown_until */
    CHECK_EQ(upd(&fsm, 1600, 2, false, MENU).navigate, 0);
    CHECK_EQ(fsm.accum, 2);
    CHECK_EQ(upd(&fsm, 1633, 1, false, MENU).navigate, 1);
    CHECK_EQ(fsm.cooldown_until_ms, 1633 + 600);
}

static void test_detail_raw_diff(void)
{
    SCENARIO("detail: raw count delta per read, no accumulation, no cooldown");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK_EQ(upd(&fsm, 1000, 1, false, DETAIL).navigate, 1);
    CHECK_EQ(upd(&fsm, 1001, 4, false, DETAIL).navigate, 4);
    CHECK_EQ(upd(&fsm, 1002, -2, false, DETAIL).navigate, -2);
    CHECK_EQ(upd(&fsm, 1003, 0, false, DETAIL).navigate, 0);
    CHECK_EQ(fsm.accum, 0);
    CHECK_EQ(fsm.cooldown_until_ms, 0);

    /* A menu accumulator left over is kept untouched by detail reads */
    input_fsm_init(&fsm);
    CHECK_EQ(upd(&fsm, 2000, 2, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 2033, 5, false, DETAIL).navigate, 5);
    CHECK_EQ(fsm.accum, 2);
    CHECK_EQ(upd(&fsm, 2066, 1, false, MENU).navigate, 1);
}

static void test_cooldown_applies_in_detail(void)
{
    SCENARIO("detail: a running cooldown also discards detail rotation");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    /* Page step, then the user enters a detail screen within 600 ms */
    CHECK_EQ(upd(&fsm, 1000, 3, false, MENU).navigate, 1);
    CHECK_EQ(upd(&fsm, 1200, 2, false, DETAIL).navigate, 0);
    CHECK_EQ(upd(&fsm, 1600, 2, false, DETAIL).navigate, 2);

    /* After a release (e.g. the press that opened the detail) */
    input_fsm_init(&fsm);
    upd(&fsm, 5000, 0, true, MENU);
    CHECK(upd(&fsm, 5100, 0, false, MENU).press);
    CHECK_EQ(upd(&fsm, 5101, 3, false, DETAIL).navigate, 0);
    CHECK_EQ(upd(&fsm, 5399, -1, false, DETAIL).navigate, 0);
    CHECK_EQ(upd(&fsm, 5400, -1, false, DETAIL).navigate, -1);
}

static void test_short_press_debounce(void)
{
    SCENARIO("button: short press on release only when held > 50 ms");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    /* Held 50 ms exactly: a bounce, no press, but still a release */
    CHECK(no_action(upd(&fsm, 1000, 0, true, MENU)));
    CHECK(no_action(upd(&fsm, 1033, 0, true, MENU)));
    input_fsm_out_t o = upd(&fsm, 1050, 0, false, MENU);
    CHECK(no_action(o));
    CHECK_EQ(fsm.cooldown_until_ms, 1050 + 300);

    /* Held 51 ms: press fires on the release, not while held */
    CHECK(no_action(upd(&fsm, 2000, 0, true, MENU)));
    CHECK(no_action(upd(&fsm, 2051, 0, true, MENU)));
    o = upd(&fsm, 2051, 0, false, MENU);
    CHECK(o.press);
    CHECK(!o.long_press);
    CHECK_EQ(o.navigate, 0);

    /* A single-read glitch (pressed in one read, released in the next) */
    upd(&fsm, 3000, 0, true, MENU);
    CHECK(!upd(&fsm, 3033, 0, false, MENU).press);

    /* Staying released after a release fires nothing more */
    CHECK(no_action(upd(&fsm, 4000, 0, false, MENU)));
}

static void test_long_press(void)
{
    SCENARIO("button: long press once at > 1000 ms held, no short press on its release");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK(no_action(upd(&fsm, 1000, 0, true, MENU)));
    CHECK(no_action(upd(&fsm, 1500, 0, true, MENU)));
    CHECK(no_action(upd(&fsm, 2000, 0, true, MENU)));    /* exactly 1000: not yet */

    input_fsm_out_t o = upd(&fsm, 2001, 0, true, MENU);
    CHECK(o.long_press);
    CHECK(!o.press);
    CHECK(fsm.long_press_fired);

    /* Only once per hold */
    CHECK(no_action(upd(&fsm, 2033, 0, true, MENU)));
    CHECK(no_action(upd(&fsm, 9000, 0, true, MENU)));

    /* Release: no short press, accumulator reset, 300 ms cooldown */
    o = upd(&fsm, 9500, 0, false, MENU);
    CHECK(no_action(o));
    CHECK_EQ(fsm.cooldown_until_ms, 9500 + 300);

    /* The next hold is independent: short press works again */
    upd(&fsm, 10000, 0, true, MENU);
    CHECK(upd(&fsm, 10200, 0, false, MENU).press);

    /* And the long press fires again on a later hold */
    upd(&fsm, 11000, 0, true, MENU);
    CHECK(upd(&fsm, 12001, 0, true, MENU).long_press);
    CHECK(!upd(&fsm, 12100, 0, false, MENU).press);
}

static void test_long_press_detected_late(void)
{
    SCENARIO("button: a read gap still yields a long press, never a short one");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    /* LVGL stalls: first read after the press-start read is 3 s later */
    upd(&fsm, 1000, 0, true, MENU);
    CHECK(upd(&fsm, 4000, 0, true, MENU).long_press);
    CHECK(!upd(&fsm, 4033, 0, false, MENU).press);

    /* Released before any read saw it held > 1000 ms: short press, even
     * though the hold measured at release is longer */
    upd(&fsm, 5000, 0, true, MENU);
    upd(&fsm, 5900, 0, true, MENU);
    input_fsm_out_t o = upd(&fsm, 6500, 0, false, MENU);
    CHECK(o.press);
    CHECK(!o.long_press);
}

static void test_release_resets_accum_and_overrides_cooldown(void)
{
    SCENARIO("release: clears the accumulator; its 300 ms cooldown replaces a longer one");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    /* Accumulated 2, then a press/release: the 2 are gone */
    CHECK_EQ(upd(&fsm, 1000, 2, false, MENU).navigate, 0);
    upd(&fsm, 1100, 0, true, MENU);
    CHECK_EQ(fsm.accum, 2);
    upd(&fsm, 1200, 0, false, MENU);
    CHECK_EQ(fsm.accum, 0);
    CHECK_EQ(upd(&fsm, 1500, 1, false, MENU).navigate, 0);  /* after cooldown */
    CHECK_EQ(fsm.accum, 1);

    /* Menu step at 2000 (cooldown to 2600), short press released at 2100:
     * the cooldown now ends at 2400, earlier than the menu one (native) */
    input_fsm_init(&fsm);
    CHECK_EQ(upd(&fsm, 2000, 3, false, MENU).navigate, 1);
    upd(&fsm, 2010, 0, true, MENU);
    CHECK(upd(&fsm, 2100, 0, false, MENU).press);
    CHECK_EQ(fsm.cooldown_until_ms, 2400);
    CHECK_EQ(upd(&fsm, 2399, 3, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, 2400, 3, false, MENU).navigate, 1);
}

static void test_rotation_while_held(void)
{
    SCENARIO("rotation while the button is held still navigates; same-read ordering");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    upd(&fsm, 1000, 0, true, DETAIL);
    input_fsm_out_t o = upd(&fsm, 1100, 2, true, DETAIL);
    CHECK_EQ(o.navigate, 2);
    CHECK(!o.long_press);

    /* Navigation and the long press in one read */
    o = upd(&fsm, 2001, -1, true, DETAIL);
    CHECK_EQ(o.navigate, -1);
    CHECK(o.long_press);

    /* Release in the same read as a count: the count is processed first
     * (no cooldown yet), then the release starts the cooldown */
    input_fsm_init(&fsm);
    upd(&fsm, 3000, 0, true, MENU);
    CHECK_EQ(upd(&fsm, 3100, 2, true, MENU).navigate, 0);
    o = upd(&fsm, 3200, 1, false, MENU);
    CHECK_EQ(o.navigate, 1);
    CHECK(o.press);
    CHECK_EQ(fsm.cooldown_until_ms, 3200 + 300);   /* release wins over 600 */
}

static void test_press_during_cooldown(void)
{
    SCENARIO("cooldown only affects rotation: presses inside it still fire");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK_EQ(upd(&fsm, 1000, 3, false, MENU).navigate, 1);
    upd(&fsm, 1010, 0, true, MENU);
    CHECK(upd(&fsm, 1100, 0, false, MENU).press);
    upd(&fsm, 1150, 0, true, MENU);
    CHECK(upd(&fsm, 1250, 0, false, MENU).press);
}

static void test_held_at_start(void)
{
    SCENARIO("button already held at the first read: the hold starts there");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK(no_action(upd(&fsm, 70000, 0, true, MENU)));
    CHECK_EQ(fsm.press_start_ms, 70000);
    CHECK(!upd(&fsm, 71000, 0, true, MENU).long_press);
    CHECK(upd(&fsm, 71001, 0, true, MENU).long_press);
}

static void test_activity(void)
{
    SCENARIO("activity: any count (even discarded) or a held button");
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    CHECK(!upd(&fsm, 1000, 0, false, MENU).activity);
    CHECK(upd(&fsm, 1033, 1, false, MENU).activity);          /* accumulating */
    CHECK(upd(&fsm, 1066, -1, false, DETAIL).activity);

    CHECK_EQ(upd(&fsm, 2000, 3, false, MENU).navigate, 1);
    input_fsm_out_t o = upd(&fsm, 2100, 1, false, MENU);      /* in cooldown */
    CHECK_EQ(o.navigate, 0);
    CHECK(o.activity);

    CHECK(upd(&fsm, 3000, 0, true, MENU).activity);           /* press start */
    CHECK(upd(&fsm, 3500, 0, true, MENU).activity);           /* still held */
    o = upd(&fsm, 3600, 0, false, MENU);                      /* release read */
    CHECK(o.press);
    CHECK(!o.activity);
    CHECK(!upd(&fsm, 3633, 0, false, MENU).activity);
}

static void test_time_base_crosses_2_pow_32(void)
{
    SCENARIO("time base: cooldowns and press timing across now = 2^32 ms (49.7 days)");
    const int64_t wrap = (int64_t)1 << 32;
    input_fsm_t fsm;
    input_fsm_init(&fsm);

    /* Menu cooldown opened 100 ms before the boundary ends 500 ms after it;
     * a uint32 time base would read the end as 500 and skip the cooldown */
    CHECK_EQ(upd(&fsm, wrap - 100, 3, false, MENU).navigate, 1);
    CHECK_EQ(upd(&fsm, wrap - 1, 3, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, wrap, 3, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, wrap + 499, 3, false, MENU).navigate, 0);
    CHECK_EQ(upd(&fsm, wrap + 500, 3, false, MENU).navigate, 1);

    /* Short press held across the boundary: 51 ms */
    input_fsm_init(&fsm);
    upd(&fsm, wrap - 30, 0, true, MENU);
    CHECK(upd(&fsm, wrap + 21, 0, false, MENU).press);
    /* ... whose release cooldown covers wrap + 21 .. wrap + 320 */
    CHECK_EQ(upd(&fsm, wrap + 320, 2, false, DETAIL).navigate, 0);
    CHECK_EQ(upd(&fsm, wrap + 321, 2, false, DETAIL).navigate, 2);

    /* Release cooldown opened just before the boundary */
    input_fsm_init(&fsm);
    upd(&fsm, wrap - 200, 0, true, MENU);
    CHECK(upd(&fsm, wrap - 1, 0, false, MENU).press);
    CHECK_EQ(upd(&fsm, wrap + 298, 1, false, DETAIL).navigate, 0);
    CHECK_EQ(upd(&fsm, wrap + 299, 1, false, DETAIL).navigate, 1);

    /* Bounce across the boundary: 50 ms is not a press */
    input_fsm_init(&fsm);
    upd(&fsm, wrap - 25, 0, true, MENU);
    CHECK(!upd(&fsm, wrap + 25, 0, false, MENU).press);

    /* Long press across the boundary fires at > 1000 ms, exactly once */
    input_fsm_init(&fsm);
    upd(&fsm, wrap - 600, 0, true, MENU);
    CHECK(!upd(&fsm, wrap - 1, 0, true, MENU).long_press);
    CHECK(!upd(&fsm, wrap + 400, 0, true, MENU).long_press);
    CHECK(upd(&fsm, wrap + 401, 0, true, MENU).long_press);
    CHECK(!upd(&fsm, wrap + 900, 0, true, MENU).long_press);
    CHECK(!upd(&fsm, wrap + 1000, 0, false, MENU).press);
}

/* ====================================================================== */

int main(void)
{
    test_init();
    test_menu_accumulation();
    test_menu_surplus_dropped();
    test_menu_cooldown();
    test_detail_raw_diff();
    test_cooldown_applies_in_detail();
    test_short_press_debounce();
    test_long_press();
    test_long_press_detected_late();
    test_release_resets_accum_and_overrides_cooldown();
    test_rotation_while_held();
    test_press_during_cooldown();
    test_held_at_start();
    test_activity();
    test_time_base_crosses_2_pow_32();

    return CHECK_SUMMARY();
}
