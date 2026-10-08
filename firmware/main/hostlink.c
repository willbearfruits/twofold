/*
 * hostlink.c: the USB-MIDI SysEx service channel (manufacturer ID 0x7D), for host tools with no radio involved.
 *
 *   F0 7D 01 F7               -> F0 7D 02 <ascii crash report> F7           (tools/crash_report.sh)
 *   F0 7D 03 F7               -> F0 7D 04 <ascii status report> F7          (tools/twofold_status.sh)
 *   F0 7D 05 F7               -> profile with interrupts masked for 3 s (diagnostic)
 *   F0 7D 07 F7               -> F0 7D 08 <sub-stage cycles/frame> F7
 *   F0 7D 09 0x F7 / 0A 0x F7 -> WiFi / BLE off (00) or on (01)
 *   F0 7D 7E 42 4F 4F 54 F7   -> F0 7D 7F F7, then reboot into the ROM download mode ("BOOT"; tools/flash.sh)
 *
 * Runs in the ctrl task (core 0). Replies are plain 7-bit ASCII.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bootdiag.h"
#include "dsp_engine.h"
#include "dsp_port.h"
#include "params.h"
#include "storage.h"
#include "ui_sys.h"
#include "usbdev.h"
#include "audio.h"
#include "esp_cpu.h"
#include "esp_timer.h"

#define REPLY_MAX 600

static uint8_t s_reply[REPLY_MAX];

static void send_text(uint8_t kind, size_t (*fill)(char *, size_t))
{
    s_reply[0] = 0xF0; s_reply[1] = 0x7D; s_reply[2] = kind;
    size_t k = fill((char *)s_reply + 3, REPLY_MAX - 5);
    for (size_t i = 0; i < k; i++) s_reply[3 + i] &= 0x7F;
    s_reply[3 + k] = 0xF7;
    usb_midi_send_sysex(s_reply, k + 4, 200);
}

static unsigned stack_free(const char *name)
{
    TaskHandle_t t = xTaskGetHandle(name);
    return t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0u;      /* bytes never used (IDF: StackType_t = byte) */
}

static size_t dbg_report(char *b, size_t n)
{
    static const char *const nm[DSP_DBG_N] = { "sat.up1", "sat.up2", "sat.shape", "sat.dn2", "sat.dn1", "-", "in.setup",
                                               "in.conv", "in.rest", "cho.lfo", "cho.render", "-", "cho.mix",
                                               "rev.in+pre", "rev.decim+diff", "rev.taps", "rev.tank", "rev.interp",
                                               "rev.mix", "frames" };
    uint32_t d[DSP_DBG_N];
    dsp_engine_get_dbg(d);
    const double fr = d[DSP_DBG_N - 1] ? (double)d[DSP_DBG_N - 1] : 1.0;
    int k = snprintf(b, n, "cycles/frame:");
    for (int i = 0; i < DSP_DBG_N - 1 && k > 0 && (size_t)k < n; i++)
        if (d[i]) k += snprintf(b + k, n - (size_t)k, " %s %.0f", nm[i], (double)d[i] / fr);
    if (k > 0 && (size_t)k < n) k += snprintf(b + k, n - (size_t)k, " (over %lu frames)", (unsigned long)d[DSP_DBG_N - 1]);
    if (k < 0) k = 0;
    return (size_t)k < n ? (size_t)k : n - 1;
}

static size_t crash_report(char *b, size_t n) { return bootdiag_report(b, n); }

static size_t status_report(char *b, size_t n)
{
    sys_diag_t d;
    sys_status_t st;
    sys_get_diag(&d);
    sys_get_status(&st);
    /* CPU clock, measured: cycles over 20 ms of esp_timer */
    uint32_t c0 = esp_cpu_get_cycle_count(); int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t c1 = esp_cpu_get_cycle_count(); int64_t t1 = esp_timer_get_time();
    const double mhz = (double)(c1 - c0) / (double)(t1 - t0);
    int k = snprintf(b, n, "mhz %.1f | fs %.1f | cpu %.1f avg %.1f max |", mhz, (double)d.measured_fs,
                     (double)d.prof.total_avg_pct, (double)d.prof.total_max_pct);
    for (int i = 0; i < DSP_ST_COUNT && k > 0 && (size_t)k < n; i++)
        k += snprintf(b + k, n - (size_t)k, " %s %.2f/%.2f", dsp_stage_names[i], (double)d.prof.avg_pct[i],
                      (double)d.prof.max_pct[i]);
    if (k > 0 && (size_t)k < n)
        k += snprintf(b + k, n - (size_t)k,
                      " | fx%s%s%s%s%s%s | xrun play %lu rec %lu | i2s err %lu | fb %.4f | fifo out %ld in %ld"
                      " | heap int %lu min %lu psram %lu | stack free audio %u ctrl %u ui %u | up %lus | usb %s%s%s | wifi %d ble %d",
                      params_get_bool(P_SAT_ON) ? " sat" : "", params_get_bool(P_CHO_ON) ? " cho" : "",
                      params_get_bool(P_DLY_ON) ? " dly" : "", params_get_bool(P_REV_ON) ? " rev" : "",
                      params_get_bool(P_CMP_ON) ? " cmp" : "", params_get(P_MON_MIX) > 0.f ? " mon" : "",
                      (unsigned long)d.xrun_play, (unsigned long)d.xrun_rec, (unsigned long)d.i2s_errors,
                      (double)d.usb_feedback, (long)d.usb_out_fifo, (long)d.usb_in_fifo,
                      (unsigned long)d.heap_int_free, (unsigned long)d.heap_int_min, (unsigned long)d.heap_ext_free,
                      stack_free("audio"), stack_free("ctrl"), stack_free("ui"), (unsigned long)d.uptime_s, st.usb_mounted ? "up" : "down", st.play_active ? " play" : "",
                      st.rec_active ? " rec" : "", (int)st.wifi, (int)st.ble);
    if (k < 0) k = 0;
    return (size_t)k < n ? (size_t)k : n - 1;
}

void usb_midi_sysex_received(const uint8_t *m, size_t len)
{
    /* m[0] = F0, m[1] = 7D, m[len-1] = F7 */
    if (len == 4 && m[2] == 0x01) { send_text(0x02, crash_report); return; }
    if (len == 4 && m[2] == 0x03) { send_text(0x04, status_report); return; }
    if (len == 5 && m[2] == 0x09) { sys_cmd(SYS_CMD_WIFI_ENABLE, m[3] ? 1 : 0); return; }   /* radios, for measurements */
    if (len == 5 && m[2] == 0x0A) { sys_cmd(SYS_CMD_BLE_ENABLE, m[3] ? 1 : 0); return; }
    if (len == 4 && m[2] == 0x07) { send_text(0x08, dbg_report); return; }
    if (len == 4 && m[2] == 0x05) { audio_diag_irqoff(3000); return; }   /* profile with interrupts masked for 3 s */
    if (len == 8 && m[2] == 0x7E && !memcmp(m + 3, "BOOT", 4)) {
        static const uint8_t ack[] = { 0xF0, 0x7D, 0x7F, 0xF7 };
        usb_midi_send_sysex(ack, sizeof ack, 50);
        dsp_engine_soft_mute(true);                  /* no click on the way down */
        for (int i = 0; i < 50 && !dsp_engine_is_silent(); i++) vTaskDelay(pdMS_TO_TICKS(1));
        if (storage_pending()) storage_flush_now();  /* keep the last settings */
        vTaskDelay(pdMS_TO_TICKS(50));               /* let the ack leave */
        usbdev_reboot_to_bootloader();
    }
}
