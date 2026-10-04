/*
 * net_priv.h: internals shared by the net component's files.
 *
 * Threads (all core 0, all exist only while WiFi is on):
 *   net_mgr   (prio 4, 4 KB)  : owns every esp_wifi_* / netif / mdns call (single writer).
 *   httpd     (prio 5, 6 KB)  : REST API, static page, WebSocket RX, OTA upload.
 *   net_push  (prio 4, 3 KB)  : ~15 Hz WebSocket push (meters, param changes, status).
 *   net_dns   (prio 4, 3 KB)  : captive DNS, AP setup mode only.
 * None of them touch anything the core-1 audio task locks (params are lock-free).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "net.h"
#include "net_json.h"

#define NET_HOSTNAME        "twofold"          /* mDNS: twofold.local */
#define NET_AP_SSID_PREFIX  "TWOFOLD-"
#define NET_AP_IP_STR       "192.168.4.1"     /* esp_netif default SoftAP address */
#define NET_AP_URL          "http://" NET_AP_IP_STR "/"
#define NET_AP_CHANNEL      6
#define NET_AP_MAX_CONN     3

/* Max WiFi TX power in 0.25 dBm units: 44 = 11 dBm (the driver rounds to its nearest step).
 * Plenty for a desk/studio room, ~9 dB below the S3's default 20 dBm: less supply ripple next to
 * the analog stages and less 2.4 GHz noise. */
#define NET_WIFI_TX_POWER_QDBM 44

/* STA connect attempts before falling back to the AP setup portal. */
#define NET_STA_RETRIES_FIRST  5    /* never connected since enable (bad password, wrong SSID...) */
#define NET_STA_RETRIES_LOST   24   /* was connected, link dropped (router reboot): ~2-3 min */

#define NET_CREDS_KEY       "wifi.sta"

typedef struct {
    char ssid[33];
    char pass[65];
} net_creds_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t auth;      /* wifi_auth_mode_t */
    uint8_t channel;
} net_scan_ap_t;
#define NET_SCAN_MAX 20

/* ---- net_wifi.c (manager) ---- */
bool        net_wifi_ap_active(void);              /* SoftAP + captive portal running */
esp_err_t   net_wifi_request_scan(void);
esp_err_t   net_wifi_request_connect(const char *ssid, const char *pass);
void        net_wifi_json(jw_t *w);               /* {"state":..., networks...} object body fields */
const char *net_wifi_state_name(wifi_state_t s);

/* ---- net_http.c ---- */
esp_err_t   net_http_start(void);
void        net_http_stop(void);
httpd_handle_t net_http_handle(void);
/* helpers shared by the API files */
char       *net_read_body(httpd_req_t *req, size_t max, int *status);  /* malloc'd, NUL-terminated */
esp_err_t   net_send_err(httpd_req_t *req, int status, const char *msg);
esp_err_t   net_send_ok(httpd_req_t *req);
void        net_json_headers(httpd_req_t *req);

/* ---- net_api.c ---- */
esp_err_t   net_api_register(httpd_handle_t hd);
void        net_status_json(jw_t *w);              /* "status" fields, shared with the WS push */
void        net_meters_json(jw_t *w);              /* meters + CPU fields */
/* Apply one parameter write coming from the web (handles the special net.* keys). Returns
 * 0 ok, 404 unknown key, 400 bad value. */
int         net_api_set_param(const char *key, double v, float *applied);

/* ---- net_ws.c ---- */
esp_err_t   net_ws_register(httpd_handle_t hd);
esp_err_t   net_ws_start(httpd_handle_t hd);
void        net_ws_stop(void);

/* ---- net_ota.c ---- */
esp_err_t   net_ota_register(httpd_handle_t hd);
bool        net_ota_active(void);
uint8_t     net_ota_pct(void);

/* ---- net_dns.c ---- */
esp_err_t   net_dns_start(uint32_t ip_be);         /* ip in network byte order */
void        net_dns_stop(void);
