/*
 * display.h: 128x64 SSD1306 / SH1106 OLED over I2C (IDF 5.5 i2c_master, interrupt driven).
 *
 * Owned by the UI task (core 0): every function must be called from that one task. The I2C interrupt is
 * allocated on the core that calls display_init(), so call it from the UI task, never from core 1.
 *
 * Framebuffer layout ("page layout", what gfx_t holds): fb[page * 128 + x], 8 pages; bit n of a byte is
 * row page * 8 + n. This is the controllers' native GDDRAM layout.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_W       128
#define DISPLAY_H       64
#define DISPLAY_FB_SIZE (DISPLAY_W * DISPLAY_H / 8)

typedef enum { DISPLAY_SSD1306 = 0, DISPLAY_SH1106 = 1 } display_type_t;   /* = P_UI_OLED values */

/* Create the I2C bus (once), probe, initialise the controller and clear it. Returns ESP_ERR_NOT_FOUND if
 * nothing answers at BOARD_OLED_ADDR; the driver then keeps retrying from display_flush() every ~2 s. */
esp_err_t      display_init(display_type_t type);
/* Switch controller type at runtime (Settings > OLED type): re-initialises if it differs. */
esp_err_t      display_set_type(display_type_t type);
display_type_t display_get_type(void);
bool           display_present(void);

/* Send the frame. Keeps a shadow of the last frame and only transmits changed column spans per page
 * (one I2C transaction per span: page/column commands + data). Blocks the caller (not the CPU: the
 * transfer is interrupt driven) for the duration of the transfer. */
esp_err_t      display_flush(const uint8_t fb[DISPLAY_FB_SIZE]);
/* Force the next flush to send the whole frame. */
void           display_invalidate(void);

/* Brightness: perceptual level 0..255 (after the UI's curve) mapped to contrast, plus a lower
 * precharge / VCOMH setting at the low end for deeper dimming. on = false turns the panel off (RAM kept). */
void           display_set_level(uint8_t level, bool on);

typedef struct { uint32_t frames, bytes, errors; uint32_t last_flush_us; } display_stats_t;
void           display_get_stats(display_stats_t *s);

#ifdef __cplusplus
}
#endif
