/*
 * ble_midi.h: opt-in BLE-MIDI peripheral (NimBLE). Remote control only. The ESP32-S3 has no
 * Bluetooth Classic, so there is no A2DP audio streaming.
 * Incoming CCs -> params (SRC_BLE); param changes -> CC notifications (PL_BLE listener).
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "ui_sys.h"   /* ble_state_t */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t   ble_midi_enable(bool on);     /* off: host + controller fully de-initialised */
ble_state_t ble_midi_state(void);
const char *ble_midi_name(void);
void        ble_midi_poll(void);          /* called every few ms from the ctrl task: param -> CC out */
void        ble_midi_send_cc(uint8_t ch, uint8_t cc, uint8_t val);

#ifdef __cplusplus
}
#endif
