/*
 * ui_sys.h: everything the portable UI needs from "the system".
 * Implemented by the firmware (main/ui_sys_fw.c) and by the host simulator (host/sim/ui_sys_sim.c).
 * All functions are called from the UI task only (core 0) and must never block for long.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WIFI_OFF = 0, WIFI_STARTING, WIFI_AP_SETUP, WIFI_STA_CONNECTING, WIFI_STA_CONNECTED, WIFI_ERROR } wifi_state_t;
typedef enum { BLE_OFF = 0, BLE_ADVERTISING, BLE_CONNECTED, BLE_ERROR } ble_state_t;

typedef struct {
    bool     usb_mounted;        /* enumerated by a host */
    bool     play_active;        /* host streaming to us (AS OUT alt 1) */
    bool     rec_active;         /* host recording from us (AS IN alt 1) */
    uint32_t sample_rate;        /* 48000 */
    bool     clock_ext;          /* low-jitter (external oscillator) build */
    wifi_state_t wifi;
    char     wifi_ssid[33];      /* STA ssid or setup AP ssid */
    char     wifi_pass[24];      /* setup AP password (shown on the setup screen) */
    char     ip[16];             /* "192.168.4.1" etc */
    int8_t   wifi_rssi;
    ble_state_t ble;
    char     ble_name[24];
    int      preset_active;      /* 0..7, -1 none */
    bool     preset_dirty;       /* edited since load */
    char     preset_names[8][16];
    bool     storage_pending;    /* unsaved changes waiting for an audio-safe window */
    bool     ota_in_progress;
    uint8_t  ota_pct;
    const char *fw_version;
} sys_status_t;

typedef struct {
    dsp_prof_t prof;
    uint32_t xrun_play;          /* USB OUT FIFO underruns (blocks padded with silence) */
    uint32_t xrun_rec;           /* USB IN FIFO overflows */
    uint32_t i2s_errors;         /* DMA overflow/underflow events */
    int32_t  usb_out_fifo;       /* bytes in TinyUSB OUT FIFO */
    int32_t  usb_in_fifo;
    float    usb_feedback;       /* frames per ms reported to host (e.g. 48.0013) */
    uint32_t heap_int_free, heap_int_min, heap_ext_free;
    uint32_t uptime_s;
    float    measured_fs;        /* I2S frames/s measured against esp_timer */
} sys_diag_t;

typedef enum {
    SYS_CMD_WIFI_ENABLE = 0,     /* arg: 0/1 */
    SYS_CMD_WIFI_SETUP,          /* start AP + captive portal provisioning */
    SYS_CMD_WIFI_FORGET,         /* erase STA credentials */
    SYS_CMD_BLE_ENABLE,          /* arg: 0/1 */
    SYS_CMD_PRESET_LOAD,         /* arg: slot 0..7 (crossfaded) */
    SYS_CMD_PRESET_SAVE,         /* arg: slot 0..7 (current state -> slot, brief soft-mute while writing) */
    SYS_CMD_OLED_TYPE,           /* arg: 0 SSD1306, 1 SH1106 (re-inits display) */
    SYS_CMD_FACTORY_RESET,
    SYS_CMD_REBOOT,
} sys_cmd_t;

void  sys_get_status(sys_status_t *s);
void  sys_get_diag(sys_diag_t *d);
void  sys_get_meters(dsp_meters_t *m);          /* takes (resets) the since-last maxima */
size_t sys_get_spectrum_samples(float *dst, size_t n);  /* most recent mono samples @48k */
void  sys_get_viz(dsp_viz_t *v);
void  sys_cmd(sys_cmd_t cmd, int arg);

/* Display control (brightness 0..255 perceptual level after the UI's curve; on=false blanks panel). */
void  sys_display_set(uint8_t level, bool on);

/* MIDI: the UI asks to send a CC when the knob is in MIDI mode (channel from params). */
void  sys_midi_send_cc(uint8_t cc, uint8_t value);

#ifdef __cplusplus
}
#endif
