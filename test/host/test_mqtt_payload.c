/*
 * Host-side tests for the external-device payload parsing / formatting
 * (esphome/esp-nest-components/esp_nest/mqtt_payload.c).
 *
 * Build & run:  make -C test/host run
 *
 * The expectations are the native v2.0.1 behaviour (main/ha_mqtt.c),
 * quirks included: first-match key lookup, silent truncation, strtol.
 */

#include <limits.h>
#include <string.h>

#include "check.h"
#include "mqtt_payload.h"

/* Parse a NUL-terminated literal (length = strlen) */
static mqtt_light_state_t light(const char *json)
{
    mqtt_light_state_t st;
    mqtt_payload_parse_light(json, strlen(json), &st);
    return st;
}

static int fan(const char *s)
{
    return mqtt_payload_parse_fan(s, strlen(s));
}

/* Returns the parsed value, or the sentinel 'untouched' when the parser
 * rejected the payload (and must have left the output alone). */
#define UNTOUCHED  (-424242)
static int int_payload(const char *s)
{
    int v = UNTOUCHED;
    bool ok = mqtt_payload_parse_int(s, strlen(s), &v);
    if (!ok) CHECK_EQ(v, UNTOUCHED);
    return v;
}

/* JSON object padded with a "pad" member so that the following text starts
 * at byte offset `at`. */
static size_t padded(char *buf, size_t bufsz, size_t at, const char *tail)
{
    const char *head = "{\"pad\":\"";
    size_t hl = strlen(head);
    size_t fill = at - hl - 2;                  /* minus the closing `",` */
    size_t n = 0;
    memcpy(buf, head, hl); n += hl;
    memset(buf + n, 'x', fill); n += fill;
    memcpy(buf + n, "\",", 2); n += 2;
    size_t tl = strlen(tail);
    CHECK(n + tl < bufsz);
    memcpy(buf + n, tail, tl); n += tl;
    buf[n] = '\0';
    return n;
}

/* ====================================================================== */

static void test_light_basic(void)
{
    SCENARIO("light: state and brightness, case-insensitive ON, anything else is OFF");
    mqtt_light_state_t st = light("{\"state\":\"ON\",\"brightness\":128}");
    CHECK(st.have_state);
    CHECK(st.state_on);
    CHECK_STR(st.state, "ON");
    CHECK(st.have_brightness);
    CHECK_EQ(st.brightness, 128);

    st = light("{\"brightness\":254,\"state\":\"OFF\"}");
    CHECK(st.have_state);
    CHECK(!st.state_on);
    CHECK_EQ(st.brightness, 254);

    st = light("{\"state\":\"on\"}");
    CHECK(st.state_on);
    CHECK(!st.have_brightness);
    CHECK_EQ(st.brightness, -1);

    st = light("{\"state\":\"TOGGLE\"}");
    CHECK(st.have_state);
    CHECK(!st.state_on);
    CHECK_STR(st.state, "TOGGLE");

    /* Spaces and tabs between the colon and the value are skipped */
    st = light("{\"state\": \t\"On\", \"brightness\":\t 42}");
    CHECK(st.state_on);
    CHECK_EQ(st.brightness, 42);

    /* Empty payload: nothing, brightness reported as -1 */
    mqtt_payload_parse_light("", 0, &st);
    CHECK(!st.have_state);
    CHECK(!st.state_on);
    CHECK_STR(st.state, "");
    CHECK(!st.have_brightness);
    CHECK_EQ(st.brightness, -1);
}

static void test_light_first_match_anywhere(void)
{
    SCENARIO("light: first occurrence of a key anywhere wins, nesting ignored");
    /* A nested "state" before the top-level one is what native reads */
    mqtt_light_state_t st =
        light("{\"color\":{\"state\":\"OFF\"},\"state\":\"ON\",\"brightness\":200}");
    CHECK(st.have_state);
    CHECK(!st.state_on);
    CHECK_EQ(st.brightness, 200);

    st = light("{\"update\":{\"brightness\":10},\"brightness\":200}");
    CHECK_EQ(st.brightness, 10);

    /* The quoted needle does not match longer keys */
    st = light("{\"state_l1\":\"ON\",\"brightness_l1\":99}");
    CHECK(!st.have_state);
    CHECK(!st.have_brightness);

    /* A string VALUE "state" counts as the key, and its value is whatever
     * follows the next colon — here a number, so the state is absent even
     * though a real "state" key follows */
    st = light("{\"effect\":\"state\",\"brightness\":5,\"state\":\"ON\"}");
    CHECK(!st.have_state);
    CHECK_EQ(st.brightness, 5);

    /* ...and here the next colon belongs to the real key, so it is read */
    st = light("{\"effect\":\"state\",\"state\":\"ON\"}");
    CHECK(st.have_state);
    CHECK(st.state_on);
}

static void test_light_state_value_shapes(void)
{
    SCENARIO("light: state is a quoted string cut to 7 chars; unquoted is absent");
    mqtt_light_state_t st = light("{\"state\":\"ONLINE_X\"}");
    CHECK(st.have_state);
    CHECK_STR(st.state, "ONLINE_");
    CHECK(!st.state_on);

    /* "ON" padded past 7 chars is not "ON" any more */
    st = light("{\"state\":\"ON      \"}");
    CHECK(st.have_state);
    CHECK(!st.state_on);

    st = light("{\"state\":ON}");
    CHECK(!st.have_state);
    CHECK(!st.state_on);

    st = light("{\"state\":null}");
    CHECK(!st.have_state);

    /* Missing closing quote: copied to the end of the payload */
    st = light("{\"state\":\"ON");
    CHECK(st.have_state);
    CHECK(st.state_on);

    /* Key without a colon after it */
    st = light("{\"state\"}");
    CHECK(!st.have_state);

    st = light("{\"state\":\"\"}");
    CHECK(st.have_state);
    CHECK(!st.state_on);
    CHECK_STR(st.state, "");
}

static void test_light_brightness_strtol(void)
{
    SCENARIO("light: brightness via strtol (127.5 -> 127), null / strings are absent");
    mqtt_light_state_t st = light("{\"brightness\":127.5}");
    CHECK(st.have_brightness);
    CHECK_EQ(st.brightness, 127);

    st = light("{\"brightness\":null,\"state\":\"ON\"}");
    CHECK(!st.have_brightness);
    CHECK_EQ(st.brightness, -1);
    CHECK(st.state_on);

    st = light("{\"brightness\":\"128\"}");
    CHECK(!st.have_brightness);

    st = light("{\"brightness\":-5}");
    CHECK(st.have_brightness);
    CHECK_EQ(st.brightness, -5);

    st = light("{\"brightness\":+7}");
    CHECK_EQ(st.brightness, 7);

    /* strtol itself also skips newlines after the space/tab skip */
    st = light("{\"brightness\":\n 42}");
    CHECK(st.have_brightness);
    CHECK_EQ(st.brightness, 42);

    /* Saturated to the int range, as strtol does with a 32-bit long */
    st = light("{\"brightness\":99999999999}");
    CHECK_EQ(st.brightness, INT_MAX);
    st = light("{\"brightness\":-99999999999}");
    CHECK_EQ(st.brightness, INT_MIN);
}

static void test_light_truncation(void)
{
    SCENARIO("light: payload truncated to 255 chars, cutting keys and values");
    char buf[400];
    mqtt_light_state_t st;

    /* Key fully inside the first 255 bytes */
    size_t n = padded(buf, sizeof(buf), 200, "\"brightness\":200}");
    mqtt_payload_parse_light(buf, n, &st);
    CHECK(st.have_brightness);
    CHECK_EQ(st.brightness, 200);

    /* Key starts at offset 250: the cut at 255 leaves `"brig` -> absent */
    n = padded(buf, sizeof(buf), 250, "\"brightness\":200,\"state\":\"ON\"}");
    CHECK(n > MQTT_PAYLOAD_LIGHT_MAX);
    mqtt_payload_parse_light(buf, n, &st);
    CHECK(!st.have_brightness);
    CHECK(!st.have_state);

    /* Value cut: `"brightness":1` survives, `23` is beyond byte 255 */
    n = padded(buf, sizeof(buf), MQTT_PAYLOAD_LIGHT_MAX - 14, "\"brightness\":123}");
    mqtt_payload_parse_light(buf, n, &st);
    CHECK(st.have_brightness);
    CHECK_EQ(st.brightness, 1);

    /* State value cut by the truncation keeps what is left */
    n = padded(buf, sizeof(buf), MQTT_PAYLOAD_LIGHT_MAX - 10, "\"state\":\"ON\"}");
    mqtt_payload_parse_light(buf, n, &st);
    CHECK(st.have_state);
    CHECK_STR(st.state, "O");
    CHECK(!st.state_on);

    /* Exactly 255 bytes: nothing lost */
    n = padded(buf, sizeof(buf), MQTT_PAYLOAD_LIGHT_MAX - 17, "\"brightness\":123}");
    CHECK_EQ(n, MQTT_PAYLOAD_LIGHT_MAX);
    mqtt_payload_parse_light(buf, n, &st);
    CHECK_EQ(st.brightness, 123);
}

static void test_light_unterminated_and_nul(void)
{
    SCENARIO("light: only `len` bytes are read; an embedded NUL ends the payload");
    const char raw[] = "{\"state\":\"ON\"}XX,\"brightness\":9";
    mqtt_light_state_t st;
    mqtt_payload_parse_light(raw, 14, &st);     /* just the object */
    CHECK(st.state_on);
    CHECK(!st.have_brightness);

    const char nul[] = "{\"state\":\"ON\"}\0,\"brightness\":5}";
    mqtt_payload_parse_light(nul, sizeof(nul) - 1, &st);
    CHECK(st.state_on);
    CHECK(!st.have_brightness);
}

static void test_fan_parse(void)
{
    SCENARIO("fan: exact case-insensitive label match, else -1");
    CHECK_EQ(fan("Off"), 0);
    CHECK_EQ(fan("off"), 0);
    CHECK_EQ(fan("OFF"), 0);
    CHECK_EQ(fan("low"), 1);
    CHECK_EQ(fan("Medium"), 2);
    CHECK_EQ(fan("HIGH"), 3);

    CHECK_EQ(fan(""), -1);
    CHECK_EQ(fan("lo"), -1);
    CHECK_EQ(fan("low "), -1);
    CHECK_EQ(fan(" low"), -1);
    CHECK_EQ(fan("1"), -1);
    CHECK_EQ(fan("on"), -1);
    CHECK_EQ(fan("\"low\""), -1);

    /* Truncated to 31 chars: a long payload can never match */
    CHECK_EQ(fan("highhighhighhighhighhighhighhighhigh"), -1);

    /* Only `len` bytes; an embedded NUL ends the payload */
    CHECK_EQ(mqtt_payload_parse_fan("mediumXYZ", 6), 2);
    CHECK_EQ(mqtt_payload_parse_fan("high\0junk", 9), 3);
}

static void test_int_parse(void)
{
    SCENARIO("int: strtol on the first 15 chars, rejects payloads without digits");
    CHECK_EQ(int_payload("50"), 50);
    CHECK_EQ(int_payload("0"), 0);
    CHECK_EQ(int_payload("100"), 100);
    CHECK_EQ(int_payload("  42"), 42);
    CHECK_EQ(int_payload("17.9"), 17);
    CHECK_EQ(int_payload("12abc"), 12);
    CHECK_EQ(int_payload("-5"), -5);
    CHECK_EQ(int_payload("+8"), 8);

    CHECK_EQ(int_payload(""), UNTOUCHED);
    CHECK_EQ(int_payload("abc"), UNTOUCHED);
    CHECK_EQ(int_payload("unknown"), UNTOUCHED);
    CHECK_EQ(int_payload("-"), UNTOUCHED);

    /* 16 chars: the last digit is cut off */
    CHECK_EQ(int_payload("0000000000000123"), 12);

    /* Saturated like strtol with a 32-bit long */
    CHECK_EQ(int_payload("99999999999"), INT_MAX);
    CHECK_EQ(int_payload("-99999999999"), INT_MIN);

    int v = 7;
    CHECK(mqtt_payload_parse_int("33xx", 2, &v));
    CHECK_EQ(v, 33);
    CHECK(mqtt_payload_parse_int("6\0" "7", 3, &v));
    CHECK_EQ(v, 6);
}

static void test_format(void)
{
    SCENARIO("format: lights /set and /get, fan labels, volume, blinds commands");
    char buf[MQTT_PAYLOAD_OUT_SIZE];

    CHECK_EQ(mqtt_payload_light_set(0, buf, sizeof(buf)), 30);
    CHECK_STR(buf, "{\"state\":\"OFF\",\"brightness\":0}");
    mqtt_payload_light_set(50, buf, sizeof(buf));
    CHECK_STR(buf, "{\"state\":\"ON\",\"brightness\":128}");
    mqtt_payload_light_set(1, buf, sizeof(buf));
    CHECK_STR(buf, "{\"state\":\"ON\",\"brightness\":3}");
    mqtt_payload_light_set(100, buf, sizeof(buf));
    CHECK_STR(buf, "{\"state\":\"ON\",\"brightness\":255}");
    for (int pct = 0; pct <= 100; pct++)
        CHECK(mqtt_payload_light_set(pct, buf, sizeof(buf)) < (int)sizeof(buf));

    CHECK_STR(mqtt_payload_light_get(), "{\"state\":\"\"}");

    CHECK_STR(mqtt_payload_fan_label(0), "Off");
    CHECK_STR(mqtt_payload_fan_label(1), "low");
    CHECK_STR(mqtt_payload_fan_label(2), "medium");
    CHECK_STR(mqtt_payload_fan_label(3), "high");
    CHECK_STR(mqtt_payload_fan_label(-1), NULL);
    CHECK_STR(mqtt_payload_fan_label(4), NULL);
    for (int level = 0; level <= 3; level++)
        CHECK_EQ(fan(mqtt_payload_fan_label(level)), level);

    CHECK_STR(mqtt_payload_volume(1), "UP");
    CHECK_STR(mqtt_payload_volume(3), "UP");
    CHECK_STR(mqtt_payload_volume(-1), "DOWN");
    CHECK_STR(mqtt_payload_volume(0), NULL);

    blinds_sync_cmd_t cmd = {0, BLINDS_SYNC_CMD_OPEN, 100};
    CHECK_EQ(mqtt_payload_blinds_command(&cmd, buf, sizeof(buf)), 4);
    CHECK_STR(buf, "OPEN");
    cmd.kind = BLINDS_SYNC_CMD_CLOSE;
    cmd.position = 0;
    CHECK_EQ(mqtt_payload_blinds_command(&cmd, buf, sizeof(buf)), 5);
    CHECK_STR(buf, "CLOSE");
    cmd.kind = BLINDS_SYNC_CMD_POSITION;
    cmd.position = 83;
    CHECK_EQ(mqtt_payload_blinds_command(&cmd, buf, sizeof(buf)), 2);
    CHECK_STR(buf, "83");
    cmd.kind = BLINDS_SYNC_CMD_NONE;
    CHECK_EQ(mqtt_payload_blinds_command(&cmd, buf, sizeof(buf)), -1);
}

/* ====================================================================== */

int main(void)
{
    test_light_basic();
    test_light_first_match_anywhere();
    test_light_state_value_shapes();
    test_light_brightness_strtol();
    test_light_truncation();
    test_light_unterminated_and_nul();
    test_fan_parse();
    test_int_parse();
    test_format();

    return CHECK_SUMMARY();
}
