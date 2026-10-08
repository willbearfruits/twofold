/*
 * usbdev.c: USB PHY + TinyUSB task, UAC2 control requests, stream state, feedback and flow control.
 *
 * ---------------------------------------------------------------------------------------------------
 * Rate control, and why it follows whichever clock is master
 * ---------------------------------------------------------------------------------------------------
 * Playback (host -> device, ISO OUT 0x01, asynchronous): explicit feedback on ISO IN 0x81, computed by
 * TinyUSB with AUDIO_FEEDBACK_METHOD_FIFO_COUNT. After each OUT packet the driver low-pass filters the
 * OUT FIFO level (1/64 per packet) and reports nominal (48.0 frames/ms) +- a correction proportional to
 * the distance from the target level, clamped to [47, 49] frames/ms (UAC2 FMT-2.0 2.3.1.1 and Windows'
 * "+-1 audio slot" rule). The only consumer of that FIFO is the audio task, which is paced by
 * i2s_channel_read(), i.e. by the converter clock. In INTERNAL mode that is the ESP32-S3 PLL/MCLK, in
 * EXT_OSC mode it is the 12.288 MHz oscillator via the PCM1808 BCK/LRCK. Either way the host ends up
 * sending exactly what the converters consume; nothing in this file depends on the clock mode.
 *
 * Record (device -> host, ISO IN 0x82, asynchronous, no feedback needed): TinyUSB's IN flow control
 * (CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL) sends 47/48/49-frame packets depending on whether the IN FIFO is
 * below/at/above its threshold. The FIFO is filled by the same I2S-paced audio task, so the host sees the
 * converter rate in the packet sizes (UAC2 async source).
 *
 * Both FIFOs are 8 ms deep and regulated to half full (4 ms, PLAN.md 4.5): the audio task moves 32-frame
 * blocks (0.67 ms) while USB moves 48-frame packets (1 ms), so the level has a ~1.3 ms peak-to-peak
 * sawtooth around the target; 4 ms leaves ~3 ms of margin to underrun and to overflow. The target
 * (TF_USB_FIFO_TARGET_BYTES) can be lowered to ~2 ms after measurements on hardware to cut latency.
 *
 * ---------------------------------------------------------------------------------------------------
 * Full-speed feedback format (10.14 vs 16.16) on macOS / Windows / Linux
 * ---------------------------------------------------------------------------------------------------
 * USB 2.0 5.12.4.2 specifies 10.14 in 3 bytes at full speed. TinyUSB 0.21 sends 10.14 only for UAC1;
 * for UAC2 it always sends 16.16 in 4 bytes (older releases had CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_
 * CORRECTION to switch per host; 0.21 removed it). We therefore declare wMaxPacketSize = 4 on the
 * feedback EP. Rationale:
 *   - Windows (usbaudio2.sys, 10 1703+): its UAC2 driver interprets the feedback as 16.16 on every
 *     USB 2.0 device, full speed included (noted in TinyUSB's audio_device.h); a 10.14 value would be
 *     read as 1/4 of the rate.
 *   - Linux (snd-usb-audio): guesses the format on the first feedback packet by shifting the value until
 *     it lies near the nominal rate (endpoint.c, ep->freqshift), so either format works.
 *   - macOS (AppleUSBAudio): accepts the 4-byte 16.16 value on full-speed UAC2; TinyUSB reports its
 *     FIFO_COUNT UAC2 feedback as tested on Windows, Linux and macOS.
 *   To be confirmed on real hardware against all three hosts (see the final report).
 * Feedback interval is 1 frame (bInterval = 1). The DWC2 port cannot schedule ISO IN with bInterval > 1
 * reliably, and 1 ms updates suit FIFO_COUNT.
 *
 * Windows additionally requires: data EP wMaxPacketSize >= (nominal+1) frames (392 B), feedback within
 * +-1 frame (clamped by TinyUSB), and a readable clock frequency with a RANGE request (implemented below).
 */
#include <string.h>
#include "usbdev.h"
#include "usb_descriptors.h"
#include "tusb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_private/periph_ctrl.h"
#include "driver/gpio.h"
#include "bootdiag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usbdev";

#define USB_TASK_CORE   0
#define USB_TASK_PRIO   20
#define USB_TASK_STACK  4096

static usb_phy_handle_t s_phy;

static volatile bool     s_mounted;
static volatile bool     s_play_active, s_rec_active;
static volatile uint32_t s_play_gen, s_rec_gen;
static volatile bool     s_clock_valid = true;

/* FIFO pointers, cached once the audio function is opened (the tu_fifo objects are static inside the
 * TinyUSB audio driver and survive bus resets). */
static tu_fifo_t *volatile s_out_ff;
static tu_fifo_t *volatile s_in_ff;

/* Diagnostics */
static volatile uint32_t s_out_bytes_total, s_out_packets, s_in_packets;
static volatile uint32_t s_fb_value;                  /* 16.16 mirror */
static uint32_t s_fb_lvl_avg, s_fb_nom, s_fb_min, s_fb_max;
static uint16_t s_fb_rate[2];

/* ------------------------------------------------------------------------------------------------ */
bool usbdev_mounted(void) { return s_mounted; }
bool usbdev_play_active(void) { return s_play_active; }
bool usbdev_rec_active(void) { return s_rec_active; }
uint32_t usbdev_play_gen(void) { return s_play_gen; }
uint32_t usbdev_rec_gen(void) { return s_rec_gen; }

void usbdev_set_clock_valid(bool valid) { s_clock_valid = valid; }

/* ---- audio task side: lock-free SPSC with the USB ISR. tusb_fifo.c is placed in IRAM (linker.lf). ---- */
IRAM_ATTR uint32_t usbdev_play_target_bytes(void) { return TF_USB_FIFO_TARGET_BYTES; }
IRAM_ATTR uint32_t usbdev_rec_target_bytes(void) { return TF_USB_FIFO_TARGET_BYTES; }

IRAM_ATTR uint32_t usbdev_play_available(void)
{
    tu_fifo_t *ff = s_out_ff;
    return ff ? tu_fifo_count(ff) : 0;
}

IRAM_ATTR uint32_t usbdev_play_read(void *dst, uint32_t bytes)
{
    tu_fifo_t *ff = s_out_ff;
    return ff ? tu_fifo_read_n(ff, dst, (uint16_t)bytes) : 0;
}

IRAM_ATTR uint32_t usbdev_rec_count(void)
{
    tu_fifo_t *ff = s_in_ff;
    return ff ? tu_fifo_count(ff) : 0;
}

IRAM_ATTR uint32_t usbdev_rec_space(void)
{
    tu_fifo_t *ff = s_in_ff;
    return ff ? tu_fifo_remaining(ff) : 0;
}

IRAM_ATTR uint32_t usbdev_rec_write(const void *src, uint32_t bytes)
{
    tu_fifo_t *ff = s_in_ff;
    return ff ? tu_fifo_write_n(ff, src, (uint16_t)bytes) : 0;
}

/* ------------------------------------------------------------------------------------------------ */
void usbdev_get_stats(usbdev_stats_t *s)
{
    static int64_t  t_prev;
    static uint32_t b_prev;
    static float    rate;
    int64_t now = esp_timer_get_time();
    uint32_t b = s_out_bytes_total;
    if (t_prev == 0) {
        t_prev = now;
        b_prev = b;
    } else if (now - t_prev >= 500000) {
        rate = (float)(b - b_prev) / (float)USBDEV_FRAME_BYTES / ((float)(now - t_prev) / 1000.f);
        t_prev = now;
        b_prev = b;
    }
    s->out_fifo_bytes = (int32_t)usbdev_play_available();
    s->in_fifo_bytes  = (int32_t)usbdev_rec_count();
    s->feedback       = s_play_active ? (float)s_fb_value / 65536.f : 0.f;
    s->host_rate      = s_play_active ? rate : 0.f;
    s->out_packets    = s_out_packets;
    s->in_packets     = s_in_packets;
}

/* ------------------------------------------------------------------------------------------------
 * TinyUSB device callbacks
 * ------------------------------------------------------------------------------------------------ */
static void cache_fifos(void)
{
    if (!s_out_ff) s_out_ff = tud_audio_get_ep_out_ff();
    if (!s_in_ff) s_in_ff = tud_audio_get_ep_in_ff();
}

void tud_mount_cb(void)
{
    cache_fifos();
    s_mounted = true;
    ESP_LOGI(TAG, "mounted");
}

void tud_umount_cb(void)
{
    s_mounted = false;
    s_play_active = false;
    s_rec_active = false;
    s_play_gen++;
    s_rec_gen++;
    ESP_LOGI(TAG, "unmounted");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
}

void tud_resume_cb(void) {}

/* ---- UAC2 class requests ---------------------------------------------------------------------- */
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t const ctrl   = TU_U16_HIGH(req->wValue);
    uint8_t const entity = TU_U16_HIGH(req->wIndex);

    if (entity == UAC2_ENTITY_CLOCK) {
        if (ctrl == AUDIO20_CS_CTRL_SAM_FREQ) {
            if (req->bRequest == AUDIO20_CS_REQ_CUR) {
                /* Must go through tud_audio_buffer_and_schedule_control_xfer(): the driver snoops this
                 * answer to learn the IN sample rate for its flow control. */
                audio20_control_cur_4_t cur = { .bCur = (int32_t)TF_USB_SAMPLE_RATE };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof cur);
            }
            if (req->bRequest == AUDIO20_CS_REQ_RANGE) {
                audio20_control_range_4_n_t(1) range = { .wNumSubRanges = 1 };
                range.subrange[0].bMin = (int32_t)TF_USB_SAMPLE_RATE;
                range.subrange[0].bMax = (int32_t)TF_USB_SAMPLE_RATE;
                range.subrange[0].bRes = 0;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &range, sizeof range);
            }
        } else if (ctrl == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
            audio20_control_cur_1_t valid = { .bCur = s_clock_valid ? 1 : 0 };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, sizeof valid);
        }
    }
    ESP_LOGD(TAG, "stall GET entity %u ctrl %u req %u", entity, ctrl, req->bRequest);
    return false;   /* unsupported -> STALL */
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    (void)rhport;
    uint8_t const ctrl   = TU_U16_HIGH(req->wValue);
    uint8_t const entity = TU_U16_HIGH(req->wIndex);
    /* The frequency control is read-only. Some hosts still SET_CUR it; accept the one rate we run at
     * (harmless, avoids a STALL-triggered retry storm), reject everything else. */
    if (entity == UAC2_ENTITY_CLOCK && ctrl == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_CUR &&
        req->wLength == sizeof(audio20_control_cur_4_t)) {
        int32_t f;
        memcpy(&f, buf, sizeof f);
        return f == TF_USB_SAMPLE_RATE;
    }
    return false;
}

/* ---- streaming interface changes -------------------------------------------------------------- */
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(req->wIndex);
    if (itf == ITF_NUM_AUDIO_STREAMING_OUT) s_play_active = false;
    if (itf == ITF_NUM_AUDIO_STREAMING_IN) s_rec_active = false;
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(req->wIndex);
    uint8_t const alt = tu_u16_low(req->wValue);
    cache_fifos();

    if (itf == ITF_NUM_AUDIO_STREAMING_OUT) {
        /* Clean start: drop anything left in the OUT FIFO. Safe here: the host cannot send ISO data before
         * this SET_INTERFACE completes, and the audio task does not read while play is inactive. The audio
         * task then re-primes to the feedback target level before it starts consuming. */
        if (alt) {
            tud_audio_clear_ep_out_ff();
            s_fb_nom = (uint32_t)(TF_USB_SAMPLE_RATE / 1000) << 16;
            s_fb_min = (uint32_t)((TF_USB_SAMPLE_RATE - 1) / 1000) << 16;
            s_fb_max = (uint32_t)(TF_USB_SAMPLE_RATE / 1000 + 1) << 16;
            s_fb_lvl_avg = (uint32_t)TF_USB_FIFO_TARGET_BYTES << 16;
            s_fb_rate[0] = (uint16_t)((s_fb_max - s_fb_nom) / TF_USB_FIFO_TARGET_BYTES);
            s_fb_rate[1] = (uint16_t)((s_fb_nom - s_fb_min) / TF_USB_FIFO_TARGET_BYTES);
            s_fb_value = s_fb_nom;
        }
        s_play_gen++;
        s_play_active = alt != 0;
        ESP_LOGI(TAG, "playback %s", alt ? "start" : "stop");
    } else if (itf == ITF_NUM_AUDIO_STREAMING_IN) {
        if (alt) {
            /* Clean start: the host does not poll the IN EP before this request completes and the audio task
             * only writes while rec is active, so nobody touches the FIFO concurrently. */
            tud_audio_clear_ep_in_ff();
            /* IN flow control regulates the IN FIFO to this level (driver default: half the FIFO). The
             * audio task pre-fills the FIFO with silence up to it, so the first packets are full-size. */
            tud_audio_set_ep_in_fifo_threshold(TF_USB_FIFO_TARGET_BYTES);
        }
        s_rec_gen++;
        s_rec_active = alt != 0;
        ESP_LOGI(TAG, "record %s", alt ? "start" : "stop");
    }
    return true;
}

/* ---- feedback ---------------------------------------------------------------------------------- */
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *fb)
{
    (void)func_id;
    (void)alt_itf;
    fb->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    fb->sample_freq = TF_USB_SAMPLE_RATE;
    fb->fifo_count.fifo_threshold = TF_USB_FIFO_TARGET_BYTES;
}

/* Called in the USB ISR after each OUT packet, right after TinyUSB updated its own feedback value from
 * the same FIFO level. We mirror that computation (same filter, rates and clamps) for the diagnostics
 * screen, because the driver keeps its value private. */
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                           uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;
    s_out_bytes_total += n_bytes_received;
    s_out_packets++;
    tu_fifo_t *ff = s_out_ff;
    if (ff && s_fb_nom) {
        uint32_t lvl = (uint32_t)(((uint64_t)s_fb_lvl_avg * 63 + ((uint32_t)tu_fifo_count(ff) << 16)) >> 6);
        s_fb_lvl_avg = lvl;
        uint32_t ff_lvl = lvl >> 16, thr = TF_USB_FIFO_TARGET_BYTES, fbv;
        if (ff_lvl < thr) fbv = s_fb_nom + (thr - ff_lvl) * s_fb_rate[0];
        else fbv = s_fb_nom - (ff_lvl - thr) * s_fb_rate[1];
        if (fbv > s_fb_max) fbv = s_fb_max;
        if (fbv < s_fb_min) fbv = s_fb_min;
        s_fb_value = fbv;
    }
    return true;
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in,
                           uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_sent;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    s_in_packets++;
    return true;
}

/* ------------------------------------------------------------------------------------------------ */
static void usb_task(void *arg)
{
    (void)arg;
    /* tusb_init() allocates the DWC2 interrupt on the calling core: this task is pinned to core 0 so the
     * USB ISR never runs on the audio core. */
    const tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    if (!tusb_init(0, &dev_init)) {
        ESP_LOGE(TAG, "tusb_init failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;) tud_task();   /* blocks on the TinyUSB event queue */
}

esp_err_t usbdev_init(void)
{
    const usb_phy_config_t phy_conf = {
        .controller  = USB_PHY_CTRL_OTG,
        .target      = USB_PHY_TARGET_INT,      /* internal FS PHY on GPIO19 (D-) / GPIO20 (D+) */
        .otg_mode    = USB_OTG_MODE_DEVICE,
        .otg_speed   = USB_PHY_SPEED_FULL,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,                    /* bus powered: no VBUS sensing pin */
    };
    esp_err_t err = usb_new_phy(&phy_conf, &s_phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreatePinnedToCore(usb_task, "tinyusb", USB_TASK_STACK, NULL, USB_TASK_PRIO, NULL, USB_TASK_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------------------
 * Host-driven flashing (no BOOT button): detach from the bus so the host forgets the TWOFOLD device, hand the
 * USB pads back to the built-in USB-Serial/JTAG controller, ask the ROM for download mode on the next reset,
 * and restart. The ROM then enumerates as 303a:1001 and `idf.py flash` / esptool work as usual.
 * (Same sequence as Arduino-ESP32's usb_persist_restart(RESTART_BOOTLOADER) on the ESP32-S3.)
 */

void usbdev_reboot_to_bootloader(void)
{
    bootdiag_crumb("BL DETACH");
    tud_disconnect();                                   /* OTG pull-up off */
    vTaskDelay(pdMS_TO_TICKS(20));
    if (s_phy) { usb_del_phy(s_phy); s_phy = NULL; }    /* OTG off the pads */
    /* SE0 for 100 ms: D-/D+ (GPIO19/20) driven low as plain GPIOs, so the host registers a clean detach */
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    PERIPH_RCC_ATOMIC() {                               /* IDF gated the USJ clock at boot (USJ unused) */
        usb_serial_jtag_ll_enable_bus_clock(true);
        usb_serial_jtag_ll_reset_register();
    }
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_serial_jtag_ll_phy_enable_external(false);      /* internal PHY */
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(GPIO_NUM_19, 0);
    gpio_set_level(GPIO_NUM_20, 0);
    bootdiag_crumb("BL SE0");
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_reset_pin(GPIO_NUM_19);
    gpio_reset_pin(GPIO_NUM_20);
    /* hand the pads to USB-Serial/JTAG and wait (<= 1 s) for the host to reset the bus: it has seen us */
    REG_WRITE(USB_SERIAL_JTAG_INT_CLR_REG, USB_SERIAL_JTAG_USB_BUS_RESET_INT_CLR);
    usb_serial_jtag_ll_phy_enable_pad(true);
    bootdiag_crumb("BL USJ");
    for (int i = 0; i < 100; i++) {
        if (REG_READ(USB_SERIAL_JTAG_INT_RAW_REG) & USB_SERIAL_JTAG_USB_BUS_RESET_INT_RAW) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bootdiag_crumb("BL RESTART");
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);   /* consumed by the ROM on the next boot */
    esp_restart();
}
