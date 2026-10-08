/*
 * ui_sys_fw.c: firmware implementation of ui_sys.h. All functions run in the UI task (core 0) and return
 * quickly: anything slow (radios, flash writes, reboot) is posted to the sys task (ctrl.c).
 */
#include "bootdiag.h"
#include <string.h>
#include "ui_sys.h"
#include "twofold.h"
#include "board_config.h"
#include "params.h"
#include "audio.h"
#include "usbdev.h"
#include "storage.h"
#include "display.h"
#include "net.h"
#include "ble_midi.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

void sys_get_status(sys_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->usb_mounted = usbdev_mounted();
    s->play_active = usbdev_play_active();
    s->rec_active  = usbdev_rec_active();
    s->sample_rate = BOARD_SAMPLE_RATE;
    s->clock_ext   = BOARD_CLOCK_EXT_OSC;

    s->wifi = net_wifi_state();
    net_fill_status(s);               /* ssid / pass / ip / rssi / ota */

    s->ble = ble_midi_state();
    const char *bn = ble_midi_name();
    if (bn) strncpy(s->ble_name, bn, sizeof s->ble_name - 1);

    s->preset_active = storage_preset_active();
    s->preset_dirty = storage_preset_dirty();
    for (int i = 0; i < 8; i++) storage_preset_name(i, s->preset_names[i]);
    s->storage_pending = storage_pending();
    s->fw_version = esp_app_get_description()->version;
    s->boot_note = bootdiag_note();
}

void sys_get_diag(sys_diag_t *d)
{
    memset(d, 0, sizeof *d);
    dsp_engine_get_prof(&d->prof);
    audio_stats_t a;
    audio_get_stats(&a);
    d->xrun_play = a.xrun_play;
    d->xrun_rec = a.xrun_rec;
    d->i2s_errors = a.i2s_errors;
    d->tx_late = a.tx_late;
    d->stalls_flash = a.stalls_flash;
    d->measured_fs = a.measured_fs;
    usbdev_stats_t u;
    usbdev_get_stats(&u);
    d->usb_out_fifo = u.out_fifo_bytes;
    d->usb_in_fifo = u.in_fifo_bytes;
    d->usb_feedback = u.feedback;
    d->heap_int_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    d->heap_int_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    d->heap_ext_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    d->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
}

void sys_get_meters(dsp_meters_t *m) { dsp_engine_get_meters(m, true); }

size_t sys_get_spectrum_samples(float *dst, size_t n) { return dsp_engine_spectrum_read(dst, n); }

void sys_get_viz(dsp_viz_t *v) { dsp_engine_get_viz(v); }

void sys_cmd(sys_cmd_t cmd, int arg)
{
    switch (cmd) {
    case SYS_CMD_PRESET_LOAD:
        storage_preset_load(arg);                 /* RAM only (blobs cached), crossfaded by the engine */
        break;
    case SYS_CMD_OLED_TYPE:
        params_set(P_UI_OLED, (float)arg, SRC_UI);
        display_set_type(arg ? DISPLAY_SH1106 : DISPLAY_SSD1306);   /* the UI task owns the display */
        break;
    default:
        sys_worker_post(cmd, arg);                /* radios, flash writes, reboot */
        break;
    }
}

void sys_display_set(uint8_t level, bool on) { display_set_level(level, on); }

void sys_midi_send_cc(uint8_t cc, uint8_t value) { ctrl_send_cc(cc, value); }
