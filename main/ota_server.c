#include "ota_server.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_system.h"

static const char *TAG = "ota_srv";

static const char *OTA_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP Nest OTA</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:20px;"
    "background:#1a1a2e;color:#e0e0e0;text-align:center}"
    "h1{color:#0f9b8e}input[type=file]{margin:20px 0;color:#e0e0e0}"
    "button{background:#0f9b8e;color:#fff;border:none;padding:12px 32px;"
    "font-size:16px;border-radius:6px;cursor:pointer}"
    "button:hover{background:#0d8377}"
    "#prog{display:none;margin:20px auto;width:100%%}"
    "</style></head><body>"
    "<h1>ESP Nest OTA Update</h1>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'><br>"
    "<button type='submit'>Upload Firmware</button>"
    "</form>"
    "<progress id='prog' max='100' value='0'></progress>"
    "</body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, OTA_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA target partition: %s (offset 0x%lx, size 0x%lx)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Alloc failed");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int received = 0;
    bool header_skipped = false;

    ESP_LOGI(TAG, "Receiving OTA firmware (%d bytes)...", remaining);

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Receive error");
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        char *data = buf;
        int data_len = recv_len;

        /* Skip multipart form-data header on first chunk */
        if (!header_skipped) {
            char *body = strstr(buf, "\r\n\r\n");
            if (body) {
                body += 4;
                data = body;
                data_len = recv_len - (body - buf);
                header_skipped = true;
            }
        }

        if (header_skipped && data_len > 0) {
            err = esp_ota_write(ota_handle, data, data_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
                free(buf);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                return ESP_FAIL;
            }
            received += data_len;
        }

        remaining -= recv_len;
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success! Received %d bytes. Restarting...", received);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='background:#1a1a2e;color:#0f9b8e;text-align:center;"
        "font-family:sans-serif;padding-top:80px'>"
        "<h1>OTA Update Successful!</h1><p>Restarting device...</p>"
        "</body></html>");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_post_handler,
    };
    httpd_register_uri_handler(server, &update);

    ESP_LOGI(TAG, "OTA server started on port %d", config.server_port);
    return ESP_OK;
}
