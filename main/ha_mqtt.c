#include "ha_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mqtt_client.h"
#include "wifi.h"
#include "ui.h"
#include "screen.h"
#include "led_ring.h"
#include "light_sync.h"

static const char *TAG = "ha_mqtt";

#define MQTT_CONNECTED_BIT BIT0

static const char *FAN_TOPIC         = CONFIG_TOPIC_FAN;
static const char *Z2M_LIGHT_TOPIC   = CONFIG_TOPIC_LIGHTS;
static const char *Z2M_LIGHT_SET     = CONFIG_TOPIC_LIGHTS "/set";
static const char *Z2M_LIGHT_GET     = CONFIG_TOPIC_LIGHTS "/get";
static const char *STEREO_VOL_SET    = CONFIG_TOPIC_STEREO_VOL "/set";

/* RF blinds: ESPHome cover bridge, MQTT-native (Wemos + CC1101,
 * dead-reckoning time_based covers).  Six member covers carry position
 * 0-100 (0 = closed); the two group covers are broadcast-only (single RF
 * burst, no position).  Strategy: full open/close goes through the group
 * broadcast, intermediate steps via position/command to all three members.
 * Position is read back as the average of the three members.  Index:
 * 0 = group A, 1 = group B (matches ui.c).  Topics are assembled from the
 * Kconfig TOPIC_BLINDS_* options via adjacent string-literal concatenation,
 * so they stay compile-time constants with no runtime cost. */
#define BL_  CONFIG_TOPIC_BLINDS_PREFIX
#define BGA_ CONFIG_TOPIC_BLINDS_GROUP_A
#define BGB_ CONFIG_TOPIC_BLINDS_GROUP_B
static const char *BLINDS_POS_STATE[2][3] = {
    { BL_ "/" BGA_ "_1/position/state",
      BL_ "/" BGA_ "_2/position/state",
      BL_ "/" BGA_ "_3/position/state" },
    { BL_ "/" BGB_ "_1/position/state",
      BL_ "/" BGB_ "_2/position/state",
      BL_ "/" BGB_ "_3/position/state" },
};
static const char *BLINDS_POS_CMD[2][3] = {
    { BL_ "/" BGA_ "_1/position/command",
      BL_ "/" BGA_ "_2/position/command",
      BL_ "/" BGA_ "_3/position/command" },
    { BL_ "/" BGB_ "_1/position/command",
      BL_ "/" BGB_ "_2/position/command",
      BL_ "/" BGB_ "_3/position/command" },
};
static const char *BLINDS_GROUP_CMD[2] = {
    BL_ "/" BGA_ "_group/command",
    BL_ "/" BGB_ "_group/command",
};

#define BLINDS_POS_MAX_STEP  6
#define BLINDS_SETTLE_MS     500    /* wait for the user to stop stepping */
#define BLINDS_BLOCK_MS      2500   /* drop incoming echoes after a command */

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t s_mqtt_event_group = NULL;
static bool s_connected = false;
static char s_device_id[16] = {0};

/* Light sync state machine (throttle + authority window + resync),
 * see light_sync.h for the design notes. */
static light_sync_t s_light_sync;

static int s_last_pub_fan = -1;
static int64_t s_last_fan_pub_time = 0;

static int s_blinds_pending[2] = {-1, -1};
static int64_t s_blinds_change_time[2] = {0, 0};
static int64_t s_blinds_block_until[2] = {0, 0};
static int s_blinds_member_pos[2][3] = {{-1, -1, -1}, {-1, -1, -1}};

static void build_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "esp_%02x%02x%02x",
             mac[3], mac[4], mac[5]);
}

/* ====================================================================== */
/*  JSON helpers (simple, no external dependency)                          */
/* ====================================================================== */

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
    char *endp = NULL;
    long v = strtol(p, &endp, 10);
    if (endp == p) return false;
    *out = (int)v;
    return true;
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
/*  Topic helpers                                                          */
/* ====================================================================== */

static char s_base_topic[48];   /* "espnest/<id>" */
static char s_avail_topic[64];
static char s_state_topic[64];
static char s_cmd_topic[64];

static void build_topics(void)
{
    snprintf(s_base_topic,  sizeof(s_base_topic),  "espnest/%s", s_device_id);
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/avail",   s_base_topic);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state",   s_base_topic);
    snprintf(s_cmd_topic,   sizeof(s_cmd_topic),   "%s/cmd/#",   s_base_topic);
}

/* ====================================================================== */
/*  Publishing: controlled light                                           */
/* ====================================================================== */

static void publish_light_set(int brightness)
{
    const char *state = (brightness == 0) ? "OFF" : "ON";
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"brightness\":%d}",
             state, light_sync_pct_to_raw(brightness));
    esp_mqtt_client_publish(s_client, Z2M_LIGHT_SET, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Light publish: %s", payload);
}

void ha_mqtt_publish_light(void)
{
    if (!s_connected || !s_client) return;

    int brightness = ui_get_brightness();
    int64_t now = esp_timer_get_time() / 1000;
    if (light_sync_on_local_change(&s_light_sync, now, brightness)
            == LIGHT_SYNC_ACT_PUBLISH) {
        publish_light_set(brightness);
    }
}

void ha_mqtt_publish_fan(void)
{
    if (!s_connected || !s_client) return;
    int level = ui_get_fan_level();
    if (level == s_last_pub_fan) return;

    static const char *labels[] = {"Off", "low", "medium", "high"};
    esp_mqtt_client_publish(s_client, FAN_TOPIC, labels[level], 0, 0, 1);
    s_last_pub_fan = level;
    s_last_fan_pub_time = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Fan publish: %s", labels[level]);
}

/* 0-6 UI step <-> 0-100 HA cover position (100 = open, step 0) */
static int blinds_step_to_pos(int step)
{
    return 100 - (step * 100 + BLINDS_POS_MAX_STEP / 2) / BLINDS_POS_MAX_STEP;
}

static int blinds_pos_to_step(int pos)
{
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
    return ((100 - pos) * BLINDS_POS_MAX_STEP + 50) / 100;
}

void ha_mqtt_publish_blinds(int target, int step)
{
    if (target < 0 || target > 1) return;
    int64_t now = esp_timer_get_time() / 1000;
    /* Debounced: queue the newest step, flush from process_pending once
     * the user stops stepping (the RF link dislikes command bursts). */
    s_blinds_pending[target] = step;
    s_blinds_change_time[target] = now;
    s_blinds_block_until[target] = now + BLINDS_BLOCK_MS;
}

static void blinds_flush_pending(int64_t now)
{
    for (int t = 0; t < 2; t++) {
        if (s_blinds_pending[t] < 0) continue;
        if (now - s_blinds_change_time[t] < BLINDS_SETTLE_MS) continue;

        int step = s_blinds_pending[t];
        if (step == 0 || step == BLINDS_POS_MAX_STEP) {
            /* Endpoints: one broadcast RF burst moves all three motors,
             * and the end-stops re-home the dead reckoning. */
            const char *cmd = (step == 0) ? "OPEN" : "CLOSE";
            esp_mqtt_client_publish(s_client, BLINDS_GROUP_CMD[t], cmd, 0, 0, 0);
            ESP_LOGI(TAG, "Blinds publish: %s = %s", BLINDS_GROUP_CMD[t], cmd);
        } else {
            char payload[8];
            snprintf(payload, sizeof(payload), "%d", blinds_step_to_pos(step));
            for (int m = 0; m < 3; m++)
                esp_mqtt_client_publish(s_client, BLINDS_POS_CMD[t][m],
                                        payload, 0, 0, 0);
            ESP_LOGI(TAG, "Blinds publish: %s x3 = %s",
                     t == 0 ? BGA_ : BGB_, payload);
        }
        s_blinds_pending[t] = -1;
        s_blinds_block_until[t] = now + BLINDS_BLOCK_MS;
    }
}

static void handle_blinds_position(int target, int member,
                                   const char *data, int len)
{
    char msg[16] = {0};
    int n = len < (int)sizeof(msg) - 1 ? len : (int)sizeof(msg) - 1;
    memcpy(msg, data, n);
    msg[n] = '\0';

    char *endp = NULL;
    long pos = strtol(msg, &endp, 10);
    if (endp == msg) return;
    s_blinds_member_pos[target][member] = (int)pos;

    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_blinds_block_until[target] || s_blinds_pending[target] >= 0) {
        ESP_LOGD(TAG, "Blinds msg ignored (authority window)");
        return;
    }

    /* Average over the members we have heard from (retained messages
     * mean all three are usually known right after connecting). */
    int sum = 0, cnt = 0;
    for (int m = 0; m < 3; m++) {
        if (s_blinds_member_pos[target][m] >= 0) {
            sum += s_blinds_member_pos[target][m];
            cnt++;
        }
    }
    if (!cnt) return;

    int step = blinds_pos_to_step(sum / cnt);
    ui_set_blinds_pos(target, step);
    ESP_LOGI(TAG, "Blinds %s position from MQTT: avg %d%% (step %d)",
             target == 0 ? BGA_ : BGB_, sum / cnt, step);
}

void ha_mqtt_publish_volume(int dir)
{
    if (!s_connected || !s_client || dir == 0) return;

    /* Stateless relative control: the stereo is driven over IR, so we
     * only fire UP/DOWN pulses (an automation bridges this topic to NEC
     * codes).  Rate limiting lives in the UI so feedback == command. */
    esp_mqtt_client_publish(s_client, STEREO_VOL_SET,
                            dir > 0 ? "UP" : "DOWN", 0, 0, 0);
    ESP_LOGI(TAG, "Volume publish: %s", dir > 0 ? "UP" : "DOWN");
}

/* ====================================================================== */
/*  Publishing: device settings state                                      */
/* ====================================================================== */

static void publish_device_state(void)
{
    if (!s_connected || !s_client) return;

    wifi_ap_record_t ap_info;
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        rssi = ap_info.rssi;

    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t free_heap = esp_get_free_heap_size() / 1024;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,"
             "\"free_heap\":%lu,"
             "\"backlight\":%d,\"dim_level\":%d,\"idle_timeout\":%d,"
             "\"led_ring\":\"%s\"}",
             rssi, wifi_get_ip_str(), (unsigned long)uptime,
             (unsigned long)free_heap,
             screen_get_backlight(), screen_get_dim_level(),
             screen_get_idle_timeout(),
             led_ring_get_enabled() ? "ON" : "OFF");

    esp_mqtt_client_publish(s_client, s_state_topic, payload, 0, 0, 0);
}

/* ====================================================================== */
/*  Incoming message handling                                              */
/* ====================================================================== */

static void handle_fan_message(const char *data, int len)
{
    char msg[32] = {0};
    int n = len < (int)sizeof(msg) - 1 ? len : (int)sizeof(msg) - 1;
    memcpy(msg, data, n);
    msg[n] = '\0';

    int new_level = ui_get_fan_level();
    if (str_eq_icase(msg, "Off"))         new_level = 0;
    else if (str_eq_icase(msg, "low"))    new_level = 1;
    else if (str_eq_icase(msg, "medium")) new_level = 2;
    else if (str_eq_icase(msg, "high"))   new_level = 3;

    /* Echo suppression: if we recently published this same value, ignore */
    int64_t now = esp_timer_get_time() / 1000;
    if (s_last_fan_pub_time > 0
        && now - s_last_fan_pub_time < 1000
        && new_level == s_last_pub_fan) {
        ESP_LOGD(TAG, "Suppressing fan echo (%s)", msg);
        return;
    }

    if (new_level != ui_get_fan_level()) {
        s_last_pub_fan = new_level;
        ui_set_fan_level(new_level);
        ESP_LOGI(TAG, "Fan state from MQTT: %s (level %d)", msg, new_level);
    }
}

static void handle_light_message(const char *data, int len)
{
    char msg[256] = {0};
    int n = len < (int)sizeof(msg) - 1 ? len : (int)sizeof(msg) - 1;
    memcpy(msg, data, n);
    msg[n] = '\0';

    char state_str[8] = {0};
    int br = -1;
    bool have_state = parse_json_string(msg, "state", state_str, sizeof(state_str));
    bool have_br = parse_json_int(msg, "brightness", &br);
    bool state_on = have_state && str_eq_icase(state_str, "ON");

    int64_t now = esp_timer_get_time() / 1000;
    int new_level = light_sync_on_state_msg(&s_light_sync, now,
                                            have_state, state_on,
                                            have_br, br,
                                            ui_get_brightness());
    if (new_level < 0) {
        ESP_LOGD(TAG, "Light msg ignored (raw br=%d state=%s)", br, state_str);
        return;
    }

    ui_set_brightness(new_level);
    ESP_LOGI(TAG, "Light from MQTT: %d%% (raw br=%d state=%s)",
             new_level, br, state_str);
}

static void handle_device_command(const char *subtopic, const char *data, int len)
{
    char msg[64] = {0};
    int n = len < (int)sizeof(msg) - 1 ? len : (int)sizeof(msg) - 1;
    memcpy(msg, data, n);
    msg[n] = '\0';

    if (strcmp(subtopic, "restart") == 0) {
        ESP_LOGW(TAG, "Restart command received");
        esp_restart();
    } else if (strcmp(subtopic, "backlight") == 0) {
        int val = atoi(msg);
        screen_set_backlight(val);
        ESP_LOGI(TAG, "Backlight set to %d", val);
        publish_device_state();
    } else if (strcmp(subtopic, "dim_level") == 0) {
        int val = atoi(msg);
        screen_set_dim_level(val);
        ESP_LOGI(TAG, "Dim level set to %d", val);
        publish_device_state();
    } else if (strcmp(subtopic, "idle_timeout") == 0) {
        int val = atoi(msg);
        screen_set_idle_timeout(val);
        ESP_LOGI(TAG, "Idle timeout set to %d s", val);
        publish_device_state();
    } else if (strcmp(subtopic, "led_ring") == 0) {
        bool on = str_eq_icase(msg, "ON") || strcmp(msg, "1") == 0;
        led_ring_set_enabled(on);
        ESP_LOGI(TAG, "LED ring %s", on ? "ON" : "OFF");
        publish_device_state();
    }
}

/* ====================================================================== */
/*  HA discovery                                                           */
/* ====================================================================== */

static void publish_ha_discovery(void)
{
    char topic[128];
    char payload[700];

    const esp_app_desc_t *app = esp_app_get_description();
    char dev_json[256];
    snprintf(dev_json, sizeof(dev_json),
        "\"dev\":{"
            "\"ids\":[\"%s\"],"
            "\"name\":\"Nest Hub\","
            "\"mf\":\"Panlee\","
            "\"mdl\":\"ZX2D10GE01R-V4848\","
            "\"sw\":\"%s\","
            "\"cu\":\"http://%s/\""
        "}",
        s_device_id, app->version, wifi_get_ip_str());

    /* --- Diagnostic sensors --- */

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/rssi/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"WiFi Signal\","
         "\"uniq_id\":\"%s_rssi\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.rssi }}\","
         "\"unit_of_meas\":\"dBm\","
         "\"dev_cla\":\"signal_strength\","
         "\"ent_cat\":\"diagnostic\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/ip/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"IP Address\","
         "\"uniq_id\":\"%s_ip\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.ip }}\","
         "\"icon\":\"mdi:ip-network\","
         "\"ent_cat\":\"diagnostic\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/uptime/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Uptime\","
         "\"uniq_id\":\"%s_uptime\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.uptime }}\","
         "\"unit_of_meas\":\"s\","
         "\"icon\":\"mdi:timer-outline\","
         "\"ent_cat\":\"diagnostic\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/heap/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Free Heap\","
         "\"uniq_id\":\"%s_heap\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.free_heap }}\","
         "\"unit_of_meas\":\"KB\","
         "\"icon\":\"mdi:memory\","
         "\"ent_cat\":\"diagnostic\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    /* --- Restart button --- */

    snprintf(topic, sizeof(topic),
             "homeassistant/button/%s/restart/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Restart\","
         "\"uniq_id\":\"%s_restart\","
         "\"cmd_t\":\"%s/cmd/restart\","
         "\"icon\":\"mdi:restart\","
         "\"ent_cat\":\"config\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_base_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    /* --- Screen Backlight (number, config) --- */

    snprintf(topic, sizeof(topic),
             "homeassistant/number/%s/backlight/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Screen Brightness\","
         "\"uniq_id\":\"%s_backlight\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.backlight }}\","
         "\"cmd_t\":\"%s/cmd/backlight\","
         "\"min\":1,\"max\":100,\"step\":5,"
         "\"unit_of_meas\":\"%%\","
         "\"icon\":\"mdi:brightness-6\","
         "\"ent_cat\":\"config\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_base_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    /* --- Dim Level (number, config) --- */

    snprintf(topic, sizeof(topic),
             "homeassistant/number/%s/dim_level/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Dim Brightness\","
         "\"uniq_id\":\"%s_dim_level\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.dim_level }}\","
         "\"cmd_t\":\"%s/cmd/dim_level\","
         "\"min\":0,\"max\":100,\"step\":5,"
         "\"unit_of_meas\":\"%%\","
         "\"icon\":\"mdi:brightness-4\","
         "\"ent_cat\":\"config\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_base_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    /* --- Idle Timeout (number, config) --- */

    snprintf(topic, sizeof(topic),
             "homeassistant/number/%s/idle_timeout/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Idle Timeout\","
         "\"uniq_id\":\"%s_idle_timeout\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.idle_timeout }}\","
         "\"cmd_t\":\"%s/cmd/idle_timeout\","
         "\"min\":0,\"max\":600,\"step\":5,"
         "\"unit_of_meas\":\"s\","
         "\"icon\":\"mdi:timer-sand\","
         "\"ent_cat\":\"config\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_base_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    /* --- LED Ring (switch, config) --- */

    snprintf(topic, sizeof(topic),
             "homeassistant/switch/%s/led_ring/config", s_device_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"LED Ring\","
         "\"uniq_id\":\"%s_led_ring\","
         "\"stat_t\":\"%s\","
         "\"val_tpl\":\"{{ value_json.led_ring }}\","
         "\"cmd_t\":\"%s/cmd/led_ring\","
         "\"icon\":\"mdi:led-on\","
         "\"ent_cat\":\"config\","
         "\"avty_t\":\"%s\",%s}",
        s_device_id, s_state_topic, s_base_topic, s_avail_topic, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    /* --- Availability --- */

    esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, 1);

    ESP_LOGI(TAG, "HA discovery published for '%s'", s_device_id);
}

/* ====================================================================== */
/*  MQTT event handler                                                     */
/* ====================================================================== */

static void subscribe_all(void)
{
    esp_mqtt_client_subscribe(s_client, FAN_TOPIC, 1);
    esp_mqtt_client_subscribe(s_client, Z2M_LIGHT_TOPIC, 1);
    for (int t = 0; t < 2; t++)
        for (int m = 0; m < 3; m++)
            esp_mqtt_client_subscribe(s_client, BLINDS_POS_STATE[t][m], 1);
    esp_mqtt_client_subscribe(s_client, "homeassistant/status", 1);

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/cmd/#", s_base_topic);
    esp_mqtt_client_subscribe(s_client, topic, 1);

    ESP_LOGI(TAG, "Subscribed to fan, light, HA status, and command topics");
}

static void request_current_state(void)
{
    /* Z2M replies on the state topic with the full state incl. brightness */
    esp_mqtt_client_publish(s_client, Z2M_LIGHT_GET,
                            "{\"state\":\"\"}", 0, 0, 0);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        publish_ha_discovery();
        subscribe_all();
        light_sync_init(&s_light_sync);
        request_current_state();
        publish_device_state();
        s_last_pub_fan = -1;
        s_last_fan_pub_time = 0;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        if (event->topic_len <= 0) break;
        char topic_buf[128];
        int tlen = event->topic_len < (int)sizeof(topic_buf) - 1
                   ? event->topic_len : (int)sizeof(topic_buf) - 1;
        memcpy(topic_buf, event->topic, tlen);
        topic_buf[tlen] = '\0';

        if (strcmp(topic_buf, FAN_TOPIC) == 0) {
            handle_fan_message(event->data, event->data_len);
        } else if (strcmp(topic_buf, Z2M_LIGHT_TOPIC) == 0) {
            handle_light_message(event->data, event->data_len);
        } else if (strncmp(topic_buf, BL_ "/", sizeof(BL_ "/") - 1) == 0) {
            for (int t = 0; t < 2; t++)
                for (int m = 0; m < 3; m++)
                    if (strcmp(topic_buf, BLINDS_POS_STATE[t][m]) == 0)
                        handle_blinds_position(t, m, event->data, event->data_len);
        } else if (strcmp(topic_buf, "homeassistant/status") == 0) {
            char status[16] = {0};
            int slen = event->data_len < (int)sizeof(status) - 1
                       ? event->data_len : (int)sizeof(status) - 1;
            memcpy(status, event->data, slen);
            if (strcmp(status, "online") == 0) {
                ESP_LOGI(TAG, "HA birth message received, re-publishing discovery");
                publish_ha_discovery();
                publish_device_state();
            }
        } else {
            char cmd_prefix[64];
            snprintf(cmd_prefix, sizeof(cmd_prefix), "%s/cmd/", s_base_topic);
            if (strncmp(topic_buf, cmd_prefix, strlen(cmd_prefix)) == 0) {
                const char *subtopic = topic_buf + strlen(cmd_prefix);
                handle_device_command(subtopic, event->data, event->data_len);
            }
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

/* ====================================================================== */
/*  Public API                                                             */
/* ====================================================================== */

esp_err_t ha_mqtt_init(void)
{
    build_device_id();
    build_topics();
    light_sync_init(&s_light_sync);
    s_mqtt_event_group = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.last_will.topic = s_avail_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started, connecting to %s", CONFIG_MQTT_BROKER_URI);

    EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group,
        MQTT_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    return (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

void ha_mqtt_process_pending(void)
{
    if (!s_connected || !s_client) return;

    int64_t now = esp_timer_get_time() / 1000;
    int brightness;
    switch (light_sync_tick(&s_light_sync, now, &brightness)) {
    case LIGHT_SYNC_ACT_PUBLISH:
        publish_light_set(brightness);
        break;
    case LIGHT_SYNC_ACT_RESYNC:
        ESP_LOGD(TAG, "Authority window closed, requesting light state");
        request_current_state();
        break;
    default:
        break;
    }

    blinds_flush_pending(now);
}

void ha_mqtt_publish_state(void)
{
    publish_device_state();
}

bool ha_mqtt_is_connected(void)
{
    return s_connected;
}
