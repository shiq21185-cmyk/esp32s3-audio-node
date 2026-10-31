idf_component_register(
    SRCS "main.c" "wifi_stream.c" "net_push.c" "mqtt_cam.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash driver esp_psram esp_timer esp32-camera
             esp_wifi esp_netif esp_http_server esp_event lwip mqtt mbedtls)
