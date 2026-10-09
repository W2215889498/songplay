#include "xc_video.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/sockets.h"
#include "wels_dec.h"
#include "xc_airplay.h"
#include "xc_callcard.h"
#include "xc_crypto.h"

static const char *TAG = "xc_video";

#define HEADER_LEN 128
#define OP_VIDEO_FRAME 0
#define OP_VIDEO_CONFIG 1
#define BODY_MAX (2 * 1024 * 1024)
#define TAG_LEN 16
#define PANEL_W 480
#define PANEL_H 800

#define STREAM_TYPE_MAIN_SCREEN 110
#define STREAM_TYPE_ALT_SCREEN 111

/* Two data streams arrive in parallel: the CarPlay main screen (110) and the
 * instrument-cluster/alt screen (111). The P4 has one panel and this product
 * shows the cluster, so channel 1 decodes and renders, channel 0 only drains
 * its socket (the phone keeps streaming to both). */
typedef struct {
    uint16_t type;
    bool display;
    int listen_fd;
    int conn_fd;
    uint8_t key[32];
    uint64_t counter;
    wels_dec_t *dec;
    volatile bool dec_ready;
    bool has_config;
    int nal_length_size;
    int video_w, video_h;
    volatile uint32_t frames;
    int64_t stat_us;        /* periodic fps reporting window */
    uint32_t stat_frames;
    TaskHandle_t task;
} video_chan_t;

static video_chan_t s_ch[2];
static volatile bool s_running;
static bool s_cluster_activated;

/* Asking the phone to render the cluster map (showUI + forceKeyFrame) is only
 * valid once the main screen stream is up (LIVI gates on mainStreamReady) and
 * the cluster stream sent its codec config. */
static void maybe_activate_cluster(void)
{
    if (s_cluster_activated) {
        return;
    }
    if (s_ch[0].conn_fd < 0 || !s_ch[1].has_config) {
        return;
    }
    if (xc_airplay_cluster_map_enable() == ESP_OK) {
        s_cluster_activated = true;
        ESP_LOGI(TAG, "cluster map activated (showUI + forceKeyFrame)");
    }
}

static uint8_t *s_body;   /* PSRAM, cluster channel only */
static uint8_t *s_plain;  /* PSRAM, cluster channel only */
static uint8_t *s_annexb; /* PSRAM, cluster channel only */
static uint16_t *s_rgb;   /* PSRAM */
static bool s_ui_cleared;

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

static void nonce64(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 12);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(counter >> (8 * i));
    }
}

/* Diagnostic: sample the pixel we just wrote in each DPI frame buffer and log
 * whenever the "current display buffer" changes. The adapter's tear-avoid
 * fb rotation can silently swap the video area back to a stale buffer; this
 * pinpoints when and where that happens. */
static void fb_probe(uint16_t ref, int x0, int y0)
{
    static void *fbs[3];
    static int have;
    static int last = -1;
    if (have == 0) {
        if (esp_lcd_dpi_panel_get_frame_buffer(bsp_display_get_panel_handle(), 3,
                                               &fbs[0], &fbs[1], &fbs[2]) != ESP_OK) {
            have = -1;
            return;
        }
        have = 1;
    }
    if (have < 0) {
        return;
    }
    uint16_t v[3];
    int match = -1;
    for (int i = 0; i < 3; i++) {
        v[i] = ((const uint16_t *)fbs[i])[(size_t)y0 * PANEL_W + x0];
        if (v[i] == ref) {
            match = i;
        }
    }
    if (match != last) {
        ESP_LOGI(TAG, "fb probe: match %d -> %d (ref=%04x fb=[%04x %04x %04x])",
                 last, match, ref, v[0], v[1], v[2]);
        last = match;
    }
}

/* Flicker diagnostics: watch the margin pixels of all three DPI frame buffers
 * (outside the video rect) and log any change. A full-screen wipe by whatever
 * writer shows up here; a panel-only artifact does not. */
static void fb_watch_task(void *arg)
{
    (void)arg;
    void *fbs[3] = { NULL, NULL, NULL };
    if (esp_lcd_dpi_panel_get_frame_buffer(bsp_display_get_panel_handle(), 3,
                                           &fbs[0], &fbs[1], &fbs[2]) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    uint16_t prev[6] = { 0 };
    bool have = false;
    while (s_running) {
        uint16_t cur[6];
        for (int i = 0; i < 3; i++) {
            const uint16_t *fb = (const uint16_t *)fbs[i];
            cur[i * 2 + 0] = fb[(size_t)8 * PANEL_W + 8];
            cur[i * 2 + 1] = fb[(size_t)(PANEL_H - 8) * PANEL_W + (PANEL_W - 8)];
        }
        if (!have || memcmp(cur, prev, sizeof(cur)) != 0) {
            ESP_LOGI(TAG, "fbwatch TL=[%04x %04x %04x] BR=[%04x %04x %04x]",
                     cur[0], cur[2], cur[4], cur[1], cur[3], cur[5]);
            memcpy(prev, cur, sizeof(cur));
            have = true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}

/* Length-prefixed NALUs -> Annex B. */
static size_t to_annexb(const uint8_t *in, size_t len, int length_size, uint8_t *out, size_t cap)
{
    size_t r = 0, w = 0;
    while (r + (size_t)length_size <= len) {
        size_t nalu = 0;
        for (int i = 0; i < length_size; i++) {
            nalu = (nalu << 8) | in[r + i];
        }
        r += (size_t)length_size;
        if (nalu == 0 || r + nalu > len || w + 4 + nalu > cap) {
            break;
        }
        out[w++] = 0;
        out[w++] = 0;
        out[w++] = 0;
        out[w++] = 1;
        memcpy(out + w, in + r, nalu);
        w += nalu;
        r += nalu;
    }
    return w;
}

/* avcC record -> Annex B SPS/PPS prefix for the decoder. */
static size_t avcc_to_annexb(const uint8_t *avcc, size_t len, uint8_t *out, size_t cap)
{
    if (len < 7) {
        return 0;
    }
    size_t w = 0, p = 6;
    int num_sps = avcc[5] & 0x1f;
    for (int i = 0; i < num_sps; i++) {
        if (p + 2 > len) {
            return 0;
        }
        size_t n = ((size_t)avcc[p] << 8) | avcc[p + 1];
        p += 2;
        if (p + n > len || w + 4 + n > cap) {
            return 0;
        }
        out[w++] = 0; out[w++] = 0; out[w++] = 0; out[w++] = 1;
        memcpy(out + w, avcc + p, n);
        w += n;
        p += n;
    }
    if (p + 1 > len) {
        return 0;
    }
    int num_pps = avcc[p++];
    for (int i = 0; i < num_pps; i++) {
        if (p + 2 > len) {
            return 0;
        }
        size_t n = ((size_t)avcc[p] << 8) | avcc[p + 1];
        p += 2;
        if (p + n > len || w + 4 + n > cap) {
            return 0;
        }
        out[w++] = 0; out[w++] = 0; out[w++] = 0; out[w++] = 1;
        memcpy(out + w, avcc + p, n);
        w += n;
        p += n;
    }
    return w;
}

/* ---- YUV420 -> RGB565 conversion (PPA hardware, CPU fallback) --------- */

/* The P4 PPA wants "Espressif YUV420" (fourcc OUYY_EVYY): each 2x2 luma block
 * is packed into 6 bytes -- row0 = U Y00 Y01, row1 = V Y10 Y11. openh264
 * outputs planar I420, so shuffle first; the PPA then does the colour
 * conversion in hardware (the old CPU loop cost ~25 ms at 240x400). */
static ppa_client_handle_t s_ppa;
static uint8_t *s_yuv_pack;
static size_t s_yuv_pack_sz;

static esp_err_t i420_to_es420(int w, int h, const uint8_t *y, int ys, const uint8_t *u,
                               const uint8_t *v, int uvs, int sx, int sy)
{
    if ((w & 1) || (h & 1) || (sx & 1) || (sy & 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t row = (size_t)w * 3 / 2;
    for (int by = 0; by < h / 2; by++) {
        const uint8_t *y0 = y + (size_t)(sy + by * 2) * ys + sx;
        const uint8_t *y1 = y0 + ys;
        const uint8_t *ur = u + (size_t)(sy / 2 + by) * uvs + sx / 2;
        const uint8_t *vr = v + (size_t)(sy / 2 + by) * uvs + sx / 2;
        uint8_t *r0 = s_yuv_pack + (size_t)(by * 2) * row;
        uint8_t *r1 = r0 + row;
        for (int bx = 0; bx < w / 2; bx++) {
            r0[0] = ur[bx];
            r0[1] = y0[2 * bx];
            r0[2] = y0[2 * bx + 1];
            r1[0] = vr[bx];
            r1[1] = y1[2 * bx];
            r1[2] = y1[2 * bx + 1];
            r0 += 3;
            r1 += 3;
        }
    }
    return ESP_OK;
}

static bool ppa_convert(int cw, int chh, const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, const uint8_t *v_plane, int uv_stride, int sx,
                        int sy)
{
    if (s_ppa == NULL) {
        ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
        if (ppa_register_client(&pc, &s_ppa) != ESP_OK) {
            ESP_LOGW(TAG, "PPA client register failed, using CPU conversion");
            s_ppa = NULL;
            return false;
        }
        ESP_LOGI(TAG, "PPA SRM client ready (hardware YUV420->RGB565)");
    }
    size_t need = (size_t)cw * chh * 3 / 2;
    if (s_yuv_pack == NULL || s_yuv_pack_sz < need) {
        if (s_yuv_pack != NULL) {
            free(s_yuv_pack);
            s_yuv_pack = NULL;
        }
        s_yuv_pack = heap_caps_aligned_alloc(128, need, MALLOC_CAP_SPIRAM);
        s_yuv_pack_sz = need;
        if (s_yuv_pack == NULL) {
            return false;
        }
    }
    if (i420_to_es420(cw, chh, y_plane, y_stride, u_plane, v_plane, uv_stride, sx, sy) !=
        ESP_OK) {
        return false;
    }
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = s_yuv_pack,
            .pic_w = (uint32_t)cw,
            .pic_h = (uint32_t)chh,
            .block_w = (uint32_t)cw,
            .block_h = (uint32_t)chh,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = s_rgb,
            .buffer_size = (uint32_t)cw * (uint32_t)chh * 2,
            .pic_w = (uint32_t)cw,
            .pic_h = (uint32_t)chh,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PPA op failed (%s), CPU fallback", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void render_frame(video_chan_t *ch, int w, int h, int decode_ms)
{
    const uint8_t *y_plane = wels_dec_plane(ch->dec, 0);
    const uint8_t *u_plane = wels_dec_plane(ch->dec, 1);
    const uint8_t *v_plane = wels_dec_plane(ch->dec, 2);
    int y_stride = wels_dec_stride(ch->dec, 0);
    int uv_stride = wels_dec_stride(ch->dec, 1);
    if (y_plane == NULL || u_plane == NULL || v_plane == NULL || y_stride <= 0 || uv_stride <= 0) {
        return;
    }
    int64_t t_render = esp_timer_get_time();

    /* The stream can be larger than the panel (e.g. the standard 800x480
     * cluster on the 480x800 dev panel): centre-crop it. */
    int cw = w > PANEL_W ? PANEL_W : w;
    int chh = h > PANEL_H ? PANEL_H : h;
    int sx = (w - cw) / 2;
    int sy = (h - chh) / 2;

    int64_t t_conv = esp_timer_get_time();
    if (!ppa_convert(cw, chh, y_plane, y_stride, u_plane, v_plane, uv_stride, sx, sy)) {
        /* CPU fallback (also the path for odd dimensions / PPA failure). */
        for (int y = 0; y < chh; y++) {
            const uint8_t *yr = y_plane + (size_t)(sy + y) * (size_t)y_stride + sx;
            const uint8_t *ur = u_plane + (size_t)((sy + y) / 2) * (size_t)uv_stride;
            const uint8_t *vr = v_plane + (size_t)((sy + y) / 2) * (size_t)uv_stride;
            for (int x = 0; x < cw; x++) {
                int yv = yr[x] - 16;
                int uv = ur[(sx + x) / 2] - 128;
                int vv = vr[(sx + x) / 2] - 128;
                if (yv < 0) {
                    yv = 0;
                }
                int r = (298 * yv + 409 * vv + 128) >> 8;
                int g = (298 * yv - 100 * uv - 208 * vv + 128) >> 8;
                int b = (298 * yv + 516 * uv + 128) >> 8;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;
                s_rgb[(size_t)y * cw + x] =
                    (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
            }
        }
    }
    int conv_ms = (int)((esp_timer_get_time() - t_conv) / 1000);

    esp_lcd_panel_handle_t panel = bsp_display_get_panel_handle();
    int x0 = (PANEL_W - cw) / 2;
    int y0 = (PANEL_H - chh) / 2;
    bsp_display_lock(-1);
    if (!s_ui_cleared) {
        lv_obj_clean(lv_screen_active()); /* stop LVGL from overdrawing */
        s_ui_cleared = true;
    }
    /* Blend the caller-ID card into the frame first: one panel write carries
     * map + card together (a separate overlay blit would flash the map
     * through the card on every frame). */
    xc_callcard_compose(s_rgb, cw, chh);
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + cw, y0 + chh, s_rgb);
    bsp_display_unlock();
    fb_probe(s_rgb[0], x0, y0);

    int64_t now_us = esp_timer_get_time();
    int render_ms = (int)((now_us - t_render) / 1000);
    if (ch->frames < 20) {
        ESP_LOGI(TAG, "ch%u frame %u timing: decode %d ms, conv %d ms, render %d ms",
                 (unsigned)ch->type, (unsigned)ch->frames, decode_ms, conv_ms, render_ms);
    }
    /* Sustained fps every 5 s (feed_decoder bumps ch->frames right after this
     * returns, so count the current frame here). */
    if (ch->stat_us == 0) {
        ch->stat_us = now_us;
        ch->stat_frames = ch->frames;
    } else if (now_us - ch->stat_us >= 5000000) {
        uint32_t n = ch->frames + 1 - ch->stat_frames;
        int64_t dt_us = now_us - ch->stat_us;
        uint32_t fps10 = (uint32_t)((uint64_t)n * 10000000ULL / (uint64_t)dt_us);
        ESP_LOGI(TAG, "ch%u fps %u.%u (%u frames in %d ms), decode %d ms, render %d ms",
                 (unsigned)ch->type, (unsigned)(fps10 / 10), (unsigned)(fps10 % 10),
                 (unsigned)n, (int)(dt_us / 1000), decode_ms, render_ms);
        ch->stat_us = now_us;
        ch->stat_frames = ch->frames + 1;
    }
}

static void feed_decoder(video_chan_t *ch, const uint8_t *data, size_t len)
{
    if (ch->dec == NULL || len == 0) {
        return;
    }
    int64_t t_decode = esp_timer_get_time();
    if (wels_dec_decode(ch->dec, data, len) != 1) {
        return; /* parameter sets / partial access unit, no picture yet */
    }
    int decode_ms = (int)((esp_timer_get_time() - t_decode) / 1000);

    int w = wels_dec_width(ch->dec);
    int h = wels_dec_height(ch->dec);
    if (w <= 0 || h <= 0) {
        return;
    }
    if (ch->video_w != w || ch->video_h != h) {
        ch->video_w = w;
        ch->video_h = h;
        ESP_LOGI(TAG, "ch%u decoded resolution %dx%d", (unsigned)ch->type, w, h);
    }
    if (!ch->display || s_rgb == NULL) {
        ch->frames++;
        return;
    }
    render_frame(ch, w, h, decode_ms);
    ch->frames++;
}

static void handle_message(video_chan_t *ch, const uint8_t *header, size_t body_len)
{
    uint8_t opcode = header[4];
    if (opcode == OP_VIDEO_CONFIG) {
        const char *codec = "unknown";
        size_t avcc_len = 0;
        const uint8_t *avcc = NULL;
        for (size_t i = 4; i + 8 <= body_len; i++) {
            if (memcmp(s_body + i, "avcC", 4) == 0) {
                codec = "h264";
                avcc = s_body + i + 4;
                avcc_len = body_len - (i + 4);
                if (avcc_len >= 5) {
                    ch->nal_length_size = (avcc[4] & 3) + 1;
                }
                break;
            }
            if (memcmp(s_body + i, "hvcC", 4) == 0) {
                codec = "h265";
                break;
            }
        }
        /* The iPhone sends the raw avcC record (configurationVersion == 1 as
         * the first payload byte), not the 4-byte + "avcC" framing the PC
         * test tool uses. */
        if (avcc == NULL && body_len >= 7 && s_body[0] == 1) {
            codec = "h264";
            avcc = s_body;
            avcc_len = body_len;
            ch->nal_length_size = (avcc[4] & 3) + 1;
        }
        ch->has_config = true;
        ch->dec_ready = false;
        ESP_LOGI(TAG, "ch%u video config: %s, %u bytes (avcC %u), NAL length size %d",
                 (unsigned)ch->type, codec, (unsigned)body_len, (unsigned)avcc_len,
                 ch->nal_length_size);
        maybe_activate_cluster();

        if (avcc != NULL) {
            /* Rebuild the decoder per stream: the parameter sets arrive with
             * every SETUP and a stale decoder will never activate them. */
            if (ch->dec != NULL) {
                wels_dec_destroy(ch->dec);
                ch->dec = NULL;
            }
            ch->video_w = 0;
            ch->video_h = 0;
            ch->dec = wels_dec_create();
            if (ch->dec != NULL) {
                size_t n = avcc_to_annexb(avcc, avcc_len, s_annexb, BODY_MAX);
                ESP_LOGI(TAG, "ch%u openh264 decoder ready; feeding %u bytes of SPS/PPS",
                         (unsigned)ch->type, (unsigned)n);
                if (n > 0) {
                    feed_decoder(ch, s_annexb, n);
                    ch->dec_ready = true;
                }
            } else {
                ESP_LOGE(TAG, "ch%u openh264 decoder init failed", (unsigned)ch->type);
                ch->dec = NULL;
            }
        }
        return;
    }
    if (opcode != OP_VIDEO_FRAME) {
        ESP_LOGW(TAG, "ch%u unknown video opcode %u (%u bytes)", (unsigned)ch->type, opcode,
                 (unsigned)body_len);
        return;
    }

    uint8_t nonce[12];
    nonce64(ch->counter, nonce);
    if (body_len < TAG_LEN) {
        ch->counter++;
        return;
    }
    size_t ct_len = body_len - TAG_LEN;
    if (xc_chacha20poly1305_open(ch->key, nonce, header, HEADER_LEN, s_body, ct_len,
                                 s_body + ct_len, s_plain) != ESP_OK) {
        ESP_LOGW(TAG, "ch%u frame %u: decrypt failed", (unsigned)ch->type,
                 (unsigned)ch->counter);
        ch->counter++;
        return;
    }
    ch->counter++;

    /* Retry cluster activation on every frame until the gate (main stream up,
     * cluster config seen) is satisfied; iOS ignores an early showUI. */
    if (ch->display) {
        maybe_activate_cluster();
    }

    size_t n = to_annexb(s_plain, ct_len, ch->nal_length_size, s_annexb, BODY_MAX);
    if (ch->dec_ready) {
        int64_t t0 = esp_timer_get_time();
        feed_decoder(ch, s_annexb, n);
        if (ch->frames < 3) {
            ESP_LOGI(TAG, "ch%u frame %u: %u sealed -> %u annex-b, total %d ms",
                     (unsigned)ch->type, (unsigned)ch->frames, (unsigned)body_len, (unsigned)n,
                     (int)((esp_timer_get_time() - t0) / 1000));
        }
    } else if (ch->frames == 0 && ch->has_config == false) {
        /* Frames before the config would be dropped anyway. */
    }
}

/* Drain-only channel: the phone streams to it, we just keep the socket fed.
 * No decryption is needed, so no per-frame state. */
static void drain_conn(int fd)
{
    uint8_t header[HEADER_LEN];
    uint8_t scratch[4096];
    uint32_t frames = 0;
    while (s_running) {
        if (recv_all(fd, header, HEADER_LEN) != 0) {
            break;
        }
        size_t body_len = (size_t)header[0] | ((size_t)header[1] << 8) |
                          ((size_t)header[2] << 16) | ((size_t)header[3] << 24);
        if (body_len > BODY_MAX) {
            break;
        }
        while (body_len > 0) {
            size_t chunk = body_len > sizeof(scratch) ? sizeof(scratch) : body_len;
            if (recv_all(fd, scratch, chunk) != 0) {
                return;
            }
            body_len -= chunk;
        }
        frames++;
    }
    ESP_LOGW(TAG, "main stream drained (%u messages)", (unsigned)frames);
}

static void video_task(void *arg)
{
    video_chan_t *ch = (video_chan_t *)arg;
    while (s_running) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int fd = accept(ch->listen_fd, (struct sockaddr *)&src, &sl);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        ch->conn_fd = fd;
        ch->counter = 0; /* the phone restarts the frame counter per connection */
        ch->dec_ready = false;
        ESP_LOGI(TAG, "ch%u video stream connected", (unsigned)ch->type);
        maybe_activate_cluster();

        if (!ch->display) {
            drain_conn(fd);
            close(fd);
            ch->conn_fd = -1;
            continue;
        }

        uint8_t header[HEADER_LEN];
        while (s_running) {
            if (recv_all(fd, header, HEADER_LEN) != 0) {
                break;
            }
            size_t body_len = (size_t)header[0] | ((size_t)header[1] << 8) |
                              ((size_t)header[2] << 16) | ((size_t)header[3] << 24);
            if (body_len > BODY_MAX) {
                ESP_LOGW(TAG, "bad body length %u", (unsigned)body_len);
                break;
            }
            if (body_len == 0) {
                /* Zero-length control/keep-alive message from the phone. */
                continue;
            }
            if (recv_all(fd, s_body, body_len) != 0) {
                break;
            }
            handle_message(ch, header, body_len);
        }
        close(fd);
        ch->conn_fd = -1;
        ESP_LOGW(TAG, "ch%u video stream disconnected (%u frames)", (unsigned)ch->type,
                 (unsigned)ch->frames);
    }
    vTaskDelete(NULL);
}

static video_chan_t *chan_for_type(uint16_t type)
{
    if (type == STREAM_TYPE_ALT_SCREEN) {
        return &s_ch[1];
    }
    return &s_ch[0];
}

esp_err_t xc_video_start(uint16_t type, const uint8_t key[32], uint16_t *out_port)
{
    video_chan_t *ch = chan_for_type(type);

    if (s_body == NULL) {
        s_body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_plain = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_annexb = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* PPA output requires L1+L2 cache-line alignment (128 B here). */
        s_rgb = heap_caps_aligned_alloc(128, PANEL_W * PANEL_H * 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* -1 marks "no connection": a zero-initialized fd made the cluster
         * activation gate pass before the main stream was even connected. */
        s_ch[0].conn_fd = -1;
        s_ch[1].conn_fd = -1;
    }
    if (s_body == NULL || s_plain == NULL || s_annexb == NULL || s_rgb == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ch->type = type;
    ch->display = (type == STREAM_TYPE_ALT_SCREEN); /* the cluster is what we show */
    memcpy(ch->key, key, 32);
    ch->frames = 0;
    ch->has_config = false;
    ch->conn_fd = -1;

    if (ch->listen_fd <= 0) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            return ESP_FAIL;
        }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
            close(fd);
            return ESP_FAIL;
        }
        ch->listen_fd = fd;
    }

    struct sockaddr_in bound;
    socklen_t bl = sizeof(bound);
    if (getsockname(ch->listen_fd, (struct sockaddr *)&bound, &bl) != 0) {
        return ESP_FAIL;
    }
    if (out_port) {
        *out_port = ntohs(bound.sin_port);
    }

    s_running = true;
    if (ch->task == NULL) {
        xTaskCreatePinnedToCore(video_task, ch->display ? "xc_vid_cluster" : "xc_vid_main", 8192,
                                ch, 5, &ch->task, 0);
    }
    static bool watch_started;
    if (!watch_started) {
        watch_started = true;
        xTaskCreatePinnedToCore(fb_watch_task, "xc_fbwatch", 3072, NULL, 2, NULL, 0);
    }
    ESP_LOGI(TAG, "ch%u listener on port %u (%s)", (unsigned)type, out_port ? *out_port : 0,
             ch->display ? "decode+display" : "drain");
    return ESP_OK;
}

void xc_video_stop(void)
{
    s_running = false;
    for (int i = 0; i < 2; i++) {
        if (s_ch[i].conn_fd >= 0) {
            close(s_ch[i].conn_fd);
            s_ch[i].conn_fd = -1;
        }
        if (s_ch[i].listen_fd > 0) {
            close(s_ch[i].listen_fd);
            s_ch[i].listen_fd = -1;
        }
    }
}

uint32_t xc_video_frame_count(void)
{
    return s_ch[1].frames; /* the displayed channel */
}

bool xc_video_has_config(void)
{
    return s_ch[1].has_config;
}

/* ---- encode / decode / display self test ------------------------------ */

void xc_video_lcd_selftest(void)
{
    const int w = 480, h = 272;
    if (s_rgb == NULL) {
        s_rgb = heap_caps_aligned_alloc(128, PANEL_W * PANEL_H * 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_annexb == NULL) {
        s_annexb = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    /* Cache sync needs 128-byte alignment, so round every buffer up. */
    size_t yuv_size = (((size_t)w * h * 2) + 127) & ~(size_t)127;
    size_t enc_size = (((size_t)w * h) + 127) & ~(size_t)127;
    uint8_t *yuv = heap_caps_aligned_calloc(128, 1, yuv_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *enc_buf = heap_caps_aligned_calloc(128, 1, enc_size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rgb == NULL || s_annexb == NULL || yuv == NULL || enc_buf == NULL) {
        ESP_LOGE(TAG, "selftest: out of memory");
        goto done;
    }

    /* colour bars, YUYV (the format the hardware encoder takes) */
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
        { 255, 0, 255 },   { 255, 0, 0 },   { 0, 0, 255 },   { 32, 32, 32 },
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x += 2) {
            const uint8_t *c = bars[(x * 8) / w];
            int yy = (77 * c[0] + 150 * c[1] + 29 * c[2]) >> 8;
            int u = 128 + (((int)c[2] - yy) * 144 >> 8);
            int v = 128 + (((int)c[0] - yy) * 183 >> 8);
            uint8_t *p = yuv + ((size_t)y * w + x) * 2;
            p[0] = (uint8_t)yy;
            p[1] = (uint8_t)u;
            p[2] = (uint8_t)yy;
            p[3] = (uint8_t)v;
        }
    }

    /* The prebuilt encoder library for this P4 revision accepts a format the
     * header does not advertise, so try every candidate once. */
    static const struct {
        esp_h264_raw_format_t fmt;
        const char *name;
        int bytes_per_px;
    } fmts[] = {
        { ESP_H264_RAW_FMT_YUYV, "YUYV", 2 },
        { ESP_H264_RAW_FMT_UYVY, "UYVY", 2 },
        { ESP_H264_RAW_FMT_VUY, "VUY", 2 },
        { ESP_H264_RAW_FMT_O_UYY_E_VYY, "O_UYY_E_VYY", 2 },
        { ESP_H264_RAW_FMT_RGB565_LE, "RGB565_LE", 2 },
        { ESP_H264_RAW_FMT_BGR888, "BGR888", 3 },
        { ESP_H264_RAW_FMT_I420, "I420", 2 },
    };

    esp_h264_enc_handle_t enc = NULL;
    esp_h264_enc_cfg_hw_t cfg = {
        .gop = 1,
        .fps = 10,
        .res = { .width = w, .height = h },
        .rc = { .bitrate = 400000, .qp_min = 20, .qp_max = 40 },
    };
    const char *accepted = NULL;
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        cfg.pic_type = fmts[i].fmt;
        if (esp_h264_enc_hw_new(&cfg, &enc) == ESP_H264_ERR_OK && enc != NULL) {
            accepted = fmts[i].name;
            ESP_LOGI(TAG, "selftest: hardware encoder accepts %s", accepted);
            break;
        }
        enc = NULL;
    }
    if (accepted == NULL) {
        ESP_LOGE(TAG, "selftest: no accepted encoder input format");
        goto done;
    }

    /* Luma ramp with neutral chroma; the exact packing differs per format but
     * any non-uniform image proves the decode/display path. */
    for (size_t i = 0; i < (size_t)w * h * 2; i++) {
        yuv[i] = (uint8_t)((i / 2) % 256);
    }

    if (esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "selftest: hw encoder open failed");
        esp_h264_enc_del(enc);
        goto done;
    }
    esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = yuv, .len = (uint32_t)yuv_size } };
    esp_h264_enc_out_frame_t out = { .raw_data = { .buffer = enc_buf, .len = (uint32_t)enc_size } };
    esp_h264_err_t e = esp_h264_enc_process(enc, &in, &out);
    if (e == ESP_H264_ERR_OK && out.length > 0) {
        ESP_LOGI(TAG, "selftest: encoded %u bytes (type %d), decoding", (unsigned)out.length,
                 out.frame_type);
        static video_chan_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.type = STREAM_TYPE_ALT_SCREEN;
        tmp.display = true;
        tmp.dec = wels_dec_create();
        tmp.nal_length_size = 4;
        memcpy(s_annexb, enc_buf, out.length);
        feed_decoder(&tmp, s_annexb, out.length);
        ESP_LOGI(TAG, "selftest: done, %u frame(s) decoded", (unsigned)tmp.frames);
        if (tmp.dec != NULL) {
            wels_dec_destroy(tmp.dec);
            tmp.dec = NULL;
        }
    } else {
        ESP_LOGE(TAG, "selftest: encode failed (%d)", e);
    }
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);

done:
    if (yuv) {
        free(yuv);
    }
    if (enc_buf) {
        free(enc_buf);
    }
}
