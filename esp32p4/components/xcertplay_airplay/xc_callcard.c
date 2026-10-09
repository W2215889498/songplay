#include "xc_callcard.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "xc_iap2.h"

static const char *TAG = "xc_callcard";

/* Full GB2312 C-array font generated with lv_font_conv (see STATUS 9.9). */
LV_FONT_DECLARE(lv_font_xc_cjk16)
#define CARD_FONT (&lv_font_xc_cjk16)

#define CARD_W 224
#define CARD_H 56
#define CARD_TOP_MARGIN 6 /* overlay offset from the top of the cluster area */

/* CallStateUpdate status values (catplay). */
#define CALL_STATUS_DISCONNECTED 0
#define CALL_STATUS_SENDING 1
#define CALL_STATUS_RINGING 2
#define CALL_STATUS_CONNECTING 3
#define CALL_STATUS_ACTIVE 4
#define CALL_STATUS_HELD 5
#define CALL_STATUS_DISCONNECTING 6

#define CALL_DIR_INCOMING 1
#define CALL_DIR_OUTGOING 2

typedef enum {
    PH_IDLE = 0,
    PH_RINGING,
    PH_OUTGOING,
    PH_ACTIVE,
    PH_HELD,
    PH_ENDED,
} phase_t;

static lv_obj_t *s_canvas;
static uint8_t *s_argb; /* CARD_W * CARD_H * 4, ARGB8888 (B,G,R,A) */
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_tick_timer;
static esp_timer_handle_t s_dismiss_timer;

static esp_err_t ensure_init(void);

/* call state */
static phase_t s_phase = PH_IDLE;
static int s_direction;
static char s_name[64];
static char s_number[48];
static int64_t s_active_ms;
static bool s_visible;

static void stop_dismiss(void)
{
    if (s_dismiss_timer != NULL) {
        esp_timer_stop(s_dismiss_timer);
    }
}

static void start_dismiss(void)
{
    if (s_dismiss_timer != NULL) {
        esp_timer_stop(s_dismiss_timer);
        esp_timer_start_once(s_dismiss_timer, 3 * 1000 * 1000);
    }
}

/* Render the card into the ARGB canvas buffer. Caller holds s_lock. */
static void render_locked(void)
{
    memset(s_argb, 0, (size_t)CARD_W * CARD_H * 4);

    bsp_display_lock(-1);
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    /* Dark rounded background (slightly translucent). */
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(0x1c1c1e);
    bg.bg_opa = 215;
    bg.radius = 10;
    lv_area_t area = { 0, 0, CARD_W - 1, CARD_H - 1 };
    lv_draw_rect(&layer, &bg, &area);

    /* Direction accent bar: incoming green, outgoing blue, else gray. */
    uint32_t accent = 0x8e8e93;
    if (s_direction == CALL_DIR_INCOMING) {
        accent = 0x30d158;
    } else if (s_direction == CALL_DIR_OUTGOING) {
        accent = 0x0a84ff;
    }
    if (s_phase == PH_ENDED) {
        accent = 0x8e8e93;
    }
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = lv_color_hex(accent);
    bar.bg_opa = 255;
    bar.radius = 3;
    lv_area_t bar_area = { 7, 12, 12, CARD_H - 13 };
    lv_draw_rect(&layer, &bar, &bar_area);

    /* Line 1: contact name, falling back to the number. */
    const char *title = s_name[0] ? s_name : (s_number[0] ? s_number : "未知号码");
    lv_draw_label_dsc_t t1;
    lv_draw_label_dsc_init(&t1);
    t1.color = lv_color_white();
    t1.font = CARD_FONT;
    t1.text = title;
    lv_area_t a1 = { 22, 5, CARD_W - 8, 27 };
    lv_draw_label(&layer, &t1, &a1);

    /* Line 2: status text (+ call timer while up). */
    char status[48];
    int64_t elapsed_s = 0;
    if (s_phase == PH_ACTIVE || s_phase == PH_HELD) {
        elapsed_s = (esp_timer_get_time() / 1000 - s_active_ms) / 1000;
        if (elapsed_s > 99 * 60 + 59) {
            elapsed_s = 99 * 60 + 59;
        }
    }
    switch (s_phase) {
    case PH_RINGING:
        snprintf(status, sizeof(status), "%s", s_direction == CALL_DIR_OUTGOING ? "呼出中" : "来电");
        break;
    case PH_OUTGOING:
        snprintf(status, sizeof(status), "呼出中");
        break;
    case PH_ACTIVE:
        snprintf(status, sizeof(status), "通话中 %02d:%02d", (int)(elapsed_s / 60),
                 (int)(elapsed_s % 60));
        break;
    case PH_HELD:
        snprintf(status, sizeof(status), "保持中 %02d:%02d", (int)(elapsed_s / 60),
                 (int)(elapsed_s % 60));
        break;
    case PH_ENDED:
        snprintf(status, sizeof(status), "通话结束");
        break;
    default:
        status[0] = '\0';
        break;
    }
    lv_draw_label_dsc_t t2;
    lv_draw_label_dsc_init(&t2);
    t2.color = lv_color_hex(0xb8b8c0);
    t2.font = CARD_FONT;
    t2.text = status;
    lv_area_t a2 = { 22, 28, CARD_W - 8, 52 };
    lv_draw_label(&layer, &t2, &a2);

    lv_canvas_finish_layer(s_canvas, &layer);
    bsp_display_unlock();
}

void xc_callcard_on_call(int status, int direction, int service, const char *name,
                         const char *number)
{
    (void)service;
    if (ensure_init() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (name != NULL && name[0] != '\0') {
        snprintf(s_name, sizeof(s_name), "%s", name);
    }
    if (number != NULL && number[0] != '\0') {
        snprintf(s_number, sizeof(s_number), "%s", number);
    }
    if (direction != 0) {
        s_direction = direction;
    }
    switch (status) {
    case CALL_STATUS_SENDING:
    case CALL_STATUS_CONNECTING:
        s_phase = PH_OUTGOING;
        break;
    case CALL_STATUS_RINGING:
        s_phase = PH_RINGING;
        break;
    case CALL_STATUS_ACTIVE:
    case CALL_STATUS_HELD:
        if (s_phase != PH_ACTIVE && s_phase != PH_HELD) {
            s_active_ms = esp_timer_get_time() / 1000;
        }
        s_phase = (status == CALL_STATUS_HELD) ? PH_HELD : PH_ACTIVE;
        break;
    case CALL_STATUS_DISCONNECTED:
    case CALL_STATUS_DISCONNECTING:
        if (s_phase == PH_IDLE || s_phase == PH_ENDED) {
            xSemaphoreGive(s_lock);
            return;
        }
        /* Show "call ended" and dismiss it a few seconds later. This path
         * must return here: the common tail below stops the dismiss timer. */
        s_phase = PH_ENDED;
        s_visible = true;
        render_locked();
        start_dismiss();
        xSemaphoreGive(s_lock);
        return;
    default:
        xSemaphoreGive(s_lock);
        return;
    }
    stop_dismiss();
    s_visible = true;
    render_locked();
    xSemaphoreGive(s_lock);
}

void xc_callcard_compose(uint16_t *video, int cw, int ch)
{
    if (video == NULL || !s_visible) {
        return;
    }
    if (ensure_init() != ESP_OK) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    if (s_visible && cw >= CARD_W && ch >= CARD_H + CARD_TOP_MARGIN) {
        int x0 = (cw - CARD_W) / 2;
        int y0 = CARD_TOP_MARGIN;
        for (int y = 0; y < CARD_H; y++) {
            const uint8_t *arow = s_argb + (size_t)y * CARD_W * 4;
            uint16_t *vrow = video + (size_t)(y0 + y) * cw + x0;
            for (int x = 0; x < CARD_W; x++) {
                uint8_t alpha = arow[x * 4 + 3];
                if (alpha == 0) {
                    continue;
                }
                uint16_t v = vrow[x];
                uint16_t c565 = (uint16_t)(((arow[x * 4 + 2] & 0xf8) << 8) |
                                           ((arow[x * 4 + 1] & 0xfc) << 3) | (arow[x * 4] >> 3));
                if (alpha == 255) {
                    vrow[x] = c565;
                    continue;
                }
                int cr = (c565 >> 11) & 0x1f, cg = (c565 >> 5) & 0x3f, cb = c565 & 0x1f;
                int vr = (v >> 11) & 0x1f, vg = (v >> 5) & 0x3f, vb = v & 0x1f;
                int r = (cr * alpha + vr * (255 - alpha)) / 255;
                int g = (cg * alpha + vg * (255 - alpha)) / 255;
                int b = (cb * alpha + vb * (255 - alpha)) / 255;
                vrow[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
    xSemaphoreGive(s_lock);
}

static void tick_cb(void *arg)
{
    (void)arg;
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    if (s_visible && (s_phase == PH_ACTIVE || s_phase == PH_HELD)) {
        render_locked(); /* refresh the elapsed-time text */
    }
    xSemaphoreGive(s_lock);
}

static void dismiss_cb(void *arg)
{
    (void)arg;
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_phase == PH_ENDED) {
        s_phase = PH_IDLE;
        s_visible = false; /* the next composed frame carries plain video */
        memset(s_name, 0, sizeof(s_name));
        memset(s_number, 0, sizeof(s_number));
        s_direction = 0;
    }
    xSemaphoreGive(s_lock);
}

/* Create the canvas and timers on first use: the AirPlay stack may come up
 * before the display is initialized now (Plan B). */
static esp_err_t ensure_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK; /* already initialized */
    }
    s_lock = xSemaphoreCreateMutex();
    s_argb = heap_caps_malloc((size_t)CARD_W * CARD_H * 4, MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_argb == NULL) {
        ESP_LOGE(TAG, "init failed (buffer alloc)");
        return ESP_ERR_NO_MEM;
    }
    memset(s_argb, 0, (size_t)CARD_W * CARD_H * 4);

    bsp_display_lock(-1);
    /* Off-screen root: the video path cleans the active screen, so the canvas
     * must not be one of its children. */
    lv_obj_t *root = lv_obj_create(NULL);
    s_canvas = lv_canvas_create(root);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_buffer(s_canvas, s_argb, CARD_W, CARD_H, LV_COLOR_FORMAT_ARGB8888);
    bsp_display_unlock();

    const esp_timer_create_args_t tick_args = { .callback = tick_cb, .name = "call_tick" };
    if (esp_timer_create(&tick_args, &s_tick_timer) != ESP_OK ||
        esp_timer_start_periodic(s_tick_timer, 1000 * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "tick timer failed");
        return ESP_FAIL;
    }
    const esp_timer_create_args_t dis_args = { .callback = dismiss_cb, .name = "call_dismiss" };
    if (esp_timer_create(&dis_args, &s_dismiss_timer) != ESP_OK) {
        ESP_LOGE(TAG, "dismiss timer failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "caller-ID card ready (%dx%d)", CARD_W, CARD_H);
    return ESP_OK;
}

esp_err_t xc_callcard_init(void)
{
    return ensure_init();
}
