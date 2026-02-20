#include "ha_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "wifi.h"
#include "ui.h"

static const char *TAG = "ha_mqtt";

#define MQTT_CONNECTED_BIT BIT0

static const char *FAN_TOPIC         = "bashouse/livingroom-fan";
static const char *Z2M_LIGHT_TOPIC   = "zigbee2mqtt/livingroom_lights";
static const char *Z2M_LIGHT_SET     = "zigbee2mqtt/livingroom_lights/set";
static const char *Z2M_LIGHT_GET     = "zigbee2mqtt/livingroom_lights/get";

#define LIGHT_THROTTLE_MS   300
#define ECHO_SUPPRESS_MS    800

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t s_mqtt_event_group = NULL;
static bool s_connected = false;
static char s_device_id[16] = {0};

static int s_last_pub_brightness = -1;
static int s_last_pub_fan = -1;
static int64_t s_ignore_light_until = 0;
static int64_t s_last_light_pub_time = 0;
static bool s_light_pub_pending = false;
static TimerHandle_t s_light_timer = NULL;

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
/*  Publishing                                                             */
/* ====================================================================== */

static void publish_light_now(void)
{
    if (!s_connected || !s_client) return;
    int brightness = ui_get_brightness();
    if (brightness == s_last_pub_brightness) {
        s_light_pub_pending = false;
        return;
    }

    const char *state = (brightness == 0) ? "OFF" : "ON";
    int raw = (brightness * 255 + 50) / 100;
    if (raw > 255) raw = 255;

    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"brightness\":%d}", state, raw);
    esp_mqtt_client_publish(s_client, Z2M_LIGHT_SET, payload, 0, 0, 0);

    s_last_pub_brightness = brightness;
    s_last_light_pub_time = esp_timer_get_time() / 1000;
    s_ignore_light_until = s_last_light_pub_time + ECHO_SUPPRESS_MS;
    s_light_pub_pending = false;

    ESP_LOGI(TAG, "Light publish: %s", payload);
}

static void light_timer_cb(TimerHandle_t t)
{
    if (s_light_pub_pending) {
        publish_light_now();
    }
}

void ha_mqtt_publish_light(void)
{
    if (!s_connected) return;
    int brightness = ui_get_brightness();
    if (brightness == s_last_pub_brightness) {
        s_light_pub_pending = false;
        return;
    }

    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_light_pub_time >= LIGHT_THROTTLE_MS) {
        publish_light_now();
    } else {
        s_light_pub_pending = true;
        if (s_light_timer) {
            xTimerChangePeriod(s_light_timer,
                pdMS_TO_TICKS(LIGHT_THROTTLE_MS), 0);
            xTimerStart(s_light_timer, 0);
        }
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
    ESP_LOGI(TAG, "Fan publish: %s", labels[level]);
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
    if (strcmp(msg, "Off") == 0)        new_level = 0;
    else if (strcmp(msg, "low") == 0)   new_level = 1;
    else if (strcmp(msg, "medium") == 0) new_level = 2;
    else if (strcmp(msg, "high") == 0)  new_level = 3;

    if (new_level != ui_get_fan_level()) {
        s_last_pub_fan = new_level;
        ui_set_fan_level(new_level);
        ESP_LOGI(TAG, "Fan state from MQTT: %s (level %d)", msg, new_level);
    }
}

static void handle_light_message(const char *data, int len)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_ignore_light_until || s_light_pub_pending) {
        ESP_LOGD(TAG, "Ignoring light echo");
        return;
    }

    char msg[256] = {0};
    int n = len < (int)sizeof(msg) - 1 ? len : (int)sizeof(msg) - 1;
    memcpy(msg, data, n);
    msg[n] = '\0';

    char state_str[8] = {0};
    int br = -1;
    bool have_state = parse_json_string(msg, "state", state_str, sizeof(state_str));
    bool have_br = parse_json_int(msg, "brightness", &br);

    int new_level = ui_get_brightness();

    if (have_br) {
        if (br < 0) br = 0;
        if (br > 255) br = 255;
        new_level = (br == 0) ? 0 : (int)((br * 100 + 127) / 255);
    }

    if (have_state) {
        if (str_eq_icase(state_str, "OFF")) {
            new_level = 0;
        } else if (str_eq_icase(state_str, "ON") && !have_br && new_level == 0) {
            new_level = 100;
        }
    }

    if (new_level != ui_get_brightness()) {
        s_last_pub_brightness = new_level;
        ui_set_brightness(new_level);
        ESP_LOGI(TAG, "Light from MQTT: %d%% (raw br=%d state=%s)",
                 new_level, br, state_str);
    }
}

static void handle_command(const char *topic, const char *data, int data_len)
{
    if (strstr(topic, "cmd/restart")) {
        ESP_LOGW(TAG, "Restart command received");
        esp_restart();
    }
}

/* ====================================================================== */
/*  HA discovery                                                           */
/* ====================================================================== */

static void publish_ha_discovery(void)
{
    char topic[128];
    char payload[512];

    const char *dev_block =
        "\"dev\":{"
            "\"ids\":[\"%s\"],"
            "\"name\":\"Nest Hub\","
            "\"mf\":\"Panlee\","
            "\"mdl\":\"ZX2D10GE01R-V4848\","
            "\"sw\":\"%s\""
        "}";

    const esp_app_desc_t *app = esp_app_get_description();
    char dev_json[256];
    snprintf(dev_json, sizeof(dev_json), dev_block, s_device_id, app->version);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/rssi/config", s_device_id);
    snprintf(payload, sizeof(payload),
             "{"
                "\"name\":\"WiFi Signal\","
                "\"uniq_id\":\"%s_rssi\","
                "\"stat_t\":\"espnest/%s/state\","
                "\"val_tpl\":\"{{ value_json.rssi }}\","
                "\"unit_of_meas\":\"dBm\","
                "\"dev_cla\":\"signal_strength\","
                "\"ent_cat\":\"diagnostic\","
                "\"avty_t\":\"espnest/%s/avail\","
                "%s"
             "}", s_device_id, s_device_id, s_device_id, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/ip/config", s_device_id);
    snprintf(payload, sizeof(payload),
             "{"
                "\"name\":\"IP Address\","
                "\"uniq_id\":\"%s_ip\","
                "\"stat_t\":\"espnest/%s/state\","
                "\"val_tpl\":\"{{ value_json.ip }}\","
                "\"icon\":\"mdi:ip-network\","
                "\"ent_cat\":\"diagnostic\","
                "\"avty_t\":\"espnest/%s/avail\","
                "%s"
             "}", s_device_id, s_device_id, s_device_id, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/uptime/config", s_device_id);
    snprintf(payload, sizeof(payload),
             "{"
                "\"name\":\"Uptime\","
                "\"uniq_id\":\"%s_uptime\","
                "\"stat_t\":\"espnest/%s/state\","
                "\"val_tpl\":\"{{ value_json.uptime }}\","
                "\"unit_of_meas\":\"s\","
                "\"icon\":\"mdi:timer-outline\","
                "\"ent_cat\":\"diagnostic\","
                "\"avty_t\":\"espnest/%s/avail\","
                "%s"
             "}", s_device_id, s_device_id, s_device_id, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/button/%s/restart/config", s_device_id);
    snprintf(payload, sizeof(payload),
             "{"
                "\"name\":\"Restart\","
                "\"uniq_id\":\"%s_restart\","
                "\"cmd_t\":\"espnest/%s/cmd/restart\","
                "\"icon\":\"mdi:restart\","
                "\"ent_cat\":\"config\","
                "\"avty_t\":\"espnest/%s/avail\","
                "%s"
             "}", s_device_id, s_device_id, s_device_id, dev_json);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "espnest/%s/avail", s_device_id);
    esp_mqtt_client_publish(s_client, topic, "online", 0, 1, 1);

    ESP_LOGI(TAG, "HA discovery published for '%s'", s_device_id);
}

/* ====================================================================== */
/*  MQTT event handler                                                     */
/* ====================================================================== */

static void subscribe_all(void)
{
    esp_mqtt_client_subscribe(s_client, FAN_TOPIC, 1);
    esp_mqtt_client_subscribe(s_client, Z2M_LIGHT_TOPIC, 1);

    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "espnest/%s/cmd/#", s_device_id);
    esp_mqtt_client_subscribe(s_client, cmd_topic, 1);

    ESP_LOGI(TAG, "Subscribed to fan, light, and command topics");
}

static void request_current_state(void)
{
    esp_mqtt_client_publish(s_client, Z2M_LIGHT_GET,
                            "{\"state\":\"\"}", 0, 0, 0);
    esp_mqtt_client_publish(s_client, Z2M_LIGHT_GET,
                            "{\"brightness\":\"\"}", 0, 0, 0);
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
        request_current_state();
        s_last_pub_brightness = -1;
        s_last_pub_fan = -1;
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
        } else {
            handle_command(topic_buf, event->data, event->data_len);
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
    s_mqtt_event_group = xEventGroupCreate();

    s_light_timer = xTimerCreate("light_pub", pdMS_TO_TICKS(LIGHT_THROTTLE_MS),
                                  pdFALSE, NULL, light_timer_cb);

    char lwt_topic[64];
    snprintf(lwt_topic, sizeof(lwt_topic), "espnest/%s/avail", s_device_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.last_will.topic = lwt_topic,
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

void ha_mqtt_publish_state(void)
{
    if (!s_connected || !s_client) return;

    wifi_ap_record_t ap_info;
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    char topic[64];
    snprintf(topic, sizeof(topic), "espnest/%s/state", s_device_id);

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu}",
             rssi, wifi_get_ip_str(), (unsigned long)uptime);

    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
}

bool ha_mqtt_is_connected(void)
{
    return s_connected;
}
