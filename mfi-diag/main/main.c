/*
 * Standalone MFi coprocessor diagnostic v7 for the ESP32-P4 board.
 *
 * All ESP-IDF I2C driver attempts failed to start an authentication, while the
 * same sequence works from a Raspberry Pi. This build drops the hardware I2C
 * peripheral and bit-bangs SDA/SCL instead, so the transaction timing matches
 * a normal Linux i2c-dev host, and provides both separate ("sep", like LIVI)
 * and repeated-START ("comb") reads.
 *
 * Wiring: MFi module SDA=GPIO3 (pin 12), SCL=GPIO2 (pin 8), VDD=3V3, GND=GND.
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "diag7";

#define PIN_SDA 3
#define PIN_SCL 2
#define MFI_ADDR 0x10

#define REG_DEVICE_VERSION 0x00
#define REG_PROTOCOL_MAJOR 0x02
#define REG_ERROR 0x05
#define REG_AUTH_CTRL 0x10
#define REG_RESP_LEN 0x11
#define REG_RESP_DATA 0x12
#define REG_CHALLENGE_LEN 0x20
#define REG_CHALLENGE_DATA 0x21
#define REG_CERT_LEN 0x30
#define REG_CERT_DATA 0x31

#define HALF_US_DEFAULT 10 /* 50 kHz */

static int s_half_us = HALF_US_DEFAULT;
static int s_gap_us = 50; /* delay between register select and read (sep only) */

static inline void dly(void)
{
    esp_rom_delay_us(s_half_us);
}

static inline void sda_high(void)
{
    gpio_set_level(PIN_SDA, 1);
}

static inline void sda_low(void)
{
    gpio_set_level(PIN_SDA, 0);
}

static inline void scl_high(void)
{
    gpio_set_level(PIN_SCL, 1);
}

static inline void scl_low(void)
{
    gpio_set_level(PIN_SCL, 0);
}

static inline int sda_read(void)
{
    return gpio_get_level(PIN_SDA);
}

static void bb_start(void)
{
    sda_high();
    scl_high();
    dly();
    sda_low();
    dly();
    scl_low();
    dly();
}

static void bb_stop(void)
{
    sda_low();
    dly();
    scl_high();
    dly();
    sda_high();
    dly();
}

/* returns true when the slave ACKed */
static bool bb_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if (b & (1 << i)) {
            sda_high();
        } else {
            sda_low();
        }
        dly();
        scl_high();
        dly();
        scl_low();
        dly();
    }
    sda_high();
    dly();
    scl_high();
    dly();
    bool ack = (sda_read() == 0);
    scl_low();
    dly();
    return ack;
}

static uint8_t bb_read_byte(bool ack)
{
    uint8_t v = 0;
    sda_high();
    for (int i = 7; i >= 0; i--) {
        scl_high();
        dly();
        v = (uint8_t)((v << 1) | (sda_read() & 1));
        scl_low();
        dly();
    }
    if (ack) {
        sda_low();
    } else {
        sda_high();
    }
    dly();
    scl_high();
    dly();
    scl_low();
    dly();
    sda_high();
    return v;
}

static bool bb_write(uint8_t reg, const uint8_t *data, size_t len)
{
    bb_start();
    if (!bb_write_byte((uint8_t)(MFI_ADDR << 1))) {
        bb_stop();
        return false;
    }
    if (!bb_write_byte(reg)) {
        bb_stop();
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!bb_write_byte(data[i])) {
            bb_stop();
            return false;
        }
    }
    bb_stop();
    return true;
}

static bool bb_read(uint8_t reg, uint8_t *out, size_t len, bool combined)
{
    bb_start();
    if (!bb_write_byte((uint8_t)(MFI_ADDR << 1)) || !bb_write_byte(reg)) {
        bb_stop();
        return false;
    }
    if (!combined) {
        bb_stop();
        esp_rom_delay_us(s_gap_us);
    }
    bb_start();
    if (!bb_write_byte((uint8_t)((MFI_ADDR << 1) | 1))) {
        bb_stop();
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = bb_read_byte(i + 1 < len);
    }
    bb_stop();
    return true;
}

static void hexdump_head(const char *label, const uint8_t *p, size_t n)
{
    char line[3 * 16 + 1];
    size_t shown = n > 16 ? 16 : n;
    for (size_t i = 0; i < shown; i++) {
        sprintf(line + i * 3, "%02x ", p[i]);
    }
    line[shown * 3] = '\0';
    ESP_LOGI(TAG, "%s (%u bytes): %s", label, (unsigned)n, line);
}

static void attempt(const char *name, const uint8_t *ch, size_t n, bool combined_reads)
{
    ESP_LOGI(TAG, "--- %s: %u-byte challenge, reads=%s", name, (unsigned)n,
             combined_reads ? "comb" : "sep");

    uint8_t l[2] = { (uint8_t)(n >> 8), (uint8_t)n };
    if (!bb_write(REG_CHALLENGE_LEN, l, 2)) {
        ESP_LOGW(TAG, "  write challenge length NACKed");
    }
    if (!bb_write(REG_CHALLENGE_DATA, ch, n)) {
        ESP_LOGW(TAG, "  write challenge NACKed");
    }
    uint8_t one = 0x01;
    if (!bb_write(REG_AUTH_CTRL, &one, 1)) {
        ESP_LOGW(TAG, "  write start NACKed");
    }

    int64_t deadline = esp_timer_get_time() + 3000 * 1000;
    int polls = 0;
    uint8_t st = 0xEE;
    bool done = false;
    while (esp_timer_get_time() < deadline) {
        if (bb_read(REG_AUTH_CTRL, &st, 1, combined_reads)) {
            if (polls < 6) {
                ESP_LOGI(TAG, "  poll %d: status=0x%02X", polls, st);
            }
            polls++;
            if (st == 0x10) {
                done = true;
                break;
            }
        } else if (polls == 0) {
            ESP_LOGW(TAG, "  status read NACKed");
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    uint8_t rb[2] = { 0 };
    if (bb_read(REG_RESP_LEN, rb, 2, combined_reads)) {
        uint16_t rl = ((uint16_t)rb[0] << 8) | rb[1];
        ESP_LOGI(TAG, "  response length = %u", rl);
        if (rl > 0 && rl <= 128) {
            uint8_t sig[128];
            if (bb_read(REG_RESP_DATA, sig, rl, combined_reads)) {
                hexdump_head("  response", sig, rl);
            }
        }
    } else {
        ESP_LOGW(TAG, "  response length NACKed");
    }
    uint8_t ec = 0;
    bb_read(REG_ERROR, &ec, 1, combined_reads);
    ESP_LOGI(TAG, "  result: %s polls=%d status=0x%02X err=0x%02X", done ? "DONE" : "no status bit",
             polls, st, ec);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_SDA) | (1ULL << PIN_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    sda_high();
    scl_high();
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "===== MFi DIAG v8: bit-bang bring-up check =====");

    /* Does esp_rom_delay_us actually delay? */
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 100; i++) {
        esp_rom_delay_us(100);
    }
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "delay calibration: 100 x esp_rom_delay_us(100) took %d us (expect ~10000)",
             (int)dt);

    /* Idle levels: both lines must read high through the pull-ups. */
    ESP_LOGI(TAG, "idle levels: SDA=%d SCL=%d (expect 1/1)", sda_read(), gpio_get_level(PIN_SCL));

    /* Find a half-period at which the chip ACKs its address. */
    const int halves[] = { 50, 20, 10, 5, 2 };
    bool found = false;
    uint8_t ver = 0;
    for (size_t i = 0; i < sizeof(halves) / sizeof(halves[0]); i++) {
        s_half_us = halves[i];
        uint8_t v = 0;
        bool ok = bb_read(REG_DEVICE_VERSION, &v, 1, false);
        ESP_LOGI(TAG, "  half=%2d us (~%3d kHz): device version -> %s 0x%02X", halves[i],
                 1000 / (2 * halves[i]), ok ? "OK" : "NO ACK", v);
        if (ok) {
            found = true;
            ver = v;
            break;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "bit-bang cannot reach 0x%02X at any speed", MFI_ADDR);
        return;
    }
    ESP_LOGI(TAG, "using half=%d us (~%d kHz), device version 0x%02X", s_half_us,
             1000 / (2 * s_half_us), ver);

    uint8_t major = 0;
    bb_read(REG_PROTOCOL_MAJOR, &major, 1, false);
    uint8_t r[4] = { 0 };
    for (uint8_t i = 0; i < 4; i++) {
        bb_read((uint8_t)(REG_DEVICE_VERSION + i), &r[i], 1, false);
    }
    ESP_LOGI(TAG, "protocol major %u, regs 0x00..0x03 = %02x %02x %02x %02x", major, r[0], r[1],
             r[2], r[3]);

    /* How long must the chip settle between the register select and the read? */
    const int gaps[] = { 0, 50, 200, 1000, 5000, 20000 };
    uint8_t probe[4];
    int best_gap = 1000;
    bool gap_found = false;
    for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
        s_gap_us = gaps[i];
        memset(probe, 0, sizeof(probe));
        bool ok = bb_read(REG_CERT_LEN, probe, 2, false);
        ESP_LOGI(TAG, "  sep read 0x30, gap=%6d us -> %s  %02x %02x", gaps[i], ok ? "OK" : "NACK",
                 probe[0], probe[1]);
        if (ok && !gap_found) {
            best_gap = gaps[i];
            gap_found = true;
        }
    }
    s_gap_us = best_gap;
    ESP_LOGI(TAG, "using settle gap %d us", s_gap_us);

    /* certificate: try the LIVI way first, then the repeated-START way */
    uint8_t cert[1024];
    size_t cert_len = 0;
    uint8_t lb[2] = { 0 };
    if (bb_read(REG_CERT_LEN, lb, 2, false)) {
        uint16_t len = ((uint16_t)lb[0] << 8) | lb[1];
        ESP_LOGI(TAG, "cert length via sep = %u", len);
        if (len > 0 && len <= sizeof(cert) && bb_read(REG_CERT_DATA, cert, len, false)) {
            cert_len = len;
            ESP_LOGI(TAG, "certificate read via sep");
        }
    }
    if (cert_len == 0) {
        if (bb_read(REG_CERT_LEN, lb, 2, true)) {
            uint16_t len = ((uint16_t)lb[0] << 8) | lb[1];
            ESP_LOGI(TAG, "cert length via comb = %u", len);
            if (len > 0 && len <= sizeof(cert) && bb_read(REG_CERT_DATA, cert, len, true)) {
                cert_len = len;
                ESP_LOGI(TAG, "certificate read via comb");
            }
        }
    }
    if (cert_len == 0) {
        ESP_LOGE(TAG, "certificate unreadable");
        return;
    }
    hexdump_head("cert head", cert, cert_len);

    uint8_t c32[32];
    memset(c32, 0xAB, sizeof(c32));

    /* sep-style polling (LIVI) */
    attempt("auth #1, sep reads", c32, sizeof(c32), false);
    attempt("auth #2, sep reads", c32, sizeof(c32), false);
    /* same, but polling with repeated-START reads */
    attempt("auth #3, comb reads", c32, sizeof(c32), true);

    ESP_LOGI(TAG, "===== done =====");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
