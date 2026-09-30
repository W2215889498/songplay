#include "xc_mdns.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "xc_crypto.h"
#include "xc_identity.h"

static const char *TAG = "xc_mdns";

/* Placeholder long-term Ed25519 public key (32 bytes hex). Replaced when the
 * AirPlay identity (pair-setup) is implemented in M5. */
static void build_public_key_hex(const uint8_t mac[6], char out_hex[65])
{
    uint8_t digest[XC_SHA512_LEN];
    uint8_t key[32];
    if (xc_sha512(mac, 6, digest) == ESP_OK) {
        memcpy(key, digest, sizeof(key));
    } else {
        memset(key, 0x42, sizeof(key));
    }
    for (int i = 0; i < 32; i++) {
        sprintf(out_hex + i * 2, "%02x", key[i]);
    }
    out_hex[64] = '\0';
}

esp_err_t xc_mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set("xcertplay");
    mdns_instance_name_set("xcertplay");

    uint8_t mac[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    char deviceid[18];
    snprintf(deviceid, sizeof(deviceid), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char pk_hex[65];
    char pi[40];
    const xc_identity_t *id = xc_identity_get();
    if (id != NULL) {
        for (int i = 0; i < 32; i++) {
            sprintf(pk_hex + i * 2, "%02x", id->ed_pub[i]);
        }
        pk_hex[64] = '\0';
        snprintf(pi, sizeof(pi), "%s", id->pairing_id);
    } else {
        build_public_key_hex(mac, pk_hex);
        snprintf(pi, sizeof(pi), "%02X%02X%02X%02X-0000-4000-8000-000000000000",
                 mac[0], mac[1], mac[2], mac[3]);
    }

    mdns_txt_item_t txt[] = {
        { "deviceid", deviceid },
        { "features", "0x44540380,0x61" },
        { "flags", "0x4" },
        { "model", "xcertplay" },
        { "srcvers", XC_AIRPLAY_SRCVERS },
        { "protovers", "1.1" },
        { "pi", pi },
        { "pk", pk_hex },
    };

    err = mdns_service_add("xcertplay", "_airplay", "_tcp", XC_AIRPLAY_PORT,
                           txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add(_airplay): %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS up: _airplay._tcp port %d, deviceid=%s srcvers=%s",
             XC_AIRPLAY_PORT, deviceid, XC_AIRPLAY_SRCVERS);
    return ESP_OK;
}
