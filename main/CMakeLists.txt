idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer esp_wifi esp_event esp_netif nvs_flash button mt8901
)
