/*
 * led.h: onboard WS2812 status LED (GPIO21, RMT). Very dim (never above ~5 %), mode from P_UI_LED:
 *   OFF    - dark
 *   SUBTLE - slow breathing colour by state: warm white (studio), blue (WiFi on), violet (BLE on),
 *            steady amber when muted, short red flash on input clip / output over
 *   METER  - colour and brightness follow the output level (green -> amber -> red)
 * A low-priority task (core 0, prio 2) updates it at 30 Hz.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_init(void);
/* Radio state for the SUBTLE colour (updated by the ctrl task). */
void      led_set_radio(bool wifi_on, bool ble_on);

#ifdef __cplusplus
}
#endif
