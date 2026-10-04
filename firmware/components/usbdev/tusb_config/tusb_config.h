/*
 * tusb_config.h: TinyUSB 0.21 configuration for TWOFOLD (UAC2 2-in/2-out 48 kHz/24-bit + USB-MIDI 1.0).
 *
 * This directory is injected into the espressif__tinyusb component target from firmware/CMakeLists.txt
 * (esp_tinyusb is not used: its generated tusb_config.h has no audio class).
 * Endpoint / FIFO budget and the reasoning behind every size: see usb_descriptors.c.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* CFG_TUSB_MCU (OPT_MCU_ESP32S3) is defined on the command line by the TinyUSB component. */
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH        freertos/
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG              0       /* logging inside the USB ISR would break the ISO timing */
#endif

/* Device stack on the full-speed OTG port (rhport 0). Slave (non-DMA) mode: the ISR copies the DWC2
 * hardware FIFOs straight into/out of the class tu_fifo buffers (CFG_TUD_EDPT_DEDICATED_HWFIFO). */
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE      64

/* ---------------- classes ---------------- */
#define CFG_TUD_AUDIO               1
#define CFG_TUD_MIDI                1
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_VENDOR              0

/* ---------------- UAC2 ---------------- */
/* Shared with the descriptors and the audio task (keep in one place). */
#define TF_USB_SAMPLE_RATE          48000
#define TF_USB_CHANNELS             2
#define TF_USB_SUBSLOT_BYTES        4       /* 24-bit samples left-justified in 4-byte subslots */
#define TF_USB_RESOLUTION_BITS      24
#define TF_USB_FRAME_BYTES          (TF_USB_CHANNELS * TF_USB_SUBSLOT_BYTES)              /* 8 */
/* Largest packet: nominal 48 frames + 1 (the async feedback and IN flow control vary by +-1 frame). */
#define TF_USB_EP_AUDIO_SZ          ((TF_USB_SAMPLE_RATE / 1000 + 1) * TF_USB_FRAME_BYTES) /* 392 */
/* Software FIFOs: 8 ms each (PLAN.md 6), regulated to half full = 4 ms (PLAN.md 4.5). The target is the
 * single knob for the USB share of the latency; >= 2 ms keeps a safe margin (see usbdev.c). */
#define TF_USB_FIFO_BYTES           (8 * (TF_USB_SAMPLE_RATE / 1000) * TF_USB_FRAME_BYTES) /* 3072 */
#define TF_USB_FIFO_TARGET_BYTES    (4 * (TF_USB_SAMPLE_RATE / 1000) * TF_USB_FRAME_BYTES) /* 1536 */

#define CFG_TUD_AUDIO_CTRL_BUF_SZ               64
#define CFG_TUD_AUDIO_ENABLE_EP_IN              1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT             1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP        1
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP       0
/* IN flow control: the driver picks 47/48/49-frame packets from the IN FIFO level vs. its threshold. */
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL        1
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       TF_USB_EP_AUDIO_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX      TF_USB_EP_AUDIO_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    TF_USB_FIFO_BYTES
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ   TF_USB_FIFO_BYTES

/* ---------------- USB-MIDI 1.0 ---------------- */
/* 32-byte bulk packets (legal at FS: 8/16/32/64). 64 would not fit the 256-word DWC2 FIFO RAM. */
#define TF_USB_EP_MIDI_SZ           32
#define CFG_TUD_MIDI_EP_BUFSIZE     TF_USB_EP_MIDI_SZ
#define CFG_TUD_MIDI_RX_BUFSIZE     128
#define CFG_TUD_MIDI_TX_BUFSIZE     256

#ifdef __cplusplus
}
#endif
