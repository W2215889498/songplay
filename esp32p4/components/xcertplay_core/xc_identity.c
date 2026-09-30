#include "xc_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "xc_crypto.h"

static const char *TAG = "xc_identity";

#define NS_IDENT "xcident"
#define NS_PAIR "xcpair"
#define KEY_ED_PRIV "ed_priv"
#define KEY_PI "pi"
#define KEY_CTRL_ID "ctrl_id"
#define KEY_CTRL_LTPK "ctrl_ltpk"

static xc_identity_t s_id;
static bool s_loaded;

static void make_uuid(char out[37])
{
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    r[6] = (uint8_t)((r[6] & 0x0f) | 0x40);
    r[8] = (uint8_t)((r[8] & 0x3f) | 0x80);
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

esp_err_t xc_identity_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    if (xc_crypto_init() != ESP_OK) {
        return ESP_FAIL;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_IDENT, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_id.ed_priv);
    bool fresh = false;
    err = nvs_get_blob(h, KEY_ED_PRIV, s_id.ed_priv, &len);
    if (err != ESP_OK || len != sizeof(s_id.ed_priv)) {
        esp_fill_random(s_id.ed_priv, sizeof(s_id.ed_priv));
        err = nvs_set_blob(h, KEY_ED_PRIV, s_id.ed_priv, sizeof(s_id.ed_priv));
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
        fresh = true;
    }

    if (xc_ed25519_public_from_private(s_id.ed_priv, s_id.ed_pub) != ESP_OK) {
        nvs_close(h);
        ESP_LOGE(TAG, "failed to derive Ed25519 public key");
        return ESP_FAIL;
    }

    len = sizeof(s_id.pairing_id);
    err = nvs_get_str(h, KEY_PI, s_id.pairing_id, &len);
    if (err != ESP_OK) {
        make_uuid(s_id.pairing_id);
        nvs_set_str(h, KEY_PI, s_id.pairing_id);
        fresh = true;
    }
    nvs_commit(h);
    nvs_close(h);

    s_loaded = true;
    ESP_LOGI(TAG, "%s identity: pi=%s", fresh ? "created" : "loaded", s_id.pairing_id);
    return ESP_OK;
}

const xc_identity_t *xc_identity_get(void)
{
    return s_loaded ? &s_id : NULL;
}

esp_err_t xc_pairing_save(const char *controller_id, const uint8_t ltpk[32])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_PAIR, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_CTRL_ID, controller_id);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_CTRL_LTPK, ltpk, 32);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "controller paired: %s", controller_id);
    }
    return err;
}

esp_err_t xc_pairing_load(const char *controller_id, uint8_t ltpk[32])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_PAIR, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    char saved[64];
    size_t len = sizeof(saved);
    err = nvs_get_str(h, KEY_CTRL_ID, saved, &len);
    if (err != ESP_OK || strcmp(saved, controller_id) != 0) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }
    len = 32;
    err = nvs_get_blob(h, KEY_CTRL_LTPK, ltpk, &len);
    nvs_close(h);
    if (err != ESP_OK || len != 32) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
