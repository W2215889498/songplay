#include "xc_iap2.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xc_bt_link.h"
#include "xc_crypto.h"
#include "xc_identity.h"
#include "xc_mfi.h"
#include "xc_net.h"

static const char *TAG = "xc_iap2";

/* message ids */
#define MSG_START_IDENTIFICATION 0x1D00
#define MSG_IDENTIFICATION_INFO 0x1D01
#define MSG_IDENTIFICATION_ACCEPTED 0x1D02
#define MSG_IDENTIFICATION_REJECTED 0x1D03
#define MSG_AUTH_CERT_REQUEST 0xAA00
#define MSG_AUTH_CERT 0xAA01
#define MSG_AUTH_CHALLENGE_REQUEST 0xAA02
#define MSG_AUTH_RESPONSE 0xAA03
#define MSG_AUTH_FAILED 0xAA04
#define MSG_AUTH_SUCCEEDED 0xAA05
#define MSG_CARPLAY_AVAILABILITY 0x4300
#define MSG_CARPLAY_START_SESSION 0x4301
#define MSG_WIRELESS_UPDATE 0x4E0D
#define MSG_TRANSPORT_IDENTIFIERS 0x4E0E
#define MSG_REQUEST_WIFI_CONFIG 0x5702
#define MSG_WIFI_CONFIG 0x5703
#define MSG_START_LOCATION_INFO 0xFFFA
#define MSG_LOCATION_INFO 0xFFFB
#define MSG_STOP_LOCATION_INFO 0xFFFC

/* Optional identification parameters the phone may reject; they are dropped
 * and the identification is re-sent (same strategy as LIVI). */
#define OPT_VEHICLE_INFO (1u << 20)
#define OPT_VEHICLE_STATUS (1u << 21)
#define OPT_LOCATION_INFO (1u << 22)
#define OPT_ROUTE_GUIDANCE (1u << 30)
#define OPT_DROPPABLE (OPT_VEHICLE_INFO | OPT_VEHICLE_STATUS | OPT_LOCATION_INFO | OPT_ROUTE_GUIDANCE)
static uint32_t s_ident_skip;
static int s_ident_retries;

/* the ESP32 bridge's classic-Bluetooth address (used by the BT transport).
 * iAP2 transmits this little-endian (LSB first), the reverse of colon order. */
static const uint8_t BT_MAC[6] = { 0xC6, 0x43, 0x81, 0x07, 0x70, 0x00 };

#define SECURITY_WPA_WPA2 2
#define AIRPLAY_PORT 7000
#define SOURCE_VERSION "950.7.1"

/* ---- TLV builder ------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t open_at[8];
    int depth;
    bool ok;
} tlv_t;

static void tlv_begin(tlv_t *t, uint16_t id)
{
    if (t->len + 4 > t->cap) {
        t->ok = false;
        return;
    }
    t->open_at[t->depth++] = t->len;
    t->buf[t->len++] = 0;
    t->buf[t->len++] = 0;
    t->buf[t->len++] = (uint8_t)(id >> 8);
    t->buf[t->len++] = (uint8_t)(id & 0xff);
}

static void tlv_end(tlv_t *t)
{
    if (t->depth <= 0) {
        return;
    }
    size_t at = t->open_at[--t->depth];
    size_t plen = t->len - at;
    t->buf[at] = (uint8_t)(plen >> 8);
    t->buf[at + 1] = (uint8_t)(plen & 0xff);
}

static void tlv_raw(tlv_t *t, const void *data, size_t n)
{
    if (t->len + n > t->cap) {
        t->ok = false;
        return;
    }
    memcpy(t->buf + t->len, data, n);
    t->len += n;
}

static void tlv_u8(tlv_t *t, uint16_t id, uint8_t v)
{
    tlv_begin(t, id);
    tlv_raw(t, &v, 1);
    tlv_end(t);
}

static void tlv_u16(tlv_t *t, uint16_t id, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    tlv_begin(t, id);
    tlv_raw(t, b, 2);
    tlv_end(t);
}

static void tlv_u32(tlv_t *t, uint16_t id, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    tlv_begin(t, id);
    tlv_raw(t, b, 4);
    tlv_end(t);
}

static void tlv_str(tlv_t *t, uint16_t id, const char *s)
{
    tlv_begin(t, id);
    tlv_raw(t, s, strlen(s) + 1);
    tlv_end(t);
}

static void tlv_bytes(tlv_t *t, uint16_t id, const void *p, size_t n)
{
    tlv_begin(t, id);
    tlv_raw(t, p, n);
    tlv_end(t);
}

static void tlv_void(tlv_t *t, uint16_t id)
{
    tlv_begin(t, id);
    tlv_end(t);
}

/* ---- TLV reader ------------------------------------------------------- */

static bool tlv_find(const uint8_t *body, size_t len, uint16_t id, const uint8_t **out,
                     size_t *out_len)
{
    size_t off = 0;
    while (off + 4 <= len) {
        size_t plen = ((size_t)body[off] << 8) | body[off + 1];
        uint16_t pid = (uint16_t)((body[off + 2] << 8) | body[off + 3]);
        if (plen < 4 || off + plen > len) {
            break;
        }
        if (pid == id) {
            *out = body + off + 4;
            *out_len = plen - 4;
            return true;
        }
        off += plen;
    }
    return false;
}

static uint8_t bda_str_buf[18];

static const char *device_id_string(void)
{
    /* reuse the AirPlay device id string used in mDNS */
    extern void xc_airplay_device_id(char *out, size_t cap);
    xc_airplay_device_id((char *)bda_str_buf, sizeof(bda_str_buf));
    return (const char *)bda_str_buf;
}

/* ---- message senders -------------------------------------------------- */

EXT_RAM_BSS_ATTR static uint8_t s_body[4096];

static void send_identification(void)
{
    tlv_t t = { .buf = s_body, .cap = sizeof(s_body), .ok = true };
    tlv_str(&t, 0, "xcertplay");
    tlv_str(&t, 1, "xcertplay");
    tlv_str(&t, 2, "xcertplay");
    tlv_str(&t, 3, "xcertplay");
    tlv_str(&t, 4, "1.0.0");
    tlv_str(&t, 5, "1.0");

    /* u16 lists are raw big-endian arrays, not nested parameter groups.
     * Values follow the verified LIVI wireless accessory profile. */
    static const uint16_t sent[] = { 0xA101, 0x5703, 0x5000, 0x5002, 0x5200, 0x5203, 0xFFFB,
                                     0xAE00, 0xAE02, 0x4157, 0x4159, 0x4154, 0x4156, 0x4301 };
    tlv_begin(&t, 6);
    for (size_t i = 0; i < sizeof(sent) / sizeof(sent[0]); i++) {
        uint8_t b[2] = { (uint8_t)(sent[i] >> 8), (uint8_t)sent[i] };
        tlv_raw(&t, b, 2);
    }
    tlv_end(&t);

    static const uint16_t recv[] = { 0xEA00, 0xEA01, 0xA100, 0xA102, 0x4E0D, 0x4E0E, 0x5702,
                                     0x5001, 0x5201, 0x5202, 0xFFFA, 0xFFFC, 0xAE01, 0x4158,
                                     0x4155, 0x4E0B, 0x4300 };
    tlv_begin(&t, 7);
    for (size_t i = 0; i < sizeof(recv) / sizeof(recv[0]); i++) {
        uint8_t b[2] = { (uint8_t)(recv[i] >> 8), (uint8_t)recv[i] };
        tlv_raw(&t, b, 2);
    }
    tlv_end(&t);

    tlv_u8(&t, 8, 0);   /* powerProvidingCapability: wireless */
    tlv_u16(&t, 9, 20); /* maximumCurrentDrawnFromDevice */

    tlv_begin(&t, 10); /* ExternalAccessoryProtocols */
    tlv_u8(&t, 0, 1);
    tlv_str(&t, 1, "com.shilapi.xcertplay");
    tlv_u8(&t, 2, 4); /* NoActionNoCommunication */
    tlv_end(&t);

    tlv_str(&t, 12, "en");
    tlv_begin(&t, 13); /* string list: concatenated NUL-terminated strings */
    tlv_raw(&t, "en", 3);
    tlv_raw(&t, "de", 3);
    tlv_end(&t);

    tlv_begin(&t, 17); /* BluetoothTransport */
    tlv_u16(&t, 0, 0);
    tlv_str(&t, 1, "blue");
    tlv_void(&t, 2);
    tlv_bytes(&t, 3, BT_MAC, sizeof(BT_MAC));
    tlv_str(&t, 4, "blue");
    tlv_void(&t, 5);
    tlv_end(&t);

    tlv_begin(&t, 18); /* iAP2HIDComponent */
    tlv_u16(&t, 0, 0);
    tlv_str(&t, 1, "Media Playback Remote");
    tlv_u8(&t, 2, 1);
    tlv_end(&t);

    if (!(s_ident_skip & OPT_VEHICLE_INFO)) {
        tlv_begin(&t, 20); /* vehicle information */
        tlv_u16(&t, 0, 0);
        tlv_str(&t, 1, "xcertplay");
        tlv_u8(&t, 2, 1); /* engine type: diesel */
        tlv_str(&t, 3, "xcertplay");
        tlv_str(&t, 4, "xcertplay");
        tlv_end(&t);
    }

    if (!(s_ident_skip & OPT_VEHICLE_STATUS)) {
        tlv_begin(&t, 21); /* vehicle status */
        tlv_u16(&t, 0, 0);
        tlv_str(&t, 1, "xcertplay");
        tlv_void(&t, 2); /* range available */
        tlv_void(&t, 3); /* outside temperature available */
        tlv_end(&t);
    }

    if (!(s_ident_skip & OPT_LOCATION_INFO)) {
        tlv_begin(&t, 22); /* location information */
        tlv_u16(&t, 0, 0);
        tlv_str(&t, 1, "xcertplay");
        tlv_void(&t, 2); /* gps fix available */
        tlv_void(&t, 3); /* recommended minimum available */
        tlv_end(&t);
    }

    tlv_begin(&t, 24); /* WirelessCarPlayTransport */
    tlv_u16(&t, 0, 1);
    tlv_str(&t, 1, XC_AP_SSID);
    tlv_void(&t, 2);
    tlv_u16(&t, 3, 1);
    tlv_void(&t, 4);
    tlv_void(&t, 5);
    tlv_end(&t);

    if (!(s_ident_skip & OPT_ROUTE_GUIDANCE)) {
        tlv_begin(&t, 30); /* route guidance display */
        tlv_u16(&t, 0, 0);
        tlv_str(&t, 1, "xcertplay");
        tlv_u16(&t, 2, 128);
        tlv_u16(&t, 3, 128);
        tlv_u16(&t, 4, 128);
        tlv_u16(&t, 5, 128);
        tlv_u16(&t, 6, 0);
        tlv_end(&t);
    }

    if (t.ok) {
        xc_link_send_csm(MSG_IDENTIFICATION_INFO, s_body, t.len);
        ESP_LOGI(TAG, "identification sent (%u bytes)", (unsigned)t.len);
    } else {
        ESP_LOGE(TAG, "identification buffer overflow");
    }
}

static void send_wifi_config(void)
{
    tlv_t t = { .buf = s_body, .cap = sizeof(s_body), .ok = true };
    tlv_str(&t, 1, XC_AP_SSID);
    tlv_str(&t, 2, XC_AP_PASSWORD);
    tlv_u8(&t, 3, SECURITY_WPA_WPA2);
    tlv_u8(&t, 4, XC_AP_CHANNEL);
    xc_link_send_csm(MSG_WIFI_CONFIG, s_body, t.len);
    ESP_LOGI(TAG, "wifi config sent (ssid=%s)", XC_AP_SSID);
}

static void send_start_session(void)
{
    const xc_identity_t *id = xc_identity_get();
    char pk_hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(pk_hex + i * 2, "%02x", id->ed_pub[i]);
    }
    pk_hex[64] = '\0';

    static const uint8_t ap_ip[] = { 192, 168, 4, 1 };

    tlv_t t = { .buf = s_body, .cap = sizeof(s_body), .ok = true };
    tlv_begin(&t, 1); /* wireless group */
    tlv_str(&t, 0, XC_AP_SSID);
    tlv_str(&t, 1, XC_AP_PASSWORD);
    tlv_u8(&t, 2, XC_AP_CHANNEL);
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3]);
    tlv_str(&t, 3, ip);
    tlv_u8(&t, 4, SECURITY_WPA_WPA2);
    tlv_end(&t);

    tlv_u32(&t, 2, AIRPLAY_PORT);
    tlv_str(&t, 3, device_id_string());
    tlv_str(&t, 4, pk_hex);
    tlv_str(&t, 5, SOURCE_VERSION);

    xc_link_send_csm(MSG_CARPLAY_START_SESSION, s_body, t.len);
    ESP_LOGI(TAG, "CarPlayStartSession sent (ssid=%s, ip=%s, port=%d)", XC_AP_SSID, ip,
             AIRPLAY_PORT);
}

/* ---- location information (0xFFFA/0xFFFB/0xFFFC) ----------------------
 * The identification advertises Start/StopLocationInformation, so the phone
 * asks for NMEA data once the CarPlay session starts (xcertplay/LIVI answer
 * with a $GPGGA + $GPRMC pair; ignoring the request makes iOS give up). */

static volatile bool s_loc_active;
static esp_timer_handle_t s_loc_timer;

static void send_location_update(void)
{
    static const char NMEA[] =
        "$GPGGA,000000.00,3113.8240,N,12128.4220,E,1,08,1.0,0.0,M,0.0,M,,*57\r\n"
        "$GPRMC,000000.00,A,3113.8240,N,12128.4220,E,0.00,0.00,010125,,*36\r\n";
    EXT_RAM_BSS_ATTR static uint8_t loc_body[256];
    tlv_t t = { .buf = loc_body, .cap = sizeof(loc_body), .ok = true };
    tlv_str(&t, 0, NMEA);
    if (t.ok) {
        xc_link_send_csm(MSG_LOCATION_INFO, loc_body, t.len);
    }
}

static void location_timer_cb(void *arg)
{
    (void)arg;
    if (s_loc_active) {
        send_location_update();
    }
}

/* ---- message handling ------------------------------------------------- */

static void on_message(uint16_t msg_id, const uint8_t *body, size_t len)
{
    switch (msg_id) {
    case MSG_START_IDENTIFICATION:
        ESP_LOGI(TAG, "phone requested identification");
        send_identification();
        break;

    case MSG_IDENTIFICATION_ACCEPTED:
        ESP_LOGI(TAG, "identification accepted");
        break;

    case MSG_IDENTIFICATION_REJECTED: {
        /* The body lists the rejected parameter ids as 4-byte TLVs. */
        uint32_t rejected = 0;
        size_t off = 0;
        while (off + 4 <= len) {
            size_t plen = ((size_t)body[off] << 8) | body[off + 1];
            uint16_t pid = (uint16_t)((body[off + 2] << 8) | body[off + 3]);
            if (plen < 4 || off + plen > len) {
                break;
            }
            ESP_LOGW(TAG, "  phone rejects identification parameter 0x%04X", pid);
            rejected |= 1u << (pid & 31);
            off += plen;
        }
        if (rejected == 0 || (rejected & ~OPT_DROPPABLE) != 0) {
            ESP_LOGE(TAG, "identification rejected with non-droppable parameters, giving up");
            break;
        }
        if (++s_ident_retries > 4) {
            ESP_LOGE(TAG, "identification still rejected after retries, giving up");
            break;
        }
        s_ident_skip |= rejected;
        ESP_LOGW(TAG, "retrying identification without parameters 0x%08x", (unsigned)rejected);
        send_identification();
        break;
    }

    case MSG_AUTH_CERT_REQUEST: {
        const uint8_t *cert = NULL;
        size_t cert_len = 0;
        if (!xc_mfi_cached_certificate(&cert, &cert_len)) {
            /* No boot-time copy: try a fresh read as a last resort. */
            EXT_RAM_BSS_ATTR static uint8_t cert_fresh[XC_MFI_MAX_CERTIFICATE];
            if (xc_mfi_read_certificate(cert_fresh, sizeof(cert_fresh), &cert_len) == ESP_OK) {
                cert = cert_fresh;
                ESP_LOGW(TAG, "MFi: boot cache empty, using fresh certificate read");
            } else {
                ESP_LOGE(TAG, "no cached MFi certificate");
                break;
            }
        }
        tlv_t t = { .buf = s_body, .cap = sizeof(s_body), .ok = true };
        tlv_bytes(&t, 0, cert, cert_len);
        xc_link_send_csm(MSG_AUTH_CERT, s_body, t.len);
        ESP_LOGI(TAG, "MFi certificate sent (%u bytes)", (unsigned)cert_len);
        break;
    }

    case MSG_AUTH_CHALLENGE_REQUEST: {
        const uint8_t *ch = NULL;
        size_t ch_len = 0;
        if (!tlv_find(body, len, 0, &ch, &ch_len) || ch_len == 0) {
            ESP_LOGE(TAG, "challenge param missing");
            break;
        }
        /* This coprocessor authenticates only right after a fresh certificate
         * window read (see the AirPlay /auth-setup path); re-read and discard
         * before signing or iOS silently rejects the signature. */
        EXT_RAM_BSS_ATTR static uint8_t cert_prime[XC_MFI_MAX_CERTIFICATE];
        size_t prime_len = 0;
        if (xc_mfi_read_certificate(cert_prime, sizeof(cert_prime), &prime_len) != ESP_OK) {
            ESP_LOGW(TAG, "MFi: certificate prime read failed");
        }
        static uint8_t sig[256];
        size_t sig_len = 0;
        if (xc_mfi_sign_challenge(ch, ch_len, sig, sizeof(sig), &sig_len) != ESP_OK) {
            ESP_LOGE(TAG, "MFi sign failed");
            break;
        }
        tlv_t t = { .buf = s_body, .cap = sizeof(s_body), .ok = true };
        tlv_bytes(&t, 0, sig, sig_len);
        xc_link_send_csm(MSG_AUTH_RESPONSE, s_body, t.len);
        ESP_LOGI(TAG, "MFi signature sent (challenge %u -> signature %u bytes)", (unsigned)ch_len,
                 (unsigned)sig_len);
        break;
    }

    case MSG_AUTH_SUCCEEDED:
        ESP_LOGI(TAG, "MFi authentication SUCCEEDED");
        break;
    case MSG_AUTH_FAILED:
        ESP_LOGW(TAG, "MFi authentication FAILED");
        break;

    case MSG_REQUEST_WIFI_CONFIG:
        ESP_LOGI(TAG, "phone requested Wi-Fi configuration");
        send_wifi_config();
        break;

    case MSG_CARPLAY_AVAILABILITY:
        ESP_LOGI(TAG, "CarPlay availability received");
        send_start_session();
        break;

    case MSG_WIRELESS_UPDATE:
        ESP_LOGI(TAG, "wireless CarPlay update (%u bytes)", (unsigned)len);
        break;

    case MSG_TRANSPORT_IDENTIFIERS:
        ESP_LOGI(TAG, "transport identifiers received");
        send_wifi_config();
        break;

    case MSG_START_LOCATION_INFO:
        ESP_LOGI(TAG, "phone requested location information");
        if (s_loc_timer == NULL) {
            const esp_timer_create_args_t args = {
                .callback = location_timer_cb,
                .name = "iap2_loc",
            };
            if (esp_timer_create(&args, &s_loc_timer) != ESP_OK) {
                ESP_LOGE(TAG, "location timer create failed");
                break;
            }
        }
        s_loc_active = true;
        send_location_update();
        esp_timer_stop(s_loc_timer);
        esp_timer_start_periodic(s_loc_timer, 1000000);
        break;

    case MSG_STOP_LOCATION_INFO:
        ESP_LOGI(TAG, "phone stopped location information");
        s_loc_active = false;
        if (s_loc_timer != NULL) {
            esp_timer_stop(s_loc_timer);
        }
        break;

    default:
        ESP_LOGI(TAG, "CSM 0x%04X unhandled (%u bytes)", msg_id, (unsigned)len);
        break;
    }
}

esp_err_t xc_iap2_start(void)
{
    return xc_link_start(on_message);
}
