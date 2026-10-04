/*
 * net_ws.c: WebSocket at /ws.
 *
 * RX (httpd task): {"t":"set","k":"eq.b2.gain","v":3.5}  or  {"t":"set","values":{...}}
 * TX: the "net_push" task wakes at ~15 Hz, builds one JSON frame
 *       {"t":"u","m":{meters...},"p":{changed params},"s":{status (1 Hz or on change)}}
 *     and hands it to the httpd task with httpd_queue_work(), which sends it to every WS client.
 *     Only one frame is in flight at a time; while one is pending, parameter changes accumulate
 *     in a mask, so a slow client delays updates but never loses them or grows memory.
 * The push task never touches sockets itself and never blocks the audio path (params are
 * lock-free; meters/profile are lock-free reads).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "params.h"
#include "net_priv.h"


static const char *TAG = "net_ws";

#define WS_RX_MAX     1024
#define WS_TX_CAP     3072
#define WS_PERIOD_MS  66          /* ~15 Hz */
#define WS_MAX_FDS    8

typedef struct {
    httpd_handle_t hd;
    size_t len;
    char data[];
} ws_msg_t;

static httpd_handle_t    s_hd;
static TaskHandle_t      s_task;
static volatile bool     s_run;
static volatile bool     s_busy;          /* a frame is queued in the httpd task */
static volatile int      s_clients;       /* WS clients seen by the last send (hint) */
static volatile bool     s_new_client;
static StaticSemaphore_t s_done_buf;
static SemaphoreHandle_t s_done;

/* ------------------------------------------------------------------ RX */
static void apply_set(const cJSON *j)
{
    const cJSON *vals = cJSON_GetObjectItemCaseSensitive(j, "values");
    if (cJSON_IsObject(vals)) {
        int n = 0;
        for (const cJSON *it = vals->child; it && n < 64; it = it->next, n++)
            if (cJSON_IsNumber(it)) net_api_set_param(it->string, it->valuedouble, NULL);
            else if (cJSON_IsBool(it)) net_api_set_param(it->string, cJSON_IsTrue(it), NULL);
        return;
    }
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "k");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v");
    if (!cJSON_IsString(k) || !k->valuestring) return;
    if (cJSON_IsNumber(v)) net_api_set_param(k->valuestring, v->valuedouble, NULL);
    else if (cJSON_IsBool(v)) net_api_set_param(k->valuestring, cJSON_IsTrue(v), NULL);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {                 /* handshake complete */
        s_clients++;
        s_new_client = true;
        return ESP_OK;
    }
    httpd_ws_frame_t f = { 0 };
    esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
    if (e != ESP_OK) return e;
    if (f.len > WS_RX_MAX) return ESP_FAIL;        /* closes the socket */
    if (f.len == 0) return ESP_OK;
    uint8_t *rx = malloc(f.len + 1);
    if (!rx) return ESP_ERR_NO_MEM;
    f.payload = rx;
    e = httpd_ws_recv_frame(req, &f, f.len);
    if (e != ESP_OK || f.type != HTTPD_WS_TYPE_TEXT) { free(rx); return e; }
    rx[f.len] = 0;
    cJSON *j = cJSON_ParseWithLength((const char *)rx, f.len);
    free(rx);
    if (cJSON_IsObject(j)) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "t");
        if (cJSON_IsString(t) && t->valuestring && !strcmp(t->valuestring, "set")) apply_set(j);
    }
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t net_ws_register(httpd_handle_t hd)
{
    static const httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
        .is_websocket = true, .handle_ws_control_frames = false,
    };
    return httpd_register_uri_handler(hd, &ws);
}

/* ------------------------------------------------------------------ TX (runs in the httpd task) */
static void ws_send_work(void *arg)
{
    ws_msg_t *m = arg;
    int fds[WS_MAX_FDS];
    size_t n = WS_MAX_FDS;
    int clients = 0;
    if (httpd_get_client_list(m->hd, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(m->hd, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            httpd_ws_frame_t f = { .final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)m->data, .len = m->len };
            if (httpd_ws_send_frame_async(m->hd, fds[i], &f) != ESP_OK) httpd_sess_trigger_close(m->hd, fds[i]);
            else clients++;
        }
    }
    s_clients = clients;
    free(m);
    s_busy = false;
}

/* ------------------------------------------------------------------ push task */
static bool build_frame(jw_t *w, const uint32_t *mask, bool with_params, bool with_status)
{
    jw_obj(w, NULL);
    jw_str(w, "t", "u");
    jw_obj(w, "m");
    net_meters_json(w);
    jw_obj_end(w);
    if (with_params) {
        jw_obj(w, "p");
        for (int i = 0; i < P_COUNT; i++)
            if (params_mask_test(mask, (param_id_t)i)) jw_num(w, g_param_desc[i].key, params_get((param_id_t)i), 7);
        jw_obj_end(w);
    } else {
        jw_bool(w, "resync", true);                /* too many changes: client refetches /api/state */
    }
    if (with_status) {
        jw_obj(w, "s");
        net_status_json(w);
        jw_obj_end(w);
    }
    jw_obj_end(w);
    return jw_finish(w) == ESP_OK;
}

static void push_task(void *arg)
{
    uint32_t pend[PARAMS_MASK_WORDS] = { 0 };
    TickType_t last = xTaskGetTickCount();
    int ticks = 0;
    while (s_run) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(WS_PERIOD_MS));
        uint32_t mask[PARAMS_MASK_WORDS];
        bool any = params_take_dirty(PL_WEB, mask);
        if (s_clients <= 0) {                      /* nobody listening: drop changes */
            memset(pend, 0, sizeof pend);
            continue;
        }
        if (any) for (int i = 0; i < PARAMS_MASK_WORDS; i++) pend[i] |= mask[i];
        if (s_busy) continue;

        bool with_status = s_new_client || (++ticks >= 1000 / WS_PERIOD_MS);
        if (with_status) { ticks = 0; s_new_client = false; }

        ws_msg_t *m = heap_caps_malloc(sizeof *m + WS_TX_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m) m = malloc(sizeof *m + WS_TX_CAP);
        if (!m) continue;
        jw_t w;
        jw_init(&w, m->data, WS_TX_CAP, NULL);
        if (!build_frame(&w, pend, true, with_status)) {
            jw_init(&w, m->data, WS_TX_CAP, NULL);
            if (!build_frame(&w, pend, false, with_status)) { free(m); continue; }
        }
        memset(pend, 0, sizeof pend);
        m->hd = s_hd;
        m->len = w.len;
        s_busy = true;
        if (httpd_queue_work(s_hd, ws_send_work, m) != ESP_OK) {
            free(m);
            s_busy = false;
        }
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t net_ws_start(httpd_handle_t hd)
{
    if (s_task) return ESP_OK;
    if (!s_done) s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    s_hd = hd;
    s_run = true;
    s_busy = false;
    s_clients = 0;
    s_new_client = false;
    if (xTaskCreatePinnedToCore(push_task, "net_push", 3584, NULL, 4, &s_task, 0) != pdPASS) {
        s_task = NULL;
        s_run = false;
        ESP_LOGE(TAG, "push task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_ws_stop(void)
{
    if (!s_task) return;
    s_run = false;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
    s_task = NULL;
    /* let the httpd task run (and free) a frame that is still queued */
    for (int i = 0; i < 20 && s_busy; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_hd = NULL;
}
