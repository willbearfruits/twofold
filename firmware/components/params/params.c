/* params.c: see params.h for the threading model. */
#include "params.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- enum name tables ---- */
#define E_DEF(name, ...) static const char *const name[] = { __VA_ARGS__ };
PARAMS_ENUMS(E_DEF)
#undef E_DEF

#define X_DESC(id, key_, name_, unit_, type_, curve_, min_, max_, def_, step_, flags_, enums_, cc_) \
    [id] = { .key = key_, .name = name_, .unit = unit_, .type = type_, .curve = curve_, .flags = flags_,  \
             .midi_cc = cc_, .min = min_, .max = max_, .def = def_, .step = step_,                       \
             .enum_names = enums_,                                                  \
             .enum_count = (type_ == PT_ENUM ? (uint8_t)((max_) - (min_) + 1.f) : 0) },
const param_desc_t g_param_desc[P_COUNT] = { PARAMS_TABLE(X_DESC) };
#undef X_DESC

/* Table sanity check (run by host tests): enum ranges match name tables, keys unique, defaults in range. */
int params_selftest(void)
{
    int err = 0;
#define E_CHK(name, ...)                                                                   \
    for (int i = 0; i < P_COUNT; i++)                                                      \
        if (g_param_desc[i].enum_names == name &&                                          \
            g_param_desc[i].enum_count != sizeof(name) / sizeof(name[0])) err++;
    PARAMS_ENUMS(E_CHK)
#undef E_CHK
    for (int i = 0; i < P_COUNT; i++) {
        const param_desc_t *d = &g_param_desc[i];
        if (d->def < d->min || d->def > d->max) err++;
        if (d->type == PT_ENUM && !d->enum_names) err++;
        if (d->curve == PC_LOG && d->min <= 0.f) err++;
        for (int j = i + 1; j < P_COUNT; j++)
            if (!strcmp(d->key, g_param_desc[j].key) ||
                (d->midi_cc != NO_CC && d->midi_cc == g_param_desc[j].midi_cc)) err++;
    }
    return err;
}

static volatile float    s_val[P_COUNT];
static volatile uint8_t  s_src[P_COUNT];
static volatile uint32_t s_seq;
static volatile uint32_t s_epoch;
static volatile uint32_t s_dirty[PL_COUNT][PARAMS_MASK_WORDS];
static volatile uint8_t  s_cc_of[P_COUNT];

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void params_init(void)
{
    for (int i = 0; i < P_COUNT; i++) {
        s_val[i] = g_param_desc[i].def;
        s_src[i] = SRC_INIT;
    }
    params_cc_reset();
    for (int l = 0; l < PL_COUNT; l++)
        for (int w = 0; w < PARAMS_MASK_WORDS; w++) s_dirty[l][w] = 0;
    s_seq = 1;
}

float params_get(param_id_t id) { return s_val[id]; }
bool  params_get_bool(param_id_t id) { return s_val[id] >= 0.5f; }
int   params_get_int(param_id_t id) { return (int)lrintf(s_val[id]); }
param_src_t params_last_src(param_id_t id) { return (param_src_t)s_src[id]; }

static float quantize(const param_desc_t *d, float v)
{
    if (!isfinite(v)) v = d->def;
    v = clampf(v, d->min, d->max);
    if (d->type != PT_FLOAT) v = rintf(v);
    return v;
}

static void mark_dirty(param_id_t id)
{
    uint32_t bit = 1u << (id & 31);
    for (int l = 0; l < PL_COUNT; l++) __atomic_fetch_or(&s_dirty[l][id >> 5], bit, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_seq, 1, __ATOMIC_RELEASE);
}

bool params_set(param_id_t id, float v, param_src_t src)
{
    if ((unsigned)id >= P_COUNT) return false;
    v = quantize(&g_param_desc[id], v);
    if (s_val[id] == v) return false;
    s_val[id] = v;
    s_src[id] = (uint8_t)src;
    mark_dirty(id);
    return true;
}

float params_to_norm(param_id_t id, float v)
{
    const param_desc_t *d = &g_param_desc[id];
    v = clampf(v, d->min, d->max);
    if (d->curve == PC_LOG && d->min > 0.f) return logf(v / d->min) / logf(d->max / d->min);
    return (v - d->min) / (d->max - d->min);
}

float params_from_norm(param_id_t id, float n)
{
    const param_desc_t *d = &g_param_desc[id];
    n = clampf(n, 0.f, 1.f);
    if (d->curve == PC_LOG && d->min > 0.f) return d->min * powf(d->max / d->min, n);
    return d->min + n * (d->max - d->min);
}

bool params_set_norm(param_id_t id, float n, param_src_t src) { return params_set(id, params_from_norm(id, n), src); }

bool params_nudge(param_id_t id, int steps, param_src_t src)
{
    const param_desc_t *d = &g_param_desc[id];
    float v = s_val[id];
    if (d->curve == PC_LOG && d->min > 0.f) {
        v = v * powf(2.f, (float)steps / d->step);
    } else if (d->type == PT_FLOAT) {
        /* snap to the step grid so values stay tidy ("-12.5", not "-12.4999") */
        v = (rintf(v / d->step) + (float)steps) * d->step;
    } else {
        v = v + (float)steps;
    }
    return params_set(id, v, src);
}

bool params_toggle(param_id_t id, param_src_t src) { return params_set(id, params_get_bool(id) ? 0.f : 1.f, src); }

/* ---- formatting ---- */
static int fmt_hz(float v, char *b, size_t n, const param_desc_t *d)
{
    if (d->max <= 100.f) return snprintf(b, n, "%.2f", (double)v);           /* LFO rates */
    if (v < 1000.f) return snprintf(b, n, "%.0f", (double)v);
    if (v < 10000.f) return snprintf(b, n, "%.2f", (double)(v / 1000.f));
    return snprintf(b, n, "%.1f", (double)(v / 1000.f));
}

int params_format(param_id_t id, float v, char *b, size_t n)
{
    const param_desc_t *d = &g_param_desc[id];
    switch (d->type) {
    case PT_BOOL: return snprintf(b, n, "%s", v >= 0.5f ? "ON" : "OFF");
    case PT_ENUM: {
        int i = (int)lrintf(v);
        return snprintf(b, n, "%s", (d->enum_names && i >= 0) ? d->enum_names[i] : "?");
    }
    case PT_INT:
        if (id == P_MIDI_CH && lrintf(v) == 0) return snprintf(b, n, "OMNI");
        return snprintf(b, n, "%ld", lrintf(v));
    default: break;
    }
    const char *u = d->unit;
    if (!strcmp(u, "dB")) {
        if (id == P_VOLUME && v <= d->min + 0.01f) return snprintf(b, n, "-inf");
        if (d->min < 0.f && d->max > 0.f) {
            if (fabsf(v) < 0.05f) return snprintf(b, n, "0.0");
            return snprintf(b, n, "%+.1f", (double)v);
        }
        return snprintf(b, n, "%.1f", (double)v);
    }
    if (!strcmp(u, "Hz")) return fmt_hz(v, b, n, d);
    if (!strcmp(u, "ms")) {
        if (v >= 1000.f) return snprintf(b, n, "%.2f", (double)(v / 1000.f));
        if (v < 10.f) return snprintf(b, n, "%.1f", (double)v);
        return snprintf(b, n, "%.0f", (double)v);
    }
    if (!strcmp(u, "s")) return snprintf(b, n, v < 10.f ? "%.1f" : "%.0f", (double)v);
    if (!strcmp(u, "%")) return snprintf(b, n, "%.0f", (double)v);
    if (!strcmp(u, ":1")) return snprintf(b, n, v < 10.f ? "%.1f" : "%.0f", (double)v);
    if (!strcmp(u, "BPM")) return snprintf(b, n, "%.0f", (double)v);
    return snprintf(b, n, "%.2f", (double)v); /* Q etc. */
}

const char *params_format_unit(param_id_t id, float v)
{
    const param_desc_t *d = &g_param_desc[id];
    if (d->type != PT_FLOAT) return "";
    if (!strcmp(d->unit, "Hz") && d->max > 100.f && v >= 1000.f) return "kHz";
    if (!strcmp(d->unit, "ms") && v >= 1000.f) return "s";
    if (id == P_VOLUME && v <= d->min + 0.01f) return "";
    return d->unit;
}

int params_format_full(param_id_t id, float v, char *b, size_t n)
{
    int k = params_format(id, v, b, n);
    const char *u = params_format_unit(id, v);
    if (k < 0 || (size_t)k >= n || !u[0]) return k;
    if (!strcmp(u, ":1")) return k + snprintf(b + k, n - k, ":1");
    if (!strcmp(u, "%")) return k + snprintf(b + k, n - k, "%%");
    return k + snprintf(b + k, n - k, " %s", u);
}

uint32_t params_key_hash(const char *key)
{
    uint32_t h = 2166136261u;
    while (*key) { h ^= (uint8_t)*key++; h *= 16777619u; }
    return h;
}

param_id_t params_find(const char *key)
{
    for (int i = 0; i < P_COUNT; i++)
        if (!strcmp(g_param_desc[i].key, key)) return (param_id_t)i;
    return P_COUNT;
}

uint32_t params_seq(void) { return __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE); }
uint32_t params_preset_epoch(void) { return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE); }

bool params_take_dirty(param_listener_t l, uint32_t mask[PARAMS_MASK_WORDS])
{
    bool any = false;
    for (int w = 0; w < PARAMS_MASK_WORDS; w++) {
        mask[w] = __atomic_exchange_n(&s_dirty[l][w], 0u, __ATOMIC_ACQ_REL);
        any |= mask[w] != 0;
    }
    return any;
}

/* ---- MIDI CC map ---- */
uint8_t params_cc_of(param_id_t id) { return s_cc_of[id]; }

param_id_t params_param_of_cc(uint8_t cc)
{
    if (cc > 127) return P_COUNT;
    for (int i = 0; i < P_COUNT; i++)
        if (s_cc_of[i] == cc) return (param_id_t)i;
    return P_COUNT;
}

void params_cc_assign(param_id_t id, uint8_t cc)
{
    if (cc <= 127)
        for (int i = 0; i < P_COUNT; i++)
            if (s_cc_of[i] == cc) s_cc_of[i] = NO_CC;
    s_cc_of[id] = cc > 127 ? NO_CC : cc;
}

void params_cc_reset(void)
{
    for (int i = 0; i < P_COUNT; i++) s_cc_of[i] = g_param_desc[i].midi_cc;
}

/* ---- serialisation ---- */
#define BLOB_MAGIC   0x31504654u /* "TFP1" */
#define CC_MAGIC     0x31434654u /* "TFC1" */
#define BLOB_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    char     name[PRESET_NAME_LEN];
} blob_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t hash;
    float    value;
} blob_kv_t;

size_t params_blob_max(uint8_t flags_mask)
{
    size_t n = 0;
    for (int i = 0; i < P_COUNT; i++) n += (g_param_desc[i].flags & flags_mask) ? 1 : 0;
    return sizeof(blob_hdr_t) + n * sizeof(blob_kv_t);
}

static size_t serialize_values(const float *vals, uint8_t flags_mask, const char *name, uint8_t *buf, size_t cap)
{
    if (cap < params_blob_max(flags_mask)) return 0;
    blob_hdr_t h = { .magic = BLOB_MAGIC, .version = BLOB_VERSION, .count = 0 };
    memset(h.name, 0, sizeof h.name);
    if (name) strncpy(h.name, name, PRESET_NAME_LEN - 1);
    size_t off = sizeof h;
    for (int i = 0; i < P_COUNT; i++) {
        if (!(g_param_desc[i].flags & flags_mask)) continue;
        blob_kv_t kv = { .hash = params_key_hash(g_param_desc[i].key), .value = vals[i] };
        memcpy(buf + off, &kv, sizeof kv);
        off += sizeof kv;
        h.count++;
    }
    memcpy(buf, &h, sizeof h);
    return off;
}

size_t params_serialize(uint8_t flags_mask, const char *name, uint8_t *buf, size_t cap)
{
    float snap[P_COUNT];
    for (int i = 0; i < P_COUNT; i++) snap[i] = s_val[i];
    return serialize_values(snap, flags_mask, name, buf, cap);
}

bool params_deserialize(const uint8_t *buf, size_t len, uint8_t flags_mask, param_src_t src, char *name_out)
{
    blob_hdr_t h;
    if (!buf || len < sizeof h) return false;
    memcpy(&h, buf, sizeof h);
    if (h.magic != BLOB_MAGIC || h.version != BLOB_VERSION) return false;
    if (len < sizeof h + (size_t)h.count * sizeof(blob_kv_t)) return false;
    if (name_out) { memcpy(name_out, h.name, PRESET_NAME_LEN); name_out[PRESET_NAME_LEN - 1] = 0; }
    for (int k = 0; k < h.count; k++) {
        blob_kv_t kv;
        memcpy(&kv, buf + sizeof h + (size_t)k * sizeof kv, sizeof kv);
        for (int i = 0; i < P_COUNT; i++) {
            if ((g_param_desc[i].flags & flags_mask) && params_key_hash(g_param_desc[i].key) == kv.hash) {
                params_set((param_id_t)i, kv.value, src);
                break;
            }
        }
    }
    return true;
}

bool params_preset_apply(const uint8_t *blob, size_t len, char *name_out)
{
    /* Bump the epoch first so the engine sees the long glide before the new targets land. */
    __atomic_fetch_add(&s_epoch, 1, __ATOMIC_RELEASE);
    return params_deserialize(blob, len, PF_PRESET, SRC_PRESET, name_out);
}

size_t params_factory_blob(int slot, uint8_t *buf, size_t cap)
{
    if (slot < 0 || slot >= PRESET_SLOTS) return 0;
    float vals[P_COUNT];
    for (int i = 0; i < P_COUNT; i++) vals[i] = g_param_desc[i].def;
    const factory_preset_t *fp = &g_factory_presets[slot];
    for (int k = 0; k < fp->n; k++) vals[fp->kv[k].id] = quantize(&g_param_desc[fp->kv[k].id], fp->kv[k].v);
    return serialize_values(vals, PF_PRESET, fp->name, buf, cap);
}

size_t params_cc_serialize(uint8_t *buf, size_t cap)
{
    size_t need = 8 + (size_t)P_COUNT * 5;
    if (cap < need) return 0;
    uint32_t magic = CC_MAGIC, n = P_COUNT;
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &n, 4);
    size_t off = 8;
    for (int i = 0; i < P_COUNT; i++) {
        uint32_t hsh = params_key_hash(g_param_desc[i].key);
        memcpy(buf + off, &hsh, 4);
        buf[off + 4] = s_cc_of[i];
        off += 5;
    }
    return off;
}

bool params_cc_deserialize(const uint8_t *buf, size_t len)
{
    uint32_t magic, n;
    if (len < 8) return false;
    memcpy(&magic, buf, 4);
    memcpy(&n, buf + 4, 4);
    if (magic != CC_MAGIC || len < 8 + (size_t)n * 5) return false;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t hsh;
        memcpy(&hsh, buf + 8 + k * 5, 4);
        for (int i = 0; i < P_COUNT; i++)
            if (params_key_hash(g_param_desc[i].key) == hsh) { s_cc_of[i] = buf[8 + k * 5 + 4]; break; }
    }
    return true;
}
