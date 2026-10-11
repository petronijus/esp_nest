#include "mqtt_payload.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fan_sync.h"
#include "light_sync.h"

/* ====================================================================== */
/*  Helpers (ported verbatim from main/ha_mqtt.c v2.0.1)                    */
/* ====================================================================== */

/* Copy at most `max` bytes of a (not necessarily terminated) payload into
 * `buf` (size >= max + 1) and terminate it. */
static void copy_truncated(char *buf, size_t max, const char *data, size_t len)
{
    size_t n = len < max ? len : max;
    if (n) memcpy(buf, data, n);
    buf[n] = '\0';
}

/* strtol saturated to the int range (strtol on the 32-bit-long ESP32
 * already saturates there). Returns false when no digits were consumed. */
static bool strtol_int(const char *s, int *out)
{
    char *endp = NULL;
    long v = strtol(s, &endp, 10);
    if (endp == s) return false;
    if (v > INT_MAX) v = INT_MAX;
    if (v < INT_MIN) v = INT_MIN;
    *out = (int)v;
    return true;
}

static const char *find_key(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(json, needle);
}

static bool parse_json_string(const char *json, const char *key,
                              char *out, size_t outsz)
{
    const char *p = find_key(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static bool parse_json_int(const char *json, const char *key, int *out)
{
    const char *p = find_key(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return strtol_int(p, out);
}

static bool str_eq_icase(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ====================================================================== */
/*  Parsing                                                                */
/* ====================================================================== */

void mqtt_payload_parse_light(const char *data, size_t len,
                              mqtt_light_state_t *out)
{
    char msg[MQTT_PAYLOAD_LIGHT_MAX + 1];
    copy_truncated(msg, MQTT_PAYLOAD_LIGHT_MAX, data, len);

    memset(out->state, 0, sizeof(out->state));
    out->brightness = -1;
    out->have_state = parse_json_string(msg, "state",
                                        out->state, sizeof(out->state));
    out->have_brightness = parse_json_int(msg, "brightness", &out->brightness);
    out->state_on = out->have_state && str_eq_icase(out->state, "ON");
}

int mqtt_payload_parse_fan(const char *data, size_t len)
{
    char msg[MQTT_PAYLOAD_FAN_MAX + 1];
    copy_truncated(msg, MQTT_PAYLOAD_FAN_MAX, data, len);

    for (int level = 0; level <= FAN_SYNC_LEVEL_MAX; level++)
        if (str_eq_icase(msg, mqtt_payload_fan_label(level)))
            return level;
    return -1;
}

bool mqtt_payload_parse_int(const char *data, size_t len, int *out)
{
    char msg[MQTT_PAYLOAD_INT_MAX + 1];
    copy_truncated(msg, MQTT_PAYLOAD_INT_MAX, data, len);
    return strtol_int(msg, out);
}

/* ====================================================================== */
/*  Formatting                                                             */
/* ====================================================================== */

const char *mqtt_payload_fan_label(int level)
{
    static const char *const labels[] = {"Off", "low", "medium", "high"};
    if (level < 0 || level > FAN_SYNC_LEVEL_MAX) return NULL;
    return labels[level];
}

int mqtt_payload_light_set(int pct, char *buf, size_t bufsz)
{
    return snprintf(buf, bufsz, "{\"state\":\"%s\",\"brightness\":%d}",
                    pct == 0 ? "OFF" : "ON", light_sync_pct_to_raw(pct));
}

const char *mqtt_payload_light_get(void)
{
    return "{\"state\":\"\"}";
}

const char *mqtt_payload_volume(int dir)
{
    if (dir > 0) return "UP";
    if (dir < 0) return "DOWN";
    return NULL;
}

int mqtt_payload_blinds_command(const blinds_sync_cmd_t *cmd,
                                char *buf, size_t bufsz)
{
    switch (cmd->kind) {
    case BLINDS_SYNC_CMD_OPEN:
        return snprintf(buf, bufsz, "OPEN");
    case BLINDS_SYNC_CMD_CLOSE:
        return snprintf(buf, bufsz, "CLOSE");
    case BLINDS_SYNC_CMD_POSITION:
        return snprintf(buf, bufsz, "%d", cmd->position);
    case BLINDS_SYNC_CMD_NONE:
    default:
        return -1;
    }
}
