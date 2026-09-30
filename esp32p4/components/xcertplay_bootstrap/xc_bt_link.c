#include "xc_bt_link.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "xc_crypto.h"

static const char *TAG = "xc_link";

#define BT_UART UART_NUM_1
#define BT_TX_GPIO 4
#define BT_RX_GPIO 5
#define BT_BAUD 115200

#define FRAME_MAGIC 0xA5
#define FT_DATA 1
#define FT_CMD 2
#define FT_EVT 3

/* iAP2 link constants (see Iap2LinkEngine.kt) */
static const uint8_t IAP2_MARKER[6] = { 0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10 };
#define LINK_START_HIGH 0xFF
#define LINK_START_LOW 0x5A
#define HEADER_BYTES 9
#define CONTROL_SYNCHRONIZE 0x80
#define CONTROL_ACKNOWLEDGEMENT 0x40
#define CONTROL_EXTENDED_ACK 0x20
#define CONTROL_RESET 0x10
#define SESSION_CONTROL 10
#define INITIAL_SEQUENCE 99

static xc_csm_rx_cb_t s_rx_cb;
static volatile bool s_writable;
static volatile bool s_rfcomm_up;
static uint8_t s_seq = INITIAL_SEQUENCE;
static uint8_t s_ack;
static uint16_t s_peer_max_payload = 0xFFFF - 10;
static bool s_peer_synced;
/* Serializes CSM transmits: the link task (RX callbacks) and the location
 * timer task both send, and both s_seq and the shared frame buffer must not
 * interleave. */
static SemaphoreHandle_t s_tx_lock;

/* ---- UART framing ----------------------------------------------------- */

static void bridge_send(uint8_t type, const void *data, size_t len)
{
    uint8_t hdr[4] = { FRAME_MAGIC, type, (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff) };
    uart_write_bytes(BT_UART, hdr, sizeof(hdr));
    if (len > 0) {
        uart_write_bytes(BT_UART, data, len);
    }
}

static void bridge_command(uint8_t cmd)
{
    bridge_send(FT_CMD, &cmd, 1);
}

/* ---- iAP2 link -------------------------------------------------------- */

static uint8_t csum(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return (uint8_t)(-sum);
}

/* Writes one link packet with an explicit sequence number.
 * - synchronization uses the initial PSN (99) and does not consume it,
 * - standalone acknowledgements use the current PSN and session 0,
 * - only data packets advance the PSN (first data packet is therefore 100). */
static void link_write_packet(uint8_t control, uint8_t session, uint8_t seq,
                              const uint8_t *payload, size_t len)
{
    EXT_RAM_BSS_ATTR static uint8_t out[8200];
    uint8_t hdr[HEADER_BYTES];
    /* A packet without payload carries no payload checksum (length == 9). */
    uint16_t total = (uint16_t)(HEADER_BYTES + (len > 0 ? len + 1 : 0));
    if ((size_t)total > sizeof(out)) {
        return;
    }
    hdr[0] = LINK_START_HIGH;
    hdr[1] = LINK_START_LOW;
    hdr[2] = (uint8_t)(total >> 8);
    hdr[3] = (uint8_t)(total & 0xff);
    hdr[4] = control;
    hdr[5] = seq;
    hdr[6] = s_ack;
    hdr[7] = session;
    hdr[8] = csum(hdr, 8);

    size_t n = 0;
    memcpy(out, hdr, sizeof(hdr));
    n += sizeof(hdr);
    if (len > 0) {
        memcpy(out + n, payload, len);
        n += len;
        out[n++] = csum(payload, len);
    }
    if (control != CONTROL_ACKNOWLEDGEMENT || len > 0) {
        ESP_LOGI(TAG, "link tx ctl=0x%02x psn=%u ack=%u sess=%u len=%u", control, seq, s_ack,
                 session, (unsigned)n);
    } else {
        ESP_LOGI(TAG, "link tx ACK psn=%u ack=%u raw=%02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 seq, s_ack, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8]);
    }
    /* Everything must travel inside the bridge's A5 framing. */
    bridge_send(FT_DATA, out, n);
}

static void link_send_data(const uint8_t *payload, size_t len)
{
    s_seq++;
    link_write_packet(CONTROL_ACKNOWLEDGEMENT, SESSION_CONTROL, s_seq, payload, len);
}

static void link_send_ack(void)
{
    /* Standalone ACK: control 0x40, session 0, no payload, current PSN. */
    link_write_packet(CONTROL_ACKNOWLEDGEMENT, 0, s_seq, NULL, 0);
}

static void link_send_sync(void)
{
    uint8_t p[19];
    size_t i = 0;
    p[i++] = 1;              /* synchronization version */
    p[i++] = 4;              /* maxOutgoing */
    p[i++] = 0xFF; p[i++] = 0xFF; /* maxLength 0xFFFF */
    p[i++] = 0x0F; p[i++] = 0xA0; /* retransmission timeout 4000 */
    p[i++] = 0x01; p[i++] = 0xF4; /* acknowledgement timeout 500 */
    p[i++] = 4;              /* maxRetransmissions */
    p[i++] = 3;              /* maxAcknowledgements */
    p[i++] = 10; p[i++] = 0; p[i++] = 2; /* control session */
    p[i++] = 11; p[i++] = 2; p[i++] = 1; /* EA session */
    p[i++] = 12; p[i++] = 1; p[i++] = 2; /* file transfer session */
    link_write_packet(CONTROL_SYNCHRONIZE, 0, INITIAL_SEQUENCE, p, i);
}

static void link_handle_packet(uint8_t control, uint8_t seq, uint8_t data_session,
                               const uint8_t *payload, size_t len)
{
    if ((control & CONTROL_RESET) != 0) {
        ESP_LOGW(TAG, "phone sent iAP2 reset");
        s_writable = false;
        return;
    }
    if ((control & CONTROL_SYNCHRONIZE) != 0 && len >= 10) {
        uint16_t max_len = (uint16_t)((payload[2] << 8) | payload[3]);
        s_peer_max_payload = max_len > HEADER_BYTES ? (uint16_t)(max_len - HEADER_BYTES) : 512;
        s_peer_synced = true;
        s_ack = seq;
        ESP_LOGI(TAG, "phone sync: maxLength=%u -> payload %u", max_len, s_peer_max_payload);
        link_send_ack();
    }
    if ((control & CONTROL_ACKNOWLEDGEMENT) != 0) {
        if (s_peer_synced && !s_writable) {
            s_writable = true;
            ESP_LOGI(TAG, "iAP2 link writable");
        }
        s_ack = seq;
    }
    if ((control & (uint8_t)~CONTROL_ACKNOWLEDGEMENT) == 0 && payload != NULL) {
        /* data packet: acknowledge it so the phone considers it delivered */
        s_ack = seq;
        link_send_ack();
    }
    (void)data_session;
}

/* ---- CSM layer -------------------------------------------------------- */

EXT_RAM_BSS_ATTR static uint8_t s_csm[8192];
static size_t s_csm_len;

static void csm_offer(const uint8_t *data, size_t len)
{
    if (s_csm_len + len > sizeof(s_csm)) {
        s_csm_len = 0;
    }
    memcpy(s_csm + s_csm_len, data, len);
    s_csm_len += len;

    size_t off = 0;
    while (s_csm_len - off >= 6) {
        if (s_csm[off] != 0x40 || s_csm[off + 1] != 0x40) {
            off++;
            continue;
        }
        size_t flen = ((size_t)s_csm[off + 2] << 8) | s_csm[off + 3];
        if (flen < 6 || flen > sizeof(s_csm)) {
            off++;
            continue;
        }
        if (s_csm_len - off < flen) {
            break;
        }
        uint16_t msg_id = (uint16_t)((s_csm[off + 4] << 8) | s_csm[off + 5]);
        ESP_LOGI(TAG, "CSM rx 0x%04X (%u bytes)", msg_id, (unsigned)(flen - 6));
        if (s_rx_cb != NULL) {
            s_rx_cb(msg_id, s_csm + off + 6, flen - 6);
        }
        off += flen;
    }
    if (off > 0) {
        memmove(s_csm, s_csm + off, s_csm_len - off);
        s_csm_len -= off;
    }
}

esp_err_t xc_link_send_csm(uint16_t msg_id, const uint8_t *body, size_t len)
{
    if (!s_writable) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tx_lock != NULL) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    EXT_RAM_BSS_ATTR static uint8_t frame[8192];
    size_t flen = 6 + len;
    if (flen > sizeof(frame)) {
        if (s_tx_lock != NULL) {
            xSemaphoreGive(s_tx_lock);
        }
        return ESP_ERR_INVALID_SIZE;
    }
    frame[0] = 0x40;
    frame[1] = 0x40;
    frame[2] = (uint8_t)(flen >> 8);
    frame[3] = (uint8_t)(flen & 0xff);
    frame[4] = (uint8_t)(msg_id >> 8);
    frame[5] = (uint8_t)(msg_id & 0xff);
    if (len > 0) {
        memcpy(frame + 6, body, len);
    }

    size_t off = 0;
    while (off < flen) {
        size_t chunk = flen - off;
        if (chunk > s_peer_max_payload) {
            chunk = s_peer_max_payload;
        }
        link_send_data(frame + off, chunk);
        off += chunk;
    }
    ESP_LOGI(TAG, "CSM tx 0x%04X (%u bytes)", msg_id, (unsigned)len);
    if (s_tx_lock != NULL) {
        xSemaphoreGive(s_tx_lock);
    }
    return ESP_OK;
}

/* ---- link receive state machine --------------------------------------- */

typedef enum { LS_DETECT, LS_HEADER, LS_PAYLOAD } link_state_t;
static link_state_t s_ls = LS_DETECT;
static uint8_t s_hdr[HEADER_BYTES];
static size_t s_hdr_len;
static size_t s_need;
static uint8_t s_pending_ctl, s_pending_seq, s_pending_sess;
EXT_RAM_BSS_ATTR static uint8_t s_payload[8192];
static size_t s_payload_len;
static size_t s_marker_pos;

static void link_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (s_ls) {
        case LS_DETECT:
            if (b == IAP2_MARKER[s_marker_pos]) {
                if (++s_marker_pos == sizeof(IAP2_MARKER)) {
                    s_marker_pos = 0;
                    s_ls = LS_HEADER;
                    s_hdr_len = 0;
                    ESP_LOGI(TAG, "phone marker received");
                    link_send_sync();
                }
            } else {
                s_marker_pos = (b == IAP2_MARKER[0]) ? 1 : 0;
            }
            break;
        case LS_HEADER:
            s_hdr[s_hdr_len++] = b;
            if (s_hdr_len == HEADER_BYTES) {
                uint16_t total = (uint16_t)((s_hdr[2] << 8) | s_hdr[3]);
                if (csum(s_hdr, HEADER_BYTES) != 0 || total < HEADER_BYTES) {
                    s_ls = LS_DETECT;
                    s_marker_pos = 0;
                    break;
                }
                s_pending_ctl = s_hdr[4];
                s_pending_seq = s_hdr[5];
                s_pending_sess = s_hdr[7];
                s_need = total - HEADER_BYTES;
                s_payload_len = 0;
                s_ls = (s_need > 0) ? LS_PAYLOAD : LS_HEADER;
                if (s_need == 0) {
                    link_handle_packet(s_pending_ctl, s_pending_seq, s_pending_sess, NULL, 0);
                    s_hdr_len = 0;
                }
            }
            break;
        case LS_PAYLOAD:
            if (s_payload_len < sizeof(s_payload)) {
                s_payload[s_payload_len++] = b;
            }
            if (s_payload_len == s_need) {
                if (csum(s_payload, s_payload_len) == 0) {
                    size_t plen = s_payload_len - 1; /* strip payload checksum */
                    link_handle_packet(s_pending_ctl, s_pending_seq, s_pending_sess, s_payload, plen);
                    if ((s_pending_ctl & (uint8_t)~CONTROL_ACKNOWLEDGEMENT) == 0 && plen > 0) {
                        csm_offer(s_payload, plen);
                    }
                }
                s_payload_len = 0;
                s_hdr_len = 0;
                s_ls = LS_HEADER;
            }
            break;
        }
    }
}

/* ---- tasks ------------------------------------------------------------ */

static void uart_task(void *arg)
{
    static uint8_t buf[2048];
    size_t used = 0;
    int64_t next_marker = 0;
    for (;;) {
        int n = uart_read_bytes(BT_UART, buf + used, sizeof(buf) - used, pdMS_TO_TICKS(50));
        if (n > 0) {
            used += (size_t)n;
        }
        while (used >= 4) {
            if (buf[0] != FRAME_MAGIC) {
                memmove(buf, buf + 1, used - 1);
                used--;
                continue;
            }
            size_t len = (size_t)buf[2] | ((size_t)buf[3] << 8);
            if (len > sizeof(buf) - 4) {
                memmove(buf, buf + 1, used - 1);
                used--;
                continue;
            }
            if (used < 4 + len) {
                break;
            }
            uint8_t type = buf[1];
            if (type == FT_DATA) {
                link_feed(buf + 4, len);
            } else if (type == FT_EVT) {
                char text[200];
                size_t n2 = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
                memcpy(text, buf + 4, n2);
                text[n2] = '\0';
                ESP_LOGI("xc_bridge", "%s", text);
                if (strstr(text, "iAP2 channel is") != NULL ||
                    strstr(text, "connected on rfcomm channel") != NULL) {
                    s_rfcomm_up = true;
                    s_ls = LS_DETECT;
                    s_marker_pos = 0;
                } else if (strstr(text, "rfcomm closed") != NULL) {
                    s_rfcomm_up = false;
                    s_writable = false;
                    s_peer_synced = false;
                    s_ls = LS_DETECT;
                    s_marker_pos = 0;
                }
            }
            memmove(buf, buf + 4 + len, used - 4 - len);
            used -= 4 + len;
        }

        /* Re-send the iAP2 marker every second until the phone answers. It is
         * safe to send unconditionally: the bridge drops data while its RFCOMM
         * channel is down, and xcertplay does exactly the same. */
        int64_t now = esp_timer_get_time() / 1000;
        if (now >= next_marker) {
            next_marker = now + 1000;
            if (!s_peer_synced) {
                bridge_send(FT_DATA, IAP2_MARKER, sizeof(IAP2_MARKER));
            }
            if (s_peer_synced && !s_writable) {
                link_send_sync();
            }
        }
    }
}

esp_err_t xc_link_start(xc_csm_rx_cb_t on_message)
{
    s_rx_cb = on_message;
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
    }

    uart_config_t uc = {
        .baud_rate = BT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    esp_err_t err = uart_driver_install(BT_UART, 8192, 8192, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    uart_param_config(BT_UART, &uc);
    uart_set_pin(BT_UART, BT_TX_GPIO, BT_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreatePinnedToCore(uart_task, "bt_link", 8192, NULL, 6, NULL, 0);

    /* Ask the bridge to (re)connect if it is not connected yet. */
    vTaskDelay(pdMS_TO_TICKS(500));
    bridge_command(1);
    ESP_LOGI(TAG, "bridge link up (UART%d TX=%d RX=%d)", BT_UART, BT_TX_GPIO, BT_RX_GPIO);
    return ESP_OK;
}

bool xc_link_writable(void)
{
    return s_writable;
}

bool xc_link_rfcomm_up(void)
{
    return s_rfcomm_up;
}
