#include "ha_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "wifi.h"

static const char *TAG = "ha_mqtt";

#define MQTT_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t s_mqtt_event_group = NULL;
static bool s_connected = false;
static char s_device_id[16] = {0};

static void build_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "esp_%02x%02x%02x",
             mac[3], mac[4], mac[5]);
}

static void publish_ha_discovery(void)
{
    char topic[128];
    char payload[512];

    const char *dev_block =
        "\"dev\":{"
            "\"ids\":[\"%s\"],"
            "\"name\":\"ESP Nest\","
            "\"mf\":\"Panlee\","
            "\"mdl\":\"ZX2D10GE01R-V4848\","
            "\"sw\":\"%s\""
        "}";

    const esp_app_desc_t *app = esp_app_get_description();
    char dev_json[256];
    snprintf(dev_json, sizeof(dev_json), dev_block, s_device_id, app->version);

    /* WiFi RSSI sensor */
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

    /* IP address sensor */
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

    /* Uptime sensor */
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

    /* OTA update button */
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

    /* Publish availability */
    snprintf(topic, sizeof(topic), "espnest/%s/avail", s_device_id);
    esp_mqtt_client_publish(s_client, topic, "online", 0, 1, 1);

    ESP_LOGI(TAG, "HA discovery published for device '%s'", s_device_id);
}

static void subscribe_to_commands(void)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "espnest/%s/cmd/#", s_device_id);
    esp_mqtt_client_subscribe(s_client, topic, 1);
    ESP_LOGI(TAG, "Subscribed to %s", topic);
}

static void handle_command(const char *topic, const char *data, int data_len)
{
    char cmd_restart[128];
    snprintf(cmd_restart, sizeof(cmd_restart), "espnest/%s/cmd/restart", s_device_id);

    if (strstr(topic, "cmd/restart")) {
        ESP_LOGW(TAG, "Restart command received, restarting...");
        esp_restart();
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        s_connected = true;
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        publish_ha_discovery();
        subscribe_to_commands();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA:
        if (event->topic_len > 0 && event->data_len >= 0) {
            char topic_buf[128];
            int len = event->topic_len < (int)sizeof(topic_buf) - 1
                      ? event->topic_len : (int)sizeof(topic_buf) - 1;
            memcpy(topic_buf, event->topic, len);
            topic_buf[len] = '\0';
            handle_command(topic_buf, event->data, event->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

esp_err_t ha_mqtt_init(void)
{
    build_device_id();
    s_mqtt_event_group = xEventGroupCreate();

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

    ESP_LOGI(TAG, "MQTT client started, connecting to %s ...", CONFIG_MQTT_BROKER_URI);

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
