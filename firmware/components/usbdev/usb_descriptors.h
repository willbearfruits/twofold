/* usb_descriptors.h: interface / endpoint / entity numbering shared by the descriptors and the callbacks. */
#pragma once

/* VID 0x303A = Espressif. PID: PLACEHOLDER, NOT ALLOCATED. A shipping product must obtain its own
 * VID/PID (or an Espressif-allocated PID). Bump bcdDevice whenever the descriptors change, because
 * Windows and macOS cache them per VID/PID/bcdDevice. */
#define TF_USB_VID                  0x303A
#define TF_USB_PID                  0x4A57      /* PLACEHOLDER */
#define TF_USB_BCD_DEVICE           0x0102      /* 0x0102: vendor interface + BOS added */

enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_OUT,    /* playback: host -> DAC */
    ITF_NUM_AUDIO_STREAMING_IN,     /* record:   ADC -> host */
    ITF_NUM_MIDI_CONTROL,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_VENDOR,                 /* WebUSB / WinUSB control channel (the web editor over USB) */
    ITF_NUM_TOTAL
};

/* Endpoints. ESP32-S3 DWC2: EP0..6, at most 5 IN endpoints active incl. EP0 IN (all 5 used). */
#define EPNUM_AUDIO_OUT             0x01
#define EPNUM_AUDIO_FB              0x81
#define EPNUM_AUDIO_IN              0x82
#define EPNUM_MIDI_OUT              0x03
#define EPNUM_MIDI_IN               0x83
#define EPNUM_VENDOR_OUT            0x04
#define EPNUM_VENDOR_IN             0x84
#define TF_USB_EP_VENDOR_SZ         32

/* UAC2 entity IDs */
#define UAC2_ENTITY_CLOCK           0x10
#define UAC2_ENTITY_PLAY_IT         0x01    /* USB streaming in   -> */
#define UAC2_ENTITY_PLAY_OT         0x02    /* line out (DAC)        */
#define UAC2_ENTITY_REC_IT          0x03    /* line in (ADC)      -> */
#define UAC2_ENTITY_REC_OT          0x04    /* USB streaming out     */

/* String indices */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_AUDIO,
    STRID_MIDI,
    STRID_LINE_OUT,
    STRID_LINE_IN,
    STRID_VENDOR,
    STRID_COUNT
};
