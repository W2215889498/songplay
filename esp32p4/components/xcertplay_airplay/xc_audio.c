#include "xc_audio.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_audio_dec.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "xc_crypto.h"

static const char *TAG = "xc_audio";

#define MAX_STREAMS 3
#define STREAM_TYPE_AUDIO_MIN 100
#define STREAM_TYPE_AUDIO_MAX 102
#define RTP_HDR_LEN 12
#define TAG_LEN 16
#define NONCE_LEN 8
#define WIRE_MAX 2048
#define PCM_MAX (60 * 48000 / 1000 * 4) /* one 60 ms 48 kHz stereo frame */

typedef struct {
    bool used;
    uint16_t type;
    uint64_t conn_id;
    uint8_t key[32];
    int codec; /* 0 = 16-bit BE PCM, 1 = AAC-LC, 2 = Opus */
    int rate;
    int channels;
    void *opus; /* esp_opus_dec handle (codec 2) */
    int data_fd;
    int ctrl_fd;
    volatile bool stop;
    volatile uint32_t packets;
    volatile uint32_t failures;
    volatile uint32_t dec_errors;
    volatile uint32_t pcm_samples;
    TaskHandle_t task;
} audio_chan_t;

static audio_chan_t s_ch[MAX_STREAMS];
static esp_codec_dev_handle_t s_spk;
static int s_spk_rate;
static int s_spk_ch = -1;
static int s_volume = 80;

/* Mirrors xcertplay `AudioStreamCodec.fromFormatBits`. */
static void parse_audio_format(uint64_t bits, audio_chan_t *ch)
{
    if ((bits & 0x40000000ULL) || (bits & 0x20000000ULL) || (bits & 0x10000000ULL)) {
        ch->codec = 2;
        ch->rate = 48000;
        ch->channels = 1;
        return;
    }
    if (bits & 0x800000ULL) {
        ch->codec = 1;
        ch->rate = 48000;
        ch->channels = 2;
        return;
    }
    if (bits & 0x400000ULL) {
        ch->codec = 1;
        ch->rate = 44100;
        ch->channels = 2;
        return;
    }
    static const struct {
        uint64_t bit;
        int rate;
        int channels;
    } pcm[] = {
        { 0x4ULL, 8000, 1 },    { 0x8ULL, 8000, 2 },    { 0x10ULL, 16000, 1 },
        { 0x20ULL, 16000, 2 },  { 0x40ULL, 24000, 1 },  { 0x80ULL, 24000, 2 },
        { 0x100ULL, 32000, 1 }, { 0x200ULL, 32000, 2 }, { 0x400ULL, 44100, 1 },
        { 0x800ULL, 44100, 2 }, { 0x4000ULL, 48000, 1 }, { 0x8000ULL, 48000, 2 },
    };
    for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++) {
        if (bits & pcm[i].bit) {
            ch->codec = 0;
            ch->rate = pcm[i].rate;
            ch->channels = pcm[i].channels;
            return;
        }
    }
    ch->codec = 0;
    ch->rate = 44100;
    ch->channels = 2;
}

static const char *codec_name(int codec)
{
    return codec == 0 ? "pcm" : codec == 1 ? "aac" : "opus";
}

/* The Waveshare example opens the ES8311 as 16-bit stereo (channel mask 0x03),
 * so every stream is converted to interleaved stereo PCM here. */
static esp_err_t codec_ensure(int rate)
{
    if (s_spk == NULL) {
        s_spk = bsp_audio_codec_speaker_init();
        if (s_spk == NULL) {
            return ESP_FAIL;
        }
    }
    if (s_spk_rate == rate && s_spk_ch == 2) {
        return ESP_OK;
    }
    if (s_spk_ch >= 0) {
        esp_codec_dev_close(s_spk);
        s_spk_ch = -1;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = (uint32_t)rate,
    };
    if (esp_codec_dev_open(s_spk, &fs) != 0) {
        ESP_LOGW(TAG, "speaker codec open failed (%d Hz, stereo)", rate);
        return ESP_FAIL;
    }
    s_spk_rate = rate;
    s_spk_ch = 2;
    /* Set the volume after open, like the Waveshare example: the codec init
     * overwrites the DAC volume registers. */
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    ESP_LOGI(TAG, "speaker codec open: %d Hz, stereo, vol %d%%", rate, s_volume);
    return ESP_OK;
}

static void audio_task(void *arg)
{
    audio_chan_t *ch = arg;
    uint8_t *wire = heap_caps_malloc(WIRE_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *pt = heap_caps_malloc(WIRE_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *pcm = heap_caps_malloc(PCM_MAX, MALLOC_CAP_SPIRAM);
    if (wire == NULL || pt == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "type %u buffer alloc failed", ch->type);
        free(wire);
        free(pt);
        free(pcm);
        ch->used = false;
        vTaskDelete(NULL);
        return;
    }

    struct pollfd pfd[2];
    while (!ch->stop) {
        pfd[0] = (struct pollfd){ .fd = ch->data_fd, .events = POLLIN };
        pfd[1] = (struct pollfd){ .fd = ch->ctrl_fd, .events = POLLIN };
        int pr = poll(pfd, 2, 200);
        if (pr <= 0) {
            continue;
        }
        if (pfd[1].revents & POLLIN) {
            uint8_t rtcp[512];
            (void)recv(ch->ctrl_fd, rtcp, sizeof(rtcp), 0);
        }
        if (!(pfd[0].revents & POLLIN)) {
            continue;
        }
        int n = recv(ch->data_fd, wire, WIRE_MAX, 0);
        if (n < RTP_HDR_LEN + TAG_LEN + NONCE_LEN) {
            continue;
        }
        uint8_t nonce[12] = { 0 };
        memcpy(nonce + 4, wire + n - NONCE_LEN, NONCE_LEN);
        int ct_len = n - RTP_HDR_LEN - TAG_LEN - NONCE_LEN;
        const uint8_t *tag = wire + n - NONCE_LEN - TAG_LEN;
        if (xc_chacha20poly1305_open(ch->key, nonce, wire + 4, 8, wire + RTP_HDR_LEN,
                                     (size_t)ct_len, tag, pt) != ESP_OK) {
            ch->failures++;
            if (ch->failures == 1) {
                ESP_LOGW(TAG, "type %u first decrypt failure (wire %d bytes)", ch->type, n);
            }
            continue;
        }
        ch->packets++;
        if (ch->packets <= 3) {
            ESP_LOGI(TAG, "type %u packet %u: wire %d payload %d (head %02x %02x %02x %02x)",
                     ch->type, ch->packets, n, ct_len, pt[0], pt[1], pt[2], pt[3]);
        }
        if (ch->codec == 0) {
            /* 16-bit big-endian PCM -> little-endian interleaved stereo. */
            const int16_t *src = (const int16_t *)pt;
            int16_t *dst = (int16_t *)pcm;
            int frames = ct_len / 2 / ch->channels;
            if (ch->channels == 2) {
                for (int i = 0; i < frames * 2; i++) {
                    dst[i] = (int16_t)((src[i] >> 8) | (src[i] << 8));
                }
            } else {
                for (int i = 0; i < frames; i++) {
                    int16_t s = (int16_t)((src[i] >> 8) | (src[i] << 8));
                    dst[2 * i] = s;
                    dst[2 * i + 1] = s;
                }
            }
            if (codec_ensure(ch->rate) == ESP_OK) {
                esp_codec_dev_write(s_spk, pcm, frames * 4);
                ch->pcm_samples += (uint32_t)frames;
            }
        } else if (ch->codec == 2) {
            /* Opus mono 48 kHz: the wireless low-latency streams (100/101). */
            if (ch->opus == NULL) {
                esp_opus_dec_cfg_t cfg = {
                    .sample_rate = ESP_AUDIO_SAMPLE_RATE_48K,
                    .channel = ESP_AUDIO_MONO,
                    .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID,
                    .self_delimited = false,
                };
                if (esp_opus_dec_open(&cfg, sizeof(cfg), &ch->opus) != ESP_AUDIO_ERR_OK) {
                    ch->opus = NULL;
                    ESP_LOGW(TAG, "type %u opus decoder open failed", ch->type);
                }
            }
            if (ch->opus != NULL) {
                esp_audio_dec_in_raw_t raw = { .buffer = pt, .len = (uint32_t)ct_len };
                esp_audio_dec_out_frame_t out = { .buffer = pcm, .len = PCM_MAX / 2 };
                esp_audio_dec_info_t info = { 0 };
                esp_audio_err_t er = esp_opus_dec_decode(ch->opus, &raw, &out, &info);
                if (er == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                    /* Decoded mono PCM -> interleaved stereo in place (backwards). */
                    int frames = (int)out.decoded_size / 2;
                    int16_t *p = (int16_t *)pcm;
                    for (int i = frames - 1; i >= 0; i--) {
                        int16_t s = p[i];
                        p[2 * i] = s;
                        p[2 * i + 1] = s;
                    }
                    if (codec_ensure(48000) == ESP_OK) {
                        esp_codec_dev_write(s_spk, pcm, frames * 4);
                        ch->pcm_samples += (uint32_t)frames;
                    }
                } else if (er != ESP_AUDIO_ERR_OK) {
                    ch->dec_errors++;
                    if (ch->dec_errors == 1) {
                        ESP_LOGW(TAG, "type %u opus decode error %d", ch->type, (int)er);
                    }
                }
            }
        } else if (ch->codec == 1 && ch->packets == 1) {
            ESP_LOGW(TAG, "type %u AAC payload not decoded yet", ch->type);
        }
        if ((ch->packets % 200) == 0) {
            ESP_LOGI(TAG, "type %u packets=%u failures=%u dec_errors=%u pcm_samples=%u", ch->type,
                     ch->packets, ch->failures, ch->dec_errors, ch->pcm_samples);
        }
    }

    ESP_LOGW(TAG, "type %u task exit: %u packets, %u failures, %u samples", ch->type, ch->packets,
             ch->failures, ch->pcm_samples);
    close(ch->data_fd);
    close(ch->ctrl_fd);
    ch->data_fd = -1;
    ch->ctrl_fd = -1;
    if (ch->opus) {
        esp_opus_dec_close(ch->opus);
        ch->opus = NULL;
    }
    free(wire);
    free(pt);
    free(pcm);
    ch->used = false;
    vTaskDelete(NULL);
}

esp_err_t xc_audio_open(uint16_t type, uint64_t conn_id, const uint8_t key[32],
                        uint64_t format_bits, const char *audio_type, uint16_t *out_data_port,
                        uint16_t *out_control_port)
{
    if (type < STREAM_TYPE_AUDIO_MIN || type > STREAM_TYPE_AUDIO_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_chan_t *ch = &s_ch[type - STREAM_TYPE_AUDIO_MIN];

    /* The phone re-runs SETUP frequently (one stream per navigation prompt), so
     * retire the previous task and reuse its slot. */
    if (ch->used) {
        ch->stop = true;
        for (int i = 0; i < 100 && ch->used; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (ch->used) {
            ESP_LOGW(TAG, "type %u previous task did not exit in time", type);
            return ESP_ERR_INVALID_STATE;
        }
    }

    int dfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int cfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dfd < 0 || cfd < 0) {
        if (dfd >= 0) {
            close(dfd);
        }
        if (cfd >= 0) {
            close(cfd);
        }
        return ESP_FAIL;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = 0,
    };
    if (bind(dfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        bind(cfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(dfd);
        close(cfd);
        return ESP_FAIL;
    }
    socklen_t al = sizeof(addr);
    uint16_t dport = 0;
    uint16_t cport = 0;
    if (getsockname(dfd, (struct sockaddr *)&addr, &al) == 0) {
        dport = ntohs(addr.sin_port);
    }
    al = sizeof(addr);
    if (getsockname(cfd, (struct sockaddr *)&addr, &al) == 0) {
        cport = ntohs(addr.sin_port);
    }

    memset(ch, 0, sizeof(*ch));
    ch->type = type;
    ch->conn_id = conn_id;
    memcpy(ch->key, key, 32);
    parse_audio_format(format_bits, ch);
    ch->data_fd = dfd;
    ch->ctrl_fd = cfd;
    ch->stop = false;
    ch->used = true;
    if (xTaskCreatePinnedToCore(audio_task, "xc_audio", 8192, ch, 5, &ch->task, 0) != pdPASS) {
        close(dfd);
        close(cfd);
        ch->used = false;
        return ESP_ERR_NO_MEM;
    }

    *out_data_port = dport;
    *out_control_port = cport;
    ESP_LOGI(TAG, "type %u open: %s %d Hz %d ch, audioType=%s fmt=0x%llx, dataPort=%u controlPort=%u",
             type, codec_name(ch->codec), ch->rate, ch->channels,
             audio_type ? audio_type : "default", (unsigned long long)format_bits, dport, cport);
    return ESP_OK;
}

void xc_audio_close(uint16_t type)
{
    if (type < STREAM_TYPE_AUDIO_MIN || type > STREAM_TYPE_AUDIO_MAX) {
        return;
    }
    audio_chan_t *ch = &s_ch[type - STREAM_TYPE_AUDIO_MIN];
    if (ch->used) {
        ESP_LOGI(TAG, "type %u closing (%u packets, %u failures)", type, ch->packets,
                 ch->failures);
        ch->stop = true;
    }
}

void xc_audio_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (s_spk) {
        esp_codec_dev_set_out_vol(s_spk, percent);
    }
}

/* Boot self test: a short 440 Hz square beep through the same speaker path the
 * CarPlay audio uses, so hardware problems are distinguishable from protocol
 * problems at first listen. */
void xc_audio_tone_test(void)
{
    if (codec_ensure(48000) != ESP_OK) {
        ESP_LOGW(TAG, "tone test skipped: speaker codec unavailable");
        return;
    }
    enum { N = 1024 }; /* interleaved stereo samples */
    static int16_t buf[N];
    int rc = 0;
    for (int k = 0; k < 20; k++) { /* ~210 ms */
        for (int i = 0; i < N / 2; i++) {
            int idx = k * (N / 2) + i;
            int16_t s = (int16_t)(((idx * 440 / 48) % 64 < 32) ? 8000 : -8000);
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
        }
        rc = esp_codec_dev_write(s_spk, buf, sizeof(buf));
        if (rc != 0) {
            break;
        }
    }
    ESP_LOGI(TAG, "tone test done (%s)", rc == 0 ? "speaker path ok" : "write failed");
}
