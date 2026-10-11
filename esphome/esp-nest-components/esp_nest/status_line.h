#pragma once

/*
 * The bottom status line of the UI: connection state and firmware version.
 *
 * The strings of the native v2.0.1 firmware (main/main.c update_status_line
 * and app_main), kept as is:
 *
 *   Starting...                              until the first state is known
 *   WiFi: disconnected | v<ver>
 *   IP: <a.b.c.d> | MQTT: offline | v<ver>
 *   IP: <a.b.c.d> | MQTT: ok | v<ver>
 *
 * Pure C, no ESP-IDF / LVGL / ESPHome dependencies, host-tested in
 * test/host/. The caller owns the buffer, so no string ever crosses
 * threads: nest_ui formats the line from the connection flags it reads.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shown before the first connection state is published (native: the text
 * the UI starts with). */
#define STATUS_LINE_STARTING "Starting..."

/* Native buffer size (char buf[128]); a longer line is truncated. */
#define STATUS_LINE_SIZE 128

/* Formats the status line into `buf` (snprintf semantics: always
 * NUL-terminated for size > 0, returns the length it would have had).
 * `ip_v4` is the station address in host byte order (a.b.c.d = a<<24 |
 * b<<16 | c<<8 | d); it is only shown while `wifi` is true. `version` is
 * the firmware version without the leading "v". */
int status_line_format(char *buf, size_t size, bool wifi, uint32_t ip_v4,
                       bool mqtt, const char *version);

#ifdef __cplusplus
}
#endif
