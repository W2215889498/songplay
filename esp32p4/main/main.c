/*
 * Xcertplay - wireless CarPlay receiver for the ESP32-P4-WIFI6-Touch-LCD-4.3 board.
 *
 * M1: display + LVGL shell, ESP-Hosted Wi-Fi softAP from the C6, MFi
 * coprocessor discovery on the board I2C bus, and the crypto self-test.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "xc_selftest.h"
#include "xc_net.h"
#include "xc_mfi.h"
#include "xc_mdns.h"
#include "xc_airplay.h"
#include "xc_identity.h"
#include "xc_video.h"
#include "xc_iap2.h"

static const char *TAG = "xcertplay";

static lv_obj_t *s_status;

static void set_status_tx(const char *text)
{
    bsp_display_lock(-1);
    lv_label_set_text(s_status, text);
    bsp_display_unlock();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Xcertplay wireless CarPlay starting (ESP-IDF %s)", esp_get_idf_version());
    ESP_LOGI(TAG, "free PSRAM: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "display initialization failed");
        return;
    }
    bsp_display_backlight_on();

    bsp_display_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Xcertplay");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Wireless CarPlay for ESP32-P4");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9AA0A6), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 66);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "starting...");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status, 440);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 0);
    bsp_display_unlock();

    esp_err_t ap_err = xc_net_start_ap();

    esp_err_t mdns_err = ESP_FAIL;
    if (ap_err == ESP_OK) {
        xc_identity_init();
        mdns_err = xc_mdns_start();
        if (mdns_err == ESP_OK) {
            xc_airplay_start();
        }
    }

    xc_mfi_scan_result_t mfi = { 0 };
    xc_mfi_scan(&mfi);

    esp_err_t mfi_auth = ESP_FAIL;
    if (mfi.detected && xc_mfi_open(mfi.address) == ESP_OK) {
        mfi_auth = xc_mfi_selftest();
    }

    bool crypto_ok = xc_selftest_run();

    char line[320];    snprintf(line, sizeof(line),
             "Wi-Fi AP: %s\npassword: %s\nmDNS: %s\nMFi: %s (%s)\ncrypto: %s",
             ap_err == ESP_OK ? XC_AP_SSID : "FAILED",
             XC_AP_PASSWORD,
             mdns_err == ESP_OK ? "_airplay._tcp up" : "FAILED",
             mfi.detected ? "detected" : "not found",
             mfi_auth == ESP_OK ? "auth selftest ok" : "auth test failed",
             crypto_ok ? "passed" : "FAILED");
    set_status_tx(line);

    /* M6 bring-up check: encode a colour-bar frame in hardware, decode it in
     * software and draw it, proving the whole video path works. */
    xc_video_lcd_selftest();

    /* Wireless bootstrap: talk iAP2 to the phone over the classic-BT bridge so
     * it joins our hotspot and starts CarPlay. */
    if (xc_iap2_start() != ESP_OK) {
        ESP_LOGE(TAG, "iAP2 bootstrap failed to start");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive; free PSRAM: %u bytes",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
