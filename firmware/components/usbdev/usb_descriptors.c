/*
 * usb_descriptors.c: device / configuration / string descriptors for TWOFOLD.
 *
 * Composite device with IADs (bDeviceClass 0xEF/0x02/0x01):
 *
 *  IAD 0 (UAC2, itf 0..2)
 *   itf 0  AudioControl (UAC2, no interrupt EP)
 *          Clock Source 0x10: internal fixed 48 kHz. Sample-freq control READ-ONLY, clock-valid READ-ONLY.
 *          IT 0x01 USB streaming (2 ch, FL|FR) -> OT 0x02 line connector (DAC)       [playback]
 *          IT 0x03 line connector (ADC)      -> OT 0x04 USB streaming               [record]
 *          No Feature Unit: the encoder is the hardware master volume and the OS volume stays a
 *          bit-transparent software control (PLAN.md 9).
 *   itf 1  AS OUT: alt0 zero-bandwidth, alt1 2ch PCM 24-bit in 4-byte subslots,
 *          ISO OUT 0x01 async data (392 B, 1 ms) + ISO IN 0x81 explicit feedback (4 B, 1 ms)
 *   itf 2  AS IN:  alt0 zero-bandwidth, alt1 2ch PCM 24-bit in 4-byte subslots, ISO IN 0x82 async (392 B, 1 ms)
 *  IAD 1 (USB-MIDI 1.0, itf 3..4)
 *   itf 3  AudioControl (UAC1 header, MIDI)
 *   itf 4  MIDIStreaming: 1 embedded + 1 external jack each way, bulk OUT 0x03 / IN 0x83 (32 B)
 *
 * Endpoint budget (ESP32-S3 DWC2: EP0..EP6, max 5 concurrently allocated IN EPs incl. EP0 IN):
 *   IN : EP0, 0x81 feedback, 0x82 audio, 0x83 MIDI   = 4 of 5
 *   OUT: EP0, 0x01 audio, 0x03 MIDI
 *
 * DWC2 FIFO RAM budget (256 x 32-bit words shared by the RX FIFO and every TX FIFO). Checked at compile
 * time below; the RX formula is TinyUSB's with the 1x-packet patch from firmware/CMakeLists.txt:
 *   RX  = 13 (setup) + 1 (global NAK) + (392/4 + 1) (one largest packet + status) + 2*7 (per OUT EP) = 127
 *   TX  = EP0 64 B (16) + feedback 4 B (1) + audio IN 392 B (98) + MIDI IN 32 B (8)                 = 123
 *   total 250 / 256 words.
 */
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "esp_mac.h"

#define TF_TERM_TYPE_LINE_CONNECTOR 0x0603   /* UAC "External Terminal Types": line connector */

/* ------------------------------------------------------------------------------------------------
 * FIFO budget check (see header comment). Mirrors calc_device_grxfsiz() (patched) and dfifo_alloc().
 * ------------------------------------------------------------------------------------------------ */
#define DWC2_FIFO_WORDS        256
#define DWC2_EP_COUNT          7
#define W(bytes)               (((bytes) + 3) / 4)
#define DWC2_RX_WORDS          (13 + 1 + (W(TF_USB_EP_AUDIO_SZ) + 1) + 2 * DWC2_EP_COUNT)
#define DWC2_TX_WORDS          (W(CFG_TUD_ENDPOINT0_SIZE) + W(4) + W(TF_USB_EP_AUDIO_SZ) + W(TF_USB_EP_MIDI_SZ) + W(TF_USB_EP_VENDOR_SZ))
_Static_assert(DWC2_RX_WORDS + DWC2_TX_WORDS <= DWC2_FIFO_WORDS, "DWC2 FIFO RAM overflow: shrink an endpoint");
_Static_assert(TF_USB_EP_AUDIO_SZ == 392, "2ch x 4B x 49 frames");
_Static_assert(TF_USB_FIFO_BYTES >= 4 * TF_USB_EP_AUDIO_SZ, "FIFO_COUNT feedback needs >= 4 packets of FIFO");

/* ------------------------------------------------------------------------------------------------ */
static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,                  /* 2.1: BOS descriptor (WebUSB + MS OS 2.0) */
    .bDeviceClass       = TUSB_CLASS_MISC,         /* IAD composite */
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = TF_USB_VID,
    .idProduct          = TF_USB_PID,
    .bcdDevice          = TF_USB_BCD_DEVICE,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&s_desc_device; }

/* ---------------- UAC2 function ---------------- */
#define AC_CS_TOTAL_LEN (TUD_AUDIO20_DESC_CLK_SRC_LEN + 2 * TUD_AUDIO20_DESC_INPUT_TERM_LEN + 2 * TUD_AUDIO20_DESC_OUTPUT_TERM_LEN)

#define UAC2_FUNC_LEN (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN + AC_CS_TOTAL_LEN \
    /* AS OUT */ + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN               \
    + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN            \
    + TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP_LEN                                                                                  \
    /* AS IN */ + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN                 \
    + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

#define TF_CH_CFG_STEREO (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT)
#define TF_ISO_ASYNC_DATA ((uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_DATA))

#define MIDI_IAD_LEN  8
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + UAC2_FUNC_LEN + MIDI_IAD_LEN + TUD_MIDI_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const uint8_t s_desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),

    /* ======== UAC2 audio function ======== */
    TUD_AUDIO20_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 3, STRID_AUDIO),
    TUD_AUDIO20_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0, STRID_AUDIO),
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_IO_BOX, AC_CS_TOTAL_LEN, 0x00),
    /* Clock source: internal fixed; frequency R, validity R. */
    TUD_AUDIO20_DESC_CLK_SRC(UAC2_ENTITY_CLOCK, AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                             (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS) |
                             (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_VAL_POS),
                             0x00, 0x00),
    /* Playback: USB streaming IT -> line-out OT */
    TUD_AUDIO20_DESC_INPUT_TERM(UAC2_ENTITY_PLAY_IT, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, UAC2_ENTITY_CLOCK,
                                TF_USB_CHANNELS, TF_CH_CFG_STEREO, 0x00, 0x0000, 0x00),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC2_ENTITY_PLAY_OT, TF_TERM_TYPE_LINE_CONNECTOR, 0x00, UAC2_ENTITY_PLAY_IT,
                                 UAC2_ENTITY_CLOCK, 0x0000, STRID_LINE_OUT),
    /* Record: line-in IT -> USB streaming OT */
    TUD_AUDIO20_DESC_INPUT_TERM(UAC2_ENTITY_REC_IT, TF_TERM_TYPE_LINE_CONNECTOR, 0x00, UAC2_ENTITY_CLOCK,
                                TF_USB_CHANNELS, TF_CH_CFG_STEREO, 0x00, 0x0000, STRID_LINE_IN),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC2_ENTITY_REC_OT, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, UAC2_ENTITY_REC_IT,
                                 UAC2_ENTITY_CLOCK, 0x0000, 0x00),

    /* ---- AS OUT (playback) ---- */
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_OUT, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_OUT, 0x01, 0x02, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC2_ENTITY_PLAY_IT, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM,
                               TF_USB_CHANNELS, TF_CH_CFG_STEREO, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(TF_USB_SUBSLOT_BYTES, TF_USB_RESOLUTION_BITS),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT, TF_ISO_ASYNC_DATA, TF_USB_EP_AUDIO_SZ, 1),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
    /* Explicit feedback: 4 bytes = 16.16 (see usbdev.c for the FS format discussion), every frame. */
    TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP(EPNUM_AUDIO_FB, 4, 1),

    /* ---- AS IN (record) ---- */
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_IN, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_IN, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC2_ENTITY_REC_OT, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM,
                               TF_USB_CHANNELS, TF_CH_CFG_STEREO, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(TF_USB_SUBSLOT_BYTES, TF_USB_RESOLUTION_BITS),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN, TF_ISO_ASYNC_DATA, TF_USB_EP_AUDIO_SZ, 1),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),

    /* ======== USB-MIDI 1.0 function ======== */
    /* IAD: function class = class/subclass of its first interface (Audio/AudioControl, protocol 0), so
     * Windows binds it to usbaudio.sys (MIDI), never to usbaudio2.sys. */
    MIDI_IAD_LEN, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_MIDI_CONTROL, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL,
    AUDIO_FUNC_PROTOCOL_CODE_UNDEF, STRID_MIDI,
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI_CONTROL, STRID_MIDI, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, TF_USB_EP_MIDI_SZ),

    /* ======== vendor interface: WebUSB (Chrome) / WinUSB (Windows via MS OS 2.0) ======== */
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRID_VENDOR, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, TF_USB_EP_VENDOR_SZ),
};
_Static_assert(sizeof(s_desc_config) == CONFIG_TOTAL_LEN, "configuration descriptor length mismatch");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_config;
}

/* ---------------- strings ---------------- */
static const char *const s_strings[STRID_COUNT] = {
    [STRID_MANUFACTURER] = "Twofold",
    [STRID_PRODUCT]      = "TWOFOLD",
    [STRID_SERIAL]       = NULL,            /* from the factory MAC */
    [STRID_AUDIO]        = "TWOFOLD",
    [STRID_MIDI]         = "TWOFOLD MIDI",
    [STRID_LINE_OUT]     = "Line Out",
    [STRID_LINE_IN]      = "Line In",
    [STRID_VENDOR]       = "TWOFOLD Control",
};

static uint16_t s_str_buf[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t n = 0;
    if (index == STRID_LANGID) {
        s_str_buf[1] = 0x0409;
        n = 1;
    } else if (index == STRID_SERIAL) {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        static const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 6; i++) {
            s_str_buf[1 + n++] = (uint16_t)hex[mac[i] >> 4];
            s_str_buf[1 + n++] = (uint16_t)hex[mac[i] & 15];
        }
    } else if (index < STRID_COUNT && s_strings[index]) {
        const char *s = s_strings[index];
        n = strlen(s);
        if (n > 32) n = 32;
        for (size_t i = 0; i < n; i++) s_str_buf[1 + i] = (uint16_t)(uint8_t)s[i];
    } else {
        return NULL;
    }
    s_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return s_str_buf;
}

/* ------------------------------------------------------------------------------------------------
 * BOS: WebUSB platform capability (so Chrome lets a page claim the vendor interface) and the Microsoft OS 2.0
 * descriptor set (Windows binds WinUSB to the vendor interface automatically: no driver, no Zadig).
 * ------------------------------------------------------------------------------------------------ */
enum { VENDOR_REQUEST_WEBUSB = 1, VENDOR_REQUEST_MICROSOFT = 2 };
#define BOS_TOTAL_LEN     (TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN 0xB2

static const uint8_t s_desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 2),
    TUD_BOS_WEBUSB_DESCRIPTOR(VENDOR_REQUEST_WEBUSB, 0),              /* no landing page */
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};
_Static_assert(sizeof(s_desc_bos) == BOS_TOTAL_LEN, "BOS length");

uint8_t const *tud_descriptor_bos_cb(void) { return s_desc_bos; }

static const uint8_t s_desc_ms_os_20[] = {
    /* set header */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    /* configuration subset header */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
    /* function subset header: the vendor interface only */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
    /* compatible ID: WINUSB */
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ) */
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY), U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, '6', 0, 'B', 0, '0', 0, 'F', 0, '4', 0, '2', 0, 'A', 0, '1', 0, '-', 0, '2', 0, 'C', 0, '7', 0, 'E', 0, '-', 0,
    '4', 0, 'D', 0, '1', 0, '9', 0, '-', 0, '9', 0, 'A', 0, '0', 0, '5', 0, '-', 0, '7', 0, 'F', 0, '2', 0, '1', 0, 'C', 0,
    '0', 0, 'B', 0, 'E', 0, '5', 0, '3', 0, 'D', 0, '4', 0, '}', 0, 0, 0, 0, 0,
};
_Static_assert(sizeof(s_desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
        if (request->bRequest == VENDOR_REQUEST_MICROSOFT && request->wIndex == 7) {
            uint16_t total;
            memcpy(&total, s_desc_ms_os_20 + 8, 2);
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_desc_ms_os_20, total);
        }
        return false;                                     /* WebUSB GET_URL: no landing page */
    }
    return false;
}
