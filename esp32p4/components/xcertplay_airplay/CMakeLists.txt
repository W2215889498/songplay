idf_component_register(
    SRCS "xc_airplay.c" "xc_bplist.c" "xc_bplist_read.c" "xc_video.c" "xc_audio.c"
         "xc_callcard.c" "xc_cjk16.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_wifi xcertplay_core xcertplay_platform xcertplay_bootstrap lvgl
             openh264_dec espressif__esp_h264 espressif__esp_codec_dev
             espressif__esp_audio_codec esp_driver_ppa
             waveshare__esp32_p4_wifi6_touch_lcd_4_3
)
