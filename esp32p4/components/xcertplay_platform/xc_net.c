#include "xc_net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "xc_net";

static bool s_ap_ready;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "softAP started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " joined", MAC2STR(e->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " left", MAC2STR(e->mac));
    }
}

esp_err_t xc_net_start_ap(void)
{
    if (s_ap_ready) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "create default wifi ap netif failed");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init (ESP-Hosted/C6) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler,
                                              NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register wifi events: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wc = { 0 };
    memcpy(wc.ap.ssid, XC_AP_SSID, sizeof(XC_AP_SSID) - 1);
    wc.ap.ssid_len = sizeof(XC_AP_SSID) - 1;
    memcpy(wc.ap.password, XC_AP_PASSWORD, sizeof(XC_AP_PASSWORD) - 1);
    wc.ap.channel = XC_AP_CHANNEL;
    wc.ap.max_connection = XC_AP_MAX_STA;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting softAP failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_ready = true;

    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "AP SSID=\"%s\" pass=\"%s\" channel=%d ip=" IPSTR,
                 XC_AP_SSID, XC_AP_PASSWORD, XC_AP_CHANNEL, IP2STR(&ip.ip));
    }
    return ESP_OK;
}

bool xc_net_ap_ready(void)
{
    return s_ap_ready;
}
