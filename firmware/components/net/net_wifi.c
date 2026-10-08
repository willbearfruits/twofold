/*
 * net_wifi.c: WiFi lifecycle manager.
 *
 * Studio Mode (default): nothing exists. No driver, no netif, no task, no event loop we created.
 * net_wifi_enable(true) spawns the "net_mgr" task, which brings everything up; enable(false)
 * makes it tear everything down (httpd, WS push, DNS, mDNS, esp_wifi_stop/deinit, handlers,
 * netifs, event loop if we created it, scan buffers) and then the task deletes itself.
 *
 * All esp_wifi_* / netif / mdns calls are made by the manager task only, so there are no races
 * between the UI task, httpd handlers and the WiFi event loop: they all just post messages.
 * The public API never blocks for more than a mutex hand-off (a few us).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "storage.h"
#include "net_priv.h"
#include "bootdiag.h"

static const char *TAG = "net";

/* ------------------------------------------------------------------ messages */
enum {
    M_ON = 1, M_OFF, M_SETUP, M_CONNECT, M_FORGET, M_SCAN,
    M_EV_STA_START, M_EV_STA_STOP, M_EV_STA_CONNECTED, M_EV_STA_DISC, M_EV_GOT_IP, M_EV_LOST_IP,
    M_EV_SCAN_DONE,
};
typedef struct { uint8_t type; int32_t arg; } net_msg_t;

#define NET_QLEN 24
static StaticQueue_t     s_q_buf;
static uint8_t           s_q_store[NET_QLEN * sizeof(net_msg_t)];
static QueueHandle_t     s_q;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;      /* guards the task handle and the published strings */
static TaskHandle_t      s_task;

/* ------------------------------------------------------------------ published state (s_lock) */
static volatile wifi_state_t s_state = WIFI_OFF;
static char           s_sta_ssid[33];
static char           s_ap_ssid[33];
static char           s_ap_pass[12];
static char           s_ip[16];
static volatile int8_t s_rssi;
#define NET_ERR_LEN 96
static char           s_err[NET_ERR_LEN];
static net_scan_ap_t *s_scan;          /* NET_SCAN_MAX entries, allocated on first scan */
static uint8_t        s_scan_n;
static volatile bool  s_scanning;
static bool           s_have_creds;
static net_creds_t    s_pending;       /* connect request from the portal */
static volatile bool  s_ap_on;

/* ------------------------------------------------------------------ manager-private */
static bool           s_up, s_started, s_sta_started, s_own_loop, s_mdns_on, s_http_on;
static bool           s_want_sta, s_ever_connected;
static int            s_retries;
static int64_t        s_retry_at, s_ap_off_at, s_rssi_at;    /* esp_timer us, 0 = none */
static esp_netif_t   *s_netif_sta, *s_netif_ap;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;
static net_creds_t    s_creds;
static size_t         s_heap_int0, s_heap_ext0;
static wifi_mode_t    s_mode = WIFI_MODE_NULL;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void set_state(wifi_state_t st)
{
    if (s_state != st) ESP_LOGI(TAG, "state %s -> %s", net_wifi_state_name(s_state), net_wifi_state_name(st));
    s_state = st;
}

static void set_err(const char *e)
{
    lock();
    snprintf(s_err, sizeof s_err, "%s", e ? e : "");
    unlock();
}

const char *net_wifi_state_name(wifi_state_t s)
{
    switch (s) {
    case WIFI_OFF:            return "off";
    case WIFI_STARTING:       return "starting";
    case WIFI_AP_SETUP:       return "setup";
    case WIFI_STA_CONNECTING: return "connecting";
    case WIFI_STA_CONNECTED:  return "connected";
    default:                  return "error";
    }
}

/* ------------------------------------------------------------------ event loop -> manager */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    net_msg_t m = { 0, 0 };
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:     m.type = M_EV_STA_START; break;
        case WIFI_EVENT_STA_STOP:      m.type = M_EV_STA_STOP; break;
        case WIFI_EVENT_STA_CONNECTED: m.type = M_EV_STA_CONNECTED; break;
        case WIFI_EVENT_STA_DISCONNECTED:
            m.type = M_EV_STA_DISC;
            m.arg = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
            break;
        case WIFI_EVENT_SCAN_DONE:     m.type = M_EV_SCAN_DONE; break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "AP: client joined");
            break;
        default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP && data) {
            m.type = M_EV_GOT_IP;
            m.arg = (int32_t)((ip_event_got_ip_t *)data)->ip_info.ip.addr;
        } else if (id == IP_EVENT_STA_LOST_IP) {
            m.type = M_EV_LOST_IP;
        }
    }
    if (m.type && xQueueSend(s_q, &m, 0) != pdTRUE) ESP_LOGW(TAG, "event queue full, dropped %d", m.type);
}

/* ------------------------------------------------------------------ helpers */
static void gen_ap_identity(void)
{
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";   /* no l/o/0/1 */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    lock();
    snprintf(s_ap_ssid, sizeof s_ap_ssid, NET_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    for (int i = 0; i < 8; i++) s_ap_pass[i] = alphabet[esp_random() % (sizeof alphabet - 1)];
    s_ap_pass[8] = 0;
    unlock();
}

static const char *reason_text(int r)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_CONNECTION_FAIL:
        return "wrong password?";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_BEACON_TIMEOUT:
        return "no response from network";
    default:
        return "connection failed";
    }
}

/* Applied after every mode change: low TX power, HT20, modem sleep in pure STA. */
static void apply_radio_settings(void)
{
    esp_err_t e = esp_wifi_set_max_tx_power(NET_WIFI_TX_POWER_QDBM);
    if (e != ESP_OK) ESP_LOGW(TAG, "set_max_tx_power: %s", esp_err_to_name(e));
    if (s_mode == WIFI_MODE_STA || s_mode == WIFI_MODE_APSTA) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (s_mode == WIFI_MODE_AP || s_mode == WIFI_MODE_APSTA)  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    /* Modem sleep needs a pure STA; the SoftAP must stay awake for its clients. */
    esp_wifi_set_ps(s_mode == WIFI_MODE_STA ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

static esp_err_t set_mode(wifi_mode_t m)
{
    if (m == s_mode) return ESP_OK;
    esp_err_t e = esp_wifi_set_mode(m);
    if (e == ESP_OK) s_mode = m;
    return e;
}

static esp_err_t ensure_started(void)
{
    if (s_started) { apply_radio_settings(); return ESP_OK; }
    bootdiag_crumb("WIFI START");
    esp_err_t e = esp_wifi_start();
    bootdiag_crumb("WIFI ON");
    if (e != ESP_OK) return e;
    s_started = true;
    apply_radio_settings();
    return ESP_OK;
}

static void mdns_start(void)
{
    if (s_mdns_on) return;
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mdns_init failed"); return; }
    mdns_hostname_set(NET_HOSTNAME);
    mdns_instance_name_set("TWOFOLD");
    mdns_txt_item_t txt[] = { { "path", "/" } };
    mdns_service_add("TWOFOLD", "_http", "_tcp", 80, txt, 1);
    s_mdns_on = true;
}

/* ------------------------------------------------------------------ bring-up / teardown */
static void teardown(void);

static esp_err_t bring_up(void)
{
    if (s_up) return ESP_OK;
    s_heap_int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_heap_ext0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    set_state(WIFI_STARTING);

    /* The WiFi blob does not survive allocation failures gracefully (it crashed in ieee80211_hostap_attach):
     * refuse to start rather than crash when internal RAM is short. */
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < NET_MIN_INTERNAL_HEAP) {
        ESP_LOGE(TAG, "not enough internal RAM for WiFi: %u B free", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        set_state(WIFI_ERROR);
        set_err("Not enough memory");
        return ESP_ERR_NO_MEM;
    }
    bootdiag_crumb("WIFI INIT");
    esp_err_t e = esp_netif_init();                 /* idempotent; lwIP tcpip task persists */
    if (e != ESP_OK) goto fail;
    e = esp_event_loop_create_default();
    if (e == ESP_OK) s_own_loop = true;
    else if (e != ESP_ERR_INVALID_STATE) goto fail;

    s_netif_sta = esp_netif_create_default_wifi_sta();
    if (!s_netif_sta) { e = ESP_ERR_NO_MEM; goto fail; }
    esp_netif_set_hostname(s_netif_sta, NET_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;                              /* never touch flash from the driver */
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) goto fail;
    s_up = true;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    s_mode = WIFI_MODE_NULL;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_h_ip);
    esp_wifi_set_country_code("01", true);           /* world-safe, follow the AP's 802.11d info */

    bootdiag_crumb("WIFI MDNS");
    mdns_start();
    bootdiag_crumb("WIFI HTTP");
    if (net_http_start() == ESP_OK) s_http_on = true;
    bootdiag_crumb("WIFI ON");
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "bring-up failed: %s", esp_err_to_name(e));
    teardown();
    set_state(WIFI_ERROR);
    set_err("WiFi start failed");
    return e;
}

static void teardown(void)
{
    bootdiag_crumb("WIFI STOP");
    if (s_http_on) {
        vTaskDelay(pdMS_TO_TICKS(250));   /* let a just-sent HTTP response (e.g. "Studio Mode") leave */
        net_http_stop();                  /* also stops the WS push task; waits for handlers */
        s_http_on = false;
    }
    net_dns_stop();
    if (s_mdns_on) { mdns_free(); s_mdns_on = false; }
    if (s_up) {
        if (s_started) esp_wifi_stop();
        if (s_h_wifi) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
        if (s_h_ip) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_h_ip);
        s_h_wifi = s_h_ip = NULL;
        esp_wifi_deinit();
    }
    if (s_netif_ap)  { esp_netif_destroy_default_wifi(s_netif_ap);  s_netif_ap = NULL; }
    if (s_netif_sta) { esp_netif_destroy_default_wifi(s_netif_sta); s_netif_sta = NULL; }
    if (s_own_loop)  { esp_event_loop_delete_default(); s_own_loop = false; }

    lock();
    free(s_scan);
    s_scan = NULL;
    s_scan_n = 0;
    s_ip[0] = 0;
    s_ap_pass[0] = 0;
    memset(&s_pending, 0, sizeof s_pending);
    unlock();
    memset(&s_creds, 0, sizeof s_creds);
    s_scanning = false;
    s_ap_on = false;
    s_up = s_started = s_sta_started = s_want_sta = s_ever_connected = false;
    s_retries = 0;
    s_retry_at = s_ap_off_at = s_rssi_at = 0;
    s_mode = WIFI_MODE_NULL;
    s_rssi = 0;
    set_state(WIFI_OFF);
    bootdiag_crumb(NULL);

    if (s_heap_int0) {
        size_t i1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t x1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "WiFi off. heap internal %u -> %u (%+d), psram %u -> %u (%+d)",
                 (unsigned)s_heap_int0, (unsigned)i1, (int)i1 - (int)s_heap_int0,
                 (unsigned)s_heap_ext0, (unsigned)x1, (int)x1 - (int)s_heap_ext0);
        s_heap_int0 = s_heap_ext0 = 0;
    }
}

/* ------------------------------------------------------------------ modes */
static void go_ap_setup(const char *why)
{
    s_want_sta = false;
    s_retry_at = s_ap_off_at = 0;
    if (s_sta_started) esp_wifi_disconnect();
    lock();
    s_ip[0] = 0;
    unlock();
    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();
    if (!s_netif_ap) { set_state(WIFI_ERROR); set_err("AP netif failed"); return; }
    if (!s_ap_on) gen_ap_identity();

    wifi_config_t ac = { 0 };
    lock();
    memcpy(ac.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    ac.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    memcpy(ac.ap.password, s_ap_pass, strlen(s_ap_pass));
    unlock();
    ac.ap.channel = NET_AP_CHANNEL;
    ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ac.ap.max_connection = NET_AP_MAX_CONN;
    ac.ap.beacon_interval = 100;
    ac.ap.pmf_cfg.required = false;

    /* APSTA: the STA half is needed to scan for networks while the portal is up. */
    esp_err_t e = set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ac);
    if (e == ESP_OK) e = ensure_started();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(e));
        set_state(WIFI_ERROR);
        set_err("AP start failed");
        return;
    }
    /* RFC 8910 captive-portal URI in DHCP (option 114): modern phones open the portal directly. */
    static const char captive_uri[] = NET_AP_URL;
    esp_netif_dhcps_stop(s_netif_ap);
    esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           (void *)captive_uri, sizeof captive_uri - 1);
    esp_netif_dhcps_start(s_netif_ap);

    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(s_netif_ap, &ip);
    net_dns_start(ip.ip.addr);
    s_ap_on = true;
    set_err(why);
    set_state(WIFI_AP_SETUP);
    ESP_LOGI(TAG, "setup AP \"%s\" up%s%s", s_ap_ssid, why ? ": " : "", why ? why : "");
}

static void go_sta(void)
{
    wifi_config_t wc = { 0 };
    size_t sl = strnlen(s_creds.ssid, 32), pl = strnlen(s_creds.pass, 64);
    memcpy(wc.sta.ssid, s_creds.ssid, sl);
    memcpy(wc.sta.password, s_creds.pass, pl);
    wc.sta.threshold.authmode = pl ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    lock();
    snprintf(s_sta_ssid, sizeof s_sta_ssid, "%s", s_creds.ssid);
    s_ip[0] = 0;
    unlock();
    s_want_sta = true;
    s_retries = 0;
    s_retry_at = 0;
    s_ever_connected = false;

    if (s_sta_started) esp_wifi_disconnect();
    esp_err_t e = set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e == ESP_OK) e = ensure_started();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "STA start failed: %s", esp_err_to_name(e));
        go_ap_setup("WiFi error");
        return;
    }
    if (s_sta_started) esp_wifi_connect();   /* else: connect on STA_START */
    set_state(WIFI_STA_CONNECTING);
}

static void stop_ap(void)
{
    if (!s_ap_on) return;
    net_dns_stop();
    s_ap_on = false;
    set_mode(WIFI_MODE_STA);
    apply_radio_settings();
    ESP_LOGI(TAG, "setup AP closed");
}

static bool load_creds(void)
{
    net_creds_t c;
    size_t len = sizeof c;
    memset(&c, 0, sizeof c);
    if (storage_kv_get(NET_CREDS_KEY, &c, &len) != ESP_OK || len != sizeof c) return false;
    c.ssid[32] = 0;
    c.pass[64] = 0;
    if (!c.ssid[0]) return false;
    s_creds = c;
    return true;
}

static void on_scan_done(void)
{
    s_scanning = false;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    wifi_ap_record_t *rec = n ? heap_caps_calloc(n, sizeof *rec, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (n && !rec) rec = calloc(n, sizeof *rec);
    if (!rec || esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) n = 0;
    esp_wifi_clear_ap_list();

    lock();
    if (!s_scan) s_scan = calloc(NET_SCAN_MAX, sizeof *s_scan);
    s_scan_n = 0;
    for (int i = 0; s_scan && i < n; i++) {
        const char *ss = (const char *)rec[i].ssid;
        if (!ss[0]) continue;                                   /* hidden */
        int k;
        for (k = 0; k < s_scan_n; k++) if (!strcmp(s_scan[k].ssid, ss)) break;
        if (k < s_scan_n) {                                     /* duplicate (mesh): keep strongest */
            if (rec[i].rssi > s_scan[k].rssi) { s_scan[k].rssi = rec[i].rssi; s_scan[k].channel = rec[i].primary; }
            continue;
        }
        if (s_scan_n >= NET_SCAN_MAX) break;
        net_scan_ap_t *a = &s_scan[s_scan_n++];
        snprintf(a->ssid, sizeof a->ssid, "%s", ss);
        a->rssi = rec[i].rssi;
        a->auth = (uint8_t)rec[i].authmode;
        a->channel = rec[i].primary;
    }
    /* insertion sort by RSSI, strongest first */
    for (int i = 1; i < s_scan_n; i++) {
        net_scan_ap_t t = s_scan[i];
        int j = i - 1;
        while (j >= 0 && s_scan[j].rssi < t.rssi) { s_scan[j + 1] = s_scan[j]; j--; }
        s_scan[j + 1] = t;
    }
    unlock();
    free(rec);
    ESP_LOGI(TAG, "scan: %d networks", s_scan_n);
}

/* ------------------------------------------------------------------ message handling */
static void handle(const net_msg_t *m)
{
    switch (m->type) {
    case M_ON:
        if (s_up) break;
        if (bring_up() != ESP_OK) break;
        s_have_creds = load_creds();
        if (s_have_creds) go_sta(); else go_ap_setup(NULL);
        break;

    case M_SETUP:
        if (!s_up && bring_up() != ESP_OK) break;
        s_have_creds = load_creds();
        go_ap_setup(NULL);
        break;

    case M_OFF:
        teardown();
        break;

    case M_CONNECT: {
        lock();
        net_creds_t c = s_pending;
        memset(&s_pending, 0, sizeof s_pending);
        unlock();
        if (!c.ssid[0]) break;
        /* explicit user action: soft-muted flash write inside storage */
        esp_err_t e = storage_kv_set(NET_CREDS_KEY, &c, sizeof c);
        if (e != ESP_OK) ESP_LOGW(TAG, "saving credentials failed: %s", esp_err_to_name(e));
        s_have_creds = (e == ESP_OK);
        s_creds = c;
        if (!s_up && bring_up() != ESP_OK) break;
        set_err(NULL);
        go_sta();           /* keeps the AP up (APSTA) so the portal page can follow the result */
        break;
    }

    case M_FORGET:
        storage_kv_erase(NET_CREDS_KEY);
        s_have_creds = false;
        memset(&s_creds, 0, sizeof s_creds);
        lock();
        s_sta_ssid[0] = 0;
        unlock();
        if (s_up) go_ap_setup("network forgotten");
        break;

    case M_SCAN:
        if (!s_up || s_scanning) break;
        if (s_mode == WIFI_MODE_AP) { set_mode(WIFI_MODE_APSTA); apply_radio_settings(); }
        if (s_want_sta && s_state == WIFI_STA_CONNECTING) break;   /* scanning while connecting fails */
        {
            wifi_scan_config_t sc = { 0 };
            sc.show_hidden = false;
            sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            sc.scan_time.active.min = 60;
            sc.scan_time.active.max = 120;
            esp_err_t e = esp_wifi_scan_start(&sc, false);
            s_scanning = (e == ESP_OK);
            if (e != ESP_OK) ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(e));
        }
        break;

    case M_EV_SCAN_DONE:
        if (s_up) on_scan_done();
        break;

    case M_EV_STA_START:
        s_sta_started = true;
        if (s_up && s_want_sta) esp_wifi_connect();
        break;

    case M_EV_STA_STOP:
        s_sta_started = false;
        break;

    case M_EV_STA_CONNECTED:
        break;

    case M_EV_STA_DISC: {
        lock();
        s_ip[0] = 0;
        unlock();
        s_rssi = 0;
        if (!s_up || !s_want_sta) break;
        const char *why = reason_text(m->arg);
        int limit = s_ever_connected ? NET_STA_RETRIES_LOST : NET_STA_RETRIES_FIRST;
        s_retries++;
        ESP_LOGW(TAG, "STA disconnected (reason %d: %s), attempt %d/%d", (int)m->arg, why, s_retries, limit);
        if (s_retries >= limit) {
            char msg[NET_ERR_LEN];
            lock();
            snprintf(msg, sizeof msg, "Could not join \"%.24s\": %s", s_sta_ssid, why);
            unlock();
            go_ap_setup(msg);
        } else {
            int ms = s_retries * 1000;
            if (ms > 8000) ms = 8000;
            s_retry_at = esp_timer_get_time() + (int64_t)ms * 1000;
            set_err(why);
            set_state(WIFI_STA_CONNECTING);
        }
        break;
    }

    case M_EV_GOT_IP: {
        if (!s_up) break;
        esp_ip4_addr_t a = { .addr = (uint32_t)m->arg };
        lock();
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&a));
        unlock();
        s_retries = 0;
        s_retry_at = 0;
        s_ever_connected = true;
        set_err(NULL);
        set_state(WIFI_STA_CONNECTED);
        s_rssi_at = esp_timer_get_time();
        ESP_LOGI(TAG, "connected: http://%s/  http://" NET_HOSTNAME ".local/", s_ip);
        /* Portal flow: keep the AP 20 s so the setup page can show the new address, then close it. */
        if (s_ap_on) s_ap_off_at = esp_timer_get_time() + 20 * 1000000LL;
        break;
    }

    case M_EV_LOST_IP:
        lock();
        s_ip[0] = 0;
        unlock();
        break;

    default:
        break;
    }
}

static void timers(void)
{
    if (!s_up) return;
    int64_t now = esp_timer_get_time();
    if (s_retry_at && now >= s_retry_at) {
        s_retry_at = 0;
        if (s_want_sta) esp_wifi_connect();
    }
    if (s_ap_off_at && now >= s_ap_off_at) {
        s_ap_off_at = 0;
        if (s_state == WIFI_STA_CONNECTED) stop_ap();
    }
    if (s_state == WIFI_STA_CONNECTED && now >= s_rssi_at) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
        s_rssi_at = now + 2000000;
    }
}

static TickType_t next_wait(void)
{
    if (!s_up) return 0;
    int64_t now = esp_timer_get_time(), t = now + 2000000;     /* at least every 2 s */
    if (s_retry_at && s_retry_at < t) t = s_retry_at;
    if (s_ap_off_at && s_ap_off_at < t) t = s_ap_off_at;
    if (s_state == WIFI_STA_CONNECTED && s_rssi_at && s_rssi_at < t) t = s_rssi_at;
    int64_t ms = (t - now) / 1000;
    if (ms < 1) ms = 1;
    return pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1;
}

static void mgr_task(void *arg)
{
    for (;;) {
        net_msg_t m;
        if (xQueueReceive(s_q, &m, next_wait()) == pdTRUE) handle(&m);
        timers();
        lock();
        if (!s_up && uxQueueMessagesWaiting(s_q) == 0) {
            s_task = NULL;          /* Studio Mode: the manager itself goes away too */
            unlock();
            vTaskDelete(NULL);
        }
        unlock();
    }
}

static esp_err_t post(uint8_t type, int32_t arg)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    net_msg_t m = { type, arg };
    esp_err_t ret = ESP_OK;
    lock();
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        ret = ESP_ERR_NO_MEM;
    } else if (!s_task) {
        /* internal-RAM stack: this task writes flash (credentials) via storage */
        if (xTaskCreatePinnedToCore(mgr_task, "net_mgr", 8192, NULL, 4, &s_task, 0) != pdPASS) {
            s_task = NULL;
            xQueueReset(s_q);
            ret = ESP_ERR_NO_MEM;
        }
    }
    unlock();
    return ret;
}

/* ------------------------------------------------------------------ public API (net.h) */
esp_err_t net_init(void)
{
    if (s_q) return ESP_OK;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_q = xQueueCreateStatic(NET_QLEN, sizeof(net_msg_t), s_q_store, &s_q_buf);
    if (net_usb_start() != ESP_OK) ESP_LOGW(TAG, "USB control channel not started");
    return (s_lock && s_q) ? ESP_OK : ESP_FAIL;
}

esp_err_t net_wifi_enable(bool on)   { return post(on ? M_ON : M_OFF, 0); }
esp_err_t net_wifi_setup(void)       { return post(M_SETUP, 0); }
esp_err_t net_wifi_forget(void)      { return post(M_FORGET, 0); }
wifi_state_t net_wifi_state(void)    { return s_state; }
bool net_wifi_ap_active(void)        { return s_ap_on; }

esp_err_t net_wifi_request_scan(void)
{
    if (s_state == WIFI_OFF) return ESP_ERR_INVALID_STATE;
    s_scanning = true;      /* optimistic, so an immediate GET shows "scanning" */
    return post(M_SCAN, 0);
}

esp_err_t net_wifi_request_connect(const char *ssid, const char *pass)
{
    size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
    if (sl < 1 || sl > 32) return ESP_ERR_INVALID_ARG;
    if (pl != 0 && (pl < 8 || pl > 64)) return ESP_ERR_INVALID_ARG;
    lock();
    memset(&s_pending, 0, sizeof s_pending);
    memcpy(s_pending.ssid, ssid, sl);
    if (pl) memcpy(s_pending.pass, pass, pl);
    unlock();
    return post(M_CONNECT, 0);
}

void net_fill_status(sys_status_t *s)
{
    if (!s || !s_lock) return;
    s->wifi = s_state;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        bool setup = (s_state == WIFI_AP_SETUP);
        snprintf(s->wifi_ssid, sizeof s->wifi_ssid, "%s", setup ? s_ap_ssid : s_sta_ssid);
        snprintf(s->wifi_pass, sizeof s->wifi_pass, "%s", setup ? s_ap_pass : "");
        snprintf(s->ip, sizeof s->ip, "%s", setup ? NET_AP_IP_STR : s_ip);
        xSemaphoreGive(s_lock);
    }
    s->wifi_rssi = s_rssi;
    s->ota_in_progress = net_ota_active();
    s->ota_pct = net_ota_pct();
}

static const char *auth_name(uint8_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:         return "open";
    case WIFI_AUTH_WEP:          return "wep";
    case WIFI_AUTH_WPA3_PSK:     return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:return "wpa2/3";
    case WIFI_AUTH_WPA2_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "enterprise";
    default:                     return "wpa2";
    }
}

void net_wifi_json(jw_t *w)
{
    /* Snapshot under the lock, write outside it (writing may block on the socket). */
    struct {
        char ssid[33], ip[16], ap_ssid[33], err[NET_ERR_LEN];
        net_scan_ap_t scan[NET_SCAN_MAX];
        uint8_t n;
    } *c = malloc(sizeof *c);
    if (!c) { w->err = true; return; }
    wifi_state_t st = s_state;
    bool ap = s_ap_on;
    lock();
    memcpy(c->ssid, s_sta_ssid, sizeof c->ssid);
    memcpy(c->ip, s_ip, sizeof c->ip);
    memcpy(c->ap_ssid, ap ? s_ap_ssid : "", ap ? sizeof c->ap_ssid : 1);
    memcpy(c->err, s_err, sizeof c->err);
    c->n = s_scan ? s_scan_n : 0;
    if (c->n) memcpy(c->scan, s_scan, c->n * sizeof c->scan[0]);
    unlock();

    const char *mode = "off";
    if (st != WIFI_OFF) mode = ap ? (st == WIFI_AP_SETUP ? "ap" : "apsta") : "sta";
    jw_str(w, "state", net_wifi_state_name(st));
    jw_str(w, "mode", mode);
    jw_str(w, "hostname", NET_HOSTNAME ".local");
    jw_str(w, "ssid", c->ssid);
    jw_str(w, "ip", c->ip);
    jw_int(w, "rssi", s_rssi);
    jw_str(w, "ap_ssid", c->ap_ssid);
    jw_str(w, "ap_ip", ap ? NET_AP_IP_STR : "");
    jw_str(w, "error", c->err);
    jw_bool(w, "saved", s_have_creds);
    jw_bool(w, "scanning", s_scanning);
    jw_arr(w, "networks");
    for (int i = 0; i < c->n; i++) {
        jw_obj(w, NULL);
        jw_str(w, "ssid", c->scan[i].ssid);
        jw_int(w, "rssi", c->scan[i].rssi);
        jw_str(w, "auth", auth_name(c->scan[i].auth));
        jw_int(w, "ch", c->scan[i].channel);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    free(c);
}
