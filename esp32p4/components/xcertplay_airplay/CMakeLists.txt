idf_component_register(
    SRCS "xc_airplay.c" "xc_bplist.c" "xc_bplist_read.c" "xc_video.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_wifi xcertplay_core xcertplay_platform lvgl
             espressif__esp_h264 waveshare__esp32_p4_wifi6_touch_lcd_4_3
)
