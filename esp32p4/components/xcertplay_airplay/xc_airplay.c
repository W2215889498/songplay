#include "xc_airplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "xc_bplist.h"
#include "xc_bplist_read.h"
#include "xc_audio.h"
#include "xc_bt_link.h"
#include "xc_callcard.h"
#include "xc_crypto.h"
#include "xc_iap2.h"
#include "xc_identity.h"
#include "xc_mfi.h"
#include "xc_srp.h"
#include "xc_tlv8.h"
#include "xc_video.h"

static const char *TAG = "xc_airplay";

#define XC_AIRPLAY_PORT 7000
#define MAIN_UUID "b7e6c5a0-1111-4000-8000-000000000001"
#define ALT_UUID "b7e6c5a0-2222-4000-8000-000000000002"
#define SRCVERS "950.7.1"
#define CARPLAY_FEATURES 0x615653aee2ULL

/* TLV8 types */
#define TLV_METHOD 0x00
#define TLV_IDENTIFIER 0x01
#define TLV_SALT 0x02
#define TLV_PUBLIC_KEY 0x03
#define TLV_PROOF 0x04
#define TLV_ENCRYPTED 0x05
#define TLV_STATE 0x06
#define TLV_ERROR 0x07
#define TLV_SIGNATURE 0x0a
#define TLV_ERROR_AUTH 2

/* ---- HID descriptors (from xcertplay AirPlayHid) ---------------------- */

static const uint8_t KNOB_DESC[] = {
    0x05, 0x01, 0x09, 0x08, 0xa1, 0x01,
    0x05, 0x09, 0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x0c, 0x0a, 0x23, 0x02, 0x0a, 0x24, 0x02, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0,
};

static const uint8_t MEDIA_DESC[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
    0x15, 0x00, 0x25, 0x06, 0x05, 0x0c,
    0x0a, 0x00, 0x00, 0x0a, 0xb0, 0x00, 0x0a, 0xb1, 0x00, 0x0a, 0xcd, 0x00,
    0x0a, 0xb5, 0x00, 0x0a, 0xb6, 0x00, 0x0a, 0x9e, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
};

static const uint8_t TELEPHONY_DESC[] = {
    0x05, 0x0b, 0x09, 0x07, 0xa1, 0x01,
    0x15, 0x00, 0x25, 0x11, 0x05, 0x0b,
    0x09, 0x00, 0x09, 0x20, 0x09, 0x21, 0x09, 0x26, 0x09, 0x2f,
    0x09, 0xb0, 0x09, 0xb1, 0x09, 0xb2, 0x09, 0xb3, 0x09, 0xb4, 0x09, 0xb5,
    0x09, 0xb6, 0x09, 0xb7, 0x09, 0xb8, 0x09, 0xb9, 0x09, 0xba, 0x09, 0xbb,
    0x05, 0x07, 0x09, 0x2a,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
};

static size_t build_touch_desc(uint8_t *out, int xmax, int ymax)
{
    size_t p = 0;
    const uint8_t head[] = { 0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01 };
    memcpy(out + p, head, sizeof(head));
    p += sizeof(head);
    for (int f = 0; f < 2; f++) {
        const uint8_t pre[] = { 0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02,
                                0x09, 0x38, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
                                0x15, 0x00, 0x25, 0x01, 0x09, 0x33, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
                                0x95, 0x07, 0x81, 0x03,
                                0x05, 0x01, 0x26, (uint8_t)(xmax & 0xff), (uint8_t)((xmax >> 8) & 0xff),
                                0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
                                0x26, (uint8_t)(ymax & 0xff), (uint8_t)((ymax >> 8) & 0xff),
                                0x09, 0x31, 0x81, 0x02, 0xc0 };
        memcpy(out + p, pre, sizeof(pre));
        p += sizeof(pre);
    }
    out[p++] = 0xc0;
    return p;
}

/* ---- /info builder ---------------------------------------------------- */

static void get_device_id(char out[18])
{
    uint8_t mac[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void xc_airplay_device_id(char *out, size_t cap)
{
    char tmp[18];
    get_device_id(tmp);
    snprintf(out, cap, "%s", tmp);
}

static volatile bool s_session_started;

bool xc_airplay_session_started(void)
{
    return s_session_started;
}

static int push(xc_bplist_t *b, int *keys, int *vals, int n, const char *k, int v)
{
    keys[n] = xc_bplist_str(b, k);
    vals[n] = v;
    return n + 1;
}

esp_err_t xc_airplay_build_info(uint8_t *buf, size_t cap, size_t *out_len)
{
    xc_bplist_t *b = xc_bplist_new();
    if (b == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char device_id[18];
    get_device_id(device_id);

    int keys[XC_BPLIST_MAX_REF];
    int vals[XC_BPLIST_MAX_REF];
    int n = 0;

    int dk[12], dv[12];
    n = 0;
    n = push(b, dk, dv, n, "uuid", xc_bplist_str(b, MAIN_UUID));
    n = push(b, dk, dv, n, "type", xc_bplist_int(b, 110));
    n = push(b, dk, dv, n, "maxFPS", xc_bplist_int(b, 60));
    n = push(b, dk, dv, n, "widthPixels", xc_bplist_int(b, 480));
    n = push(b, dk, dv, n, "heightPixels", xc_bplist_int(b, 800));
    n = push(b, dk, dv, n, "widthPhysical", xc_bplist_int(b, 93));
    n = push(b, dk, dv, n, "heightPhysical", xc_bplist_int(b, 155));
    n = push(b, dk, dv, n, "features", xc_bplist_int(b, 0x0a));
    n = push(b, dk, dv, n, "primaryInputDevice", xc_bplist_int(b, 1));

    int sk[5], sv[5];
    sk[0] = xc_bplist_str(b, "widthPixels");   sv[0] = xc_bplist_int(b, 480);
    sk[1] = xc_bplist_str(b, "heightPixels");  sv[1] = xc_bplist_int(b, 800);
    sk[2] = xc_bplist_str(b, "originXPixels"); sv[2] = xc_bplist_int(b, 0);
    sk[3] = xc_bplist_str(b, "originYPixels"); sv[3] = xc_bplist_int(b, 0);
    sk[4] = xc_bplist_str(b, "drawUIOutsideSafeArea"); sv[4] = xc_bplist_bool(b, true);
    int safe = xc_bplist_dict(b, sk, sv, 5);

    int vk[5], vv[5];
    vk[0] = xc_bplist_str(b, "widthPixels");   vv[0] = xc_bplist_int(b, 480);
    vk[1] = xc_bplist_str(b, "heightPixels");  vv[1] = xc_bplist_int(b, 800);
    vk[2] = xc_bplist_str(b, "originXPixels"); vv[2] = xc_bplist_int(b, 0);
    vk[3] = xc_bplist_str(b, "originYPixels"); vv[3] = xc_bplist_int(b, 0);
    vk[4] = xc_bplist_str(b, "safeArea");      vv[4] = safe;
    int view_area = xc_bplist_dict(b, vk, vv, 5);
    int view_areas = xc_bplist_array(b, &view_area, 1);
    n = push(b, dk, dv, n, "viewAreas", view_areas);
    n = push(b, dk, dv, n, "initialViewArea", xc_bplist_int(b, 0));
    int display = xc_bplist_dict(b, dk, dv, n);

    /* Second display entry: the instrument cluster / alt screen (type 111,
     * ALT_UUID). The phone then sets up the type-111 screen stream, which is
     * the one this product renders. 240x400 is the portrait variant requested
     * for comparison (map fits the portrait dev panel better). Physical size
     * follows the reference default: 200 mm width, height derived from the
     * aspect (LIVI getInfo.ts / xcertplay AirPlayDisplaySettings). */
    int dk2[12], dv2[12];
    n = 0;
    n = push(b, dk2, dv2, n, "uuid", xc_bplist_str(b, ALT_UUID));
    n = push(b, dk2, dv2, n, "type", xc_bplist_int(b, 111));
    n = push(b, dk2, dv2, n, "maxFPS", xc_bplist_int(b, 60));
    n = push(b, dk2, dv2, n, "widthPixels", xc_bplist_int(b, 240));
    n = push(b, dk2, dv2, n, "heightPixels", xc_bplist_int(b, 400));
    n = push(b, dk2, dv2, n, "widthPhysical", xc_bplist_int(b, 6));
    n = push(b, dk2, dv2, n, "heightPhysical", xc_bplist_int(b, 8));
    n = push(b, dk2, dv2, n, "features", xc_bplist_int(b, 0x0a));
    n = push(b, dk2, dv2, n, "primaryInputDevice", xc_bplist_int(b, 1));
    /* Per-display view area (LIVI builds one areaDict per display): the cluster
     * gets its own 240x400 geometry instead of reusing the main screen's. */
    int sk2[5], sv2[5];
    sk2[0] = xc_bplist_str(b, "widthPixels");   sv2[0] = xc_bplist_int(b, 240);
    sk2[1] = xc_bplist_str(b, "heightPixels");  sv2[1] = xc_bplist_int(b, 400);
    sk2[2] = xc_bplist_str(b, "originXPixels"); sv2[2] = xc_bplist_int(b, 0);
    sk2[3] = xc_bplist_str(b, "originYPixels"); sv2[3] = xc_bplist_int(b, 0);
    sk2[4] = xc_bplist_str(b, "drawUIOutsideSafeArea"); sv2[4] = xc_bplist_bool(b, true);
    int safe2 = xc_bplist_dict(b, sk2, sv2, 5);

    int vk2[5], vv2[5];
    vk2[0] = xc_bplist_str(b, "widthPixels");   vv2[0] = xc_bplist_int(b, 240);
    vk2[1] = xc_bplist_str(b, "heightPixels");  vv2[1] = xc_bplist_int(b, 400);
    vk2[2] = xc_bplist_str(b, "originXPixels"); vv2[2] = xc_bplist_int(b, 0);
    vk2[3] = xc_bplist_str(b, "originYPixels"); vv2[3] = xc_bplist_int(b, 0);
    vk2[4] = xc_bplist_str(b, "safeArea");      vv2[4] = safe2;
    int view_area2 = xc_bplist_dict(b, vk2, vv2, 5);
    int view_areas2 = xc_bplist_array(b, &view_area2, 1);
    n = push(b, dk2, dv2, n, "viewAreas", view_areas2);
    n = push(b, dk2, dv2, n, "initialViewArea", xc_bplist_int(b, 0));
    int display_alt = xc_bplist_dict(b, dk2, dv2, n);

    int disp_arr[2] = { display, display_alt };
    int displays = xc_bplist_array(b, disp_arr, 2);

    int r1k[6], r1v[6];
    r1k[0] = xc_bplist_str(b, "resourceID");       r1v[0] = xc_bplist_int(b, 1);
    r1k[1] = xc_bplist_str(b, "transferType");     r1v[1] = xc_bplist_int(b, 1);
    r1k[2] = xc_bplist_str(b, "transferPriority"); r1v[2] = xc_bplist_int(b, 100);
    r1k[3] = xc_bplist_str(b, "takeConstraint");   r1v[3] = xc_bplist_int(b, 100);
    r1k[4] = xc_bplist_str(b, "borrowConstraint"); r1v[4] = xc_bplist_int(b, 100);
    r1k[5] = xc_bplist_str(b, "unborrowConstraint"); r1v[5] = xc_bplist_int(b, 100);
    int r1 = xc_bplist_dict(b, r1k, r1v, 6);
    int r2k[6] = { r1k[0], r1k[1], r1k[2], r1k[3], r1k[4], r1k[5] };
    int r2v[6];
    r2v[0] = xc_bplist_int(b, 2); r2v[1] = r1v[1]; r2v[2] = r1v[2];
    r2v[3] = r1v[3]; r2v[4] = r1v[4]; r2v[5] = r1v[5];
    int r2 = xc_bplist_dict(b, r2k, r2v, 6);
    int resources_arr[2] = { r1, r2 };
    int resources = xc_bplist_array(b, resources_arr, 2);

    int a1k[2] = { xc_bplist_str(b, "appStateID"), xc_bplist_str(b, "state") };
    int a1v[2] = { xc_bplist_int(b, 2), xc_bplist_bool(b, false) };
    int a1 = xc_bplist_dict(b, a1k, a1v, 2);
    int a2k[2] = { xc_bplist_str(b, "appStateID"), xc_bplist_str(b, "speechMode") };
    int a2v[2] = { xc_bplist_int(b, 1), xc_bplist_int(b, (uint64_t)-1 & 0xffffffffULL) };
    int a2 = xc_bplist_dict(b, a2k, a2v, 2);
    int a3k[2] = { a1k[0], a1k[1] };
    int a3v[2] = { xc_bplist_int(b, 3), xc_bplist_bool(b, false) };
    int a3 = xc_bplist_dict(b, a3k, a3v, 2);
    int appstates_arr[3] = { a1, a2, a3 };
    int appstates = xc_bplist_array(b, appstates_arr, 3);
    int mk[2] = { xc_bplist_str(b, "resources"), xc_bplist_str(b, "appStates") };
    int mv[2] = { resources, appstates };
    int modes = xc_bplist_dict(b, mk, mv, 2);

    int lat_ids[16];
    int nl = 0;
    const char *types[] = { NULL, "default", "media", "telephony", "speechRecognition", "alert" };
    for (int i = 0; i < 6; i++) {
        int lk[4], lv[4], m = 0;
        lk[m] = xc_bplist_str(b, "type"); lv[m++] = xc_bplist_int(b, 100);
        if (types[i]) { lk[m] = xc_bplist_str(b, "audioType"); lv[m++] = xc_bplist_str(b, types[i]); }
        lk[m] = xc_bplist_str(b, "inputLatencyMicros"); lv[m++] = xc_bplist_int(b, 0);
        lk[m] = xc_bplist_str(b, "outputLatencyMicros"); lv[m++] = xc_bplist_int(b, 0);
        lat_ids[nl++] = xc_bplist_dict(b, lk, lv, m);
    }
    for (int t = 101; t <= 102; t++) {
        int lk[4], lv[4];
        lk[0] = xc_bplist_str(b, "type"); lv[0] = xc_bplist_int(b, (uint64_t)t);
        lk[1] = xc_bplist_str(b, "audioType"); lv[1] = xc_bplist_str(b, "default");
        lk[2] = xc_bplist_str(b, "inputLatencyMicros"); lv[2] = xc_bplist_int(b, 0);
        lk[3] = xc_bplist_str(b, "outputLatencyMicros"); lv[3] = xc_bplist_int(b, 0);
        lat_ids[nl++] = xc_bplist_dict(b, lk, lv, 4);
    }
    {
        int lk[3], lv[3];
        lk[0] = xc_bplist_str(b, "type"); lv[0] = xc_bplist_int(b, 101);
        lk[1] = xc_bplist_str(b, "inputLatencyMicros"); lv[1] = xc_bplist_int(b, 0);
        lk[2] = xc_bplist_str(b, "outputLatencyMicros"); lv[2] = xc_bplist_int(b, 0);
        lat_ids[nl++] = xc_bplist_dict(b, lk, lv, 3);
    }
    int latencies = xc_bplist_array(b, lat_ids, nl);

    int fmt_ids[16];
    int nf = 0;
    const int pcm = 0x3fc | 0xc000;
    const int pcm_mono = 0x154 | 0x4000;
    const int aac_lc = 0x800000;
    /* 0x70000000 = Opus 16k/24k/48k mono. Wireless CarPlay only uses PCM over
     * USB, so the low-latency streams (100/101) must offer Opus or the phone
     * keeps navigation prompts and Siri on its own speaker. */
    const int opus = 0x70000000;
    struct { int type; const char *audio; int out; } fmts[] = {
        { 100, "compatibility", pcm }, { 101, "compatibility", pcm },
        { 100, "default", pcm | opus }, { 100, "alert", pcm | opus },
        { 100, "media", pcm },
        { 100, "telephony", pcm_mono | opus }, { 100, "speechRecognition", pcm_mono | opus },
        { 101, "default", pcm | opus }, { 102, "media", aac_lc },
    };
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        int fk[3], fv[3];
        fk[0] = xc_bplist_str(b, "type"); fv[0] = xc_bplist_int(b, (uint64_t)fmts[i].type);
        fk[1] = xc_bplist_str(b, "audioType"); fv[1] = xc_bplist_str(b, fmts[i].audio);
        fk[2] = xc_bplist_str(b, "audioOutputFormats"); fv[2] = xc_bplist_int(b, (uint64_t)fmts[i].out);
        fmt_ids[nf++] = xc_bplist_dict(b, fk, fv, 3);
    }
    int formats = xc_bplist_array(b, fmt_ids, nf);

    int hids[4];
    uint8_t touch_desc[160];
    size_t touch_len = build_touch_desc(touch_desc, 480, 800);
    struct { const char *uuid; const char *name; const uint8_t *desc; size_t len; } hd[] = {
        { "2a2a2a2a", "xcertplay Touchscreen", touch_desc, touch_len },
        { "2a2a2a2b", "xcertplay Knob", KNOB_DESC, sizeof(KNOB_DESC) },
        { "2a2a2a2c", "xcertplay Media", MEDIA_DESC, sizeof(MEDIA_DESC) },
        { "2a2a2a2d", "xcertplay Telephony", TELEPHONY_DESC, sizeof(TELEPHONY_DESC) },
    };
    for (int i = 0; i < 4; i++) {
        int hk[7], hv[7];
        hk[0] = xc_bplist_str(b, "hidProductID");   hv[0] = xc_bplist_int(b, 1);
        hk[1] = xc_bplist_str(b, "hidVendorID");    hv[1] = xc_bplist_int(b, 2);
        hk[2] = xc_bplist_str(b, "hidCountryCode"); hv[2] = xc_bplist_int(b, 0);
        hk[3] = xc_bplist_str(b, "uuid");           hv[3] = xc_bplist_str(b, hd[i].uuid);
        hk[4] = xc_bplist_str(b, "name");           hv[4] = xc_bplist_str(b, hd[i].name);
        hk[5] = xc_bplist_str(b, "displayUUID");    hv[5] = xc_bplist_str(b, MAIN_UUID);
        hk[6] = xc_bplist_str(b, "hidDescriptor");  hv[6] = xc_bplist_data(b, hd[i].desc, hd[i].len);
        hids[i] = xc_bplist_dict(b, hk, hv, 7);
    }
    int hid_devices = xc_bplist_array(b, hids, 4);

    int ext_ids[2] = { xc_bplist_str(b, "vocoderInfo"), xc_bplist_str(b, "enhancedRequestCarUI") };
    int extended = xc_bplist_array(b, ext_ids, 2);

    int bt_ids[1] = { xc_bplist_str(b, "02:00:00:00:00:01") };
    int bt = xc_bplist_array(b, bt_ids, 1);

    n = 0;
    n = push(b, keys, vals, n, "sourceVersion", xc_bplist_str(b, SRCVERS));
    n = push(b, keys, vals, n, "features", xc_bplist_int(b, CARPLAY_FEATURES));
    n = push(b, keys, vals, n, "statusFlags", xc_bplist_int(b, 4));
    n = push(b, keys, vals, n, "model", xc_bplist_str(b, "xcertplay"));
    n = push(b, keys, vals, n, "manufacturer", xc_bplist_str(b, "xcertplay"));
    n = push(b, keys, vals, n, "deviceID", xc_bplist_str(b, device_id));
    n = push(b, keys, vals, n, "bluetoothIDs", bt);
    n = push(b, keys, vals, n, "name", xc_bplist_str(b, "xcertplay"));
    n = push(b, keys, vals, n, "rightHandDrive", xc_bplist_bool(b, false));
    n = push(b, keys, vals, n, "keepAliveLowPower", xc_bplist_bool(b, false));
    n = push(b, keys, vals, n, "keepAliveSendStatsAsBody", xc_bplist_bool(b, false));
    n = push(b, keys, vals, n, "modes", modes);
    n = push(b, keys, vals, n, "audioLatencies", latencies);
    n = push(b, keys, vals, n, "audioFormats", formats);
    n = push(b, keys, vals, n, "extendedFeatures", extended);
    n = push(b, keys, vals, n, "displays", displays);
    n = push(b, keys, vals, n, "hidDevices", hid_devices);
    int root = xc_bplist_dict(b, keys, vals, n);

    esp_err_t err = xc_bplist_serialize(b, root, buf, cap, out_len);
    xc_bplist_free(b);
    return err;
}

/* ---- HTTP/RTSP request handling --------------------------------------- */

typedef struct {
    char method[16];
    char path[256];
    char cseq[24];
    bool keep_alive;
    const uint8_t *body;
    size_t body_len;
} rtsp_req_t;

#define XC_CONN_BUF 8192
#define XC_PLIST_BUF 8192
#define XC_CTRL_MAX 4096

static uint8_t *s_conn;
static uint8_t *s_plist;
static uint8_t *s_ct;      /* control-frame ciphertext buffer (PSRAM) */
static uint8_t *s_plain;   /* control-frame plaintext buffer (PSRAM) */
static uint8_t *s_tx;      /* control-frame tx buffer (PSRAM) */

/* Control channel (ChaCha20-Poly1305) state for the current connection. */
static bool s_ctrl_on;
static uint8_t s_ctrl_rkey[32];
static uint8_t s_ctrl_wkey[32];
static uint64_t s_ctrl_rd;
static uint64_t s_ctrl_wr;

static void ctrl_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 12);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(counter >> (8 * i));
    }
}

static void ctrl_reset(void)
{
    s_ctrl_on = false;
    s_ctrl_rd = 0;
    s_ctrl_wr = 0;
}

static int recv_all(int fd, uint8_t *out, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = recv(fd, out + got, len - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int parse_request(uint8_t *buf, size_t total, rtsp_req_t *req)
{
    int hdr_end = -1;
    for (size_t i = 0; i + 3 < total; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            hdr_end = (int)(i + 4);
            break;
        }
    }
    if (hdr_end < 0) {
        return -1;
    }

    memset(req, 0, sizeof(*req));
    req->keep_alive = true;
    char reqline[300];
    size_t rl = 0;
    while (rl < (size_t)hdr_end && buf[rl] != '\r' && rl < sizeof(reqline) - 1) {
        reqline[rl] = (char)buf[rl];
        rl++;
    }
    reqline[rl] = '\0';
    sscanf(reqline, "%15s %255s", req->method, req->path);
    ESP_LOGI(TAG, "RTSP << %s %s (%u bytes)%s", req->method, req->path, (unsigned)total,
             s_ctrl_on ? " [encrypted]" : "");

    size_t content_length = 0;
    for (size_t i = 0; i + 1 < (size_t)hdr_end; i++) {
        if (i + 15 <= (size_t)hdr_end &&
            (memcmp(buf + i, "Content-Length:", 15) == 0 ||
             memcmp(buf + i, "content-length:", 15) == 0)) {
            content_length = (size_t)strtoul((char *)buf + i + 15, NULL, 10);
        } else if (i + 5 <= (size_t)hdr_end &&
                   (memcmp(buf + i, "CSeq:", 5) == 0 || memcmp(buf + i, "cseq:", 5) == 0)) {
            const char *v = (char *)buf + i + 5;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            size_t k = 0;
            while (k < sizeof(req->cseq) - 1 && v[k] != '\r' && v[k] != '\n' && v[k] != '\0') {
                req->cseq[k] = v[k];
                k++;
            }
            req->cseq[k] = '\0';
        } else if (i + 17 <= (size_t)hdr_end && memcmp(buf + i, "Connection: close", 17) == 0) {
            req->keep_alive = false;
        }
    }

    req->body = buf + hdr_end;
    req->body_len = total - (size_t)hdr_end;
    if (req->body_len > content_length) {
        req->body_len = content_length;
    }
    return 0;
}

static int read_request(int fd, rtsp_req_t *req)
{
    if (s_ctrl_on) {
        uint8_t hdr[2];
        if (recv_all(fd, hdr, sizeof(hdr)) != 0) {
            return -1;
        }
        size_t len = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
        if (len > XC_CTRL_MAX) {
            return -1;
        }
        if (recv_all(fd, s_ct, len + 16) != 0) {
            return -1;
        }
        uint8_t nonce[12];
        ctrl_nonce(s_ctrl_rd, nonce);
        if (xc_chacha20poly1305_open(s_ctrl_rkey, nonce, hdr, 2, s_ct, len, s_ct + len, s_plain) != ESP_OK) {
            /* The two directions are easy to mix up: try the other key once and
             * remember the correct orientation. */
            if (xc_chacha20poly1305_open(s_ctrl_wkey, nonce, hdr, 2, s_ct, len, s_ct + len,
                                         s_plain) == ESP_OK) {
                ESP_LOGW(TAG, "control channel: read/write keys were swapped, correcting");
                uint8_t tmp[32];
                memcpy(tmp, s_ctrl_rkey, sizeof(tmp));
                memcpy(s_ctrl_rkey, s_ctrl_wkey, sizeof(tmp));
                memcpy(s_ctrl_wkey, tmp, sizeof(tmp));
            } else {
                char hx[3 * 32 + 1];
                size_t n = len < 32 ? len : 32;
                for (size_t i = 0; i < n; i++) {
                    sprintf(hx + i * 3, "%02x ", s_ct[i]);
                }
                hx[n * 3] = '\0';
                ESP_LOGW(TAG, "ctrl decrypt failed: len=%u hdr=%02x%02x rdctr=%llu ct=%s",
                         (unsigned)len, hdr[0], hdr[1], (unsigned long long)s_ctrl_rd, hx);
                char kx[3 * 32 + 1];
                for (int i = 0; i < 32; i++) {
                    sprintf(kx + i * 3, "%02x ", s_ctrl_rkey[i]);
                }
                kx[96] = '\0';
                ESP_LOGW(TAG, "ctrl rkey: %s", kx);
                for (int i = 0; i < 32; i++) {
                    sprintf(kx + i * 3, "%02x ", s_ctrl_wkey[i]);
                }
                ESP_LOGW(TAG, "ctrl wkey: %s", kx);
                return -1;
            }
        }
        s_ctrl_rd++;
        if (len == 0) {
            /* an empty keep-alive frame: nothing to parse */
            return read_request(fd, req);
        }
        return parse_request(s_plain, len, req);
    }

    size_t total = 0;
    int hdr_end = -1;
    while (total < XC_CONN_BUF) {
        int r = recv(fd, s_conn + total, XC_CONN_BUF - total, 0);
        if (r <= 0) {
            return -1;
        }
        total += r;
        for (size_t i = 0; i + 3 < total; i++) {
            if (s_conn[i] == '\r' && s_conn[i + 1] == '\n' && s_conn[i + 2] == '\r' && s_conn[i + 3] == '\n') {
                hdr_end = (int)(i + 4);
                break;
            }
        }
        if (hdr_end >= 0) {
            break;
        }
    }
    if (hdr_end < 0) {
        return -1;
    }

    size_t content_length = 0;
    for (size_t i = 0; i + 17 < (size_t)hdr_end; i++) {
        if (memcmp(s_conn + i, "Content-Length:", 15) == 0 ||
            memcmp(s_conn + i, "content-length:", 15) == 0) {
            content_length = (size_t)strtoul((char *)s_conn + i + 15, NULL, 10);
            break;
        }
    }
    size_t have = total - (size_t)hdr_end;
    while (have < content_length && total < XC_CONN_BUF) {
        int r = recv(fd, s_conn + total, XC_CONN_BUF - total, 0);
        if (r <= 0) {
            break;
        }
        total += r;
        have += r;
    }
    return parse_request(s_conn, total, req);
}

static void send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "control send failed (errno %d, %u bytes left)", errno, (unsigned)len);
            return;
        }
        p += n;
        len -= (size_t)n;
    }
}

/* Send bytes on the control channel, ChaCha20-Poly1305 framed when enabled. */
static void send_plain(int fd, const uint8_t *data, size_t len)
{
    if (!s_ctrl_on) {
        send_all(fd, data, len);
        return;
    }
    size_t off = 0;
    do {
        size_t chunk = (len - off) > XC_CTRL_MAX ? XC_CTRL_MAX : (len - off);
        uint8_t hdr[2] = { (uint8_t)(chunk & 0xff), (uint8_t)(chunk >> 8) };
        uint8_t nonce[12];
        ctrl_nonce(s_ctrl_wr, nonce);
        if (xc_chacha20poly1305_seal(s_ctrl_wkey, nonce, hdr, 2, data + off, chunk, s_tx,
                                    s_tx + chunk) != ESP_OK) {
            return;
        }
        send_all(fd, hdr, sizeof(hdr));
        send_all(fd, s_tx, chunk + 16);
        ESP_LOGI(TAG, "ctrl tx frame %u bytes ctr=%llu", (unsigned)chunk,
                 (unsigned long long)s_ctrl_wr);
        s_ctrl_wr++;
        off += chunk;
    } while (off < len);
}

/* Send an RTSP response. Once the control cipher is on, head+body must travel
 * as ONE ChaCha frame (xcertplay writes cipher.encrypt(head + body) and LIVI
 * does the same); iOS drops the session when a message is split across frames. */
EXT_RAM_BSS_ATTR static uint8_t s_wire[8192];

static void send_response(int fd, const char *head, size_t head_len, const uint8_t *body,
                          size_t body_len)
{
    if (!s_ctrl_on) {
        send_all(fd, head, head_len);
        if (body_len > 0) {
            send_all(fd, body, body_len);
        }
        return;
    }
    if (head_len + body_len > sizeof(s_wire)) {
        /* Never drop a response: fall back to a plain concatenation. */
        send_plain(fd, (const uint8_t *)head, head_len);
        if (body_len > 0) {
            send_plain(fd, body, body_len);
        }
        return;
    }
    memcpy(s_wire, head, head_len);
    if (body_len > 0) {
        memcpy(s_wire + head_len, body, body_len);
    }
    send_plain(fd, s_wire, head_len + body_len);
}

static void send_info(int fd, const rtsp_req_t *req)
{
    size_t len = 0;
    if (xc_airplay_build_info(s_plist, XC_PLIST_BUF, &len) != ESP_OK) {
        const char *e = "RTSP/1.0 500 Internal Server Error\r\n\r\n";
        send_plain(fd, (const uint8_t *)e, strlen(e));
        return;
    }
    char hdr[192];
    int hn = req->cseq[0]
                 ? snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\n"
                            "Content-Type: application/x-apple-binary-plist\r\n"
                            "CSeq: %s\r\n"
                            "Content-Length: %u\r\n\r\n",
                            req->cseq, (unsigned)len)
                 : snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\n"
                            "Content-Type: application/x-apple-binary-plist\r\n"
                            "Content-Length: %u\r\n\r\n",
                            (unsigned)len);
    send_response(fd, hdr, (size_t)hn, s_plist, len);
}

static void send_tlv(int fd, const uint8_t *tlv, size_t len, const rtsp_req_t *req)
{
    char hdr[192];
    int hn = req->cseq[0]
                 ? snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\n"
                            "Content-Type: application/pairing+tlv8\r\n"
                            "CSeq: %s\r\n"
                            "Content-Length: %u\r\n\r\n",
                            req->cseq, (unsigned)len)
                 : snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\n"
                            "Content-Type: application/pairing+tlv8\r\n"
                            "Content-Length: %u\r\n\r\n",
                            (unsigned)len);
    send_response(fd, hdr, (size_t)hn, tlv, len);
}

static void send_tlv_error(int fd, uint8_t state, const rtsp_req_t *req)
{
    uint8_t out[16];
    size_t pos = 0;
    xc_tlv8_write_u8(out, sizeof(out), &pos, TLV_STATE, state);
    xc_tlv8_write_u8(out, sizeof(out), &pos, TLV_ERROR, TLV_ERROR_AUTH);
    send_tlv(fd, out, pos, req);
}

static esp_err_t hkdf32(const uint8_t *ikm, size_t ikm_len, const char *salt, const char *info,
                        uint8_t out[32])
{
    return xc_hkdf_sha512((const uint8_t *)salt, strlen(salt), ikm, ikm_len,
                          (const uint8_t *)info, strlen(info), out, 32);
}

static size_t seal(const uint8_t key[32], const char *label, const uint8_t *pt, size_t pt_len,
                   uint8_t *out)
{
    uint8_t nonce[12];
    xc_nonce_label(label, nonce);
    if (xc_chacha20poly1305_seal(key, nonce, NULL, 0, pt, pt_len, out, out + pt_len) != ESP_OK) {
        return 0;
    }
    return pt_len + 16;
}

static bool unseal(const uint8_t key[32], const char *label, const uint8_t *ct, size_t ct_len,
                   uint8_t *out, size_t *out_len)
{
    if (ct_len < 16) {
        return false;
    }
    uint8_t nonce[12];
    xc_nonce_label(label, nonce);
    size_t pt_len = ct_len - 16;
    if (xc_chacha20poly1305_open(key, nonce, NULL, 0, ct, pt_len, ct + pt_len, out) != ESP_OK) {
        return false;
    }
    *out_len = pt_len;
    return true;
}

/* ---- pair-setup ------------------------------------------------------- */

static xc_srp_session_t s_ps;
static bool s_ps_active;
static uint8_t s_ps_key[64];
static bool s_ps_has_key;

static void handle_pair_setup(int fd, const rtsp_req_t *req)
{
    uint8_t state = 0;
    if (xc_tlv8_find_u8(req->body, req->body_len, TLV_STATE, &state) != ESP_OK) {
        send_tlv_error(fd, 0, req);
        return;
    }

    EXT_RAM_BSS_ATTR static uint8_t out[768];
    size_t pos = 0;

    if (state == 1) {
        xc_srp_free(&s_ps);
        s_ps_active = false;
        s_ps_has_key = false;
        if (xc_srp_start(&s_ps, "Pair-Setup", "3939") != ESP_OK) {
            send_tlv_error(fd, state, req);
            return;
        }
        s_ps_active = true;
        xc_tlv8_write_u8(out, sizeof(out), &pos, TLV_STATE, 2);
        xc_tlv8_write(out, sizeof(out), &pos, TLV_PUBLIC_KEY, s_ps.B, sizeof(s_ps.B));
        xc_tlv8_write(out, sizeof(out), &pos, TLV_SALT, s_ps.salt, sizeof(s_ps.salt));
        send_tlv(fd, out, pos, req);
        return;
    }

    if (state == 3) {
        uint8_t a[512], proof[64];
        size_t a_len = 0, proof_len = 0;
        if (!s_ps_active ||
            xc_tlv8_find(req->body, req->body_len, TLV_PUBLIC_KEY, a, sizeof(a), &a_len) != ESP_OK ||
            xc_tlv8_find(req->body, req->body_len, TLV_PROOF, proof, sizeof(proof), &proof_len) != ESP_OK ||
            proof_len != 64) {
            send_tlv_error(fd, state, req);
            return;
        }
        uint8_t server_proof[64];
        if (xc_srp_verify(&s_ps, a, a_len, proof, proof_len, s_ps_key, server_proof) != ESP_OK) {
            send_tlv_error(fd, state, req);
            return;
        }
        s_ps_has_key = true;
        xc_tlv8_write_u8(out, sizeof(out), &pos, TLV_STATE, 4);
        xc_tlv8_write(out, sizeof(out), &pos, TLV_PROOF, server_proof, 64);
        send_tlv(fd, out, pos, req);
        return;
    }

    if (state == 5) {
        EXT_RAM_BSS_ATTR static uint8_t enc[640];
        EXT_RAM_BSS_ATTR static uint8_t sub[640];
        EXT_RAM_BSS_ATTR static uint8_t sub2[320];
        size_t enc_len = 0, sub_len = 0;
        if (!s_ps_has_key ||
            xc_tlv8_find(req->body, req->body_len, TLV_ENCRYPTED, enc, sizeof(enc), &enc_len) != ESP_OK) {
            send_tlv_error(fd, state, req);
            return;
        }
        uint8_t ekey[32];
        hkdf32(s_ps_key, 64, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", ekey);
        if (!unseal(ekey, "PS-Msg05", enc, enc_len, sub, &sub_len)) {
            send_tlv_error(fd, state, req);
            return;
        }
        uint8_t cid[128], cltpk[32], csig[64];
        size_t cid_len = 0, cl_len = 0, cs_len = 0;
        if (xc_tlv8_find(sub, sub_len, TLV_IDENTIFIER, cid, sizeof(cid), &cid_len) != ESP_OK ||
            xc_tlv8_find(sub, sub_len, TLV_PUBLIC_KEY, cltpk, sizeof(cltpk), &cl_len) != ESP_OK ||
            xc_tlv8_find(sub, sub_len, TLV_SIGNATURE, csig, sizeof(csig), &cs_len) != ESP_OK ||
            cl_len != 32 || cs_len != 64) {
            send_tlv_error(fd, state, req);
            return;
        }
        uint8_t csk[32];
        hkdf32(s_ps_key, 64, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", csk);
        EXT_RAM_BSS_ATTR static uint8_t buf[256];
        memcpy(buf, csk, 32);
        memcpy(buf + 32, cid, cid_len);
        memcpy(buf + 32 + cid_len, cltpk, 32);
        if (xc_ed25519_verify(cltpk, buf, 32 + cid_len + 32, csig) != ESP_OK) {
            ESP_LOGW(TAG, "pair-setup: controller signature invalid");
            send_tlv_error(fd, state, req);
            return;
        }
        char cid_str[129];
        size_t cpy = cid_len < sizeof(cid_str) - 1 ? cid_len : sizeof(cid_str) - 1;
        memcpy(cid_str, cid, cpy);
        cid_str[cpy] = '\0';
        xc_pairing_save(cid_str, cltpk);

        const xc_identity_t *id = xc_identity_get();
        uint8_t ask[32];
        hkdf32(s_ps_key, 64, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", ask);
        EXT_RAM_BSS_ATTR static uint8_t buf2[256];
        size_t aid_len = strlen(id->pairing_id);
        memcpy(buf2, ask, 32);
        memcpy(buf2 + 32, id->pairing_id, aid_len);
        memcpy(buf2 + 32 + aid_len, id->ed_pub, 32);
        uint8_t asig[64];
        if (xc_ed25519_sign(id->ed_priv, buf2, 32 + aid_len + 32, asig) != ESP_OK) {
            send_tlv_error(fd, state, req);
            return;
        }
        size_t s2 = 0;
        xc_tlv8_write(sub2, sizeof(sub2), &s2, TLV_IDENTIFIER, (const uint8_t *)id->pairing_id, aid_len);
        xc_tlv8_write(sub2, sizeof(sub2), &s2, TLV_PUBLIC_KEY, id->ed_pub, 32);
        xc_tlv8_write(sub2, sizeof(sub2), &s2, TLV_SIGNATURE, asig, 64);

        EXT_RAM_BSS_ATTR static uint8_t sealed[320];
        size_t sealed_len = seal(ekey, "PS-Msg06", sub2, s2, sealed);
        if (sealed_len == 0) {
            send_tlv_error(fd, state, req);
            return;
        }
        pos = 0;
        xc_tlv8_write_u8(out, sizeof(out), &pos, TLV_STATE, 6);
        xc_tlv8_write(out, sizeof(out), &pos, TLV_ENCRYPTED, sealed, sealed_len);
        send_tlv(fd, out, pos, req);
        ESP_LOGI(TAG, "pair-setup complete");
        return;
    }

    send_tlv_error(fd, state, req);
}

/* ---- pair-verify ------------------------------------------------------ */

static struct {
    bool active;
    uint8_t our_priv[32];
    uint8_t our_pub[32];
    uint8_t client_pub[32];
    uint8_t shared[32];
    uint8_t ekey[32];
} s_pv;

uint8_t g_control_read_key[32];
uint8_t g_control_write_key[32];
bool g_control_ready;

/* pair-verify shared secret, needed to derive per-stream DataStream keys */
static uint8_t s_shared[32];
static bool s_has_shared;

static void handle_pair_verify(int fd, const rtsp_req_t *req)
{
    uint8_t state = 0;
    if (xc_tlv8_find_u8(req->body, req->body_len, TLV_STATE, &state) != ESP_OK) {
        send_tlv_error(fd, 0, req);
        return;
    }

    EXT_RAM_BSS_ATTR static uint8_t out[768];
    size_t pos = 0;

    if (state == 1) {
        uint8_t client_pub[32];
        size_t cp_len = 0;
        if (xc_tlv8_find(req->body, req->body_len, TLV_PUBLIC_KEY, client_pub, sizeof(client_pub), &cp_len) != ESP_OK ||
            cp_len != 32) {
            send_tlv_error(fd, state, req);
            return;
        }
        if (xc_x25519_generate(s_pv.our_priv, s_pv.our_pub) != ESP_OK ||
            xc_x25519_shared(s_pv.our_priv, client_pub, s_pv.shared) != ESP_OK) {
            send_tlv_error(fd, state, req);
            return;
        }
        memcpy(s_pv.client_pub, client_pub, 32);
        memcpy(s_shared, s_pv.shared, 32);
        s_has_shared = true;
        hkdf32(s_pv.shared, 32, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", s_pv.ekey);

        const xc_identity_t *id = xc_identity_get();
        size_t aid_len = strlen(id->pairing_id);
        EXT_RAM_BSS_ATTR static uint8_t data[128];
        memcpy(data, s_pv.our_pub, 32);
        memcpy(data + 32, id->pairing_id, aid_len);
        memcpy(data + 32 + aid_len, client_pub, 32);
        uint8_t sig[64];
        if (xc_ed25519_sign(id->ed_priv, data, 32 + aid_len + 32, sig) != ESP_OK) {
            send_tlv_error(fd, state, req);
            return;
        }
        EXT_RAM_BSS_ATTR static uint8_t sub[256];
        size_t s = 0;
        xc_tlv8_write(sub, sizeof(sub), &s, TLV_IDENTIFIER, (const uint8_t *)id->pairing_id, aid_len);
        xc_tlv8_write(sub, sizeof(sub), &s, TLV_SIGNATURE, sig, 64);

        EXT_RAM_BSS_ATTR static uint8_t sealed[256];
        size_t sealed_len = seal(s_pv.ekey, "PV-Msg02", sub, s, sealed);
        if (sealed_len == 0) {
            send_tlv_error(fd, state, req);
            return;
        }
        s_pv.active = true;
        xc_tlv8_write_u8(out, sizeof(out), &pos, TLV_STATE, 2);
        xc_tlv8_write(out, sizeof(out), &pos, TLV_PUBLIC_KEY, s_pv.our_pub, 32);
        xc_tlv8_write(out, sizeof(out), &pos, TLV_ENCRYPTED, sealed, sealed_len);
        send_tlv(fd, out, pos, req);
        return;
    }

    if (state == 3) {
        EXT_RAM_BSS_ATTR static uint8_t enc[640];
        EXT_RAM_BSS_ATTR static uint8_t sub[640];
        size_t enc_len = 0, sub_len = 0;
        if (!s_pv.active ||
            xc_tlv8_find(req->body, req->body_len, TLV_ENCRYPTED, enc, sizeof(enc), &enc_len) != ESP_OK ||
            !unseal(s_pv.ekey, "PV-Msg03", enc, enc_len, sub, &sub_len)) {
            send_tlv_error(fd, state, req);
            return;
        }
        uint8_t cid[128], csig[64];
        size_t cid_len = 0, cs_len = 0;
        if (xc_tlv8_find(sub, sub_len, TLV_IDENTIFIER, cid, sizeof(cid), &cid_len) != ESP_OK ||
            xc_tlv8_find(sub, sub_len, TLV_SIGNATURE, csig, sizeof(csig), &cs_len) != ESP_OK ||
            cs_len != 64) {
            send_tlv_error(fd, state, req);
            return;
        }
        char cid_str[129];
        size_t cpy = cid_len < sizeof(cid_str) - 1 ? cid_len : sizeof(cid_str) - 1;
        memcpy(cid_str, cid, cpy);
        cid_str[cpy] = '\0';

        uint8_t cltpk[32];
        if (xc_pairing_load(cid_str, cltpk) != ESP_OK) {
            ESP_LOGW(TAG, "pair-verify: unknown controller %s", cid_str);
            send_tlv_error(fd, state, req);
            return;
        }
        EXT_RAM_BSS_ATTR static uint8_t data[128];
        memcpy(data, s_pv.client_pub, 32);
        memcpy(data + 32, cid, cid_len);
        memcpy(data + 32 + cid_len, s_pv.our_pub, 32);
        if (xc_ed25519_verify(cltpk, data, 32 + cid_len + 32, csig) != ESP_OK) {
            ESP_LOGW(TAG, "pair-verify: controller signature invalid");
            send_tlv_error(fd, state, req);
            return;
        }
        hkdf32(s_pv.shared, 32, "Control-Salt", "Control-Read-Encryption-Key", g_control_read_key);
        hkdf32(s_pv.shared, 32, "Control-Salt", "Control-Write-Encryption-Key", g_control_write_key);
        g_control_ready = true;
        s_pv.active = false;
        xc_tlv8_write_u8(out, sizeof(out), &pos, TLV_STATE, 4);
        send_tlv(fd, out, pos, req);
        /* Everything after this frame is ChaCha20-Poly1305 encrypted. */
        memcpy(s_ctrl_rkey, g_control_read_key, 32);
        memcpy(s_ctrl_wkey, g_control_write_key, 32);
        s_ctrl_rd = 0;
        s_ctrl_wr = 0;
        s_ctrl_on = true;
        ESP_LOGI(TAG, "pair-verify complete: control channel encryption enabled");
        return;
    }

    send_tlv_error(fd, state, req);
}

/* ---- /auth-setup (MFi SAP over the encrypted control channel) --------- */

static void send_status(int fd, const char *status, const rtsp_req_t *req)
{
    char hdr[160];
    int hn = req->cseq[0]
                 ? snprintf(hdr, sizeof(hdr), "RTSP/1.0 %s\r\nCSeq: %s\r\nContent-Length: 0\r\n\r\n",
                            status, req->cseq)
                 : snprintf(hdr, sizeof(hdr), "RTSP/1.0 %s\r\nContent-Length: 0\r\n\r\n", status);
    send_response(fd, hdr, (size_t)hn, NULL, 0);
}

static void handle_auth_setup(int fd, const rtsp_req_t *req)
{
    EXT_RAM_BSS_ATTR static uint8_t resp[1400];
    if (req->body_len != 33 || req->body[0] != 0x01) {
        send_status(fd, "400 Bad Request", req);
        return;
    }
    const uint8_t *peer = req->body + 1;
    uint8_t our_priv[32], our_pub[32], shared[32];
    if (xc_x25519_generate(our_priv, our_pub) != ESP_OK ||
        xc_x25519_shared(our_priv, peer, shared) != ESP_OK) {
        send_status(fd, "500 Internal Server Error", req);
        return;
    }

    uint8_t tmp[7 + 32], h[20];
    uint8_t aes_key[16], aes_iv[16];
    memcpy(tmp, "AES-KEY", 7);
    memcpy(tmp + 7, shared, 32);
    xc_sha1(tmp, sizeof(tmp), h);
    memcpy(aes_key, h, 16);
    memcpy(tmp, "AES-IV", 6);
    memcpy(tmp + 6, shared, 32);
    xc_sha1(tmp, 6 + 32, h);
    memcpy(aes_iv, h, 16);

    uint8_t major = 0;
    xc_mfi_protocol_major(&major);
    uint8_t challenge[64];
    memcpy(challenge, our_pub, 32);
    memcpy(challenge + 32, peer, 32);
    uint8_t digest[32];
    size_t digest_len = 32;
    if (major == 2) {
        uint8_t d20[20];
        xc_sha1(challenge, sizeof(challenge), d20);
        memcpy(digest, d20, 20);
        digest_len = 20;
    } else {
        xc_sha256(challenge, sizeof(challenge), digest);
    }

    /* This coprocessor performs an authentication only right after a fresh
     * certificate-window read, so try that first and fall back to the copy
     * cached at boot. */
    const uint8_t *cert = NULL;
    size_t cert_len = 0;
    EXT_RAM_BSS_ATTR static uint8_t cert_fresh[XC_MFI_MAX_CERTIFICATE];
    if (xc_mfi_read_certificate(cert_fresh, sizeof(cert_fresh), &cert_len) == ESP_OK) {
        cert = cert_fresh;
        ESP_LOGI(TAG, "/auth-setup: certificate re-read before signing");
    } else if (xc_mfi_cached_certificate(&cert, &cert_len)) {
        ESP_LOGI(TAG, "/auth-setup: using cached certificate");
    } else {
        ESP_LOGE(TAG, "/auth-setup: certificate unavailable");
        send_status(fd, "500 Internal Server Error", req);
        return;
    }

    EXT_RAM_BSS_ATTR static uint8_t sig[XC_MFI_MAX_SIGNATURE];
    size_t sig_len = 0;
    if (xc_mfi_sign_challenge(digest, digest_len, sig, sizeof(sig), &sig_len) != ESP_OK) {
        ESP_LOGE(TAG, "/auth-setup: MFi sign failed");
        send_status(fd, "500 Internal Server Error", req);
        return;
    }
    EXT_RAM_BSS_ATTR static uint8_t enc_sig[XC_MFI_MAX_SIGNATURE];
    if (xc_aes128_ctr(aes_key, aes_iv, sig, enc_sig, sig_len) != ESP_OK) {
        send_status(fd, "500 Internal Server Error", req);
        return;
    }

    size_t p = 0;
    memcpy(resp + p, our_pub, 32);
    p += 32;
    resp[p++] = (uint8_t)(cert_len >> 24);
    resp[p++] = (uint8_t)(cert_len >> 16);
    resp[p++] = (uint8_t)(cert_len >> 8);
    resp[p++] = (uint8_t)cert_len;
    memcpy(resp + p, cert, cert_len);
    p += cert_len;
    resp[p++] = (uint8_t)(sig_len >> 24);
    resp[p++] = (uint8_t)(sig_len >> 16);
    resp[p++] = (uint8_t)(sig_len >> 8);
    resp[p++] = (uint8_t)sig_len;
    memcpy(resp + p, enc_sig, sig_len);
    p += sig_len;

    char hdr[192];
    int hn = req->cseq[0]
                 ? snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                            "CSeq: %s\r\nContent-Length: %u\r\n\r\n",
                            req->cseq, (unsigned)p)
                 : snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                            "Content-Length: %u\r\n\r\n",
                            (unsigned)p);
    send_response(fd, hdr, (size_t)hn, resp, p);
    ESP_LOGI(TAG, "/auth-setup: MFi cert %u B, encrypted signature %u B (major %u)",
             (unsigned)cert_len, (unsigned)sig_len, major);
}

/* ---- auxiliary channels: timing (NTP), keep-alive, event --------------- */

static int s_ntp_fd = -1;
static int s_keepalive_fd = -1;
static int s_event_fd = -1;
static uint16_t s_ntp_port;
static uint16_t s_keepalive_port;
static uint16_t s_event_port;

/* Event channel (phone -> us for HID acks, us -> phone for /command). Unlike
 * the control channel the keys are NOT swapped: we write with Events-Write and
 * read with Events-Read (LIVI cpStack._openEventChannel). */
static int s_event_conn = -1;
static uint8_t s_ev_wkey[32];
static volatile bool s_ev_ready;
static uint64_t s_ev_wr;
static uint16_t s_ev_cseq;

static uint16_t aux_open_udp(int *out_fd)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return 0;
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return 0;
    }
    struct sockaddr_in b;
    socklen_t bl = sizeof(b);
    getsockname(fd, (struct sockaddr *)&b, &bl);
    *out_fd = fd;
    return ntohs(b.sin_port);
}

static uint16_t aux_open_tcp(int *out_fd)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return 0;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return 0;
    }
    struct sockaddr_in b;
    socklen_t bl = sizeof(b);
    getsockname(fd, (struct sockaddr *)&b, &bl);
    *out_fd = fd;
    return ntohs(b.sin_port);
}

/* PT_REQUEST 210 -> PT_RESPONSE 211, 32-byte UDP packets, NTP64 timestamps. */
static void ntp_handle(const uint8_t *pkt, size_t len, const struct sockaddr_storage *src,
                       socklen_t sl, int fd)
{
    if (len < 32) {
        return;
    }
    if (pkt[1] != 210) {
        /* PT_RESPONSE from the phone answering our own clock-sync request. */
        ESP_LOGI(TAG, "ntp response from port %u (pt %u)",
                 ntohs(((const struct sockaddr_in *)src)->sin_port), pkt[1]);
        return;
    }
    ESP_LOGI(TAG, "ntp request from port %u (pt %u)", ntohs(((const struct sockaddr_in *)src)->sin_port),
             pkt[1]);
    uint8_t r[32] = { 0 };
    r[0] = 0x80;
    r[1] = 211;
    r[2] = 0;
    r[3] = 7;
    memcpy(r + 8, pkt + 24, 8); /* originate = peer transmit */
    int64_t us = esp_timer_get_time();
    uint64_t secs = (uint64_t)(us / 1000000) + 2208988800ULL;
    uint64_t frac = (uint64_t)((us % 1000000) * 4294.967296);
    uint64_t ts = (secs << 32) | (frac & 0xffffffffULL);
    for (int i = 0; i < 8; i++) {
        r[16 + i] = (uint8_t)(ts >> (56 - 8 * i));
        r[24 + i] = (uint8_t)(ts >> (56 - 8 * i));
    }
    sendto(fd, r, sizeof(r), 0, (const struct sockaddr *)src, sl);
}

static void aux_udp_task(void *arg)
{
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_ntp_fd, &rfds);
        FD_SET(s_keepalive_fd, &rfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int mx = (s_ntp_fd > s_keepalive_fd ? s_ntp_fd : s_keepalive_fd) + 1;
        if (select(mx, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        if (FD_ISSET(s_ntp_fd, &rfds)) {
            uint8_t buf[64];
            struct sockaddr_storage src;
            socklen_t sl = sizeof(src);
            int n = recvfrom(s_ntp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
            if (n >= 32) {
                ntp_handle(buf, (size_t)n, &src, sl, s_ntp_fd);
            }
        }
        if (FD_ISSET(s_keepalive_fd, &rfds)) {
            uint8_t buf[512];
            struct sockaddr_storage src;
            socklen_t sl = sizeof(src);
            recvfrom(s_keepalive_fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
        }
    }
}

static void event_task(void *arg)
{
    while (1) {
        struct sockaddr_storage src;
        socklen_t sl = sizeof(src);
        int fd = accept(s_event_fd, (struct sockaddr *)&src, &sl);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(TAG, "event channel connected");
        uint8_t key[32];
        if (xc_hkdf_sha512((const uint8_t *)"Events-Salt", strlen("Events-Salt"), s_shared,
                           sizeof(s_shared), (const uint8_t *)"Events-Write-Encryption-Key",
                           strlen("Events-Write-Encryption-Key"), key, sizeof(key)) == ESP_OK) {
            memcpy(s_ev_wkey, key, sizeof(key));
            s_ev_wr = 0;
            s_ev_cseq = 0;
            s_event_conn = fd;
            s_ev_ready = true;
        } else {
            ESP_LOGE(TAG, "event key derivation failed");
        }
        uint8_t buf[512];
        while (recv(fd, buf, sizeof(buf), 0) > 0) {
        }
        s_ev_ready = false;
        s_event_conn = -1;
        close(fd);
        ESP_LOGW(TAG, "event channel closed");
    }
}

/* One event-channel frame: [len u16 LE][ciphertext][tag16], AAD = the length
 * header, nonce = 4 zero bytes + 8-byte LE counter (same as the control
 * channel, but with the Events-Write key and its own counter). */
static void event_send_bytes(const uint8_t *data, size_t len)
{
    if (!s_ev_ready || s_event_conn < 0) {
        return;
    }
    /* Dedicated TX buffer: the control channel's s_tx may be in use by the
     * RTSP task while other tasks send these commands. Serialize senders: the
     * iAP2 tunnel ships link frames from the bt_link task. */
    EXT_RAM_BSS_ATTR static uint8_t ev_tx[XC_CTRL_MAX + 16];
    static SemaphoreHandle_t ev_lock;
    if (ev_lock == NULL) {
        ev_lock = xSemaphoreCreateMutex();
    }
    if (ev_lock != NULL) {
        xSemaphoreTake(ev_lock, portMAX_DELAY);
    }
    size_t off = 0;
    while (off < len) {
        size_t chunk = (len - off) > XC_CTRL_MAX ? XC_CTRL_MAX : (len - off);
        uint8_t hdr[2] = { (uint8_t)(chunk & 0xff), (uint8_t)(chunk >> 8) };
        uint8_t nonce[12];
        ctrl_nonce(s_ev_wr, nonce);
        if (xc_chacha20poly1305_seal(s_ev_wkey, nonce, hdr, 2, data + off, chunk, ev_tx,
                                     ev_tx + chunk) != ESP_OK) {
            break;
        }
        send_all(s_event_conn, hdr, sizeof(hdr));
        send_all(s_event_conn, ev_tx, chunk + 16);
        s_ev_wr++;
        off += chunk;
    }
    if (ev_lock != NULL) {
        xSemaphoreGive(ev_lock);
    }
}

/* POST /command with an accessory dictionary body (LIVI _sendEventCommand). */
static void event_send_command(const uint8_t *body, size_t body_len)
{
    if (!s_ev_ready || body == NULL || body_len == 0) {
        return;
    }
    char head[192];
    s_ev_cseq++;
    int hn = snprintf(head, sizeof(head),
                      "POST /command RTSP/1.0\r\n"
                      "Content-Type: application/x-apple-binary-plist\r\n"
                      "Content-Length: %u\r\n"
                      "CSeq: %u\r\n\r\n",
                      (unsigned)body_len, (unsigned)s_ev_cseq);
    EXT_RAM_BSS_ATTR static uint8_t cmd_buf[4096];
    if ((size_t)hn + body_len > sizeof(cmd_buf)) {
        return;
    }
    memcpy(cmd_buf, head, (size_t)hn);
    memcpy(cmd_buf + hn, body, body_len);
    event_send_bytes(cmd_buf, (size_t)hn + body_len);
    ESP_LOGI(TAG, "event command sent (%u bytes)", (unsigned)body_len);
}

/* Ship iAP2 link bytes to the phone over the CarPlay tunnel
 * (LIVI cpStack: {type:'iAPSendMessage', params:{data}} on the event channel). */
static void tunnel_tx(const uint8_t *data, size_t len)
{
    if (len == 0 || len > 3900) {
        return;
    }
    xc_bplist_t *b = xc_bplist_new();
    if (b == NULL) {
        return;
    }
    EXT_RAM_BSS_ATTR static uint8_t body[4096];
    size_t out_len = 0;
    int data_obj = xc_bplist_data(b, data, len);
    int pk[1] = { xc_bplist_str(b, "data") };
    int params = xc_bplist_dict(b, pk, &data_obj, 1);
    int ck[2] = { xc_bplist_str(b, "type"), xc_bplist_str(b, "params") };
    int cv[2] = { xc_bplist_str(b, "iAPSendMessage"), params };
    int top = xc_bplist_dict(b, ck, cv, 2);
    esp_err_t err = xc_bplist_serialize(b, top, body, sizeof(body), &out_len);
    xc_bplist_free(b);
    if (err == ESP_OK) {
        event_send_command(body, out_len);
    }
}

esp_err_t xc_airplay_cluster_map_enable(void)
{
    if (!s_ev_ready || s_event_conn < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    EXT_RAM_BSS_ATTR static uint8_t body[256];
    size_t out_len = 0;

    /* showUI: open the cluster map URL on the alt display. */
    xc_bplist_t *b = xc_bplist_new();
    if (b == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int pk[2] = { xc_bplist_str(b, "uuid"), xc_bplist_str(b, "url") };
    int pv[2] = { xc_bplist_str(b, ALT_UUID),
                  xc_bplist_str(b, "maps:/car/instrumentcluster/map") };
    int params = xc_bplist_dict(b, pk, pv, 2);
    int ck[2] = { xc_bplist_str(b, "type"), xc_bplist_str(b, "params") };
    int cv[2] = { xc_bplist_str(b, "showUI"), params };
    int top = xc_bplist_dict(b, ck, cv, 2);
    esp_err_t err = xc_bplist_serialize(b, top, body, sizeof(body), &out_len);
    xc_bplist_free(b);
    if (err != ESP_OK) {
        return err;
    }
    event_send_command(body, out_len);
    ESP_LOGI(TAG, "cluster: showUI %s sent", "maps:/car/instrumentcluster/map");

    /* forceKeyFrame so the map appears immediately. */
    b = xc_bplist_new();
    if (b == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int fpk[1] = { xc_bplist_str(b, "uuid") };
    int fpv[1] = { xc_bplist_str(b, ALT_UUID) };
    int fparams = xc_bplist_dict(b, fpk, fpv, 1);
    int fck[2] = { xc_bplist_str(b, "type"), xc_bplist_str(b, "params") };
    int fcv[2] = { xc_bplist_str(b, "forceKeyFrame"), fparams };
    top = xc_bplist_dict(b, fck, fcv, 2);
    err = xc_bplist_serialize(b, top, body, sizeof(body), &out_len);
    xc_bplist_free(b);
    if (err != ESP_OK) {
        return err;
    }
    event_send_command(body, out_len);
    ESP_LOGI(TAG, "cluster: forceKeyFrame sent");
    return ESP_OK;
}

static void aux_start(void)
{
    if (s_ntp_fd >= 0) {
        return;
    }
    s_ntp_port = aux_open_udp(&s_ntp_fd);
    s_keepalive_port = aux_open_udp(&s_keepalive_fd);
    s_event_port = aux_open_tcp(&s_event_fd);
    xTaskCreatePinnedToCore(aux_udp_task, "airplay_aux", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(event_task, "airplay_evt", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "aux channels: timing=%u keepalive=%u event=%u", s_ntp_port, s_keepalive_port,
             s_event_port);
}

/* The accessory also drives clock sync towards the phone's timing port. */
static uint16_t s_peer_timing_port;
static uint32_t s_peer_ip;

static void ntp_client_task(void *arg)
{
    while (1) {
        if (s_peer_timing_port != 0 && s_peer_ip != 0) {
            uint8_t pkt[32] = { 0 };
            pkt[0] = 0x80;
            pkt[1] = 210; /* PT_REQUEST */
            pkt[2] = 0;
            pkt[3] = 7;
            int64_t us = esp_timer_get_time();
            uint64_t secs = (uint64_t)(us / 1000000) + 2208988800ULL;
            uint64_t frac = (uint64_t)((us % 1000000) * 4294.967296);
            uint64_t ts = (secs << 32) | (frac & 0xffffffffULL);
            for (int i = 0; i < 8; i++) {
                pkt[24 + i] = (uint8_t)(ts >> (56 - 8 * i));
            }
            struct sockaddr_in to = {
                .sin_family = AF_INET,
                .sin_port = htons(s_peer_timing_port),
                .sin_addr.s_addr = s_peer_ip,
            };
            sendto(s_ntp_fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- /stream SETUP ---------------------------------------------------- */

/* RTSP TEARDOWN body is {"streams": [type, ...]} (LIVI cpStack). Audio streams
 * are short-lived (one per navigation prompt) and closed by type here. */
static void handle_teardown(int fd, const rtsp_req_t *req)
{
    if (req->body_len > 0) {
        xc_plist_t pl;
        if (xc_plist_open(req->body, req->body_len, &pl) == ESP_OK) {
            int root = xc_plist_root(&pl);
            int streams = xc_plist_dict_get(&pl, root, "streams");
            int count = (streams >= 0) ? xc_plist_array_count(&pl, streams) : 0;
            for (int i = 0; i < count; i++) {
                int64_t type = 0;
                if (xc_plist_get_int(&pl, xc_plist_array_get(&pl, streams, i), &type)) {
                    ESP_LOGI(TAG, "TEARDOWN stream type %d", (int)type);
                    xc_audio_close((uint16_t)type);
                }
            }
        }
    }
    send_status(fd, "200 OK", req);
}

#define STREAM_TYPE_MAIN_SCREEN 110
#define STREAM_TYPE_ALT_SCREEN 111
#define STREAM_TYPE_MAIN_AUDIO 100
#define STREAM_TYPE_ALT_AUDIO 101
#define STREAM_TYPE_MAIN_HIGH_AUDIO 102

static void handle_stream(int fd, const rtsp_req_t *req)
{
    if (!s_has_shared) {
        ESP_LOGW(TAG, "/stream without pair-verify");
        send_status(fd, "403 Forbidden", req);
        return;
    }
    xc_plist_t pl;
    if (xc_plist_open(req->body, req->body_len, &pl) != ESP_OK) {
        ESP_LOGW(TAG, "/stream: bad plist (%u bytes)", (unsigned)req->body_len);
        send_status(fd, "400 Bad Request", req);
        return;
    }

    int root = xc_plist_root(&pl);
    int streams = xc_plist_dict_get(&pl, root, "streams");
    int count = (streams >= 0) ? xc_plist_array_count(&pl, streams) : 0;
    ESP_LOGI(TAG, "/stream SETUP: %d stream(s)", count);
    if (count == 0) {
        /* Device-information SETUP: the phone sends its own parameters and
         * expects the timing / keep-alive / event channels plus the CarPlay
         * feature flags back (see xcertplay AirPlaySession.handleSetup). */
        s_session_started = true;
        aux_start();

        /* Drive clock sync towards the phone's timing port (like xcertplay). */
        int64_t peer_timing = 0;
        xc_plist_get_int(&pl, xc_plist_dict_get(&pl, root, "timingPort"), &peer_timing);
        if (peer_timing > 0) {
            s_peer_timing_port = (uint16_t)peer_timing;
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0) {
                s_peer_ip = peer.sin_addr.s_addr;
            }
            static bool ntp_client_started;
            if (!ntp_client_started) {
                ntp_client_started = true;
                xTaskCreatePinnedToCore(ntp_client_task, "airplay_ntp", 4096, NULL, 4, NULL, 0);
            }
            ESP_LOGI(TAG, "phone timing port %d, starting clock sync", (int)peer_timing);
        }

        xc_bplist_t *rb = xc_bplist_new();
        if (rb == NULL) {
            send_status(fd, "500 Internal Server Error", req);
            return;
        }
        int dk[8], dv[8], m = 0;
        dk[m] = xc_bplist_str(rb, "timingPort");
        dv[m++] = xc_bplist_int(rb, s_ntp_port);
        dk[m] = xc_bplist_str(rb, "eventPort");
        dv[m++] = xc_bplist_int(rb, s_event_port);
        /* Like xcertplay/LIVI, only advertise the keep-alive port when the
         * phone asked for low-power keep-alives (keepAliveLowPower). */
        bool keep_alive_low_power = false;
        xc_plist_get_bool(&pl, xc_plist_dict_get(&pl, root, "keepAliveLowPower"),
                          &keep_alive_low_power);
        if (keep_alive_low_power) {
            dk[m] = xc_bplist_str(rb, "keepAlivePort");
            dv[m++] = xc_bplist_int(rb, s_keepalive_port);
        }
        int f1 = xc_bplist_str(rb, "iAPChannel");
        int f2 = xc_bplist_str(rb, "viewAreas");
        int f3 = xc_bplist_str(rb, "altScreen");
        int feats[3] = { f1, f2, f3 };
        dk[m] = xc_bplist_str(rb, "enabledFeatures");
        dv[m++] = xc_bplist_array(rb, feats, 3);
        int top = xc_bplist_dict(rb, dk, dv, m);

        static uint8_t reply[512];
        size_t reply_len = 0;
        esp_err_t err = xc_bplist_serialize(rb, top, reply, sizeof(reply), &reply_len);
        xc_bplist_free(rb);
        if (err != ESP_OK) {
            send_status(fd, "500 Internal Server Error", req);
            return;
        }
        char hdr[192];
        int hn = req->cseq[0]
                     ? snprintf(hdr, sizeof(hdr),
                                "RTSP/1.0 200 OK\r\nContent-Type: application/x-apple-binary-plist\r\n"
                                "CSeq: %s\r\nContent-Length: %u\r\n\r\n",
                                req->cseq, (unsigned)reply_len)
                     : snprintf(hdr, sizeof(hdr),
                                "RTSP/1.0 200 OK\r\nContent-Type: application/x-apple-binary-plist\r\n"
                                "Content-Length: %u\r\n\r\n",
                                (unsigned)reply_len);
        send_response(fd, hdr, (size_t)hn, reply, reply_len);
        ESP_LOGI(TAG, "SETUP reply: timing=%u event=%u keepalive=%u features=iAPChannel,viewAreas,altScreen",
                 s_ntp_port, s_event_port, s_keepalive_port);
        return;
    }

    xc_bplist_t *b = xc_bplist_new();
    if (b == NULL) {
        send_status(fd, "500 Internal Server Error", req);
        return;
    }
    int vals[12];
    int nv = 0;

    for (int i = 0; i < count && nv < 12; i++) {
        int s = xc_plist_array_get(&pl, streams, i);
        int64_t type = 0;
        xc_plist_get_int(&pl, xc_plist_dict_get(&pl, s, "type"), &type);
        char id[24] = { 0 };
        xc_plist_get_u64_str(&pl, xc_plist_dict_get(&pl, s, "streamConnectionID"), id, sizeof(id));

        if ((type == STREAM_TYPE_MAIN_SCREEN || type == STREAM_TYPE_ALT_SCREEN) && id[0]) {
            char salt[64];
            snprintf(salt, sizeof(salt), "DataStream-Salt%s", id);
            uint8_t key[32];
            if (xc_hkdf_sha512((const uint8_t *)salt, strlen(salt), s_shared, sizeof(s_shared),
                               (const uint8_t *)"DataStream-Output-Encryption-Key",
                               strlen("DataStream-Output-Encryption-Key"), key, sizeof(key)) ==
                ESP_OK) {
                uint16_t port = 0;
                if (xc_video_start((uint16_t)type, key, &port) == ESP_OK) {
                    int dk[2] = { xc_bplist_str(b, "type"), xc_bplist_str(b, "dataPort") };
                    int dv[2] = { xc_bplist_int(b, (uint64_t)type), xc_bplist_int(b, port) };
                    vals[nv++] = xc_bplist_dict(b, dk, dv, 2);
                    ESP_LOGI(TAG, "/stream: screen type %d -> dataPort %u (id %s)", (int)type, port,
                             id);
                    continue;
                }
            }
        }
        if ((type == STREAM_TYPE_MAIN_AUDIO || type == STREAM_TYPE_ALT_AUDIO ||
             type == STREAM_TYPE_MAIN_HIGH_AUDIO) &&
            id[0]) {
            char salt[64];
            snprintf(salt, sizeof(salt), "DataStream-Salt%s", id);
            uint8_t key[32];
            int64_t fmt = 0;
            xc_plist_get_int(&pl, xc_plist_dict_get(&pl, s, "audioFormat"), &fmt);
            char atype[33] = "default";
            if (!xc_plist_get_str(&pl, xc_plist_dict_get(&pl, s, "audioType"), atype,
                                  sizeof(atype))) {
                memcpy(atype, "default", sizeof("default"));
            }
            uint64_t conn_id = strtoull(id, NULL, 10);
            uint16_t dport = 0, cport = 0;
            if (xc_hkdf_sha512((const uint8_t *)salt, strlen(salt), s_shared, sizeof(s_shared),
                               (const uint8_t *)"DataStream-Output-Encryption-Key",
                               strlen("DataStream-Output-Encryption-Key"), key, sizeof(key)) ==
                    ESP_OK &&
                xc_audio_open((uint16_t)type, conn_id, key, (uint64_t)fmt, atype, &dport,
                              &cport) == ESP_OK) {
                /* The phone needs its streamConnectionID echoed back or it
                 * tears the stream down immediately (LIVI cpStack). */
                int dk[4] = { xc_bplist_str(b, "type"), xc_bplist_str(b, "dataPort"),
                              xc_bplist_str(b, "controlPort"),
                              xc_bplist_str(b, "streamConnectionID") };
                int dv[4] = { xc_bplist_int(b, (uint64_t)type), xc_bplist_int(b, dport),
                              xc_bplist_int(b, cport), xc_bplist_int(b, conn_id) };
                vals[nv++] = xc_bplist_dict(b, dk, dv, 4);
                ESP_LOGI(TAG, "/stream: audio type %d -> dataPort %u controlPort %u (id %s)",
                         (int)type, dport, cport, id);
                continue;
            }
            ESP_LOGW(TAG, "/stream: audio type %d rejected (id %s)", (int)type, id);
        }
        /* Unsupported stream types (data, ...) are omitted from the reply,
         * like xcertplay/LIVI, so iOS only sees the streams we accept. */
        ESP_LOGI(TAG, "/stream: stream type %d unsupported, skipped", (int)type);
    }

    EXT_RAM_BSS_ATTR static uint8_t out[1024];
    size_t out_len = 0;
    int arr = xc_bplist_array(b, vals, nv);
    int k = xc_bplist_str(b, "streams");
    int top = xc_bplist_dict(b, &k, &arr, 1);
    esp_err_t err = xc_bplist_serialize(b, top, out, sizeof(out), &out_len);
    xc_bplist_free(b);

    if (err != ESP_OK) {
        send_status(fd, "500 Internal Server Error", req);
        return;
    }
    char hdr[192];
    int hn = req->cseq[0]
                 ? snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\nContent-Type: application/x-apple-binary-plist\r\n"
                            "CSeq: %s\r\nContent-Length: %u\r\n\r\n",
                            req->cseq, (unsigned)out_len)
                 : snprintf(hdr, sizeof(hdr),
                            "RTSP/1.0 200 OK\r\nContent-Type: application/x-apple-binary-plist\r\n"
                            "Content-Length: %u\r\n\r\n",
                            (unsigned)out_len);
    send_response(fd, hdr, (size_t)hn, out, out_len);
}

/* ---- server ----------------------------------------------------------- */

static void handle_client(int fd)
{
    ctrl_reset();
    while (1) {
        rtsp_req_t req;
        if (read_request(fd, &req) != 0) {
            ESP_LOGW(TAG, "control connection ended (read_request failed)");
            break;
        }
        if (strstr(req.path, "/info") != NULL) {
            send_info(fd, &req);
        } else if (strstr(req.path, "/pair-setup") != NULL) {
            handle_pair_setup(fd, &req);
        } else if (strstr(req.path, "/pair-verify") != NULL) {
            handle_pair_verify(fd, &req);
        } else if (strstr(req.path, "/auth-setup") != NULL) {
            handle_auth_setup(fd, &req);
        } else if (strncmp(req.method, "TEARDOWN", 8) == 0) {
            handle_teardown(fd, &req);
        } else if (strstr(req.path, "/stream") != NULL ||
                   strncmp(req.method, "SETUP", 5) == 0) {
            handle_stream(fd, &req);
        } else {
            /* iOS keep-alive chatter (/command, /feedback, OPTIONS) is answered
             * with a bare 200, like xcertplay's unhandled-request path. Command
             * bodies are inspected: iAPSendMessage carries the iAP2 stream when
             * the phone moves it onto the CarPlay tunnel, disableBluetooth asks
             * for that handoff, the rest is CarPlay UI/audio keep-alive. */
            bool quiet = false;
            if (strncmp(req.method, "POST", 4) == 0 && strstr(req.path, "/command") != NULL &&
                req.body_len > 0) {
                xc_plist_t cpl;
                if (xc_plist_open(req.body, req.body_len, &cpl) == ESP_OK) {
                    char ctype[64] = { 0 };
                    if (xc_plist_get_str(
                            &cpl, xc_plist_dict_get(&cpl, xc_plist_root(&cpl), "type"), ctype,
                            sizeof(ctype)) &&
                        ctype[0]) {
                        if (strcmp(ctype, "iAPSendMessage") == 0) {
                            quiet = true;
                            int params = xc_plist_dict_get(&cpl, xc_plist_root(&cpl), "params");
                            int data_node =
                                params >= 0 ? xc_plist_dict_get(&cpl, params, "data") : -1;
                            const uint8_t *dp = NULL;
                            size_t dl = 0;
                            if (data_node >= 0 && xc_plist_get_data(&cpl, data_node, &dp, &dl) &&
                                dl > 0) {
                                xc_link_tunnel_rx(dp, dl);
                            }
                        } else {
                            ESP_LOGI(TAG, "command type='%s'", ctype);
                            if (strcmp(ctype, "disableBluetooth") == 0) {
                                ESP_LOGI(TAG,
                                         "disableBluetooth: handing iAP2 to the CarPlay tunnel");
                                xc_link_enter_tunnel();
                            }
                            if ((strcmp(ctype, "modesChanged") == 0 ||
                                 strcmp(ctype, "duckAudio") == 0 ||
                                 strcmp(ctype, "unduckAudio") == 0) &&
                                req.body_len <= 256) {
                                char hx[3 * 256 + 1];
                                for (size_t k = 0; k < req.body_len; k++) {
                                    sprintf(hx + k * 3, "%02x ", req.body[k]);
                                }
                                hx[req.body_len * 3] = '\0';
                                ESP_LOGI(TAG, "command body: %s", hx);
                            }
                        }
                    }
                }
            }
            if (!quiet) {
                ESP_LOGI(TAG, "ack request %s %s (%u body bytes)", req.method, req.path,
                         (unsigned)req.body_len);
            }
            send_status(fd, "200 OK", &req);
        }
        if (!req.keep_alive) {
            break;
        }
    }
}

static void rtsp_task(void *arg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(XC_AIRPLAY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 4) != 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "AirPlay RTSP listening on port %d", XC_AIRPLAY_PORT);

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int fd = accept(listen_fd, (struct sockaddr *)&src, &sl);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        ESP_LOGI(TAG, "control connection from %s:%u", inet_ntoa(src.sin_addr),
                 ntohs(src.sin_port));
        handle_client(fd);
        close(fd);
        ESP_LOGW(TAG, "control connection closed");
    }
}

static void selftest_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(XC_AIRPLAY_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "selftest connect failed");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    const char *req = "GET /info RTSP/1.0\r\nUser-Agent: xcertplay-selftest\r\n\r\n";
    send_all(fd, req, strlen(req));
    /* Read the whole reply and hex-dump the /info body once at boot: the phone
     * decides on audio (Opus formats) etc. from exactly these bytes, so a dump
     * makes wire-level diffs against the reference encoder possible. */
    uint8_t *resp = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resp == NULL) {
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    int total = 0;
    int body_off = -1;
    long content_len = -1;
    while (total < 8192) {
        int r = recv(fd, resp + total, 8192 - total, 0);
        if (r <= 0) {
            break;
        }
        total += r;
        if (body_off < 0) {
            for (int i = 0; i + 3 < total; i++) {
                if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' &&
                    resp[i + 3] == '\n') {
                    body_off = i + 4;
                    if (body_off < total) {
                        uint8_t saved = resp[body_off];
                        resp[body_off] = 0;
                        char *cl = strstr((char *)resp, "Content-Length: ");
                        if (cl != NULL) {
                            content_len = strtol(cl + 16, NULL, 10);
                        }
                        resp[body_off] = saved;
                    }
                    break;
                }
            }
        }
        if (body_off >= 0 && content_len >= 0 && total - body_off >= content_len) {
            break;
        }
    }
    if (total > 0) {
        if (body_off < 0) {
            body_off = total; /* no body */
        }
        ESP_LOGI(TAG, "selftest /info body %u bytes", (unsigned)(total - body_off));
        const uint8_t *body = resp + body_off;
        int blen = total - body_off;
        for (int off = 0; off < blen; off += 96) {
            char hx[3 * 96 + 1];
            int n = blen - off < 96 ? blen - off : 96;
            for (int i = 0; i < n; i++) {
                sprintf(hx + i * 3, "%02x ", body[off + i]);
            }
            hx[n * 3] = '\0';
            ESP_LOGI(TAG, "INFO %04x: %s", (unsigned)off, hx);
        }
    } else {
        ESP_LOGW(TAG, "selftest no response");
    }
    free(resp);
    close(fd);
    vTaskDelete(NULL);
}

esp_err_t xc_airplay_start(void)
{
    /* When the phone hands iAP2 over to the CarPlay tunnel, link frames ship
     * as event-channel iAPSendMessage commands (see xc_bt_link.c). */
    xc_link_set_tunnel_tx(tunnel_tx);

    /* Caller-ID card on the cluster: feed it the phone's CallStateUpdate
     * stream and it overlays itself over the map while a call is up. */
    xc_callcard_init();
    xc_iap2_set_call_state_cb(xc_callcard_on_call);

    /* Keep these large buffers out of internal RAM: ESP-Hosted needs internal
     * DMA memory for its SDIO mempool at boot. */
    if (s_conn == NULL) {
        s_conn = heap_caps_malloc(XC_CONN_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_plist == NULL) {
        s_plist = heap_caps_malloc(XC_PLIST_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_ct == NULL) {
        s_ct = heap_caps_malloc(XC_CTRL_MAX + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_plain == NULL) {
        s_plain = heap_caps_malloc(XC_CTRL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_tx == NULL) {
        s_tx = heap_caps_malloc(XC_CTRL_MAX + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_conn == NULL || s_plist == NULL || s_ct == NULL || s_plain == NULL || s_tx == NULL) {
        ESP_LOGE(TAG, "failed to allocate AirPlay buffers");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(rtsp_task, "airplay_rtsp", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(selftest_task, "airplay_selftest", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}
