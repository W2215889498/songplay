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
#include "lvgl_private.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "xc_selftest.h"
#include "xc_net.h"
#include "xc_mfi.h"
#include "xc_mdns.h"
#include "xc_airplay.h"
#include "xc_identity.h"
#include "xc_video.h"
#include "xc_audio.h"
#include "xc_iap2.h"

static const char *TAG = "xcertplay";

static lv_obj_t *s_status;

/* ---- flicker diagnostics (temporary) ---------------------------------- */
static lv_display_flush_cb_t s_lvgl_flush_cb;

static void flush_probe_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ESP_LOGI(TAG, "lvgl flush area (%d,%d)-(%d,%d)", (int)area->x1, (int)area->y1,
             (int)area->x2, (int)area->y2);
    if (s_lvgl_flush_cb) {
        s_lvgl_flush_cb(disp, area, px_map);
    }
}

static void disp_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_REQUEST:
        ESP_LOGI(TAG, "lvgl: refresh requested");
        break;
    case LV_EVENT_RENDER_START:
        ESP_LOGI(TAG, "lvgl: render start");
        break;
    case LV_EVENT_RENDER_READY:
        ESP_LOGI(TAG, "lvgl: render ready");
        break;
    default:
        break;
    }
}

static void set_status_tx(const char *text)
{
    bsp_display_lock(-1);
    lv_label_set_text(s_status, text);
    bsp_display_unlock();
}

typedef struct {
    esp_err_t ap_err;
    esp_err_t mdns_err;
    xc_mfi_scan_result_t mfi;
    esp_err_t mfi_auth;
} selftest_ctx_t;

static selftest_ctx_t s_selftest_ctx;

/* Plan B: bring the Wi-Fi AP up from its own task, started as early as
 * possible. esp_wifi_init is what kicks off the C6 slave firmware boot, so
 * running it here lets the ~1.5 s C6 boot overlap the rest of the startup
 * (MFi, iAP2, display) instead of following it. mDNS, the AirPlay stack and
 * the iAP2 AP-ready notification follow in the same task. */
static void net_task(void *arg)
{
    (void)arg;
    esp_err_t ap_err = xc_net_start_ap();
    s_selftest_ctx.ap_err = ap_err;
    esp_err_t mdns_err = ESP_FAIL;
    if (ap_err == ESP_OK) {
        mdns_err = xc_mdns_start();
        if (mdns_err == ESP_OK) {
            xc_airplay_start();
        }
        xc_iap2_notify_ap_ready();
    }
    s_selftest_ctx.mdns_err = mdns_err;
    vTaskDelete(NULL);
}

/* 9.3 item 2: heavy boot checks off the critical path. They run 10 s after
 * boot (so the phone's BT handshake is never blocked by them) and are skipped
 * entirely once the phone has started a CarPlay session. The MFi certificate
 * cache read stays at boot -- it must happen before the first sign operation
 * and is cheap -- so only the crypto and video checks live here. */
static void selftest_task(void *arg)
{
    selftest_ctx_t *ctx = arg;
    vTaskDelay(pdMS_TO_TICKS(10000));
    if (xc_airplay_session_started()) {
        ESP_LOGI(TAG, "boot self-tests skipped (phone session already running)");
        vTaskDelete(NULL);
        return;
    }
    bool crypto_ok = xc_selftest_run();
    char line[320];
    snprintf(line, sizeof(line),
             "Wi-Fi AP: %s\npassword: %s\nmDNS: %s\nMFi: %s (%s)\ncrypto: %s",
             ctx->ap_err == ESP_OK ? XC_AP_SSID : "FAILED", XC_AP_PASSWORD,
             ctx->mdns_err == ESP_OK ? "_airplay._tcp up" : "FAILED",
             ctx->mfi.detected ? "detected" : "not found",
             ctx->mfi_auth == ESP_OK ? "auth selftest ok" : "auth test failed",
             crypto_ok ? "passed" : "FAILED");
    set_status_tx(line);
    /* M6 bring-up check: encode a colour-bar frame in hardware, decode it in
     * software and draw it, proving the whole video path works. */
    xc_video_lcd_selftest();
    vTaskDelete(NULL);
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

    /* Plan A: bring up the BT/iAP2 bootstrap FIRST. It only needs UART1 (to
     * the bridge) and the bit-banged MFi I2C, so the whole phone handshake
     * (marker -> identification -> MFi -> CarPlayStartSession) runs in
     * parallel with the C6/Wi-Fi boot instead of after it. The certificate
     * must be read before the phone's first sign request, hence the order. */
    xc_identity_init();
    /* Plan B: start the AP (and thus the C6 slave boot) in its own task now. */
    xTaskCreatePinnedToCore(net_task, "xc_net_start", 8192, NULL, 5, NULL, 0);
    xc_mfi_scan_result_t mfi = { 0 };
    xc_mfi_scan(&mfi);
    esp_err_t mfi_auth = ESP_FAIL;
    if (mfi.detected && xc_mfi_open(mfi.address) == ESP_OK) {
        mfi_auth = xc_mfi_selftest();
    }
    if (xc_iap2_start() != ESP_OK) {
        ESP_LOGE(TAG, "iAP2 bootstrap failed to start");
    }

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

    /* Flicker diagnostics: wrap the adapter's LVGL flush and log refresh
     * events, so a screen wipe shows up with its area and trigger. */
    s_lvgl_flush_cb = disp->flush_cb;
    lv_display_set_flush_cb(disp, flush_probe_cb);
    lv_display_add_event_cb(disp, disp_event_cb, LV_EVENT_REFR_REQUEST, NULL);
    lv_display_add_event_cb(disp, disp_event_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, disp_event_cb, LV_EVENT_RENDER_READY, NULL);

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

    /* AP, mDNS and the AirPlay stack run in net_task (spawned right after
     * boot); its ap_err/mdns_err feed the deferred self-test status line. */

    /* 9.3 item 2: heavy self-tests off the critical path. They run 10 s after
     * boot and are skipped when the phone already started a session. The
     * speaker beep stays inline: it is the hardware smoke test. */
    s_selftest_ctx.mfi = mfi;
    s_selftest_ctx.mfi_auth = mfi_auth;
    xc_audio_tone_test();
    xTaskCreatePinnedToCore(selftest_task, "xc_selftest", 12288, &s_selftest_ctx, 4, NULL, 1);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive; free PSRAM: %u bytes",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
