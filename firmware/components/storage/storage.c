/*
 * storage.c: NVS persistence with audio-safe writes. Implements storage.h; policy in PLAN.md 13.
 *
 * NVS layout (partition "nvs", 24 KB)
 *   namespace "twofold":  "init" u8 (layout version, absent = first boot)
 *                         "settings" blob  (params_serialize(PF_SETTING))
 *                         "ccmap"    blob  (params_cc_serialize)
 *                         "p0".."p7" blob  (params_serialize(PF_PRESET, name))
 *                         "n0".."n7" str   (preset names, the source of truth for the UI)
 *                         "active"   i8    (last loaded / saved preset, -1 none)
 *   namespace "tf_kv":    generic small blobs for other modules (e.g. WiFi STA credentials)
 *
 * Preset blobs are cached in RAM at boot (preset loads never touch flash).
 *
 * When writes happen (any flash erase/program stalls BOTH cores, PLAN.md 13)
 *   Deferred (settings, CC map, active slot): the storage task (core 0, prio 3, 250 ms) collects changes
 *   via params_take_dirty(PL_STORE) and a CC-map compare, waits >= 3 s after the last change, then commits
 *   only in an audio-safe window:
 *     (a) no USB stream active and dsp_engine_output_quiet()          -> plain write
 *     (b) dsp_engine_output_quiet() (DAC < -90 dBFS for >= 2 s), and the record path not carrying signal
 *         (no record stream, or input peak < -70 dBFS)                  -> plain write
 *     (c) dirty for >= 10 min                                           -> soft-muted write
 *   Explicit (preset save/rename/import, kv_set/erase, factory reset, flush_now): soft-muted write:
 *     dsp_engine_soft_mute(true) -> wait dsp_engine_is_silent() (<= 100 ms) -> write -> soft_mute(false).
 *   Flash is never written from core 1 (checked).
 */
#include <stdio.h>
#include <string.h>
#include "storage.h"
#include "params.h"
#include "audio.h"
#include "dsp_engine.h"
#include "usbdev.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "storage";

#define NS_MAIN              "twofold"
#define NS_KV                "tf_kv"
#define LAYOUT_VERSION       1
#define STORAGE_TASK_CORE    0
#define STORAGE_TASK_PRIO    3
#define STORAGE_TASK_STACK   4096
#define STORAGE_PERIOD_MS    250
#define DEBOUNCE_US          (3LL * 1000000)
#define FORCE_AFTER_US       (10LL * 60 * 1000000)
#define SILENCE_TIMEOUT_MS   100
#define IN_QUIET_LIN         0.000316f      /* -70 dBFS */
#define BLOB_HEADROOM        (32 * 8)       /* 32 extra {hash, value} pairs */

/* Preset blob header as written by params.c (magic u32 "TFP1", version u16, count u16, name[16]). Only the
 * magic and the name are read here (import validation / name extraction). */
#define PRESET_MAGIC         0x31504654u
#define PRESET_HDR_NAME_OFS  8
#define PRESET_HDR_SIZE      (8 + PRESET_NAME_LEN)

static nvs_handle_t s_nvs;
static SemaphoreHandle_t s_mtx;
static bool s_ready;

static uint8_t *s_blob[PRESET_SLOTS];        /* RAM cache of preset blobs */
static size_t   s_blob_len[PRESET_SLOTS];
static size_t   s_blob_cap;
static char     s_name[PRESET_SLOTS][PRESET_NAME_LEN];

static volatile int  s_active = -1;
static volatile bool s_preset_dirty;
static volatile bool s_settings_dirty, s_cc_dirty, s_active_dirty;
static volatile bool s_poke;                 /* set by API calls that make something dirty */
static int64_t s_last_change_us, s_dirty_since_us;
static uint8_t *s_cc_saved, *s_cc_tmp;
static size_t   s_cc_saved_len, s_cc_cap;
static uint8_t *s_scratch;                   /* settings serialisation */
static size_t   s_scratch_cap;

/* ------------------------------------------------------------------------------------------------ */
static void key_of(char *k, char c, int slot) { k[0] = c; k[1] = (char)('0' + slot); k[2] = 0; }

static bool on_core0(void)
{
    if (xPortGetCoreID() != 0) {
        ESP_LOGE(TAG, "flash write requested from core %d: refused", xPortGetCoreID());
        return false;
    }
    return true;
}

static void lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

/* Soft-mute the DAC + USB record path for the duration of an explicit write. */
static void mute_begin(void)
{
    audio_flash_write_hint();
    dsp_engine_soft_mute(true);
    for (int i = 0; i < SILENCE_TIMEOUT_MS && !dsp_engine_is_silent(); i++) vTaskDelay(1);
}
static void mute_end(void) { dsp_engine_soft_mute(false); }

/* ---- raw writers (caller holds the lock; core 0) ---- */
static esp_err_t write_settings_locked(void)
{
    audio_flash_write_hint();
    size_t n = params_serialize(PF_SETTING, NULL, s_scratch, s_scratch_cap);
    if (!n) return ESP_ERR_INVALID_SIZE;
    return nvs_set_blob(s_nvs, "settings", s_scratch, n);
}

static esp_err_t write_cc_locked(void)
{
    audio_flash_write_hint();
    size_t n = params_cc_serialize(s_cc_tmp, s_cc_cap);
    if (!n) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = nvs_set_blob(s_nvs, "ccmap", s_cc_tmp, n);
    if (err == ESP_OK) {
        memcpy(s_cc_saved, s_cc_tmp, n);
        s_cc_saved_len = n;
    }
    return err;
}

static esp_err_t write_preset_locked(int slot)
{
    char k[3];
    key_of(k, 'p', slot);
    esp_err_t err = nvs_set_blob(s_nvs, k, s_blob[slot], s_blob_len[slot]);
    key_of(k, 'n', slot);
    if (err == ESP_OK) err = nvs_set_str(s_nvs, k, s_name[slot]);
    return err;
}

static esp_err_t write_active_locked(void) { return nvs_set_i8(s_nvs, "active", (int8_t)s_active); }

/* Commit everything that is dirty. Caller holds the lock. */
static esp_err_t commit_dirty_locked(void)
{
    esp_err_t err = ESP_OK;
    if (s_settings_dirty) { s_settings_dirty = false; err = write_settings_locked(); }
    if (err == ESP_OK && s_cc_dirty) { s_cc_dirty = false; err = write_cc_locked(); }
    if (err == ESP_OK && s_active_dirty) { s_active_dirty = false; err = write_active_locked(); }
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "commit: %s", esp_err_to_name(err));
    s_dirty_since_us = 0;
    return err;
}

/* ------------------------------------------------------------------------------------------------ */
static bool blob_name(const uint8_t *b, size_t len, char out[PRESET_NAME_LEN])
{
    uint32_t magic;
    if (!b || len < PRESET_HDR_SIZE) return false;
    memcpy(&magic, b, 4);
    if (magic != PRESET_MAGIC) return false;
    memcpy(out, b + PRESET_HDR_NAME_OFS, PRESET_NAME_LEN);
    out[PRESET_NAME_LEN - 1] = 0;
    return true;
}

static void make_factory(int slot)
{
    s_blob_len[slot] = params_factory_blob(slot, s_blob[slot], s_blob_cap);
    snprintf(s_name[slot], PRESET_NAME_LEN, "%s", g_factory_presets[slot].name ? g_factory_presets[slot].name : "PRESET");
}

/* Write factory presets + current settings + CC map (first boot / factory reset). Lock held. */
static esp_err_t write_factory_locked(void)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < PRESET_SLOTS && err == ESP_OK; i++) {
        make_factory(i);
        err = write_preset_locked(i);
    }
    s_active = 0;
    if (err == ESP_OK) err = write_active_locked();
    if (err == ESP_OK) err = write_settings_locked();
    if (err == ESP_OK) err = write_cc_locked();
    if (err == ESP_OK) err = nvs_set_u8(s_nvs, "init", LAYOUT_VERSION);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    return err;
}

/* ------------------------------------------------------------------------------------------------
 * Storage task: deferred, audio-safe commits
 * ------------------------------------------------------------------------------------------------ */
static bool input_quiet(void)
{
    dsp_meters_t m;
    dsp_engine_get_meters(&m, false);     /* non-destructive read: the UI owns the "take" */
    return m.in_peak[0] < IN_QUIET_LIN && m.in_peak[1] < IN_QUIET_LIN;
}

static void storage_task(void *arg)
{
    uint32_t mask[PARAMS_MASK_WORDS];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STORAGE_PERIOD_MS));
        int64_t const now = esp_timer_get_time();
        bool changed = false;

        if (params_take_dirty(PL_STORE, mask)) {
            for (int i = 0; i < P_COUNT; i++) {
                if (!params_mask_test(mask, (param_id_t)i)) continue;
                uint8_t const f = g_param_desc[i].flags;
                if (f & PF_SETTING) { s_settings_dirty = true; changed = true; }
                if (f & PF_PRESET) {
                    param_src_t const src = params_last_src((param_id_t)i);
                    if (src != SRC_PRESET && src != SRC_STORAGE && src != SRC_INIT) s_preset_dirty = true;
                }
            }
        }
        /* The CC map has no dirty mask: compare with what is stored. */
        lock();
        size_t n = params_cc_serialize(s_cc_tmp, s_cc_cap);
        if (n && (n != s_cc_saved_len || memcmp(s_cc_tmp, s_cc_saved, n))) {
            if (!s_cc_dirty) changed = true;
            s_cc_dirty = true;
        }
        unlock();
        if (s_poke) { s_poke = false; changed = true; }

        bool const dirty = s_settings_dirty || s_cc_dirty || s_active_dirty;
        if (!dirty) continue;
        if (changed) s_last_change_us = now;
        if (s_dirty_since_us == 0) s_dirty_since_us = now;
        if (now - s_last_change_us < DEBOUNCE_US) continue;

        bool const quiet = dsp_engine_output_quiet();
        bool const window_a = !usbdev_stream_active() && quiet;
        bool const window_b = quiet && (!usbdev_rec_active() || input_quiet());
        bool const forced = now - s_dirty_since_us >= FORCE_AFTER_US;
        if (!(window_a || window_b || forced)) continue;

        lock();
        if (forced && !window_a && !window_b) {
            ESP_LOGI(TAG, "dirty for 10 min: soft-muted commit");
            mute_begin();
            commit_dirty_locked();
            mute_end();
        } else {
            commit_dirty_locked();
        }
        unlock();
    }
}

/* ------------------------------------------------------------------------------------------------
 * Boot
 * ------------------------------------------------------------------------------------------------ */
esp_err_t storage_init(void)
{
    if (s_ready) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition reset (%s)", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    if ((err = nvs_open(NS_MAIN, NVS_READWRITE, &s_nvs)) != ESP_OK) return err;

    /* Headroom for blobs written by a firmware that had more parameters (unknown keys are skipped on load). */
    s_blob_cap = params_blob_max(PF_PRESET) + BLOB_HEADROOM;
    s_scratch_cap = params_blob_max(PF_SETTING) + BLOB_HEADROOM;
    s_cc_cap = 8 + (size_t)P_COUNT * 5 + 16;
    s_scratch = heap_caps_malloc(s_scratch_cap, MALLOC_CAP_8BIT);
    s_cc_saved = heap_caps_calloc(1, s_cc_cap, MALLOC_CAP_8BIT);
    s_cc_tmp = heap_caps_calloc(1, s_cc_cap, MALLOC_CAP_8BIT);
    if (!s_scratch || !s_cc_saved || !s_cc_tmp) return ESP_ERR_NO_MEM;
    for (int i = 0; i < PRESET_SLOTS; i++) {
        /* PSRAM if available: the cache is only touched from core 0 on preset load/save. */
        s_blob[i] = heap_caps_malloc(s_blob_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_blob[i]) s_blob[i] = heap_caps_malloc(s_blob_cap, MALLOC_CAP_8BIT);
        if (!s_blob[i]) return ESP_ERR_NO_MEM;
    }

    /* Boot happens before the audio task runs, so plain (un-muted) writes are fine here. */
    uint8_t layout = 0;
    if (nvs_get_u8(s_nvs, "init", &layout) != ESP_OK || layout != LAYOUT_VERSION) {
        ESP_LOGI(TAG, "first boot: writing factory presets");
        err = write_factory_locked();
        if (err != ESP_OK) ESP_LOGE(TAG, "factory write: %s", esp_err_to_name(err));
    } else {
        /* settings */
        size_t len = s_scratch_cap;
        if (nvs_get_blob(s_nvs, "settings", s_scratch, &len) == ESP_OK)
            params_deserialize(s_scratch, len, PF_SETTING, SRC_STORAGE, NULL);
        /* CC map */
        len = s_cc_cap;
        if (nvs_get_blob(s_nvs, "ccmap", s_cc_tmp, &len) == ESP_OK) params_cc_deserialize(s_cc_tmp, len);
        /* presets */
        bool repaired = false;
        for (int i = 0; i < PRESET_SLOTS; i++) {
            char k[3];
            key_of(k, 'p', i);
            len = s_blob_cap;
            /* A missing or unreadable blob (e.g. larger than s_blob_cap) falls back to the factory preset. */
            if (nvs_get_blob(s_nvs, k, s_blob[i], &len) == ESP_OK && blob_name(s_blob[i], len, s_name[i])) {
                s_blob_len[i] = len;
                key_of(k, 'n', i);
                size_t sl = PRESET_NAME_LEN;
                char nm[PRESET_NAME_LEN];
                if (nvs_get_str(s_nvs, k, nm, &sl) == ESP_OK) memcpy(s_name[i], nm, PRESET_NAME_LEN);
            } else {
                make_factory(i);
                write_preset_locked(i);
                repaired = true;
            }
        }
        int8_t a = 0;
        if (nvs_get_i8(s_nvs, "active", &a) != ESP_OK || a < -1 || a >= PRESET_SLOTS) a = 0;
        s_active = a;
        if (repaired) nvs_commit(s_nvs);
    }

    /* Apply the last active preset (or factory slot 0). */
    int const a = s_active >= 0 ? s_active : 0;
    params_preset_apply(s_blob[a], s_blob_len[a], NULL);
    if (s_active < 0) s_active = 0;
    s_preset_dirty = false;

    /* Baseline: the boot-time restore must not schedule writes. */
    uint32_t mask[PARAMS_MASK_WORDS];
    params_take_dirty(PL_STORE, mask);
    s_cc_saved_len = params_cc_serialize(s_cc_saved, s_cc_cap);
    s_settings_dirty = s_cc_dirty = s_active_dirty = false;

    s_ready = true;
    if (xTaskCreatePinnedToCore(storage_task, "storage", STORAGE_TASK_STACK, NULL, STORAGE_TASK_PRIO, NULL,
                                STORAGE_TASK_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready: active preset %d \"%s\"", s_active, s_name[a]);
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------------------
 * Presets
 * ------------------------------------------------------------------------------------------------ */
static bool slot_ok(int slot) { return s_ready && slot >= 0 && slot < PRESET_SLOTS; }

bool storage_preset_load(int slot)
{
    if (!slot_ok(slot) || !s_blob_len[slot]) return false;
    lock();
    bool ok = params_preset_apply(s_blob[slot], s_blob_len[slot], NULL);
    if (ok) {
        s_active = slot;
        s_active_dirty = true;          /* deferred: the slot number is not worth a soft-mute */
        s_poke = true;
        s_preset_dirty = false;
    }
    unlock();
    return ok;
}

bool storage_preset_save(int slot, const char *name)
{
    if (!slot_ok(slot) || !on_core0()) return false;
    lock();
    char nm[PRESET_NAME_LEN];
    snprintf(nm, sizeof nm, "%s", name ? name : s_name[slot]);
    size_t n = params_serialize(PF_PRESET, nm, s_blob[slot], s_blob_cap);
    bool ok = n > 0;
    if (ok) {
        s_blob_len[slot] = n;
        memcpy(s_name[slot], nm, PRESET_NAME_LEN);
        s_active = slot;
        mute_begin();
        esp_err_t err = write_preset_locked(slot);
        if (err == ESP_OK) err = write_active_locked();
        s_active_dirty = false;
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        mute_end();
        ok = err == ESP_OK;
        if (ok) s_preset_dirty = false;
        else ESP_LOGE(TAG, "preset save: %s", esp_err_to_name(err));
    }
    unlock();
    return ok;
}

bool storage_preset_rename(int slot, const char *name)
{
    if (!slot_ok(slot) || !name || !on_core0()) return false;
    lock();
    snprintf(s_name[slot], PRESET_NAME_LEN, "%s", name);
    char k[3];
    key_of(k, 'n', slot);
    mute_begin();
    esp_err_t err = nvs_set_str(s_nvs, k, s_name[slot]);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    mute_end();
    unlock();
    return err == ESP_OK;
}

void storage_preset_name(int slot, char out[16])
{
    if (!slot_ok(slot)) { out[0] = 0; return; }
    memcpy(out, s_name[slot], PRESET_NAME_LEN);
    out[PRESET_NAME_LEN - 1] = 0;
}

int  storage_preset_active(void) { return s_active; }
bool storage_preset_dirty(void) { return s_preset_dirty; }

size_t storage_preset_export(int slot, uint8_t *buf, size_t cap)
{
    if (!slot_ok(slot) || !buf || cap < s_blob_len[slot]) return 0;
    lock();
    size_t n = s_blob_len[slot];
    memcpy(buf, s_blob[slot], n);
    unlock();
    return n;
}

bool storage_preset_import(int slot, const uint8_t *buf, size_t len)
{
    char nm[PRESET_NAME_LEN];
    if (!slot_ok(slot) || !blob_name(buf, len, nm) || len > s_blob_cap || !on_core0()) return false;
    lock();
    memcpy(s_blob[slot], buf, len);
    s_blob_len[slot] = len;
    memcpy(s_name[slot], nm, PRESET_NAME_LEN);
    mute_begin();
    esp_err_t err = write_preset_locked(slot);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    mute_end();
    unlock();
    return err == ESP_OK;
}

/* ------------------------------------------------------------------------------------------------ */
bool storage_pending(void) { return s_settings_dirty || s_cc_dirty || s_active_dirty; }

void storage_flush_now(void)
{
    if (!s_ready || !on_core0()) return;
    /* Pick up changes the task has not seen yet. */
    uint32_t mask[PARAMS_MASK_WORDS];
    if (params_take_dirty(PL_STORE, mask)) {
        for (int i = 0; i < P_COUNT; i++)
            if (params_mask_test(mask, (param_id_t)i)) {
                if (g_param_desc[i].flags & PF_SETTING) s_settings_dirty = true;
                if ((g_param_desc[i].flags & PF_PRESET) && params_last_src((param_id_t)i) != SRC_PRESET &&
                    params_last_src((param_id_t)i) != SRC_STORAGE)
                    s_preset_dirty = true;
            }
    }
    lock();
    size_t n = params_cc_serialize(s_cc_tmp, s_cc_cap);
    if (n && (n != s_cc_saved_len || memcmp(s_cc_tmp, s_cc_saved, n))) s_cc_dirty = true;
    if (!storage_pending()) {
        unlock();
        return;
    }
    mute_begin();
    commit_dirty_locked();
    mute_end();
    unlock();
}

void storage_factory_reset(void)
{
    if (!s_ready || !on_core0()) return;
    lock();
    /* Defaults first (audible change ramps through the engine smoothers), then one muted flash burst. */
    for (int i = 0; i < P_COUNT; i++) params_set((param_id_t)i, g_param_desc[i].def, SRC_SYSTEM);
    params_cc_reset();
    mute_begin();
    nvs_erase_all(s_nvs);
    nvs_handle_t kv;
    if (nvs_open(NS_KV, NVS_READWRITE, &kv) == ESP_OK) {
        nvs_erase_all(kv);
        nvs_commit(kv);
        nvs_close(kv);
    }
    esp_err_t err = write_factory_locked();
    mute_end();
    params_preset_apply(s_blob[0], s_blob_len[0], NULL);
    s_preset_dirty = false;
    uint32_t mask[PARAMS_MASK_WORDS];
    params_take_dirty(PL_STORE, mask);
    s_settings_dirty = s_cc_dirty = s_active_dirty = false;
    s_dirty_since_us = 0;
    unlock();
    ESP_LOGW(TAG, "factory reset: %s", esp_err_to_name(err));
}

/* ------------------------------------------------------------------------------------------------
 * Generic key/value blobs (explicit, soft-muted)
 * ------------------------------------------------------------------------------------------------ */
esp_err_t storage_kv_set(const char *key, const void *data, size_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!on_core0()) return ESP_ERR_INVALID_STATE;
    lock();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_KV, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        mute_begin();
        err = nvs_set_blob(h, key, data, len);
        if (err == ESP_OK) err = nvs_commit(h);
        mute_end();
        nvs_close(h);
    }
    unlock();
    return err;
}

esp_err_t storage_kv_get(const char *key, void *data, size_t *len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    lock();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_KV, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, key, data, len);
        nvs_close(h);
    }
    unlock();
    return err;
}

esp_err_t storage_kv_erase(const char *key)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!on_core0()) return ESP_ERR_INVALID_STATE;
    lock();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_KV, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        mute_begin();
        err = nvs_erase_key(h, key);
        if (err == ESP_OK) err = nvs_commit(h);
        mute_end();
        nvs_close(h);
    }
    unlock();
    return err;
}
