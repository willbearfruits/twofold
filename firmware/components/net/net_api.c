/*
 * net_api.c: REST API (see API.md). Every handler validates its inputs (body size, JSON types,
 * string lengths, ranges, slot numbers) before touching params/storage. Parameter writes use
 * params_set(..., SRC_WEB); the WebSocket push (net_ws.c) reports the resulting changes.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "dsp_engine.h"
#include "params.h"
#include "storage.h"
#include "ui_sys.h"
#include "net_priv.h"

static const char *TAG = "net_api";

#define BODY_MAX_SMALL   1024
#define BODY_MAX_PARAMS  4096
#define BODY_MAX_IMPORT  8192
#define KEY_MAX          32

/* ------------------------------------------------------------------ small JSON input helpers */
static bool j_num(const cJSON *o, const char *k, double *out)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(i)) { *out = i->valuedouble; return isfinite(*out); }
    if (cJSON_IsBool(i))   { *out = cJSON_IsTrue(i) ? 1.0 : 0.0; return true; }
    return false;
}

static bool j_int(const cJSON *o, const char *k, int lo, int hi, int *out)
{
    double v;
    if (!j_num(o, k, &v) || v != floor(v) || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static const char *j_str(const cJSON *o, const char *k, size_t maxlen)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(i) || !i->valuestring || strlen(i->valuestring) > maxlen) return NULL;
    return i->valuestring;
}

/* Reads + parses a JSON object body. On failure an error response has been sent. */
static cJSON *read_json(httpd_req_t *req, size_t max)
{
    int st = 0;
    char *b = net_read_body(req, max, &st);
    if (!b) { net_send_err(req, st, st == 413 ? "body too large" : "bad body"); return NULL; }
    cJSON *j = cJSON_ParseWithLength(b, req->content_len);
    free(b);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        net_send_err(req, 400, "invalid JSON");
        return NULL;
    }
    return j;
}

/* Preset/OLED names: the 5x7 OLED font has caps, digits and a few symbols. */
static bool clean_name(const char *in, char out[PRESET_NAME_LEN])
{
    int n = 0;
    if (!in) return false;
    while (*in == ' ') in++;
    for (const unsigned char *p = (const unsigned char *)in; *p && n < PRESET_NAME_LEN - 1; p++) {
        unsigned c = *p;
        if (c >= 128) continue;
        c = (unsigned)toupper((int)c);
        if (isalnum((int)c) || strchr(" -_.+/#&()!?:'", (int)c)) out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    return n > 0;
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case PT_BOOL: return "bool";
    case PT_ENUM: return "enum";
    case PT_INT:  return "int";
    default:      return "float";
    }
}

static const char *flag_name(uint8_t f)
{
    if (f & PF_PRESET)  return "preset";
    if (f & PF_SETTING) return "setting";
    return "volatile";
}

static double lin_db(float v) { return v > 1e-5f ? 20.0 * log10((double)v) : -100.0; }

/* ------------------------------------------------------------------ shared JSON blocks */
static const char *ble_state_name(ble_state_t s)
{
    switch (s) {
    case BLE_OFF:         return "off";
    case BLE_ADVERTISING: return "advertising";
    case BLE_CONNECTED:   return "connected";
    default:              return "error";
    }
}

void net_status_json(jw_t *w)
{
    sys_status_t *st = calloc(1, sizeof *st);
    if (!st) { w->err = true; return; }
    sys_get_status(st);
    net_fill_status(st);              /* our own fields are authoritative */
    dsp_prof_t prof;
    dsp_engine_get_prof(&prof);
    jw_bool(w, "usb", st->usb_mounted);
    jw_bool(w, "play", st->play_active);
    jw_bool(w, "rec", st->rec_active);
    jw_int(w, "preset", st->preset_active);
    jw_bool(w, "preset_dirty", st->preset_dirty);
    jw_bool(w, "pending", st->storage_pending);
    jw_str(w, "wifi", net_wifi_state_name(st->wifi));
    jw_str(w, "ssid", st->wifi_ssid);
    jw_str(w, "ip", st->ip);
    jw_int(w, "rssi", st->wifi_rssi);
    jw_str(w, "ble", ble_state_name(st->ble));
    jw_bool(w, "ota", st->ota_in_progress);
    jw_int(w, "ota_pct", st->ota_pct);
    jw_fix(w, "cpu", prof.total_avg_pct, 1);
    jw_fix(w, "cpu_max", prof.total_max_pct, 1);
    free(st);
}

void net_meters_json(jw_t *w)
{
    dsp_meters_t m;
    dsp_engine_get_meters(&m, false);   /* read-only: the UI task owns "take" */
    jw_arr(w, "in");   jw_fix(w, NULL, lin_db(m.in_peak[0]), 1);   jw_fix(w, NULL, lin_db(m.in_peak[1]), 1);   jw_arr_end(w);
    jw_arr(w, "out");  jw_fix(w, NULL, lin_db(m.out_peak[0]), 1);  jw_fix(w, NULL, lin_db(m.out_peak[1]), 1);  jw_arr_end(w);
    jw_arr(w, "play"); jw_fix(w, NULL, lin_db(m.play_peak[0]), 1); jw_fix(w, NULL, lin_db(m.play_peak[1]), 1); jw_arr_end(w);
    jw_arr(w, "clip"); jw_int(w, NULL, (long)m.in_clip[0]);        jw_int(w, NULL, (long)m.in_clip[1]);        jw_arr_end(w);
    jw_arr(w, "over"); jw_int(w, NULL, (long)m.out_over[0]);       jw_int(w, NULL, (long)m.out_over[1]);       jw_arr_end(w);
    jw_fix(w, "gr", m.comp_gr_db, 1);
    jw_fix(w, "lim", m.lim_gr_db, 1);
    dsp_prof_t p;
    dsp_engine_get_prof(&p);
    jw_fix(w, "cpu", p.total_avg_pct, 1);
    jw_fix(w, "cpu_max", p.total_max_pct, 1);
}

static void ccmap_json(jw_t *w)
{
    jw_int(w, "ch", params_get_int(P_MIDI_CH));
    jw_obj(w, "map");
    for (int i = 0; i < P_COUNT; i++) {
        uint8_t cc = params_cc_of((param_id_t)i);
        if (cc > 127) jw_null(w, g_param_desc[i].key); else jw_int(w, g_param_desc[i].key, cc);
    }
    jw_obj_end(w);
}

/* Streams a JSON object built by fn() as the response body. */
static esp_err_t send_obj(httpd_req_t *req, void (*fn)(jw_t *, void *), void *ctx)
{
    char *b = malloc(1024);
    if (!b) return net_send_err(req, 500, "no memory");
    jw_t w;
    jw_init(&w, b, 1024, req);
    net_json_headers(req);
    jw_obj(&w, NULL);
    fn(&w, ctx);
    jw_obj_end(&w);
    esp_err_t e = jw_finish(&w);
    free(b);
    return e;
}

/* ------------------------------------------------------------------ GET /api/params */
static void params_body(jw_t *w, void *ctx)
{
    jw_int(w, "fs", 48000);
    jw_int(w, "count", P_COUNT);
    jw_arr(w, "params");
    for (int i = 0; i < P_COUNT; i++) {
        const param_desc_t *d = &g_param_desc[i];
        jw_obj(w, NULL);
        jw_str(w, "key", d->key);
        jw_str(w, "name", d->name);
        jw_str(w, "unit", d->unit);
        jw_str(w, "type", type_name(d->type));
        jw_str(w, "curve", d->curve == PC_LOG ? "log" : "lin");
        jw_num(w, "min", d->min, 7);
        jw_num(w, "max", d->max, 7);
        jw_num(w, "def", d->def, 7);
        jw_num(w, "step", d->step, 7);
        jw_str(w, "flags", flag_name(d->flags));
        if (d->midi_cc > 127) jw_null(w, "cc"); else jw_int(w, "cc", d->midi_cc);
        if (d->type == PT_ENUM && d->enum_names) {
            jw_arr(w, "enum");
            for (int k = 0; k < d->enum_count; k++) jw_str(w, NULL, d->enum_names[k]);
            jw_arr_end(w);
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
}
static esp_err_t h_params(httpd_req_t *req) { return send_obj(req, params_body, NULL); }

/* ------------------------------------------------------------------ GET /api/state */
static void state_body(jw_t *w, void *ctx)
{
    jw_int(w, "seq", (long)params_seq());
    jw_int(w, "epoch", (long)params_preset_epoch());
    jw_obj(w, "values");
    for (int i = 0; i < P_COUNT; i++) jw_num(w, g_param_desc[i].key, params_get((param_id_t)i), 7);
    jw_obj_end(w);
    jw_obj(w, "status");
    net_status_json(w);
    jw_obj_end(w);
}
static esp_err_t h_state(httpd_req_t *req) { return send_obj(req, state_body, NULL); }

static void meters_body(jw_t *w, void *ctx) { net_meters_json(w); }
static esp_err_t h_meters(httpd_req_t *req) { return send_obj(req, meters_body, NULL); }

/* ------------------------------------------------------------------ POST /api/param */
int net_api_set_param(const char *key, double v, float *applied)
{
    if (!key || strlen(key) > KEY_MAX) return 400;
    param_id_t id = params_find(key);
    if (id == P_COUNT) return 404;
    if (!isfinite(v) || fabs(v) > 1e9) return 400;
    /* Radio switches go through the firmware's command path (it owns the radio state). */
    if (id == P_NET_WIFI) {
        if (v < 0.5) sys_cmd(SYS_CMD_WIFI_ENABLE, 0);     /* async teardown; response leaves first */
        if (applied) *applied = v < 0.5 ? 0.f : 1.f;
        return 0;
    }
    if (id == P_NET_BLE) {
        sys_cmd(SYS_CMD_BLE_ENABLE, v >= 0.5 ? 1 : 0);
        if (applied) *applied = v >= 0.5 ? 1.f : 0.f;
        return 0;
    }
    params_set(id, (float)v, SRC_WEB);
    if (applied) *applied = params_get(id);
    return 0;
}

/* {"key","value"} or {"values":{key:value,...}} -> {"ok":true,"values":{key:applied}}; 0 or an HTTP status. */
static int param_apply(cJSON *j, jw_t *w, const char **err)
{
    jw_obj(w, NULL);
    jw_bool(w, "ok", true);
    jw_obj(w, "values");
    int status = 0;
    const cJSON *vals = cJSON_GetObjectItemCaseSensitive(j, "values");
    if (cJSON_IsObject(vals)) {
        int n = 0;
        for (const cJSON *it = vals->child; it && !status; it = it->next) {
            if (++n > 64) { status = 400; break; }
            double v;
            if (cJSON_IsNumber(it)) v = it->valuedouble;
            else if (cJSON_IsBool(it)) v = cJSON_IsTrue(it);
            else { status = 400; break; }
            float a;
            status = net_api_set_param(it->string, v, &a);
            if (!status) jw_num(w, it->string, a, 7);
        }
    } else {
        const char *key = j_str(j, "key", KEY_MAX);
        double v;
        if (!key || !j_num(j, "value", &v)) status = 400;
        else {
            float a;
            status = net_api_set_param(key, v, &a);
            if (!status) jw_num(w, key, a, 7);
        }
    }
    jw_obj_end(w);
    jw_obj_end(w);
    if (status) *err = status == 404 ? "unknown parameter" : "bad parameter/value";
    return status;
}

/* HTTP glue for the *_apply functions: body -> JSON -> apply into a buffer -> send. */
typedef int (*apply_fn_t)(cJSON *j, jw_t *w, const char **err);
static esp_err_t run_apply(httpd_req_t *req, size_t max, apply_fn_t fn)
{
    cJSON *j = read_json(req, max);
    if (!j) return ESP_OK;
    char *b = malloc(2048);
    if (!b) { cJSON_Delete(j); return net_send_err(req, 500, "no memory"); }
    jw_t w;
    jw_init(&w, b, 2048, NULL);
    const char *err = "error";
    int status = fn(j, &w, &err);
    cJSON_Delete(j);
    esp_err_t e;
    if (status) e = net_send_err(req, status, err);
    else if (jw_finish(&w) != ESP_OK) e = net_send_ok(req);
    else { net_json_headers(req); e = httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN); }
    free(b);
    return e;
}
static esp_err_t h_param(httpd_req_t *req) { return run_apply(req, BODY_MAX_PARAMS, param_apply); }

/* ------------------------------------------------------------------ presets */
/* Mirror of the params blob layout (params.c: "TFP1" v1, header + n * {u32 key_hash, f32 value}).
 * Used only for JSON export/import; blob_layout_ok() guards against a silent format change. */
#define PBLOB_MAGIC   0x31504654u
#define PBLOB_VERSION 1
typedef struct __attribute__((packed)) { uint32_t magic; uint16_t version, count; char name[PRESET_NAME_LEN]; } pblob_hdr_t;
typedef struct __attribute__((packed)) { uint32_t hash; float value; } pblob_kv_t;

static int preset_param_count(void)
{
    int n = 0;
    for (int i = 0; i < P_COUNT; i++) n += (g_param_desc[i].flags & PF_PRESET) ? 1 : 0;
    return n;
}

static bool blob_layout_ok(void)
{
    return params_blob_max(PF_PRESET) == sizeof(pblob_hdr_t) + (size_t)preset_param_count() * sizeof(pblob_kv_t);
}

static void presets_body(jw_t *w, void *ctx)
{
    jw_int(w, "active", storage_preset_active());
    jw_bool(w, "dirty", storage_preset_dirty());
    jw_bool(w, "pending", storage_pending());
    jw_arr(w, "slots");
    for (int s = 0; s < PRESET_SLOTS; s++) {
        char name[PRESET_NAME_LEN + 1] = { 0 };
        storage_preset_name(s, name);
        name[PRESET_NAME_LEN] = 0;
        jw_obj(w, NULL);
        jw_int(w, "slot", s);
        jw_str(w, "name", name);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}
static esp_err_t h_presets(httpd_req_t *req) { return send_obj(req, presets_body, NULL); }

static int preset_op_apply(const char *op, cJSON *j, jw_t *w, const char **err)
{
    int slot;
    char name[PRESET_NAME_LEN];
    const char *nm = j_str(j, "name", 64);
    bool has_name = nm && clean_name(nm, name);
    bool ok = false;
    *err = "bad request";
    if (!j_int(j, "slot", 0, PRESET_SLOTS - 1, &slot)) {
        *err = "slot must be 0..7";
    } else if (!strcmp(op, "load")) {
        ok = storage_preset_load(slot);
        *err = "load failed";
    } else if (!strcmp(op, "save")) {
        ok = storage_preset_save(slot, has_name ? name : NULL);   /* soft-muted write in storage */
        *err = "save failed";
    } else if (!strcmp(op, "rename")) {
        if (!has_name) *err = "invalid name";
        else { ok = storage_preset_rename(slot, name); *err = "rename failed"; }
    }
    if (!ok) return 400;
    jw_obj(w, NULL); presets_body(w, NULL); jw_obj_end(w);
    return 0;
}
static int preset_load_apply(cJSON *j, jw_t *w, const char **e)   { return preset_op_apply("load", j, w, e); }
static int preset_save_apply(cJSON *j, jw_t *w, const char **e)   { return preset_op_apply("save", j, w, e); }
static int preset_rename_apply(cJSON *j, jw_t *w, const char **e) { return preset_op_apply("rename", j, w, e); }
static esp_err_t h_preset_op(httpd_req_t *req)
{
    const char *op = strrchr(req->uri, '/');
    op = op ? op + 1 : "";
    return run_apply(req, BODY_MAX_SMALL, !strcmp(op, "load") ? preset_load_apply :
                     !strcmp(op, "save") ? preset_save_apply : preset_rename_apply);
}

static int preset_export_body(int slot, jw_t *w, const char **err)
{
    if (slot < 0 || slot >= PRESET_SLOTS) { *err = "slot must be 0..7"; return 400; }
    if (!blob_layout_ok()) { *err = "preset format changed; update net_api.c"; return 500; }
    size_t cap = params_blob_max(PF_PRESET);
    uint8_t *blob = malloc(cap);
    if (!blob) { *err = "no memory"; return 500; }
    size_t n = storage_preset_export(slot, blob, cap);
    pblob_hdr_t h;
    if (n < sizeof h) { free(blob); *err = "slot empty"; return 404; }
    memcpy(&h, blob, sizeof h);
    if (h.magic != PBLOB_MAGIC || h.version != PBLOB_VERSION || n < sizeof h + (size_t)h.count * sizeof(pblob_kv_t)) {
        free(blob);
        *err = "unrecognised preset blob";
        return 500;
    }
    char name[PRESET_NAME_LEN + 1];
    memcpy(name, h.name, PRESET_NAME_LEN);
    name[PRESET_NAME_LEN] = 0;
    jw_obj(w, NULL);
    jw_str(w, "format", "twofold-preset");
    jw_int(w, "version", 1);
    jw_str(w, "name", name);
    jw_int(w, "slot", slot);
    jw_str(w, "fw", esp_app_get_description()->version);
    jw_obj(w, "params");
    for (int k = 0; k < h.count; k++) {
        pblob_kv_t kv;
        memcpy(&kv, blob + sizeof h + (size_t)k * sizeof kv, sizeof kv);
        for (int i = 0; i < P_COUNT; i++) {
            if ((g_param_desc[i].flags & PF_PRESET) && params_key_hash(g_param_desc[i].key) == kv.hash) {
                jw_num(w, g_param_desc[i].key, kv.value, 7);
                break;
            }
        }
    }
    jw_obj_end(w);
    jw_obj_end(w);
    free(blob);
    return 0;
}

static int query_slot(const char *q)
{
    char v[8];
    if (!q || httpd_query_key_value(q, "slot", v, sizeof v) != ESP_OK) return -1;
    char *end;
    long s = strtol(v, &end, 10);
    return (*end == 0 && s >= 0 && s < PRESET_SLOTS) ? (int)s : -1;
}

static esp_err_t h_preset_export(httpd_req_t *req)
{
    char q[32] = "";
    httpd_req_get_url_query_str(req, q, sizeof q);
    char *b = malloc(1024);
    if (!b) return net_send_err(req, 500, "no memory");
    jw_t w;
    jw_init(&w, b, 1024, req);
    net_json_headers(req);
    const char *err = "error";
    int status = preset_export_body(query_slot(q), &w, &err);
    esp_err_t e = status ? net_send_err(req, status, err) : jw_finish(&w);
    free(b);
    return e;
}

static int preset_import_apply(cJSON *j, jw_t *w, const char **err)
{
    if (!blob_layout_ok()) { *err = "preset format changed; update net_api.c"; return 500; }
    int slot;
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(j, "params");
    const char *fmt = j_str(j, "format", 32);
    if (!j_int(j, "slot", 0, PRESET_SLOTS - 1, &slot) || !cJSON_IsObject(pv) || (fmt && strcmp(fmt, "twofold-preset"))) {
        *err = "expected {slot, params:{...}} (format twofold-preset)";
        return 400;
    }
    pblob_hdr_t h = { .magic = PBLOB_MAGIC, .version = PBLOB_VERSION, .count = 0 };
    memset(h.name, 0, sizeof h.name);
    if (!clean_name(j_str(j, "name", 64), h.name)) {
        memcpy(h.name, "IMPORT ", 7);
        h.name[7] = (char)('1' + slot);
    }
    size_t cap = params_blob_max(PF_PRESET);
    uint8_t *blob = malloc(cap);
    if (!blob) { *err = "no memory"; return 500; }
    int used = 0, known = 0;
    size_t off = sizeof h;
    for (int i = 0; i < P_COUNT; i++) {
        const param_desc_t *d = &g_param_desc[i];
        if (!(d->flags & PF_PRESET)) continue;
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(pv, d->key);
        double v = d->def;                         /* missing keys -> defaults: a complete preset */
        if (cJSON_IsNumber(it) && isfinite(it->valuedouble)) { v = it->valuedouble; used++; }
        else if (cJSON_IsBool(it)) { v = cJSON_IsTrue(it); used++; }
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        pblob_kv_t kv = { .hash = params_key_hash(d->key), .value = (float)v };
        memcpy(blob + off, &kv, sizeof kv);
        off += sizeof kv;
        h.count++;
    }
    for (const cJSON *it = pv->child; it; it = it->next) known++;
    memcpy(blob, &h, sizeof h);
    bool ok = storage_preset_import(slot, blob, off);
    free(blob);
    if (!ok) { *err = "import failed"; return 500; }
    ESP_LOGI(TAG, "preset import slot %d: %d of %d keys used", slot, used, known);
    jw_obj(w, NULL);
    jw_bool(w, "ok", true); jw_int(w, "slot", slot); jw_int(w, "used", used); jw_int(w, "ignored", known - used);
    jw_obj_end(w);
    return 0;
}
static esp_err_t h_preset_import(httpd_req_t *req) { return run_apply(req, BODY_MAX_IMPORT, preset_import_apply); }

/* ------------------------------------------------------------------ MIDI CC map */
static void ccmap_body(jw_t *w, void *ctx) { ccmap_json(w); }
static esp_err_t h_ccmap_get(httpd_req_t *req) { return send_obj(req, ccmap_body, NULL); }

static int ccmap_set_apply(cJSON *j, jw_t *w, const char **err)
{
    const char *key = j_str(j, "key", KEY_MAX);
    const cJSON *ccj = cJSON_GetObjectItemCaseSensitive(j, "cc");
    param_id_t id = key ? params_find(key) : P_COUNT;
    int cc = -2;
    if (cJSON_IsNull(ccj)) cc = -1;
    else j_int(j, "cc", -1, 127, &cc);
    if (id == P_COUNT) { *err = "unknown parameter"; return 404; }
    if (cc < -1) { *err = "cc must be 0..127 or null"; return 400; }
    params_cc_assign(id, cc < 0 ? NO_CC : (uint8_t)cc);
    jw_obj(w, NULL); ccmap_body(w, NULL); jw_obj_end(w);
    return 0;
}
static esp_err_t h_ccmap_set(httpd_req_t *req) { return run_apply(req, BODY_MAX_SMALL, ccmap_set_apply); }

static esp_err_t h_ccmap_reset(httpd_req_t *req)
{
    params_cc_reset();
    return send_obj(req, ccmap_body, NULL);
}

static esp_err_t h_ccmap_save(httpd_req_t *req)
{
    storage_flush_now();                        /* explicit user action: soft-muted commit */
    return net_send_ok(req);
}

/* ------------------------------------------------------------------ WiFi setup */
static void wifi_body(jw_t *w, void *ctx) { net_wifi_json(w); }
static esp_err_t h_wifi_get(httpd_req_t *req) { return send_obj(req, wifi_body, NULL); }

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    if (net_wifi_request_scan() != ESP_OK) return net_send_err(req, 503, "WiFi busy");
    return net_send_ok(req);
}

static int wifi_connect_apply(cJSON *j, jw_t *w, const char **err)
{
    const char *ssid = j_str(j, "ssid", 32);
    const char *pass = cJSON_GetObjectItemCaseSensitive(j, "pass") ? j_str(j, "pass", 64) : "";
    esp_err_t e = (ssid && pass) ? net_wifi_request_connect(ssid, pass) : ESP_ERR_INVALID_ARG;
    if (e == ESP_ERR_INVALID_ARG) { *err = "SSID 1-32 bytes, password empty or 8-64 characters"; return 400; }
    if (e != ESP_OK) { *err = "busy"; return 503; }
    jw_obj(w, NULL); jw_bool(w, "ok", true); jw_obj_end(w);
    return 0;
}
static esp_err_t h_wifi_connect(httpd_req_t *req) { return run_apply(req, BODY_MAX_SMALL, wifi_connect_apply); }

static esp_err_t h_wifi_forget(httpd_req_t *req)
{
    net_wifi_forget();
    return net_send_ok(req);
}

/* ------------------------------------------------------------------ commands */
typedef enum { AFTER_NONE, AFTER_WIFI_OFF, AFTER_REBOOT, AFTER_FACTORY, AFTER_WIFI_SETUP } after_t;

static int cmd_apply(cJSON *j, jw_t *w, const char **err, after_t *after)
{
    const char *c = j_str(j, "cmd", 24);
    int arg = 0;
    j_int(j, "arg", -1000000, 1000000, &arg);
    char cmd[25];
    snprintf(cmd, sizeof cmd, "%s", c ? c : "");
    *after = AFTER_NONE;
    if (!strcmp(cmd, "studio") || !strcmp(cmd, "wifi_off")) *after = AFTER_WIFI_OFF;
    else if (!strcmp(cmd, "reboot")) *after = AFTER_REBOOT;
    else if (!strcmp(cmd, "factory_reset")) *after = AFTER_FACTORY;
    else if (!strcmp(cmd, "wifi_setup")) *after = AFTER_WIFI_SETUP;
    else if (!strcmp(cmd, "wifi_on")) sys_cmd(SYS_CMD_WIFI_ENABLE, 1);
    else if (!strcmp(cmd, "ble")) sys_cmd(SYS_CMD_BLE_ENABLE, arg ? 1 : 0);
    else if (!strcmp(cmd, "flush")) storage_flush_now();
    else { *err = "unknown cmd"; return 400; }
    jw_obj(w, NULL); jw_bool(w, "ok", true); jw_obj_end(w);
    return 0;
}

static void cmd_after(after_t a)
{
    switch (a) {
    case AFTER_WIFI_OFF:   sys_cmd(SYS_CMD_WIFI_ENABLE, 0); break;   /* teardown is async and waits for the handler */
    case AFTER_REBOOT:     sys_cmd(SYS_CMD_REBOOT, 0); break;
    case AFTER_FACTORY:    sys_cmd(SYS_CMD_FACTORY_RESET, 0); break;
    case AFTER_WIFI_SETUP: net_wifi_setup(); break;
    default: break;
    }
}

static esp_err_t h_cmd(httpd_req_t *req)
{
    cJSON *j = read_json(req, BODY_MAX_SMALL);
    if (!j) return ESP_OK;
    char b[128];
    jw_t w;
    jw_init(&w, b, sizeof b, NULL);
    const char *err = "error";
    after_t after;
    int status = cmd_apply(j, &w, &err, &after);
    cJSON_Delete(j);
    if (status) return net_send_err(req, status, err);
    net_send_ok(req);                            /* respond first: some commands take the network down */
    cmd_after(after);
    return ESP_OK;
}

/* ------------------------------------------------------------------ GET /api/info */
static void info_body(jw_t *w, void *ctx)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char sha[17];
    for (int i = 0; i < 8; i++) snprintf(sha + 2 * i, 3, "%02x", d->app_elf_sha256[i]);
    jw_str(w, "device", "TWOFOLD");
    jw_str(w, "fw", d->version);
    jw_str(w, "project", d->project_name);
    jw_str(w, "idf", d->idf_ver);
    jw_str(w, "built", d->date);
    jw_str(w, "built_time", d->time);
    jw_str(w, "sha", sha);

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    jw_obj(w, "ota");
    jw_bool(w, "supported", next != NULL);
    jw_str(w, "running", run ? run->label : "");
    jw_str(w, "next", next ? next->label : "");
    jw_int(w, "max_size", next ? (long)next->size : 0);
    jw_bool(w, "active", net_ota_active());
    jw_int(w, "pct", net_ota_pct());
    jw_obj_end(w);

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ms[18];
    snprintf(ms, sizeof ms, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    jw_str(w, "mac", ms);
    jw_int(w, "chip_rev", ci.revision);
    jw_int(w, "uptime", (long)(esp_timer_get_time() / 1000000));

    jw_obj(w, "heap");
    jw_int(w, "int_free", (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    jw_int(w, "int_min", (long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    jw_int(w, "int_largest", (long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    jw_int(w, "psram_free", (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    jw_obj_end(w);

    jw_obj(w, "wifi");
    net_wifi_json(w);
    jw_obj_end(w);

    sys_diag_t *dg = calloc(1, sizeof *dg);
    if (dg) {
        sys_get_diag(dg);
        jw_obj(w, "audio");
        jw_int(w, "rate", 48000);
        jw_fix(w, "measured_fs", dg->measured_fs, 2);
        jw_fix(w, "usb_feedback", dg->usb_feedback, 4);
        jw_int(w, "xrun_play", (long)dg->xrun_play);
        jw_int(w, "xrun_rec", (long)dg->xrun_rec);
        jw_int(w, "i2s_errors", (long)dg->i2s_errors);
        jw_obj_end(w);
        jw_obj(w, "cpu");
        jw_fix(w, "avg", dg->prof.total_avg_pct, 1);
        jw_fix(w, "max", dg->prof.total_max_pct, 1);
        jw_arr(w, "stages");
        for (int i = 0; i < DSP_ST_COUNT; i++) {
            jw_obj(w, NULL);
            jw_str(w, "name", dsp_stage_names[i]);
            jw_fix(w, "avg", dg->prof.avg_pct[i], 2);
            jw_fix(w, "max", dg->prof.max_pct[i], 2);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        jw_obj_end(w);
        free(dg);
    }
    jw_obj(w, "status");
    net_status_json(w);
    jw_obj_end(w);
}
static esp_err_t h_info(httpd_req_t *req) { return send_obj(req, info_body, NULL); }

/* ------------------------------------------------------------------ registration */
esp_err_t net_api_register(httpd_handle_t hd)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/params",         .method = HTTP_GET,  .handler = h_params },
        { .uri = "/api/state",          .method = HTTP_GET,  .handler = h_state },
        { .uri = "/api/meters",         .method = HTTP_GET,  .handler = h_meters },
        { .uri = "/api/info",           .method = HTTP_GET,  .handler = h_info },
        { .uri = "/api/param",          .method = HTTP_POST, .handler = h_param },
        { .uri = "/api/presets",        .method = HTTP_GET,  .handler = h_presets },
        { .uri = "/api/preset/load",    .method = HTTP_POST, .handler = h_preset_op },
        { .uri = "/api/preset/save",    .method = HTTP_POST, .handler = h_preset_op },
        { .uri = "/api/preset/rename",  .method = HTTP_POST, .handler = h_preset_op },
        { .uri = "/api/preset/export",  .method = HTTP_GET,  .handler = h_preset_export },
        { .uri = "/api/preset/import",  .method = HTTP_POST, .handler = h_preset_import },
        { .uri = "/api/ccmap",          .method = HTTP_GET,  .handler = h_ccmap_get },
        { .uri = "/api/ccmap",          .method = HTTP_POST, .handler = h_ccmap_set },
        { .uri = "/api/ccmap/reset",    .method = HTTP_POST, .handler = h_ccmap_reset },
        { .uri = "/api/ccmap/save",     .method = HTTP_POST, .handler = h_ccmap_save },
        { .uri = "/api/wifi",           .method = HTTP_GET,  .handler = h_wifi_get },
        { .uri = "/api/wifi/scan",      .method = HTTP_POST, .handler = h_wifi_scan },
        { .uri = "/api/wifi/connect",   .method = HTTP_POST, .handler = h_wifi_connect },
        { .uri = "/api/wifi/forget",    .method = HTTP_POST, .handler = h_wifi_forget },
        { .uri = "/api/cmd",            .method = HTTP_POST, .handler = h_cmd },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        esp_err_t e = httpd_register_uri_handler(hd, &uris[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "register %s: %s", uris[i].uri, esp_err_to_name(e));
    }
    return ESP_OK;
}

static after_t s_pending_after;
static void run_pending_after(void) { after_t a = s_pending_after; s_pending_after = AFTER_NONE; cmd_after(a); }

/* ------------------------------------------------------------------ transport-independent dispatcher (USB)
 * Same API as HTTP: method + path(+query) + JSON body -> status + JSON into a buffer-mode writer. */
typedef struct { const char *path; void (*body)(jw_t *, void *); } get_route_t;
static const get_route_t k_gets[] = {
    { "/api/params", params_body }, { "/api/state", state_body }, { "/api/meters", meters_body },
    { "/api/info", info_body }, { "/api/presets", presets_body }, { "/api/ccmap", ccmap_body }, { "/api/wifi", wifi_body },
};
typedef struct { const char *path; apply_fn_t fn; size_t max; } post_route_t;
static const post_route_t k_posts[] = {
    { "/api/param", param_apply, BODY_MAX_PARAMS }, { "/api/preset/load", preset_load_apply, BODY_MAX_SMALL },
    { "/api/preset/save", preset_save_apply, BODY_MAX_SMALL }, { "/api/preset/rename", preset_rename_apply, BODY_MAX_SMALL },
    { "/api/preset/import", preset_import_apply, BODY_MAX_IMPORT }, { "/api/ccmap", ccmap_set_apply, BODY_MAX_SMALL },
    { "/api/wifi/connect", wifi_connect_apply, BODY_MAX_SMALL },
};

int net_api_dispatch(const char *method, const char *path, const char *body, size_t body_len, jw_t *w, const char **err,
                     void (**after)(void))
{
    *after = NULL;
    char p[64];
    snprintf(p, sizeof p, "%s", path);
    char *q = strchr(p, '?');
    if (q) *q++ = 0;
    if (!strcmp(method, "GET")) {
        for (size_t i = 0; i < sizeof k_gets / sizeof k_gets[0]; i++)
            if (!strcmp(p, k_gets[i].path)) { jw_obj(w, NULL); k_gets[i].body(w, NULL); jw_obj_end(w); return 0; }
        if (!strcmp(p, "/api/preset/export")) return preset_export_body(query_slot(q), w, err);
        *err = "not found"; return 404;
    }
    if (strcmp(method, "POST")) { *err = "method"; return 405; }
    cJSON *j = cJSON_ParseWithLength(body, body_len);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); *err = "invalid JSON"; return 400; }
    int status = 404;
    *err = "not found";
    if (!strcmp(p, "/api/cmd")) {
        status = cmd_apply(j, w, err, &s_pending_after);
        if (!status && s_pending_after != AFTER_NONE) *after = run_pending_after;
    } else if (!strcmp(p, "/api/ccmap/reset")) { params_cc_reset(); jw_obj(w, NULL); ccmap_body(w, NULL); jw_obj_end(w); status = 0; }
    else if (!strcmp(p, "/api/ccmap/save")) { storage_flush_now(); jw_obj(w, NULL); jw_bool(w, "ok", true); jw_obj_end(w); status = 0; }
    else if (!strcmp(p, "/api/wifi/scan")) { status = net_wifi_request_scan() == ESP_OK ? 0 : 503; if (status) *err = "WiFi busy"; else { jw_obj(w, NULL); jw_bool(w, "ok", true); jw_obj_end(w); } }
    else if (!strcmp(p, "/api/wifi/forget")) { net_wifi_forget(); jw_obj(w, NULL); jw_bool(w, "ok", true); jw_obj_end(w); status = 0; }
    else for (size_t i = 0; i < sizeof k_posts / sizeof k_posts[0]; i++)
        if (!strcmp(p, k_posts[i].path)) { if (body_len > k_posts[i].max) { status = 413; *err = "body too large"; } else status = k_posts[i].fn(j, w, err); break; }
    cJSON_Delete(j);
    return status;
}
