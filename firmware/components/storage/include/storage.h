/*
 * storage.h: NVS persistence for settings, MIDI CC map and 8 presets, with debounced,
 * audio-safe writes (PLAN.md "Storage policy").
 *
 * Owner: storage task (core 0, low priority). The API is callable from any core-0 task.
 * Flash writes stall both CPUs on the ESP32-S3, so writes only happen
 *   (a) in an audio-safe window: no USB stream and output quiet, or output quiet >= 2 s, or
 *   (b) on explicit user actions (preset save, WiFi credentials, factory reset), wrapped in
 *       dsp_engine_soft_mute(true) ... write ... dsp_engine_soft_mute(false).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Init NVS, load settings + CC map + preset metadata into params (SRC_STORAGE), apply the last
 * active preset (or factory slot 0), and start the storage task. Call after params_init(). */
esp_err_t storage_init(void);

bool  storage_preset_load(int slot);                       /* crossfaded via params_preset_apply */
bool  storage_preset_save(int slot, const char *name);     /* name NULL = keep existing name */
bool  storage_preset_rename(int slot, const char *name);
void  storage_preset_name(int slot, char out[16]);
int   storage_preset_active(void);                         /* -1 if none */
bool  storage_preset_dirty(void);                          /* edited since load/save */
size_t storage_preset_export(int slot, uint8_t *buf, size_t cap);    /* raw params blob */
bool  storage_preset_import(int slot, const uint8_t *buf, size_t len);

bool  storage_pending(void);                               /* deferred writes waiting */
void  storage_flush_now(void);                             /* explicit, soft-muted commit */
void  storage_factory_reset(void);                         /* erase + factory presets, no reboot */

/* Small generic blobs for other modules (e.g. WiFi STA credentials). Writes are explicit and
 * soft-muted (never deferred). Namespace is private to storage. */
esp_err_t storage_kv_set(const char *key, const void *data, size_t len);
esp_err_t storage_kv_get(const char *key, void *data, size_t *len);
esp_err_t storage_kv_erase(const char *key);

#ifdef __cplusplus
}
#endif
