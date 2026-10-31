idf_component_register(SRCS "main.c" "cam_reg.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif driver esp_psram
                                esp32-camera mqtt mbedtls esp_timer esp-tls)
