/*
 * net.h: opt-in WiFi (Studio Mode = fully de-initialised).
 * Setup flow: AP "TWOFOLD-XXXX" + captive portal -> user picks network -> credentials saved
 * (storage_kv_set) -> station mode. Web editor + preset management + OTA served in both modes.
 * Everything runs on core 0. Reduced TX power; modem sleep in STA.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "ui_sys.h"   /* wifi_state_t */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_init(void);                 /* registers nothing radio-related; cheap */
esp_err_t net_wifi_enable(bool on);       /* on: STA if credentials exist else AP setup; off: full deinit */
esp_err_t net_wifi_setup(void);           /* force AP + captive portal provisioning */
esp_err_t net_wifi_forget(void);
wifi_state_t net_wifi_state(void);
/* Fills ssid/pass/ip/rssi/ota fields of sys_status_t. */
void      net_fill_status(sys_status_t *s);

#ifdef __cplusplus
}
#endif
