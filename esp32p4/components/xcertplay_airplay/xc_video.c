#include "xc_video.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/sockets.h"
#include "xc_crypto.h"

static const char *TAG = "xc_video";

#define HEADER_LEN 128
#define OP_VIDEO_FRAME 0
#define OP_VIDEO_CONFIG 1
#define BODY_MAX (2 * 1024 * 1024)
#define TAG_LEN 16
#define PANEL_W 480
#define PANEL_H 800

static uint8_t *s_body;   /* PSRAM */
static uint8_t *s_plain;  /* PSRAM */
static uint8_t *s_annexb; /* PSRAM */
static uint16_t *s_rgb;   /* PSRAM */
static uint8_t s_key[32];
static volatile bool s_running;
static volatile bool s_has_config;
static volatile uint32_t s_frames;
static uint64_t s_counter;
static int s_listen_fd = -1;
static int s_conn_fd = -1;
static esp_h264_dec_handle_t s_dec;
static int s_nal_length_size = 4;
static int s_video_w, s_video_h;
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

/* Length-prefixed NALUs -> Annex B. */
static size_t to_annexb(const uint8_t *in, size_t len, int length_size, uint8_t *out, size_t cap)
{
    size_t r = 0, w = 0;
    while (r + (size_t)length_size <= len) {
        size_t nalu = 0;
        for (int i = 0; i < length_size; i++) {
            nalu = (nalu << 8) | in[r + i];
        }
        r += length_size;
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

static void feed_decoder(const uint8_t *data, size_t len)
{
    if (s_dec == NULL) {
        return;
    }
    esp_h264_dec_in_frame_t in = {
        .raw_data = { .buffer = (uint8_t *)data, .len = (uint32_t)len },
    };
    esp_h264_dec_out_frame_t out = { 0 };
    int guard = 0;
    while (in.raw_data.len > 0 && guard++ < 64) {
        esp_h264_err_t err = esp_h264_dec_process(s_dec, &in, &out);
        if (err != ESP_H264_ERR_OK) {
            break;
        }
        if (in.consume == 0) {
            break;
        }
        if (out.out_size == 0 || out.outbuf == NULL) {
            in.raw_data.buffer += in.consume;
            in.raw_data.len -= in.consume;
            continue;
        }

        if (s_video_w == 0 || s_video_h == 0) {
            esp_h264_dec_param_sw_handle_t param = NULL;
            if (esp_h264_dec_sw_get_param_hd(s_dec, &param) == ESP_H264_ERR_OK && param) {
                esp_h264_resolution_t res = { 0 };
                if (esp_h264_dec_get_resolution(param, &res) == ESP_H264_ERR_OK) {
                    s_video_w = res.width;
                    s_video_h = res.height;
                    ESP_LOGI(TAG, "decoded resolution %dx%d", s_video_w, s_video_h);
                }
            }
        }

        if (s_video_w > 0 && s_video_h > 0 && s_video_w <= PANEL_W && s_video_h <= PANEL_H &&
            s_rgb != NULL) {
            const uint8_t *y_plane = out.outbuf;
            const uint8_t *u_plane = y_plane + s_video_w * s_video_h;
            const uint8_t *v_plane = u_plane + (s_video_w / 2) * (s_video_h / 2);
            int uw = s_video_w / 2;
            for (int y = 0; y < s_video_h; y++) {
                for (int x = 0; x < s_video_w; x++) {
                    int yv = y_plane[y * s_video_w + x] - 16;
                    int uv = u_plane[(y / 2) * uw + (x / 2)] - 128;
                    int vv = v_plane[(y / 2) * uw + (x / 2)] - 128;
                    if (yv < 0) {
                        yv = 0;
                    }
                    int r = (298 * yv + 409 * vv + 128) >> 8;
                    int g = (298 * yv - 100 * uv - 208 * vv + 128) >> 8;
                    int b = (298 * yv + 516 * uv + 128) >> 8;
                    if (r < 0) r = 0; else if (r > 255) r = 255;
                    if (g < 0) g = 0; else if (g > 255) g = 255;
                    if (b < 0) b = 0; else if (b > 255) b = 255;
                    s_rgb[y * s_video_w + x] =
                        (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
                }
            }

            esp_lcd_panel_handle_t panel = bsp_display_get_panel_handle();
            int x0 = (PANEL_W - s_video_w) / 2;
            int y0 = (PANEL_H - s_video_h) / 2;
            bsp_display_lock(-1);
            if (!s_ui_cleared) {
                lv_obj_clean(lv_screen_active()); /* stop LVGL from overdrawing */
                s_ui_cleared = true;
            }
            esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + s_video_w, y0 + s_video_h, s_rgb);
            bsp_display_unlock();
        }

        s_frames++;
        in.raw_data.buffer += in.consume;
        in.raw_data.len -= in.consume;
    }
}

static void handle_message(const uint8_t *header, size_t body_len)
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
                    s_nal_length_size = (avcc[4] & 3) + 1;
                }
                break;
            }
            if (memcmp(s_body + i, "hvcC", 4) == 0) {
                codec = "h265";
                break;
            }
        }
        s_has_config = true;
        ESP_LOGI(TAG, "video config: %s, %u bytes (avcC %u), NAL length size %d", codec,
                 (unsigned)body_len, (unsigned)avcc_len, s_nal_length_size);

        if (avcc != NULL) {
            /* Rebuild the decoder per stream: the parameter sets arrive with
             * every SETUP and a stale decoder will never activate them. */
            if (s_dec != NULL) {
                esp_h264_dec_close(s_dec);
                esp_h264_dec_del(s_dec);
                s_dec = NULL;
            }
            s_video_w = 0;
            s_video_h = 0;
            esp_h264_dec_cfg_sw_t cfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
            if (esp_h264_dec_sw_new(&cfg, &s_dec) == ESP_H264_ERR_OK &&
                esp_h264_dec_open(s_dec) == ESP_H264_ERR_OK) {
                size_t n = avcc_to_annexb(avcc, avcc_len, s_annexb, BODY_MAX);
                ESP_LOGI(TAG, "decoder ready; feeding %u bytes of SPS/PPS", (unsigned)n);
                feed_decoder(s_annexb, n);
            } else {
                ESP_LOGE(TAG, "H.264 decoder init failed");
                s_dec = NULL;
            }
        }
        return;
    }
    if (opcode != OP_VIDEO_FRAME) {
        ESP_LOGW(TAG, "unknown video opcode %u (%u bytes)", opcode, (unsigned)body_len);
        return;
    }

    uint8_t nonce[12];
    nonce64(s_counter, nonce);
    if (body_len < TAG_LEN) {
        s_counter++;
        return;
    }
    size_t ct_len = body_len - TAG_LEN;
    if (xc_chacha20poly1305_open(s_key, nonce, header, HEADER_LEN, s_body, ct_len, s_body + ct_len,
                                 s_plain) != ESP_OK) {
        ESP_LOGW(TAG, "frame %u: decrypt failed", (unsigned)s_counter);
        s_counter++;
        return;
    }
    s_counter++;

    size_t n = to_annexb(s_plain, ct_len, s_nal_length_size, s_annexb, BODY_MAX);
    if (s_frames < 3) {
        ESP_LOGI(TAG, "frame %u: %u sealed -> %u annex-b", (unsigned)s_frames, (unsigned)body_len,
                 (unsigned)n);
    }
    if (n > 0) {
        int64_t t0 = esp_timer_get_time();
        feed_decoder(s_annexb, n);
        if (s_frames < 3) {
            ESP_LOGI(TAG, "frame %u: decoded in %d ms", (unsigned)s_frames,
                     (int)((esp_timer_get_time() - t0) / 1000));
        }
    }
}

static void video_task(void *arg)
{
    while (s_running) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int fd = accept(s_listen_fd, (struct sockaddr *)&src, &sl);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_conn_fd = fd;
        s_counter = 0; /* the phone restarts the frame counter per connection */
        ESP_LOGI(TAG, "video stream connected");

        static uint8_t header[HEADER_LEN];
        while (s_running) {
            if (recv_all(fd, header, HEADER_LEN) != 0) {
                break;
            }
            size_t body_len = (size_t)header[0] | ((size_t)header[1] << 8) |
                              ((size_t)header[2] << 16) | ((size_t)header[3] << 24);
            if (body_len == 0 || body_len > BODY_MAX) {
                ESP_LOGW(TAG, "bad body length %u", (unsigned)body_len);
                break;
            }
            if (recv_all(fd, s_body, body_len) != 0) {
                break;
            }
            handle_message(header, body_len);
        }
        close(fd);
        s_conn_fd = -1;
        ESP_LOGW(TAG, "video stream disconnected (%u frames)", (unsigned)s_frames);
    }
    vTaskDelete(NULL);
}

esp_err_t xc_video_start(const uint8_t key[32], uint16_t *out_port)
{
    if (s_body == NULL) {
        s_body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_plain = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_annexb = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_rgb = heap_caps_malloc(PANEL_W * PANEL_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_body == NULL || s_plain == NULL || s_annexb == NULL || s_rgb == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_key, key, 32);
    s_frames = 0;
    s_has_config = false;

    if (s_listen_fd < 0) {
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
        s_listen_fd = fd;
    }

    struct sockaddr_in bound;
    socklen_t bl = sizeof(bound);
    if (getsockname(s_listen_fd, (struct sockaddr *)&bound, &bl) != 0) {
        return ESP_FAIL;
    }
    if (out_port) {
        *out_port = ntohs(bound.sin_port);
    }

    if (!s_running) {
        s_running = true;
        xTaskCreatePinnedToCore(video_task, "xc_video", 8192, NULL, 5, NULL, 0);
    }
    ESP_LOGI(TAG, "video listener on port %u", out_port ? *out_port : 0);
    return ESP_OK;
}

void xc_video_stop(void)
{
    s_running = false;
    if (s_conn_fd >= 0) {
        close(s_conn_fd);
        s_conn_fd = -1;
    }
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
}

uint32_t xc_video_frame_count(void)
{
    return s_frames;
}

bool xc_video_has_config(void)
{
    return s_has_config;
}

/* ---- encode / decode / display self test ------------------------------ */

void xc_video_lcd_selftest(void)
{
    const int w = 480, h = 272;
    if (s_rgb == NULL) {
        s_rgb = heap_caps_malloc(PANEL_W * PANEL_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        if (s_dec == NULL) {
            esp_h264_dec_cfg_sw_t dcfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
            esp_h264_dec_sw_new(&dcfg, &s_dec);
            esp_h264_dec_open(s_dec);
        }
        memcpy(s_annexb, enc_buf, out.length);
        s_video_w = 0;
        s_video_h = 0;
        feed_decoder(s_annexb, out.length);
        ESP_LOGI(TAG, "selftest: done, %u frame(s) decoded", (unsigned)s_frames);
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
