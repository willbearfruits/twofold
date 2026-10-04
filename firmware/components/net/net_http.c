/*
 * net_http.c: esp_http_server lifecycle, the embedded web app, captive-portal redirects and
 * small request/response helpers. The API lives in net_api.c, WebSocket in net_ws.c, OTA in
 * net_ota.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "net_priv.h"

static const char *TAG = "net_http";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static httpd_handle_t s_hd;
static char s_etag[20];

httpd_handle_t net_http_handle(void) { return s_hd; }

/* ------------------------------------------------------------------ helpers */
void net_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

esp_err_t net_send_err(httpd_req_t *req, int status, const char *msg)
{
    const char *st = "400 Bad Request";
    switch (status) {
    case 404: st = "404 Not Found"; break;
    case 409: st = "409 Conflict"; break;
    case 413: st = "413 Payload Too Large"; break;
    case 500: st = "500 Internal Server Error"; break;
    case 503: st = "503 Service Unavailable"; break;
    default: break;
    }
    char b[160];
    jw_t w;
    jw_init(&w, b, sizeof b, NULL);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", false);
    jw_str(&w, "error", msg);
    jw_obj_end(&w);
    jw_finish(&w);
    httpd_resp_set_status(req, st);
    net_json_headers(req);
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

esp_err_t net_send_ok(httpd_req_t *req)
{
    net_json_headers(req);
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

char *net_read_body(httpd_req_t *req, size_t max, int *status)
{
    *status = 0;
    size_t len = req->content_len;
    if (len == 0) { *status = 400; return NULL; }
    if (len > max) { *status = 413; return NULL; }
    char *b = malloc(len + 1);
    if (!b) { *status = 500; return NULL; }
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int n = httpd_req_recv(req, b + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) { free(b); *status = 400; return NULL; }
        got += (size_t)n;
    }
    b[len] = 0;
    return b;
}

/* ------------------------------------------------------------------ static app */
static esp_err_t h_index(httpd_req_t *req)
{
    char inm[24];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof inm) == ESP_OK && !strcmp(inm, s_etag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", s_etag);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", s_etag);
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

static esp_err_t h_no_content(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ captive portal */
static esp_err_t redirect_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", NET_AP_URL);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* iOS needs a body to treat this as a portal */
    return httpd_resp_send(req, "<a href=\"" NET_AP_URL "\">TWOFOLD setup</a>", HTTPD_RESP_USE_STRLEN);
}

/* OS connectivity probes: Apple hotspot-detect, Android generate_204, Windows connecttest/ncsi,
 * Firefox canonical. In setup mode they all redirect to the portal; otherwise they 404. */
static esp_err_t h_probe(httpd_req_t *req)
{
    if (net_wifi_ap_active()) return redirect_portal(req);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    return ESP_OK;
}

static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    if (net_wifi_ap_active() && req->method == HTTP_GET && strncmp(req->uri, "/api/", 5) != 0)
        return redirect_portal(req);
    return net_send_err(req, 404, "not found");
}

/* ------------------------------------------------------------------ lifecycle */
esp_err_t net_http_start(void)
{
    if (s_hd) return ESP_OK;
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s_etag, sizeof s_etag, "\"%02x%02x%02x%02x%02x%02x\"", d->app_elf_sha256[0], d->app_elf_sha256[1],
             d->app_elf_sha256[2], d->app_elf_sha256[3], d->app_elf_sha256[4], d->app_elf_sha256[5]);

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 6144;              /* JSON + cJSON parse + OTA header checks; internal RAM */
    c.core_id = 0;
    c.task_priority = 5;
    c.max_uri_handlers = 40;
    c.max_open_sockets = 7;           /* LWIP_MAX_SOCKETS 12 - 3 internal - DNS - spare */
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 10;
    c.send_wait_timeout = 10;
    c.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t e = httpd_start(&s_hd, &c);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        s_hd = NULL;
        return e;
    }

    static const httpd_uri_t statics[] = {
        { .uri = "/",            .method = HTTP_GET, .handler = h_index },
        { .uri = "/index.html",  .method = HTTP_GET, .handler = h_index },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = h_no_content },
        /* captive-portal probes */
        { .uri = "/hotspot-detect.html",        .method = HTTP_GET, .handler = h_probe },  /* Apple */
        { .uri = "/library/test/success.html",  .method = HTTP_GET, .handler = h_probe },  /* Apple (old) */
        { .uri = "/generate_204",               .method = HTTP_GET, .handler = h_probe },  /* Android/Chrome */
        { .uri = "/gen_204",                    .method = HTTP_GET, .handler = h_probe },
        { .uri = "/connecttest.txt",            .method = HTTP_GET, .handler = h_probe },  /* Windows 10/11 */
        { .uri = "/ncsi.txt",                   .method = HTTP_GET, .handler = h_probe },  /* Windows (old) */
        { .uri = "/redirect",                   .method = HTTP_GET, .handler = h_probe },  /* Windows */
        { .uri = "/canonical.html",             .method = HTTP_GET, .handler = h_probe },  /* Firefox */
        { .uri = "/success.txt",                .method = HTTP_GET, .handler = h_probe },  /* Firefox */
    };
    for (size_t i = 0; i < sizeof statics / sizeof statics[0]; i++) httpd_register_uri_handler(s_hd, &statics[i]);
    net_api_register(s_hd);
    net_ota_register(s_hd);
    net_ws_register(s_hd);
    httpd_register_err_handler(s_hd, HTTPD_404_NOT_FOUND, h_404);
    net_ws_start(s_hd);
    ESP_LOGI(TAG, "httpd up (app %u bytes gz)", (unsigned)(index_html_gz_end - index_html_gz_start));
    return ESP_OK;
}

void net_http_stop(void)
{
    if (!s_hd) return;
    net_ws_stop();
    httpd_stop(s_hd);
    s_hd = NULL;
}
