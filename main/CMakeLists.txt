idf_component_register(
    SRCS "main.c" "tusb_desc.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip esp_tinyusb esp_timer )
