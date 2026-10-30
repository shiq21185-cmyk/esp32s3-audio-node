idf_component_register(SRCS "main.c" "codec.c" "oled.c" "ui.c" "font5x7.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif driver mqtt mbedtls esp_audio_codec esp_lcd)
