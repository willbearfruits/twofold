/*
 * usbdev.h: TWOFOLD USB device (TinyUSB 0.21): UAC2 2-in/2-out 48 kHz/24-bit (async, explicit feedback)
 * + USB-MIDI 1.0. PLAN.md 9.
 *
 * Threads
 *   - TinyUSB device task: core 0, prio 20 (created by usbdev_init). The DWC2 ISR is allocated on core 0.
 *   - Audio task (core 1) is the only reader of the playback FIFO and the only writer of the record FIFO
 *     (lock-free SPSC with the USB ISR): usbdev_play_* / usbdev_rec_*.
 *   - The ctrl task (core 0) owns MIDI: usb_midi_poll_rx() / usb_midi_send_cc().
 *
 * Rate control: playback uses UAC2 asynchronous explicit feedback computed by TinyUSB from the OUT FIFO
 * level (AUDIO_FEEDBACK_METHOD_FIFO_COUNT); record uses TinyUSB's IN flow control (47/48/49 frames per
 * packet from the IN FIFO level). Both FIFOs are drained/filled by the I2S-paced audio task, so both
 * loops follow whichever clock is master (ESP32 PLL or the external oscillator), see usbdev.c.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USBDEV_SAMPLE_RATE      48000
#define USBDEV_FRAME_BYTES      8           /* 2 ch x 4-byte subslot (int32, 24-bit left-justified) */

/* Initialise the USB PHY (internal, OTG device mode on GPIO19/20) and start the TinyUSB task. */
esp_err_t usbdev_init(void);

bool     usbdev_mounted(void);
bool     usbdev_play_active(void);              /* host streams to us (AS OUT alt 1) */
bool     usbdev_rec_active(void);               /* host records from us (AS IN alt 1) */
static inline bool usbdev_stream_active(void) { return usbdev_play_active() || usbdev_rec_active(); }

/* Incremented on every SET_INTERFACE of the respective AS interface. The audio task compares these to
 * detect stream (re)starts even when alt0 -> alt1 happens within one audio block. */
uint32_t usbdev_play_gen(void);
uint32_t usbdev_rec_gen(void);

/* Reported to the host as the clock source's CLOCK_VALID control (the audio component clears it in
 * EXT_OSC mode while no BCK/LRCK arrives from the PCM1808). */
void     usbdev_set_clock_valid(bool valid);

/* ---- audio task side (core 1, lock-free, IRAM) ---- */
uint32_t usbdev_play_target_bytes(void);        /* OUT FIFO level the feedback regulates to */
uint32_t usbdev_play_available(void);           /* bytes in the OUT FIFO */
uint32_t usbdev_play_read(void *dst, uint32_t bytes);
uint32_t usbdev_rec_target_bytes(void);         /* IN FIFO level the flow control regulates to */
uint32_t usbdev_rec_count(void);                /* bytes in the IN FIFO */
uint32_t usbdev_rec_space(void);
uint32_t usbdev_rec_write(const void *src, uint32_t bytes);

/* ---- diagnostics (any core-0 task) ---- */
typedef struct {
    int32_t  out_fifo_bytes;
    int32_t  in_fifo_bytes;
    float    feedback;           /* frames per 1 ms frame currently reported to the host (mirror of TinyUSB's
                                    FIFO_COUNT computation) */
    float    host_rate;          /* frames per ms actually received from the host (last ~1 s) */
    uint32_t out_packets;
    uint32_t in_packets;
} usbdev_stats_t;
void usbdev_get_stats(usbdev_stats_t *s);

/* USB-MIDI SysEx service channel (manufacturer ID 0x7D). Complete incoming messages addressed to 0x7D are passed
 * to usb_midi_sysex_received() (weak; main/hostlink.c implements it), from the ctrl task. */
void usb_midi_sysex_received(const uint8_t *msg, size_t len);
bool usb_midi_send_sysex(const uint8_t *buf, size_t len, uint32_t timeout_ms);
/* Leave the application for the ROM serial/JTAG download mode (host-driven flashing, no BOOT button). */
void usbdev_reboot_to_bootloader(void);

/* ---- USB-MIDI 1.0 (ctrl task) ---- */
bool usb_midi_mounted(void);
/* Drain received USB-MIDI packets: CC on the configured channel (P_MIDI_CH, 0 = omni) of a mapped CC
 * -> params_set_norm(..., SRC_MIDI). Returns the number of CCs applied. */
int  usb_midi_poll_rx(void);
/* ch: 0..15. Returns false if not mounted or the TX FIFO is full. */
bool usb_midi_send_cc(uint8_t ch, uint8_t cc, uint8_t val);

#ifdef __cplusplus
}
#endif
