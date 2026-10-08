/*
 * ctrl.c: the ctrl task (core 0, prio 8, 2 ms) and the sys worker task (core 0, prio 4).
 *
 * ctrl task, every 2 ms:
 *   - USB-MIDI in: CC -> params (SRC_MIDI), see usbdev/usb_midi.c
 *   - USB-MIDI out: params_take_dirty(PL_MIDI) -> CC for every mapped parameter whose last change did not
 *     come from MIDI (no echo). Sends that do not fit in the TinyUSB TX FIFO stay pending for the next tick.
 *   - knob CCs requested by the UI (MIDI knob mode) -> USB-MIDI and, if connected, BLE-MIDI
 *   - ble_midi_poll() (BLE-MIDI param -> CC notifications, owned by the ble component)
 *   - every 100 ms: radio state -> status LED
 * sys task: executes slow commands posted by the UI (radio enable/disable can take hundreds of ms,
 * flash writes are soft-muted and can take ~100 ms) so the UI task never blocks on them.
 */
#include <math.h>
#include <string.h>
#include "twofold.h"
#include "params.h"
#include "usbdev.h"
#include "storage.h"
#include "net.h"
#include "ble_midi.h"
#include "led.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ctrl";

typedef struct { uint8_t cc, val; } cc_req_t;
typedef struct { sys_cmd_t cmd; int arg; } sys_req_t;

static QueueHandle_t s_cc_q, s_sys_q;

void ctrl_send_cc(uint8_t cc, uint8_t value)
{
    cc_req_t r = { .cc = cc & 0x7F, .val = value & 0x7F };
    if (s_cc_q) xQueueSend(s_cc_q, &r, 0);
}

void sys_worker_post(sys_cmd_t cmd, int arg)
{
    sys_req_t r = { .cmd = cmd, .arg = arg };
    if (!s_sys_q || xQueueSend(s_sys_q, &r, 0) != pdTRUE) ESP_LOGW(TAG, "sys queue full, cmd %d dropped", cmd);
}

static uint8_t midi_out_channel(void)
{
    int ch = params_get_int(P_MIDI_CH);       /* 0 = omni: transmit on channel 1 */
    return (uint8_t)(ch <= 0 ? 0 : ch - 1);
}

static void ctrl_task(void *arg)
{
    uint32_t pend[PARAMS_MASK_WORDS] = { 0 }, taken[PARAMS_MASK_WORDS];
    TickType_t wake = xTaskGetTickCount();
    uint32_t tick = 0;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TF_CTRL_PERIOD_MS));
        tick++;

        usb_midi_poll_rx();

        /* ---- param changes -> USB-MIDI CC ---- */
        if (params_take_dirty(PL_MIDI, taken))
            for (int w = 0; w < PARAMS_MASK_WORDS; w++) pend[w] |= taken[w];
        if (!usb_midi_mounted()) {
            memset(pend, 0, sizeof pend);          /* nobody listening: do not build a backlog */
        } else {
            uint8_t const ch = midi_out_channel();
            for (int i = 0; i < P_COUNT; i++) {
                if (!params_mask_test(pend, (param_id_t)i)) continue;
                uint8_t const cc = params_cc_of((param_id_t)i);
                if (cc <= 127 && params_last_src((param_id_t)i) != SRC_MIDI) {
                    float const n = params_to_norm((param_id_t)i, params_get((param_id_t)i));
                    uint8_t const v = (uint8_t)lrintf(fminf(fmaxf(n, 0.f), 1.f) * 127.f);
                    if (!usb_midi_send_cc(ch, cc, v)) break;     /* TX FIFO full: retry next tick */
                }
                pend[i >> 5] &= ~(1u << (i & 31));
            }
        }

        /* ---- knob CCs from the UI ---- */
        cc_req_t r;
        while (xQueueReceive(s_cc_q, &r, 0) == pdTRUE) {
            uint8_t const ch = midi_out_channel();
            usb_midi_send_cc(ch, r.cc, r.val);
            if (ble_midi_state() == BLE_CONNECTED) ble_midi_send_cc(ch, r.cc, r.val);
        }

        ble_midi_poll();

        if (tick % (100 / TF_CTRL_PERIOD_MS) == 0)
            led_set_radio(net_wifi_state() != WIFI_OFF, ble_midi_state() != BLE_OFF);
    }
}

static void sys_task(void *arg)
{
    sys_req_t r;
    for (;;) {
        if (xQueueReceive(s_sys_q, &r, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = ESP_OK;
        switch (r.cmd) {
        case SYS_CMD_WIFI_ENABLE:
            err = net_wifi_enable(r.arg != 0);
            params_set(P_NET_WIFI, r.arg ? 1.f : 0.f, SRC_SYSTEM);
            break;
        case SYS_CMD_WIFI_SETUP:
            err = net_wifi_setup();
            params_set(P_NET_WIFI, 1.f, SRC_SYSTEM);
            break;
        case SYS_CMD_WIFI_FORGET:
            err = net_wifi_forget();
            break;
        case SYS_CMD_BLE_ENABLE:
            err = ble_midi_enable(r.arg != 0);
            params_set(P_NET_BLE, r.arg ? 1.f : 0.f, SRC_SYSTEM);
            break;
        case SYS_CMD_PRESET_SAVE:
            if (!storage_preset_save(r.arg, NULL)) err = ESP_FAIL;
            break;
        case SYS_CMD_FACTORY_RESET:
            net_wifi_enable(false);
            ble_midi_enable(false);
            storage_factory_reset();
            break;
        case SYS_CMD_OTA_CONFIRM:
            ota_confirm_apply(r.arg != 0);
            break;
        case SYS_CMD_REBOOT:
            storage_flush_now();
            net_wifi_enable(false);
            ble_midi_enable(false);
            ESP_LOGW(TAG, "reboot");
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
            break;
        default:
            break;
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "sys cmd %d (%d): %s", r.cmd, r.arg, esp_err_to_name(err));
    }
}

esp_err_t ctrl_start(void)
{
    s_cc_q = xQueueCreate(32, sizeof(cc_req_t));
    s_sys_q = xQueueCreate(8, sizeof(sys_req_t));
    if (!s_cc_q || !s_sys_q) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(ctrl_task, "ctrl", TF_CTRL_STACK, NULL, TF_CTRL_PRIO, NULL, TF_CTRL_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(sys_task, "sys", TF_SYS_STACK, NULL, TF_SYS_PRIO, NULL, TF_SYS_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
