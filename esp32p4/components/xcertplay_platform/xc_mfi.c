/*
 * MFi authentication coprocessor driver (MFI343-class part).
 *
 * The part needs a settle time of about 5 ms between the register-select write
 * and the following read; a normal hardware-I2C driver issues the read tens of
 * microseconds later and every read is NACKed. During a signature operation the
 * chip additionally NACKs all reads for ~1.6 s, so the status poll must simply
 * keep retrying until the chip answers 0x10.
 *
 * Because of the relaxed timing this driver bit-bangs a dedicated bus
 * (SDA=GPIO3 / SCL=GPIO2) instead of sharing the touch controller's bus.
 */
#include "xc_mfi.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "xc_mfi";

#define MFI_SDA_GPIO 3
#define MFI_SCL_GPIO 2
#define MFI_ADDR_DEFAULT 0x10

#define BB_HALF_US 10       /* bit-bang half period, ~50 kHz */
#define SEL_SETTLE_US 6000  /* required between register select and read */
#define IO_RETRY_MS 3000
#define RETRY_DELAY_MS 2
#define AUTH_TIMEOUT_MS 4000
#define CERT_CHUNK 1024

#define REG_PROTOCOL_MAJOR 0x02
#define REG_ERROR 0x05
#define REG_AUTH_CTRL 0x10
#define REG_RESP_LEN 0x11
#define REG_RESP_DATA 0x12
#define REG_CHALLENGE_LEN 0x20
#define REG_CHALLENGE_DATA 0x21
#define REG_CERT_LEN 0x30
#define REG_CERT_DATA 0x31

static uint8_t s_addr = MFI_ADDR_DEFAULT;
static bool s_gpio_ready;
EXT_RAM_BSS_ATTR static uint8_t s_cert_cache[XC_MFI_MAX_CERTIFICATE];
static size_t s_cert_cache_len;

/* ---- bit-banged I2C --------------------------------------------------- */

static inline void dly(void)
{
    esp_rom_delay_us(BB_HALF_US);
}

static void bb_init(void)
{
    if (s_gpio_ready) {
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << MFI_SDA_GPIO) | (1ULL << MFI_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(MFI_SDA_GPIO, 1);
    gpio_set_level(MFI_SCL_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    s_gpio_ready = true;
}

static inline void sda(int v)
{
    gpio_set_level(MFI_SDA_GPIO, v);
}

static inline void scl(int v)
{
    gpio_set_level(MFI_SCL_GPIO, v);
}

static inline int sda_get(void)
{
    return gpio_get_level(MFI_SDA_GPIO);
}

static void bb_start(void)
{
    sda(1);
    scl(1);
    dly();
    sda(0);
    dly();
    scl(0);
    dly();
}

static void bb_stop(void)
{
    sda(0);
    dly();
    scl(1);
    dly();
    sda(1);
    dly();
}

static bool bb_wr(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        sda((b >> i) & 1);
        dly();
        scl(1);
        dly();
        scl(0);
        dly();
    }
    sda(1);
    dly();
    scl(1);
    dly();
    bool ack = (sda_get() == 0);
    scl(0);
    dly();
    return ack;
}

static uint8_t bb_rd(bool ack)
{
    uint8_t v = 0;
    sda(1);
    for (int i = 7; i >= 0; i--) {
        scl(1);
        dly();
        v = (uint8_t)((v << 1) | (sda_get() & 1));
        scl(0);
        dly();
    }
    if (ack) {
        sda(0);
    } else {
        sda(1);
    }
    dly();
    scl(1);
    dly();
    scl(0);
    dly();
    sda(1);
    return v;
}

static bool bb_write(uint8_t reg, const uint8_t *data, size_t len)
{
    bb_start();
    if (!bb_wr((uint8_t)(s_addr << 1)) || !bb_wr(reg)) {
        bb_stop();
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!bb_wr(data[i])) {
            bb_stop();
            return false;
        }
    }
    bb_stop();
    return true;
}

/* Register select, settle, then read. Returns false when the chip NACKs. */
static bool bb_read_once(uint8_t reg, uint8_t *out, size_t len)
{
    bb_start();
    if (!bb_wr((uint8_t)(s_addr << 1)) || !bb_wr(reg)) {
        bb_stop();
        return false;
    }
    bb_stop();
    esp_rom_delay_us(SEL_SETTLE_US);
    bb_start();
    if (!bb_wr((uint8_t)((s_addr << 1) | 1))) {
        bb_stop();
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = bb_rd(i + 1 < len);
    }
    bb_stop();
    return true;
}

static bool bb_read(uint8_t reg, uint8_t *out, size_t len)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)IO_RETRY_MS * 1000;
    while (1) {
        if (bb_read_once(reg, out, len)) {
            return true;
        }
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
}

static bool bb_write_retry(uint8_t reg, const uint8_t *data, size_t len)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)IO_RETRY_MS * 1000;
    while (1) {
        if (bb_write(reg, data, len)) {
            return true;
        }
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
}

static bool find_der(const uint8_t *buf, size_t n, size_t *off, size_t *total)
{
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] == 0x30 && buf[i + 1] == 0x82) {
            size_t content = ((size_t)buf[i + 2] << 8) | buf[i + 3];
            if (content + 4 + i <= n) {
                *off = i;
                *total = content + 4;
                return true;
            }
        }
    }
    return false;
}

/* ---- public API ------------------------------------------------------- */

esp_err_t xc_mfi_scan(xc_mfi_scan_result_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->detected = false;
    out->address = 0;
    out->device_count = 0;
    bb_init();

    uint8_t v = 0;
    if (bb_read_once(0x00, &v, 1)) {
        out->detected = true;
        out->address = MFI_ADDR_DEFAULT;
        out->device_count = 1;
        ESP_LOGI(TAG, "MFi coprocessor found at 0x%02X (bus SDA=%d SCL=%d), version 0x%02X",
                 MFI_ADDR_DEFAULT, MFI_SDA_GPIO, MFI_SCL_GPIO, v);
    } else {
        ESP_LOGW(TAG, "no MFi coprocessor on SDA=%d SCL=%d", MFI_SDA_GPIO, MFI_SCL_GPIO);
    }
    return ESP_OK;
}

esp_err_t xc_mfi_open(uint8_t address7bit)
{
    s_addr = address7bit ? address7bit : MFI_ADDR_DEFAULT;
    bb_init();
    return ESP_OK;
}

void xc_mfi_close(void)
{
    /* nothing to release */
}

esp_err_t xc_mfi_protocol_major(uint8_t *out_major)
{
    if (!bb_read(REG_PROTOCOL_MAJOR, out_major, 1)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t xc_mfi_certificate_length(uint16_t *out_length)
{
    if (s_cert_cache_len > 0) {
        *out_length = (uint16_t)s_cert_cache_len;
        return ESP_OK;
    }
    uint8_t b[2] = { 0 };
    if (!bb_read(REG_CERT_LEN, b, 2)) {
        return ESP_FAIL;
    }
    *out_length = ((uint16_t)b[0] << 8) | b[1];
    return ESP_OK;
}

esp_err_t xc_mfi_read_certificate(uint8_t *buf, size_t cap, size_t *out_len)
{
    uint8_t b[2] = { 0 };
    if (!bb_read(REG_CERT_LEN, b, 2)) {
        return ESP_FAIL;
    }
    size_t len = ((size_t)b[0] << 8) | b[1];
    if (len == 0 || len > cap || len > CERT_CHUNK) {
        ESP_LOGE(TAG, "certificate length %u invalid", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!bb_read(REG_CERT_DATA, buf, len)) {
        return ESP_FAIL;
    }
    *out_len = len;
    return ESP_OK;
}

esp_err_t xc_mfi_sign_challenge(const uint8_t *challenge, size_t len,
                                uint8_t *sig, size_t cap, size_t *out_sig_len)
{
    if (challenge == NULL || len < 1 || len > XC_MFI_MAX_CHALLENGE) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t len_be[2] = { (uint8_t)(len >> 8), (uint8_t)len };
    if (!bb_write_retry(REG_CHALLENGE_LEN, len_be, 2) ||
        !bb_write_retry(REG_CHALLENGE_DATA, challenge, len)) {
        return ESP_FAIL;
    }
    uint8_t start = 0x01;
    if (!bb_write_retry(REG_AUTH_CTRL, &start, 1)) {
        return ESP_FAIL;
    }

    /* The chip NACKs reads while it works (~1.6 s); keep polling until 0x10. */
    int64_t deadline = esp_timer_get_time() + (int64_t)AUTH_TIMEOUT_MS * 1000;
    bool done = false;
    uint8_t status = 0;
    while (esp_timer_get_time() < deadline) {
        if (bb_read_once(REG_AUTH_CTRL, &status, 1) && status == 0x10) {
            done = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
    if (!done) {
        uint8_t ec = 0;
        bb_read_once(REG_ERROR, &ec, 1);
        ESP_LOGW(TAG, "MFi sign timeout (last status 0x%02X, error 0x%02X)", status, ec);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t rb[2] = { 0 };
    if (!bb_read(REG_RESP_LEN, rb, 2)) {
        return ESP_FAIL;
    }
    size_t rlen = ((size_t)rb[0] << 8) | rb[1];
    if (rlen == 0 || rlen > cap) {
        ESP_LOGE(TAG, "signature length %u invalid", (unsigned)rlen);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!bb_read(REG_RESP_DATA, sig, rlen)) {
        return ESP_FAIL;
    }
    *out_sig_len = rlen;
    return ESP_OK;
}

bool xc_mfi_cached_certificate(const uint8_t **cert, size_t *len)
{
    if (s_cert_cache_len == 0) {
        return false;
    }
    *cert = s_cert_cache;
    *len = s_cert_cache_len;
    return true;
}

esp_err_t xc_mfi_selftest(void)
{
    uint8_t major = 0;
    if (xc_mfi_protocol_major(&major) != ESP_OK) {
        ESP_LOGE(TAG, "protocol major unreadable");
        return ESP_FAIL;
    }

    size_t len = 0;
    if (xc_mfi_read_certificate(s_cert_cache, sizeof(s_cert_cache), &len) != ESP_OK) {
        ESP_LOGE(TAG, "certificate read failed");
        return ESP_FAIL;
    }
    s_cert_cache_len = len;
    ESP_LOGI(TAG, "protocol major %u, certificate %u bytes cached (head %02x %02x %02x %02x)",
             major, (unsigned)len, s_cert_cache[0], s_cert_cache[1], s_cert_cache[2],
             s_cert_cache[3]);
    return ESP_OK;
}

void xc_mfi_diag(void)
{
    /* kept for compatibility with earlier builds */
}
