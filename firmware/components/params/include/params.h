/*
 * params.h: the shared parameter model.
 *
 * Threading model
 *   - Values are plain 32-bit floats. Aligned 32-bit loads/stores are atomic on Xtensa LX7 and x86, so
 *     readers never see torn values.
 *   - Any core-0 task (UI, web, USB-MIDI, BLE, storage) may call params_set(). Last writer wins.
 *   - The audio engine (core 1) only reads, once per block, and smooths internally.
 *   - Every consumer ("listener") owns a dirty bitmask, set with atomic OR and taken with atomic exchange.
 *     No locks anywhere, so the audio task can never block on this module.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "params_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
#define X_ID(id, ...) id,
    PARAMS_TABLE(X_ID)
#undef X_ID
    P_COUNT
} param_id_t;

typedef enum { PT_FLOAT, PT_BOOL, PT_ENUM, PT_INT } param_type_t;
typedef enum { PC_LIN, PC_LOG } param_curve_t;

enum {
    PF_PRESET   = 1 << 0,   /* stored in presets */
    PF_SETTING  = 1 << 1,   /* stored as device setting */
    PF_VOLATILE = 1 << 2,   /* never stored */
};

typedef struct {
    const char *key;
    const char *name;
    const char *unit;
    uint8_t type, curve, flags, midi_cc;
    float min, max, def, step;
    const char *const *enum_names;
    uint8_t enum_count;
} param_desc_t;

/* Who changed a parameter. Used for echo suppression (e.g. do not echo a MIDI CC back to MIDI). */
typedef enum {
    SRC_INIT = 0, SRC_UI, SRC_MIDI, SRC_BLE, SRC_WEB, SRC_PRESET, SRC_STORAGE, SRC_SYSTEM,
} param_src_t;

/* Consumers with independent dirty masks. */
typedef enum {
    PL_STORE = 0, PL_MIDI, PL_BLE, PL_WEB, PL_UI, PL_COUNT
} param_listener_t;

#define PARAMS_MASK_WORDS ((P_COUNT + 31) / 32)

extern const param_desc_t g_param_desc[P_COUNT];

void        params_init(void);                       /* load defaults (call once at boot) */
int         params_selftest(void);                   /* table consistency; returns error count */
static inline const param_desc_t *params_desc(param_id_t id) { return &g_param_desc[id]; }

float       params_get(param_id_t id);
bool        params_get_bool(param_id_t id);
int         params_get_int(param_id_t id);           /* enums / ints / bools */
/* Clamp, quantise (bool/enum/int), store; returns true when the value actually changed. */
bool        params_set(param_id_t id, float v, param_src_t src);
param_src_t params_last_src(param_id_t id);

/* Normalised 0..1 mapping following the param curve (MIDI CC, web sliders). */
float       params_to_norm(param_id_t id, float v);
float       params_from_norm(param_id_t id, float n);
bool        params_set_norm(param_id_t id, float n, param_src_t src);

/* Encoder-style relative change: `steps` detents (acceleration already applied by caller).
 * PC_LIN: steps * desc.step units. PC_LOG: steps / desc.step octaves. Enums wrap = false (clamp). */
bool        params_nudge(param_id_t id, int steps, param_src_t src);
bool        params_toggle(param_id_t id, param_src_t src);  /* bools */

/* Formatting. Value only (e.g. "-12.5", "1.25k", "ON", "PING-PONG", "-inf"), and display unit
 * (may differ from desc unit, e.g. "s" for long ms values, "kHz"). Returns chars written. */
int         params_format(param_id_t id, float v, char *buf, size_t n);
const char *params_format_unit(param_id_t id, float v);
int         params_format_full(param_id_t id, float v, char *buf, size_t n);  /* "value unit" */

param_id_t  params_find(const char *key);            /* P_COUNT if not found */
uint32_t    params_key_hash(const char *key);        /* FNV-1a 32 */

/* Change tracking */
uint32_t    params_seq(void);                        /* increments on every change */
bool        params_take_dirty(param_listener_t l, uint32_t mask[PARAMS_MASK_WORDS]); /* true if any */
static inline bool params_mask_test(const uint32_t *m, param_id_t id) { return (m[id >> 5] >> (id & 31)) & 1u; }

/* Preset crossfade: incremented every time a preset is applied; the engine uses a long glide
 * (PARAMS_PRESET_GLIDE_MS) for transitions that start right after an epoch change. */
#define PARAMS_PRESET_GLIDE_MS 150
uint32_t    params_preset_epoch(void);

/* MIDI CC map (runtime-editable, persisted as a setting blob). cc -> param, param -> cc. */
uint8_t     params_cc_of(param_id_t id);             /* NO_CC if none */
param_id_t  params_param_of_cc(uint8_t cc);          /* P_COUNT if none */
void        params_cc_assign(param_id_t id, uint8_t cc);  /* moves cc from any other param */
void        params_cc_reset(void);

/* Serialisation: versioned, keyed by key hash so firmware updates can add/remove params safely.
 * blob = header + n * {u32 key_hash, f32 value}. Unknown keys are ignored on load. */
#define PRESET_NAME_LEN 16
#define PRESET_SLOTS    8
size_t      params_serialize(uint8_t flags_mask, const char *name, uint8_t *buf, size_t cap);
bool        params_deserialize(const uint8_t *buf, size_t len, uint8_t flags_mask, param_src_t src,
                               char *name_out /* PRESET_NAME_LEN, may be NULL */);
size_t      params_blob_max(uint8_t flags_mask);
size_t      params_cc_serialize(uint8_t *buf, size_t cap);
bool        params_cc_deserialize(const uint8_t *buf, size_t len);

/* Presets: apply = deserialize with SRC_PRESET + epoch++.  */
bool        params_preset_apply(const uint8_t *blob, size_t len, char *name_out);

/* Factory presets (factory_presets.c): name + list of (param, value) over defaults. */
typedef struct { param_id_t id; float v; } preset_kv_t;
typedef struct { const char *name; const preset_kv_t *kv; int n; } factory_preset_t;
extern const factory_preset_t g_factory_presets[PRESET_SLOTS];
/* Build a preset blob for factory slot i (defaults for all PF_PRESET params + overrides). */
size_t      params_factory_blob(int slot, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif
