/*
 * ble_midi.c: BLE-MIDI peripheral on NimBLE (remote control only; the S3 has no BT Classic).
 *
 * Service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3
 * (read -> empty, write-without-response -> MIDI in, notify -> MIDI out). Advertised name
 * "TWOFOLD MIDI".
 *
 * Lifecycle: ble_midi_enable(true) runs nimble_port_init() (controller init+enable, host init),
 * registers the GATT table and starts the host task. ble_midi_enable(false) stops the host
 * (nimble_port_stop: waits for the host task to leave its loop), then nimble_port_deinit():
 * host deinit + esp_bt_controller_disable/deinit. Controller memory is never released
 * (no esp_bt_controller_mem_release), so the radio can be enabled again at any time.
 * No NVS: bonding off, CONFIG_BT_NIMBLE_NVS_PERSIST=n.
 *
 * Threads: NimBLE host task (core 0) runs the GATT/GAP callbacks; the firmware's ctrl task calls
 * ble_midi_poll()/ble_midi_send_cc(). A mutex serialises enable/disable against them; the poll
 * path only ever *tries* it, so the ctrl task never waits for a (slow) enable/disable.
 */
#include "bootdiag.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "params.h"
#include "ble_midi.h"

static const char *TAG = "ble_midi";

#define BLE_MIDI_NAME       "TWOFOLD MIDI"
/* 0 dBm: a studio desk is a few metres; ~9 dB below the S3 default, less 2.4 GHz noise. */
#define BLE_MIDI_TX_POWER   ESP_PWR_LVL_N0
/* Outgoing CC batching: param changes are collected and sent at most every 10 ms
 * (BLE-MIDI connection interval is 7.5..15 ms, so faster sends only queue up). */
#define BLE_MIDI_TX_PERIOD_US 10000
#define BLE_MIDI_PKT_MAX    64

/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 (little-endian byte order for NimBLE) */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
/* 7772E5DB-3868-4112-A1A9-F2669D106BF3 */
static const ble_uuid128_t s_chr_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static volatile ble_state_t s_state = BLE_OFF;
static volatile bool     s_on;                 /* host running */
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_notify;             /* central subscribed to notifications */
static volatile uint16_t s_mtu = 23;
static uint16_t          s_val_handle;
static uint8_t           s_own_addr_type;
static uint32_t          s_pend[PARAMS_MASK_WORDS];
static int64_t           s_last_tx;
static size_t            s_heap_int0, s_heap_ext0;

/* BLE-MIDI receive parser state (host task only; sysex may span packets) */
static uint8_t s_rx_status;     /* current (running) status, 0 = none */
static uint8_t s_rx_need, s_rx_have, s_rx_data[2];
static bool    s_rx_sysex;

static void advertise(void);

/* ------------------------------------------------------------------ MIDI in */
static void midi_dispatch(uint8_t st, const uint8_t *d)
{
    if ((st & 0xF0) != 0xB0) return;               /* only Control Change drives params */
    int ch_set = params_get_int(P_MIDI_CH);        /* 0 = omni, 1..16 */
    if (ch_set != 0 && (st & 0x0F) != (uint8_t)(ch_set - 1)) return;
    param_id_t id = params_param_of_cc(d[0]);
    if (id == P_COUNT) return;
    params_set_norm(id, (float)d[1] / 127.f, SRC_BLE);
}

static uint8_t data_len(uint8_t st)
{
    switch (st & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    default: break;
    }
    switch (st) {
    case 0xF1: case 0xF3: return 1;       /* MTC quarter frame, song select */
    case 0xF2: return 2;                  /* song position */
    default:   return 0;                  /* F6 tune request, etc. */
    }
}

/*
 * BLE-MIDI packet: [header 10tttttt] then messages, each preceded by a timestamp byte
 * [1ttttttt] unless it uses running status without a new timestamp. Rules handled:
 *  - a byte with bit 7 set right after a non-timestamp position is a timestamp;
 *  - after a timestamp, bit 7 set = status byte, bit 7 clear = running-status data;
 *  - several messages per packet; running status persists across messages and packets;
 *  - system real-time (F8..FF) may appear anywhere (after a timestamp) and keeps running status;
 *  - SysEx (F0 .. [ts] F7) may span packets: continuation packets are header + data bytes.
 */
static void midi_parse(const uint8_t *p, size_t n)
{
    if (n < 2 || (p[0] & 0xC0) != 0x80) return;    /* header: bit7 = 1, bit6 = 0 */
    bool after_ts = false;
    for (size_t i = 1; i < n; i++) {
        uint8_t b = p[i];
        if (b & 0x80) {
            if (!after_ts) { after_ts = true; continue; }     /* timestamp low byte */
            after_ts = false;
            if (b >= 0xF8) continue;                          /* real-time: ignore */
            if (b == 0xF0) { s_rx_sysex = true; s_rx_status = 0; continue; }
            if (b == 0xF7) { s_rx_sysex = false; s_rx_status = 0; continue; }
            s_rx_sysex = false;
            s_rx_status = b;
            s_rx_need = data_len(b);
            s_rx_have = 0;
            if (s_rx_need == 0) s_rx_status = 0;              /* complete, no running status */
            continue;
        }
        after_ts = false;
        if (s_rx_sysex || !s_rx_status) continue;            /* sysex payload / stray data */
        s_rx_data[s_rx_have++] = b;
        if (s_rx_have >= s_rx_need) {
            midi_dispatch(s_rx_status, s_rx_data);
            s_rx_have = 0;
            if (s_rx_status >= 0xF0) s_rx_status = 0;         /* system common: no running status */
        }
    }
}

/* ------------------------------------------------------------------ GATT */
static int midi_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return 0;                                  /* BLE-MIDI: read returns no payload */
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t buf[256];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0 && len == 0) return BLE_ATT_ERR_UNLIKELY;
        midi_parse(buf, len);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_chr_uuid.u,
                .access_cb = midi_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------------------ GAP */
static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_state = BLE_CONNECTED;
            s_rx_status = 0;
            s_rx_sysex = false;
            ESP_LOGI(TAG, "connected");
            /* BLE-MIDI recommends a 7.5-15 ms connection interval for low latency. */
            struct ble_gap_upd_params up = {
                .itvl_min = 6, .itvl_max = 12,          /* 1.25 ms units */
                .latency = 0, .supervision_timeout = 400,  /* 4 s */
                .min_ce_len = 0, .max_ce_len = 0,
            };
            ble_gap_update_params(s_conn, &up);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify = false;
        s_mtu = 23;
        if (s_on) advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_on && s_conn == BLE_HS_CONN_HANDLE_NONE) advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_val_handle) s_notify = ev->subscribe.cur_notify;
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* No bond storage: forget the old pairing and accept the new one. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &s_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0) {
        struct ble_hs_adv_fields r = { 0 };
        r.name = (const uint8_t *)BLE_MIDI_NAME;
        r.name_len = sizeof(BLE_MIDI_NAME) - 1;
        r.name_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&r);
    }
    if (rc == 0) {
        struct ble_gap_adv_params ap = { 0 };
        ap.conn_mode = BLE_GAP_CONN_MODE_UND;
        ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
        ap.itvl_min = BLE_GAP_ADV_ITVL_MS(100);     /* relaxed interval: less airtime, still quick to find */
        ap.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
        s_state = BLE_ERROR;
        return;
    }
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) s_state = BLE_ADVERTISING;
}

static void on_sync(void)
{
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, BLE_MIDI_TX_POWER);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, BLE_MIDI_TX_POWER);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, BLE_MIDI_TX_POWER);
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();   /* deletes this task */
}

/* ------------------------------------------------------------------ enable / disable */
static esp_err_t start(void)
{
    s_heap_int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_heap_ext0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        return e;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   /* "just works" if the central wants to pair */
    ble_hs_cfg.sm_bonding = 0;                          /* nothing to persist */
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(BLE_MIDI_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_notify = false;
    s_mtu = 23;
    s_rx_status = 0;
    s_rx_sysex = false;
    memset(s_pend, 0, sizeof s_pend);
    s_on = true;
    s_state = BLE_ADVERTISING;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "on, advertising as \"%s\"", BLE_MIDI_NAME);
    return ESP_OK;
}

static void stop(void)
{
    s_on = false;
    int rc = nimble_port_stop();     /* terminates the link, stops adv, ends the host task loop */
    if (rc != 0) ESP_LOGW(TAG, "nimble_port_stop: %d", rc);
    nimble_port_deinit();            /* host deinit + controller disable/deinit (memory kept) */
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_notify = false;
    s_state = BLE_OFF;
    size_t i1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), x1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "off. heap internal %u -> %u (%+d), psram %u -> %u (%+d)",
             (unsigned)s_heap_int0, (unsigned)i1, (int)i1 - (int)s_heap_int0,
             (unsigned)s_heap_ext0, (unsigned)x1, (int)x1 - (int)s_heap_ext0);
}

esp_err_t ble_midi_enable(bool on)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);   /* first call: from one task */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (on && !s_on) {
        bootdiag_crumb("BLE START");
        e = start();
        bootdiag_crumb(e == ESP_OK ? "BLE ON" : NULL);
        if (e != ESP_OK) s_state = BLE_ERROR;
    } else if (!on && s_on) {
        bootdiag_crumb("BLE STOP");
        stop();
        bootdiag_crumb(NULL);
    } else if (!on) {
        s_state = BLE_OFF;
    }
    xSemaphoreGive(s_lock);
    return e;
}

ble_state_t ble_midi_state(void) { return s_state; }
const char *ble_midi_name(void)  { return BLE_MIDI_NAME; }

/* ------------------------------------------------------------------ MIDI out */
static void notify(const uint8_t *pkt, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, (uint16_t)len);
    if (!om) return;                                /* out of mbufs: drop (consumes nothing) */
    ble_gatts_notify_custom(s_conn, s_val_handle, om);   /* takes ownership of om */
}

static size_t pkt_begin(uint8_t *pkt, uint8_t *ts_low)
{
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000) & 0x1FFF;   /* 13-bit ms timestamp */
    pkt[0] = (uint8_t)(0x80 | ((ms >> 7) & 0x3F));
    *ts_low = (uint8_t)(0x80 | (ms & 0x7F));
    return 1;
}

void ble_midi_send_cc(uint8_t ch, uint8_t cc, uint8_t val)
{
    if (!s_lock || s_state != BLE_CONNECTED || !s_notify) return;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    if (s_on) {
        uint8_t pkt[5], ts;
        pkt_begin(pkt, &ts);
        pkt[1] = ts;
        pkt[2] = (uint8_t)(0xB0 | (ch & 0x0F));
        pkt[3] = cc & 0x7F;
        pkt[4] = val & 0x7F;
        notify(pkt, sizeof pkt);
    }
    xSemaphoreGive(s_lock);
}

void ble_midi_poll(void)
{
    uint32_t mask[PARAMS_MASK_WORDS];
    bool any = params_take_dirty(PL_BLE, mask);      /* always drain, even when off */
    if (!s_lock || s_state != BLE_CONNECTED || !s_notify) {
        memset(s_pend, 0, sizeof s_pend);
        return;
    }
    if (any) for (int i = 0; i < PARAMS_MASK_WORDS; i++) s_pend[i] |= mask[i];
    int64_t now = esp_timer_get_time();
    if (now - s_last_tx < BLE_MIDI_TX_PERIOD_US) return;
    bool pending = false;
    for (int i = 0; i < PARAMS_MASK_WORDS; i++) pending |= s_pend[i] != 0;
    if (!pending) return;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    if (!s_on) { xSemaphoreGive(s_lock); return; }

    s_last_tx = now;
    int chs = params_get_int(P_MIDI_CH);
    uint8_t status = (uint8_t)(0xB0 | (chs > 0 ? chs - 1 : 0));
    size_t cap = (size_t)(s_mtu > 3 ? s_mtu - 3 : 20);
    if (cap > BLE_MIDI_PKT_MAX) cap = BLE_MIDI_PKT_MAX;
    uint8_t pkt[BLE_MIDI_PKT_MAX], ts;
    size_t len = pkt_begin(pkt, &ts);
    for (int i = 0; i < P_COUNT; i++) {
        if (!params_mask_test(s_pend, (param_id_t)i)) continue;
        if (params_last_src((param_id_t)i) == SRC_BLE) continue;        /* no echo */
        uint8_t cc = params_cc_of((param_id_t)i);
        if (cc > 127) continue;
        long v = lrintf(params_to_norm((param_id_t)i, params_get((param_id_t)i)) * 127.f);
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        if (len + 4 > cap) {
            notify(pkt, len);
            len = pkt_begin(pkt, &ts);
        }
        pkt[len++] = ts;
        pkt[len++] = status;
        pkt[len++] = cc;
        pkt[len++] = (uint8_t)v;
    }
    if (len > 1) notify(pkt, len);
    memset(s_pend, 0, sizeof s_pend);
    xSemaphoreGive(s_lock);
}
