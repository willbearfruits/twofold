/*
 * ota_confirm.c: OTA rollback health check (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 *
 * After an OTA update the bootloader starts the new image once as "pending verify". This module decides whether
 * to keep it: audio blocks must keep flowing for CONFIRM_AFTER_S seconds with no new I2S errors. Then the image is
 * marked valid (an otadata write: done in the sys task under the usual soft-mute). If audio never runs, or the
 * device panics first (the bootloader counts that reset), the previous image comes back on the next boot.
 *
 * An image flashed over USB is never "pending" (esptool marks it valid), so this is a no-op for development.
 */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "audio.h"
#include "bootdiag.h"
#include "dsp_engine.h"
#include "twofold.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONFIRM_AFTER_S   20     /* clean audio needed before confirming */
#define GIVE_UP_AFTER_S   180    /* no clean audio by then: roll back and reboot */

static const char *TAG = "ota";
static esp_timer_handle_t s_timer;
static uint32_t s_t, s_clean, s_blocks, s_errs;

static void tick(void *arg)
{
    (void)arg;
    audio_stats_t st;
    audio_get_stats(&st);
    bool progressed = st.running && st.blocks != s_blocks && st.i2s_errors == s_errs;
    s_blocks = st.blocks;
    s_errs = st.i2s_errors;
    s_clean = progressed ? s_clean + 1 : 0;
    s_t++;
    if (s_clean >= CONFIRM_AFTER_S) {
        esp_timer_stop(s_timer);
        sys_worker_post(SYS_CMD_OTA_CONFIRM, 1);
    } else if (s_t >= GIVE_UP_AFTER_S) {
        esp_timer_stop(s_timer);
        sys_worker_post(SYS_CMD_OTA_CONFIRM, 0);
    }
}

void ota_confirm_start(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_LOGW(TAG, "new image on %s pending verification: confirming after %d s of clean audio", run->label,
             CONFIRM_AFTER_S);
    bootdiag_crumb("ota verify");
    const esp_timer_create_args_t a = { .callback = tick, .name = "ota_confirm" };
    if (esp_timer_create(&a, &s_timer) == ESP_OK) esp_timer_start_periodic(s_timer, 1000000);
}

/* sys task (core 0): the otadata write stalls both cores, so it goes under soft-mute like any other flash write. */
void ota_confirm_apply(bool healthy)
{
    if (healthy) {
        audio_flash_write_hint();
        dsp_engine_soft_mute(true);
        for (int i = 0; i < 100 && !dsp_engine_is_silent(); i++) vTaskDelay(1);
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        dsp_engine_soft_mute(false);
        bootdiag_crumb(NULL);
        ESP_LOGW(TAG, "image confirmed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGE(TAG, "audio never ran cleanly: rolling back to the previous image");
        bootdiag_crumb("ota rollback");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}
