/*
 * Host-side tests for the status line formatter
 * (esphome/esp-nest-components/esp_nest/status_line.c).
 *
 * Build & run:  make -C test/host run
 *
 * The expectations are the strings of the native v2.0.1 firmware
 * (main/main.c update_status_line): "WiFi: disconnected | v<ver>",
 * "IP: <ip> | MQTT: offline | v<ver>" and "IP: <ip> | MQTT: ok | v<ver>".
 */

#include "check.h"
#include "status_line.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static void test_states(void)
{
    SCENARIO("the three native states");
    char buf[STATUS_LINE_SIZE];

    int n = status_line_format(buf, sizeof buf, false, 0, false, "3.0.0");
    CHECK_STR(buf, "WiFi: disconnected | v3.0.0");
    CHECK_EQ(n, (int)strlen(buf));

    n = status_line_format(buf, sizeof buf, true, IP(192, 168, 20, 213), false, "3.0.0");
    CHECK_STR(buf, "IP: 192.168.20.213 | MQTT: offline | v3.0.0");
    CHECK_EQ(n, (int)strlen(buf));

    n = status_line_format(buf, sizeof buf, true, IP(192, 168, 20, 213), true, "3.0.0");
    CHECK_STR(buf, "IP: 192.168.20.213 | MQTT: ok | v3.0.0");
    CHECK_EQ(n, (int)strlen(buf));
}

static void test_wifi_down_hides_the_rest(void)
{
    SCENARIO("without WiFi the address and the MQTT state are not shown");
    char buf[STATUS_LINE_SIZE];

    status_line_format(buf, sizeof buf, false, IP(10, 0, 0, 1), true, "2.0.1");
    CHECK_STR(buf, "WiFi: disconnected | v2.0.1");
}

static void test_address_octets(void)
{
    SCENARIO("every octet is formatted in network order, incl. 0 and 255");
    char buf[STATUS_LINE_SIZE];

    status_line_format(buf, sizeof buf, true, IP(0, 0, 0, 0), true, "x");
    CHECK_STR(buf, "IP: 0.0.0.0 | MQTT: ok | vx");
    status_line_format(buf, sizeof buf, true, IP(255, 255, 255, 255), true, "x");
    CHECK_STR(buf, "IP: 255.255.255.255 | MQTT: ok | vx");
    status_line_format(buf, sizeof buf, true, IP(1, 2, 3, 4), false, "x");
    CHECK_STR(buf, "IP: 1.2.3.4 | MQTT: offline | vx");
}

static void test_truncation(void)
{
    SCENARIO("a short buffer is truncated and NUL-terminated, like snprintf");
    char buf[16];
    memset(buf, 'z', sizeof buf);

    int n = status_line_format(buf, sizeof buf, true, IP(192, 168, 20, 213), true, "3.0.0");
    CHECK_EQ(n, (int)strlen("IP: 192.168.20.213 | MQTT: ok | v3.0.0"));
    CHECK_EQ(strlen(buf), sizeof buf - 1);
    CHECK_STR(buf, "IP: 192.168.20.");

    /* The native buffer holds the longest possible line with a long version */
    char big[STATUS_LINE_SIZE];
    n = status_line_format(big, sizeof big, true, IP(255, 255, 255, 255), false,
                           "3.0.0-rc.1+abcdef0");
    CHECK(n > 0 && n < (int)sizeof big);
}

int main(void)
{
    test_states();
    test_wifi_down_hides_the_rest();
    test_address_octets();
    test_truncation();

    return CHECK_SUMMARY();
}
