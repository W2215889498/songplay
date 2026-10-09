idf_component_register(
    SRCS "xc_net.c" "xc_mfi.c" "xc_mdns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_i2c mdns lwip xcertplay_core waveshare__esp32_p4_wifi6_touch_lcd_4_3
)
