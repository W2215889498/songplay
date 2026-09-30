idf_component_register(
    SRCS "xc_bt_link.c" "xc_iap2.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer xcertplay_core xcertplay_platform
)
