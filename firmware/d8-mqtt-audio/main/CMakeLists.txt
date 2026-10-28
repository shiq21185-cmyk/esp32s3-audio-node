idf_component_register(SRCS "main.c" "codec.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif driver mqtt mbedtls esp_audio_codec)
