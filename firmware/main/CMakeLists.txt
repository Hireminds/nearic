idf_component_register(
    SRCS "main.c" "display.c" "network.c" "provisioning.c" "wifi_settings.c" "device_access.c"
         "device_https.c" "device_bootstrap.c" "service_sync.c" "update.c" "update_journal.c" "update_download.c"
         "clock_sync.c" "roughtime_packet.c"
    PRIV_REQUIRES app_update bootloader_support esp_app_format esp_event esp_hw_support
                  esp_http_client esp_http_server esp_netif esp_timer esp_wifi esphome__libsodium
                  json lwip mbedtls nvs_flash
)
