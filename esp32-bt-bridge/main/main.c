/*
 * Classic-Bluetooth iAP2 bridge for the ESP32-P4 CarPlay project.
 *
 * The iPhone is the RFCOMM *server*: it listens on its iAP2 channel and the
 * accessory connects to it (this is what the working xcertplay/LIVI stacks do).
 * This firmware pairs with the phone, then connects to its iAP2 RFCOMM channel
 * and pipes the byte stream to the ESP32-P4 over UART2.
 *
 * UART link (115200 8N1, no flow control):
 *     ESP32 GPIO17 (TX2) -> P4 GPIO5
 *     ESP32 GPIO16 (RX2) <- P4 GPIO4
 *     GND common, ESP32 powered from the P4 board's 5V pin.
 *
 * Framing (both directions):
 *     A5 | type | len_lo | len_hi | payload...
 *     type 1 = iAP2 data, 2 = command (P4 -> ESP32), 3 = event text (ESP32 -> P4)
 *     commands: 1 = connect, 2 = disconnect
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "bt_bridge";

#define DEVICE_NAME "xcertplay"

#define P4_UART UART_NUM_2
#define P4_TX_GPIO 17
#define P4_RX_GPIO 16
#define P4_BAUD 115200

#define FRAME_MAGIC 0xA5
#define FT_DATA 1
#define FT_CMD 2
#define FT_EVT 3

#define EV_CONNECT_REQ BIT0
#define EV_SCN_OPEN BIT1
#define EV_SCN_CLOSED BIT2

static EventGroupHandle_t s_evt;
static uint32_t s_spp_handle;
static volatile bool s_connected;
/* Set by command 2 (P4 hands iAP2 to the CarPlay tunnel): stop auto-reconnect
 * until command 1 explicitly asks for RFCOMM again. */
static volatile bool s_suppress_connect;
static esp_bd_addr_t s_peer;
static volatile bool s_have_peer;
static int s_disc_scn = -1;
static int s_rx_logged;

/* ---- UART framing ----------------------------------------------------- */

static void uart_send(uint8_t type, const void *data, size_t len)
{
    uint8_t hdr[4] = { FRAME_MAGIC, type, (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff) };
    if (uart_write_bytes(P4_UART, hdr, sizeof(hdr)) < 0) {
        return;
    }
    if (len > 0) {
        uart_write_bytes(P4_UART, data, len);
    }
}

static void notify(const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n > 0) {
        uart_send(FT_EVT, text, (size_t)n);
        ESP_LOGI(TAG, "%s", text);
    }
}

static void bda_str(const uint8_t *bda, char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4],
             bda[5]);
}

/* Remember the phone ourselves: the controller bond list is not always
 * repopulated after a reboot. */
static void save_peer(const uint8_t *bda)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "peer", bda, sizeof(esp_bd_addr_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_peer(uint8_t *bda)
{
    nvs_handle_t h;
    size_t len = sizeof(esp_bd_addr_t);
    bool ok = false;
    if (nvs_open("bridge", NVS_READONLY, &h) == ESP_OK) {
        ok = (nvs_get_blob(h, "peer", bda, &len) == ESP_OK && len == sizeof(esp_bd_addr_t));
        nvs_close(h);
    }
    return ok;
}

/* ---- GAP -------------------------------------------------------------- */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char b[24];
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        bda_str(param->auth_cmpl.bda, b, sizeof(b));
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            memcpy(s_peer, param->auth_cmpl.bda, sizeof(esp_bd_addr_t));
            s_have_peer = true;
            save_peer(s_peer);
            notify("paired %s", b);
            xEventGroupSetBits(s_evt, EV_CONNECT_REQ);
        } else {
            notify("pair failed (%s) %d", b, param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 123456);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "mode changed: 0x%02x", param->mode_chg.mode);
        break;
    default:
        break;
    }
}

/* ---- SPP -------------------------------------------------------------- */

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            notify("spp ready");
        } else {
            notify("spp init failed %d", param->init.status);
        }
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
            s_disc_scn = param->disc_comp.scn[0];
            notify("sdp: %d channel(s), first scn=%d", param->disc_comp.scn_num, s_disc_scn);
        } else {
            notify("sdp: no spp service (%d)", param->disc_comp.status);
        }
        break;
    case ESP_SPP_OPEN_EVT:
        if (param->open.status == ESP_SPP_SUCCESS) {
            s_spp_handle = param->open.handle;
            s_connected = true;
            s_rx_logged = 0;
            notify("rfcomm open handle=%u", (unsigned)s_spp_handle);
            xEventGroupSetBits(s_evt, EV_SCN_OPEN);
        } else {
            notify("rfcomm open failed %d", param->open.status);
        }
        break;
    case ESP_SPP_CLOSE_EVT:
        s_connected = false;
        s_spp_handle = 0;
        notify("rfcomm closed");
        xEventGroupSetBits(s_evt, EV_SCN_CLOSED);
        break;
    case ESP_SPP_DATA_IND_EVT:
        /* Log the first 40 received frames (marker + sync alone used to fill
         * the old 4-entry budget, hiding everything the phone sent later). */
        if (s_rx_logged < 40) {
            char hex[3 * 16 + 1];
            size_t n = param->data_ind.len > 16 ? 16 : param->data_ind.len;
            for (size_t i = 0; i < n; i++) {
                snprintf(hex + i * 3, 4, "%02x ", param->data_ind.data[i]);
            }
            hex[n * 3] = '\0';
            notify("rfcomm rx %u bytes: %s", (unsigned)param->data_ind.len, hex);
            s_rx_logged++;
        }
        uart_send(FT_DATA, param->data_ind.data, param->data_ind.len);
        break;
    case ESP_SPP_WRITE_EVT:
        break;
    default:
        break;
    }
}

/* ---- HID device -------------------------------------------------------
 * iOS only lists Bluetooth devices that advertise a profile it supports, so a
 * SPP-only accessory stays invisible in Settings -> Bluetooth. Presenting a
 * plain mouse makes "xcertplay" pairable from the iPhone.
 */

static uint8_t s_mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xc0, 0xc0,
};

static esp_hidd_app_param_t s_hid_app;
static esp_hidd_qos_param_t s_hid_qos;

static void hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_INIT_EVT:
        if (param->init.status == ESP_HIDD_SUCCESS) {
            esp_bt_hid_device_register_app(&s_hid_app, &s_hid_qos, &s_hid_qos);
        }
        break;
    case ESP_HIDD_REGISTER_APP_EVT:
        if (param->register_app.status == ESP_HIDD_SUCCESS) {
            notify("hid ready%s", param->register_app.in_use ? " (in use)" : "");
            if (param->register_app.in_use) {
                esp_bt_hid_device_connect(param->register_app.bd_addr);
            }
        }
        break;
    case ESP_HIDD_OPEN_EVT:
        if (param->open.status == ESP_HIDD_SUCCESS) {
            char b[24];
            memcpy(s_peer, param->open.bd_addr, sizeof(esp_bd_addr_t));
            s_have_peer = true;
            save_peer(s_peer);
            bda_str(param->open.bd_addr, b, sizeof(b));
            notify("hid connected by %s", b);
            xEventGroupSetBits(s_evt, EV_CONNECT_REQ);
        }
        break;
    case ESP_HIDD_CLOSE_EVT:
        notify("hid disconnected");
        break;
    case ESP_HIDD_GET_REPORT_EVT: {
        static uint8_t report[4] = { 0 };
        esp_bt_hid_device_send_report(param->get_report.report_type, 0, sizeof(report), report);
        break;
    }
    default:
        break;
    }
}

/* ---- connect logic ---------------------------------------------------- */

static void connect_task(void *arg)
{
    char b[24];
    for (;;) {
        /* 9.3 item 3: keep the reconnect loop tight (was 3000 ms). */
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!s_have_peer || s_connected || s_suppress_connect) {
            continue;
        }

        bda_str(s_peer, b, sizeof(b));
        notify("connecting to %s", b);

        /* 9.3 item 3: no SDP discovery. The phone's iAP2 service hides behind
         * a 128-bit UUID that the standard SPP query never returns; walking
         * the channel numbers directly is faster. */
        s_disc_scn = -1;

        bool tried[31] = { 0 };
        int order[32];
        int count = 0;
        if (s_disc_scn > 0 && s_disc_scn <= 30) {
            order[count++] = s_disc_scn;
            tried[s_disc_scn] = true;
        }
        for (int scn = 1; scn <= 30 && count < 32; scn++) {
            if (!tried[scn]) {
                order[count++] = scn;
            }
        }

        for (int i = 0; i < count; i++) {
            if (s_connected) {
                break;
            }
            int scn = order[i];
            xEventGroupClearBits(s_evt, EV_SCN_OPEN);
            esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT,
                            ESP_SPP_ROLE_MASTER, (uint8_t)scn, s_peer);
            EventBits_t bits = xEventGroupWaitBits(s_evt, EV_SCN_OPEN, pdTRUE, pdFALSE,
                                                   pdMS_TO_TICKS(2500));
            if (!(bits & EV_SCN_OPEN)) {
                /* Let the stack release the failed MCB before the next try. */
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            /* Opened: give the phone a moment to decide whether this really is
             * the iAP2 channel (it drops the wrong ones immediately). */
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (s_connected) {
                notify("iAP2 channel is %d", scn);
                break;
            }
            notify("channel %d dropped by phone", scn);
        }
        if (!s_connected) {
            notify("no iAP2 channel found (tried %d), will retry", count);
        }
    }
}

static void uart_task(void *arg)
{
    static uint8_t buf[2048];
    size_t used = 0;
    for (;;) {
        int n = uart_read_bytes(P4_UART, buf + used, sizeof(buf) - used, pdMS_TO_TICKS(50));
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
            uint8_t *payload = buf + 4;
            if (type == FT_DATA) {
                if (s_connected && len > 0) {
                    esp_spp_write(s_spp_handle, (int)len, payload);
                }
            } else if (type == FT_CMD && len >= 1) {
                if (payload[0] == 1) {
                    s_suppress_connect = false;
                    xEventGroupSetBits(s_evt, EV_CONNECT_REQ);
                } else if (payload[0] == 2) {
                    s_suppress_connect = true;
                    if (s_connected) {
                        esp_spp_disconnect(s_spp_handle);
                    }
                }
            }
            memmove(buf, buf + 4 + len, used - 4 - len);
            used -= 4 + len;
        }
    }
}

/* ---- init ------------------------------------------------------------- */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uart_config_t uc = {
        .baud_rate = P4_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(P4_UART, 4096, 4096, 0, NULL, 0);
    uart_param_config(P4_UART, &uc);
    uart_set_pin(P4_UART, P4_TX_GPIO, P4_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "UART%d to P4: TX=%d RX=%d @%d", P4_UART, P4_TX_GPIO, P4_RX_GPIO, P4_BAUD);

    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_register_callback(gap_cb);
    esp_spp_register_callback(spp_cb);

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = false,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&spp_cfg));

    esp_bt_gap_set_device_name(DEVICE_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE; /* "Just Works" */
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    esp_bt_cod_t cod = { 0 };
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor = ESP_BT_COD_MINOR_PERIPHERAL_POINTING;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);

    s_hid_app.name = DEVICE_NAME;
    s_hid_app.description = "xcertplay head unit";
    s_hid_app.provider = "xcertplay";
    s_hid_app.subclass = ESP_HID_CLASS_MIC;
    s_hid_app.desc_list = s_mouse_desc;
    s_hid_app.desc_list_len = sizeof(s_mouse_desc);
    memset(&s_hid_qos, 0, sizeof(s_hid_qos));

    esp_bt_hid_device_register_callback(hidd_cb);
    if (esp_bt_hid_device_init() != ESP_OK) {
        notify("hid init failed (iOS may not list the device)");
    }

    char b[24];
    bda_str(esp_bt_dev_get_address(), b, sizeof(b));
    notify("bt ready, name=%s addr=%s - pair from the iPhone", DEVICE_NAME, b);

    /* If a phone is already bonded, connect straight away; otherwise fall back
     * to the address we saved ourselves. */
    int num = 0;
    esp_bd_addr_t list[8];
    if (esp_bt_gap_get_bond_device_list(&num, list) == ESP_OK && num > 0) {
        for (int i = 0; i < num; i++) {
            bda_str(list[i], b, sizeof(b));
            notify("bonded device %d: %s", i, b);
        }
        memcpy(s_peer, list[num - 1], sizeof(esp_bd_addr_t));
        s_have_peer = true;
        notify("using bonded device %s", b);
        xEventGroupSetBits(s_evt, EV_CONNECT_REQ);
    } else if (load_peer(s_peer)) {
        s_have_peer = true;
        bda_str(s_peer, b, sizeof(b));
        notify("no bond list, retrying saved peer %s", b);
        xEventGroupSetBits(s_evt, EV_CONNECT_REQ);
    } else {
        notify("no paired phone yet - pair from iPhone Settings -> Bluetooth");
    }

    xTaskCreate(uart_task, "uart_task", 4096, NULL, 12, NULL);
    xTaskCreate(connect_task, "bt_connect", 4096, NULL, 10, NULL);
}
