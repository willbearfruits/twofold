/*
 * net_usb.c: the web editor's API over USB (WebUSB in Chrome/Edge, WinUSB on Windows, plain libusb elsewhere).
 * No radio involved: it works in Studio Mode. Same JSON API as HTTP (API.md), framed over the vendor bulk pipe:
 *
 *   request  = u32 length (LE) + "METHOD /path[?query]\n" + body
 *   response = u32 length (LE) + "STATUS\n" + JSON body        (STATUS = 200 or an HTTP-style error code)
 *
 * One request at a time, handled in its own task (internal stack: preset saves write flash). The TinyUSB vendor
 * FIFO (512 B) is drained into a PSRAM request buffer; responses stream out in 64-byte packets.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tusb.h"
#include "net_priv.h"

static const char *TAG = "net_usb";

#define REQ_MAX   (16 * 1024)
#define RESP_MAX  (24 * 1024)
#define IDLE_MS   200

static TaskHandle_t s_task;
static uint8_t     *s_req;
static char        *s_resp;

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
{
    (void)idx; (void)buffer; (void)bufsize;
    if (s_task) xTaskNotifyGive(s_task);
}

/* Reads exactly n bytes from the vendor FIFO; false on timeout (framing lost: the caller resyncs). */
static bool read_exact(uint8_t *dst, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < n) {
        uint32_t k = tud_vendor_read(dst + got, (uint32_t)(n - got));
        if (k) { got += k; continue; }
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms))) return false;
    }
    return true;
}

static void write_all(const uint8_t *src, size_t n)
{
    size_t off = 0;
    int idle = 0;
    while (off < n && tud_vendor_mounted() && idle < 500) {
        uint32_t k = tud_vendor_write(src + off, (uint32_t)(n - off));
        tud_vendor_write_flush();
        if (k) { off += k; idle = 0; } else { vTaskDelay(1); idle++; }
    }
}

static void handle(size_t len)
{
    s_req[len] = 0;
    char *nl = memchr(s_req, '\n', len);
    if (!nl) return;
    *nl = 0;
    char *sp = strchr((char *)s_req, ' ');
    if (!sp) return;
    *sp = 0;
    const char *method = (const char *)s_req, *path = sp + 1, *body = nl + 1;
    size_t body_len = len - (size_t)(body - (const char *)s_req);

    jw_t w;
    jw_init(&w, s_resp + 8, RESP_MAX - 8, NULL);          /* room for the length + status line in front */
    const char *err = "error";
    void (*after)(void) = NULL;
    int status = net_api_dispatch(method, path, body, body_len, &w, &err, &after);
    if (!status && jw_finish(&w) != ESP_OK) { status = 500; err = "response too large"; }
    char head[16];
    int hl;
    if (status) {
        hl = snprintf(head, sizeof head, "%d\n", status);
        w.len = (size_t)snprintf(s_resp + 8, RESP_MAX - 8, "{\"ok\":false,\"error\":\"%s\"}", err);
    } else {
        hl = snprintf(head, sizeof head, "200\n");
    }
    /* assemble: [len][status line][json]; the status line is short, so shift it in just before the body */
    uint8_t *frame = (uint8_t *)s_resp + 8 - hl - 4;
    uint32_t total = (uint32_t)(hl + w.len);
    memcpy(frame, &total, 4);
    memcpy(frame + 4, head, (size_t)hl);
    write_all(frame, 4 + total);
    if (after) { vTaskDelay(pdMS_TO_TICKS(50)); after(); }
}

static void usb_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_MS));
        while (tud_vendor_mounted() && tud_vendor_available() >= 4) {
            uint32_t len;
            if (!read_exact((uint8_t *)&len, 4, 50)) break;
            if (len == 0 || len > REQ_MAX - 1) {          /* garbage: drop what we have and resync */
                ESP_LOGW(TAG, "bad frame length %lu", (unsigned long)len);
                tud_vendor_read_flush();
                break;
            }
            if (!read_exact(s_req, len, 1000)) { tud_vendor_read_flush(); break; }
            handle(len);
        }
    }
}

esp_err_t net_usb_start(void)
{
    if (s_task) return ESP_OK;
    s_req = heap_caps_malloc(REQ_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_req || !s_resp) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(usb_task, "usbapi", 5120, NULL, 6, &s_task, 0) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}
