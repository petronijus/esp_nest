#include "wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"

static const char *TAG = "wifi";

#define WIFI_SSID       CONFIG_WIFI_SSID
#define WIFI_PASS       CONFIG_WIFI_PASSWORD

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#define INITIAL_MAX_RETRY   10
#define RECONNECT_DELAY_MS  5000

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_connected = false;
static bool s_initial_connect_done = false;
static char s_ip_str[16] = "0.0.0.0";

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected (reason %d)", disconn->reason);
        s_connected = false;

        if (!s_initial_connect_done) {
            if (s_retry_num < INITIAL_MAX_RETRY) {
                s_retry_num++;
                ESP_LOGI(TAG, "Initial connect retry %d/%d...",
                         s_retry_num, INITIAL_MAX_RETRY);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGW(TAG, "Initial connection failed after %d retries",
                         INITIAL_MAX_RETRY);
            }
        } else {
            s_retry_num++;
            int delay = RECONNECT_DELAY_MS;
            if (s_retry_num > 30) delay = 30000;
            else if (s_retry_num > 10) delay = 10000;
            ESP_LOGI(TAG, "Reconnecting (attempt %d, backoff %dms)...",
                     s_retry_num, delay);
            vTaskDelay(pdMS_TO_TICKS(delay));
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));

        if (s_retry_num > 0) {
            ESP_LOGI(TAG, "Reconnected after %d retries. IP: %s",
                     s_retry_num, s_ip_str);
        } else {
            ESP_LOGI(TAG, "Connected. IP: %s", s_ip_str);
        }

        s_retry_num = 0;
        s_connected = true;
        s_initial_connect_done = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init_sta(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected to %s", WIFI_SSID);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to %s", WIFI_SSID);
        s_initial_connect_done = true;
        esp_wifi_connect();
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Connection timed out");
        s_initial_connect_done = true;
        esp_wifi_connect();
        return ESP_FAIL;
    }
}

const char *wifi_get_ip_str(void)
{
    return s_ip_str;
}

bool wifi_is_connected(void)
{
    return s_connected;
}
