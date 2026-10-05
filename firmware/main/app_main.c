/*
 * app_main.c: TWOFOLD boot sequence (runs on core 0).
 *
 *  1. audio_early_init   XSMT low: the DAC stays muted until clocks are stable
 *  2. params_init        defaults
 *  3. storage_init       NVS: settings, CC map, last active preset (first boot: factory presets);
 *                        starts the storage task (core 0, prio 3)
 *  4. audio_dsp_init     all big DSP buffers, before any radio exists (no fragmentation later)
 *  5. usbdev_init        USB PHY + TinyUSB task (core 0, prio 20)
 *  6. audio_start        audio task (core 1, prio 22): I2S on core 1, TX pre-fill, XSMT high after
 *                        ~50 ms, then dsp_engine_startup_ramp (volume rises from silence)
 *  7. net_init           cheap, no radio
 *  8. input_init         PCNT encoder + switch, input task (core 0, prio 9)
 *  9. led_init           status LED task (core 0, prio 2)
 * 10. ctrl_start         ctrl task (core 0, prio 8, 2 ms) + sys task (core 0, prio 4)
 * 11. ui_task_start      UI task (core 0, prio 5): display init, 60 fps UI
 * 12. radios             OFF unless P_NET_BOOT == RESTORE (then P_NET_WIFI / P_NET_BLE are restored)
 */
#include "twofold.h"
#include "bootdiag.h"
#include "params.h"
#include "storage.h"
#include "audio.h"
#include "usbdev.h"
#include "input.h"
#include "led.h"
#include "net.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "twofold";

#define CHECK(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) ESP_LOGE(TAG, "%s: %s", #x, esp_err_to_name(e_)); } while (0)

void app_main(void)
{
    bootdiag_init();             /* reset reason + last breadcrumb, shown as a toast after boot */
    audio_early_init();
    params_init();
    CHECK(storage_init());
    CHECK(audio_dsp_init());
    CHECK(usbdev_init());
    CHECK(audio_start());
    CHECK(net_init());
    CHECK(input_init());
    CHECK(led_init());
    CHECK(ctrl_start());
    CHECK(ui_task_start());

    /* Studio Mode by default: radios stay off. RESTORE brings back the last WiFi / BLE state. */
    if (params_get_int(P_NET_BOOT) == 1) {
        if (params_get_bool(P_NET_WIFI)) sys_worker_post(SYS_CMD_WIFI_ENABLE, 1);
        if (params_get_bool(P_NET_BLE)) sys_worker_post(SYS_CMD_BLE_ENABLE, 1);
    } else {
        /* Keep the stored radio state truthful (radios are off now). */
        params_set(P_NET_WIFI, 0.f, SRC_SYSTEM);
        params_set(P_NET_BLE, 0.f, SRC_SYSTEM);
    }

    ESP_LOGI(TAG, "up: internal heap %u B free (min %u), PSRAM %u B free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
