idf_component_register(
    SRC_DIRS "." "assets"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer esp_wifi esp_event esp_netif nvs_flash
             mqtt esp_http_server app_update
             led_strip button mt8901
)
