/*
 * audio.c: I2S0 full duplex + the audio task (core 1, prio 22). PLAN.md 4.
 *
 * Clocking
 *   INTERNAL (default): ESP32-S3 is I2S master. clk_src = PLL_F160M, MCLK = 256 fs = 12.288 MHz on
 *     BOARD_PIN_I2S_MCLK (13 + 1/48 fractional divider, pattern at 256 kHz, PLAN.md 4.2) -> PCM1808 SCKI.
 *     32-bit slots, so BCK = MCLK / 4 = 64 fs exactly. PCM5102A derives its clock from BCK (SCK = GND).
 *   EXT_OSC (CONFIG_TWOFOLD_CLOCK_EXT_OSC): the PCM1808 is master (MD1=MD0=1) from a 12.288 MHz
 *     oscillator; TX and RX are both I2S SLAVES (BCK/WS are inputs). The MCLK pin is not configured (left
 *     as input). The S3 still samples BCK/WS with an internal clock of 8 x BCK (bclk_div = 8, derived from
 *     PLL_F160M): that only times the sampling of the external clock; it does not set the audio rate.
 *   One i2s_new_channel() call creates TX + RX on I2S0, so they share BCK/WS in both modes.
 *
 * Pipeline (one 32-frame block per iteration, paced by the converter clock)
 *   i2s_channel_read(RX, 1 block, blocking) -> USB OUT FIFO read (pad + count xrun) ->
 *   dsp_engine_process() -> USB IN FIFO write -> i2s_channel_write(TX, 1 block).
 *
 * TX latency control
 *   2 blocks of silence are pre-loaded, TX is enabled before RX, so each RX EOF arrives just after a TX
 *   slot boundary and the processed block is written into the slot right after the one that is playing
 *   (ADC -> DAC ~ 2 blocks = 1.33 ms plus converter group delays). An on_sent ISR callback counts TX slots,
 *   so before every write we know exactly how far ahead of the DMA we are (tx_lead):
 *     lead < target: we were late (or a flash-write stall overflowed the queue): write silence to catch up;
 *     lead > target: we are too far ahead (RX backlog after a stall): drop this block.
 *   This keeps the latency constant and avoids ever writing into the buffer the DMA is playing.
 *   auto_clear_after_cb: a slot that was not refilled in time plays zeros, never a repeated buffer.
 */
#include <string.h>
#include "audio.h"
#include "board_config.h"
#include "dsp_engine.h"
#include "usbdev.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define AUDIO_TASK_CORE     1
#define AUDIO_TASK_PRIO     22
#define AUDIO_TASK_STACK    6144
#define BLOCK_SAMPLES       (AUDIO_BLOCK_FRAMES * 2)
#define BLOCK_BYTES         (BLOCK_SAMPLES * (int)sizeof(int32_t))
#define READ_TIMEOUT_MS     20
#define UNMUTE_BLOCKS       75      /* ~50 ms of stable clock before XSMT goes high */
#define BLOCK_CYCLES        ((uint32_t)((uint64_t)240000000 * AUDIO_BLOCK_FRAMES / BOARD_SAMPLE_RATE))

_Static_assert(BLOCK_BYTES == 32 * USBDEV_FRAME_BYTES, "USB frame = I2S frame = 2 x int32");

static i2s_chan_handle_t s_tx, s_rx;
static bool s_dsp_ok;

/* DMA blocks and scratch: internal RAM (static .bss), 16-byte aligned for the DSP. */
static DRAM_ATTR int32_t s_adc[BLOCK_SAMPLES] __attribute__((aligned(16)));
static DRAM_ATTR int32_t s_dac[BLOCK_SAMPLES] __attribute__((aligned(16)));
static DRAM_ATTR int32_t s_play[BLOCK_SAMPLES] __attribute__((aligned(16)));
static DRAM_ATTR int32_t s_rec[BLOCK_SAMPLES] __attribute__((aligned(16)));
static DRAM_ATTR int32_t s_zero[BLOCK_SAMPLES] __attribute__((aligned(16)));

/* ISR-updated counters */
static DRAM_ATTR volatile uint32_t s_tx_slots;        /* TX DMA slots finished = index of the playing slot */
static DRAM_ATTR volatile uint32_t s_tx_slot_cc;      /* CCOUNT at the last TX slot boundary (core 1) */
static DRAM_ATTR volatile uint32_t s_rx_ovf, s_tx_ovf;

/* Stats (written by the audio task only) */
static volatile audio_stats_t s_st;

/* Audio task stack in internal RAM, statically allocated. */
static StackType_t s_stack[AUDIO_TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_tcb;

/* ------------------------------------------------------------------------------------------------ */
void audio_early_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_DAC_XSMT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(BOARD_PIN_DAC_XSMT, 0);
    gpio_config(&io);
    gpio_set_level(BOARD_PIN_DAC_XSMT, 0);
}

/* Internal RAM kept free after DSP allocation for task stacks, drivers and the opt-in radios
 * (WiFi ~45 KB + BLE ~35 KB internal with buffers in PSRAM). Below that, DSP buffers spill to PSRAM;
 * the reverb asks for its lines shortest-first, so only its long sequential delay lines ever spill. */
#define DSP_INTERNAL_RESERVE (96 * 1024)
static size_t s_fast_spilled;

static void *alloc_fast(size_t bytes)
{
    void *p = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= bytes + DSP_INTERNAL_RESERVE)
        p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) s_fast_spilled += bytes;
    }
    return p;
}

static void *alloc_big(size_t bytes)
{
    void *p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "PSRAM alloc of %u B failed, falling back to internal RAM", (unsigned)bytes);
        p = alloc_fast(bytes);
    }
    return p;
}

esp_err_t audio_dsp_init(void)
{
    static const dsp_alloc_t alloc = { .alloc_fast = alloc_fast, .alloc_big = alloc_big };
    size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ext_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int r = dsp_engine_init(&alloc);
    s_dsp_ok = (r == 0);
    ESP_LOGI(TAG, "dsp_engine_init: %d, internal %u B, PSRAM %u B (of which %u B spilled from internal), "
             "internal free now %u B", r,
             (unsigned)(int_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)(ext_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), (unsigned)s_fast_spilled,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return s_dsp_ok ? ESP_OK : ESP_FAIL;
}

void audio_get_stats(audio_stats_t *s)
{
    memcpy(s, (const void *)&s_st, sizeof *s);
}

/* ---- I2S ISR callbacks (IRAM: CONFIG_I2S_ISR_IRAM_SAFE) ---- */
static IRAM_ATTR bool on_sent(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
    s_tx_slots++;
    s_tx_slot_cc = esp_cpu_get_cycle_count();
    return false;
}

static IRAM_ATTR bool on_send_q_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
    s_tx_ovf++;
    return false;
}

static IRAM_ATTR bool on_recv_q_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
    s_rx_ovf++;
    return false;
}

/* ------------------------------------------------------------------------------------------------ */
static esp_err_t i2s_setup(void)
{
    i2s_chan_config_t cc = {
        .id = (i2s_port_t)BOARD_I2S_PORT,
        .role = BOARD_CLOCK_EXT_OSC ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_DMA_DESC,
        .dma_frame_num = AUDIO_BLOCK_FRAMES,
        .auto_clear_after_cb = true,      /* not refilled in time -> zeros, not a stale buffer */
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 3,
    };
    esp_err_t err = i2s_new_channel(&cc, &s_tx, &s_rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t sc = {
        .clk_cfg = {
            .sample_rate_hz = BOARD_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_PLL_160M,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = (i2s_mclk_multiple_t)BOARD_MCLK_MULTIPLE,
            .bclk_div = 8,                /* slave only: internal sampling clock = 8 x BCK */
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_CLOCK_EXT_OSC ? I2S_GPIO_UNUSED : (gpio_num_t)BOARD_PIN_I2S_MCLK,
            .bclk = (gpio_num_t)BOARD_PIN_I2S_BCLK,
            .ws = (gpio_num_t)BOARD_PIN_I2S_WS,
            .dout = (gpio_num_t)BOARD_PIN_I2S_DOUT,
            .din = (gpio_num_t)BOARD_PIN_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    if ((err = i2s_channel_init_std_mode(s_tx, &sc)) != ESP_OK) return err;
    if ((err = i2s_channel_init_std_mode(s_rx, &sc)) != ESP_OK) return err;

    const i2s_event_callbacks_t tx_cbs = { .on_sent = on_sent, .on_send_q_ovf = on_send_q_ovf };
    const i2s_event_callbacks_t rx_cbs = { .on_recv_q_ovf = on_recv_q_ovf };
    if ((err = i2s_channel_register_event_callback(s_tx, &tx_cbs, NULL)) != ESP_OK) return err;
    if ((err = i2s_channel_register_event_callback(s_rx, &rx_cbs, NULL)) != ESP_OK) return err;

    /* Pre-fill: AUDIO_TX_PRELOAD blocks of silence ahead of the first processed block. */
    size_t loaded = 0;
    for (int i = 0; i < AUDIO_TX_PRELOAD; i++) {
        size_t l = 0;
        if ((err = i2s_channel_preload_data(s_tx, s_zero, BLOCK_BYTES, &l)) != ESP_OK) return err;
        loaded += l;
    }
    if (loaded != (size_t)(AUDIO_TX_PRELOAD * BLOCK_BYTES)) ESP_LOGW(TAG, "TX preload %u B", (unsigned)loaded);

    /* TX first: RX slot boundaries then trail TX boundaries by a few us, which gives the DSP almost a full
     * block of budget before the next TX slot starts. */
    if ((err = i2s_channel_enable(s_tx)) != ESP_OK) return err;
    if ((err = i2s_channel_enable(s_rx)) != ESP_OK) return err;
    ESP_LOGI(TAG, "I2S0 %s, %d Hz, 32-bit slots, MCLK %s, %d x %d frames", BOARD_CLOCK_EXT_OSC ? "SLAVE (ext osc)" : "MASTER",
             BOARD_SAMPLE_RATE, BOARD_CLOCK_EXT_OSC ? "unused" : "256fs on GPIO4", AUDIO_DMA_DESC, AUDIO_BLOCK_FRAMES);
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------------------ */
typedef enum { PLAY_IDLE = 0, PLAY_PRIMING, PLAY_RUNNING } play_state_t;

static IRAM_ATTR void tx_write(const int32_t *blk)
{
    size_t w = 0;
    i2s_channel_write(s_tx, blk, BLOCK_BYTES, &w, READ_TIMEOUT_MS);
}

static IRAM_ATTR __attribute__((noinline)) void audio_loop(void)
{
    play_state_t play_state = PLAY_IDLE;
    uint32_t play_gen = usbdev_play_gen(), rec_gen = usbdev_rec_gen();
    uint32_t w_next = AUDIO_TX_PRELOAD;       /* TX slot index our next write lands in */
    uint32_t tx_ovf_seen = 0, rx_ovf_seen = 0;
    int32_t  lead_target = 0;
    uint32_t good_blocks = 0, late_events = 0;
    uint64_t phase_acc = 0;
    uint32_t phase_n = 0;
    int64_t  fs_t0 = esp_timer_get_time();
    uint32_t fs_frames = 0;
    bool     unmuted = false, clock_ok = false;

    for (;;) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_adc, BLOCK_BYTES, &got, READ_TIMEOUT_MS);
        uint32_t const cc_now = esp_cpu_get_cycle_count();
        if (err != ESP_OK || got != BLOCK_BYTES) {
            /* No clock (EXT_OSC without oscillator / PCM1808 not master) or a driver error. */
            s_st.i2s_errors++;
            good_blocks = 0;
            if (clock_ok) {
                clock_ok = false;
                if (unmuted) gpio_set_level(BOARD_PIN_DAC_XSMT, 0);
                unmuted = false;
                usbdev_set_clock_valid(false);
            }
            s_st.clock_ok = false;
            s_st.dac_unmuted = false;
            s_st.running = false;
            continue;
        }
        if (!clock_ok) {
            /* Without BCK the TX DMA stops too and we did not write, so the slot accounting stays valid. */
            clock_ok = true;
            usbdev_set_clock_valid(true);
        }

        /* ---- I2S health: queue overflows mean the task stalled (e.g. flash write with cache off) ---- */
        uint32_t const rx_ovf = s_rx_ovf, tx_ovf = s_tx_ovf;
        if (rx_ovf != rx_ovf_seen) {
            s_st.i2s_errors += rx_ovf - rx_ovf_seen;
            rx_ovf_seen = rx_ovf;
        }

        /* ---- playback: USB OUT FIFO -> play block ---- */
        const int32_t *play = NULL;
        uint32_t const pg = usbdev_play_gen();
        if (pg != play_gen) {
            play_gen = pg;
            play_state = usbdev_play_active() ? PLAY_PRIMING : PLAY_IDLE;
        }
        if (play_state == PLAY_PRIMING && usbdev_play_available() >= usbdev_play_target_bytes())
            play_state = PLAY_RUNNING;               /* start consuming at the feedback target level */
        if (play_state == PLAY_RUNNING) {
            uint32_t n = usbdev_play_read(s_play, BLOCK_BYTES);
            if (n < (uint32_t)BLOCK_BYTES) {
                memset((uint8_t *)s_play + n, 0, BLOCK_BYTES - n);
                s_st.xrun_play++;
                play_state = PLAY_PRIMING;           /* re-buffer instead of stuttering at the edge */
            }
            play = s_play;
        }

        /* ---- record: (re)start pre-fill of the USB IN FIFO with silence up to the flow-control target ---- */
        bool const rec = usbdev_rec_active();
        uint32_t const rg = usbdev_rec_gen();
        if (rg != rec_gen) {
            rec_gen = rg;
            if (rec) {
                uint32_t have = usbdev_rec_count(), want = usbdev_rec_target_bytes();
                while (have + BLOCK_BYTES <= want) {
                    usbdev_rec_write(s_zero, BLOCK_BYTES);
                    have += BLOCK_BYTES;
                }
            }
        }

        /* ---- DSP ---- */
        if (s_dsp_ok) {
            dsp_engine_process(play, s_adc, s_dac, rec ? s_rec : NULL, AUDIO_BLOCK_FRAMES);
        } else {
            memset(s_dac, 0, sizeof s_dac);
            memset(s_rec, 0, sizeof s_rec);
        }
        if (rec) {
            if (usbdev_rec_space() >= (uint32_t)BLOCK_BYTES) usbdev_rec_write(s_rec, BLOCK_BYTES);
            else s_st.xrun_rec++;
        }

        /* ---- TX with latency control ---- */
        uint32_t const slot = s_tx_slots;            /* the slot the DMA is playing now */
        if (tx_ovf != tx_ovf_seen) {
            /* Free-buffer queue overflowed: IDF dropped the oldest entries; the oldest remaining buffer is the
             * one now playing, so our next write lands in `slot`. */
            s_st.i2s_errors += tx_ovf - tx_ovf_seen;
            tx_ovf_seen = tx_ovf;
            w_next = slot;
        }
        int32_t lead = (int32_t)(w_next - slot) - 1;  /* slots queued between the playing one and ours */
        if (lead < lead_target) {
            if (lead < 0 && good_blocks > UNMUTE_BLOCKS) {
                s_st.tx_late++;
                s_st.i2s_errors++;
                if (++late_events == 3 && lead_target == 0) {
                    lead_target = 1;
                    ESP_LOGW(TAG, "repeated late TX blocks: adding one block of output latency");
                }
            }
            for (int k = 0; lead < lead_target && k < AUDIO_DMA_DESC - 1; k++, lead++, w_next++)
                tx_write(s_zero);                    /* lands in the playing (already zeroed) or next slot */
            tx_write(s_dac);
            w_next++;
        } else if (lead > lead_target) {
            /* too far ahead (RX backlog after a stall): drop this block to restore the latency */
        } else {
            tx_write(s_dac);
            w_next++;
        }
        s_st.tx_lead = (int8_t)lead;

        /* Phase of the RX boundary inside the TX slot, averaged over the first blocks: if RX trails TX by
         * more than ~45 % of a block (possible in slave mode), give the DSP one more block of budget. */
        if (good_blocks >= 16 && phase_n < 64) {
            phase_acc += (uint32_t)(cc_now - s_tx_slot_cc);
            if (++phase_n == 64 && phase_acc / 64 > (uint64_t)BLOCK_CYCLES * 45 / 100 && lead_target == 0) {
                lead_target = 1;
                ESP_LOGI(TAG, "RX/TX phase %u cycles: TX lead 1 block", (unsigned)(phase_acc / 64));
            }
        }

        /* ---- DAC unmute once clocks are stable ---- */
        good_blocks++;
        if (!unmuted && good_blocks >= UNMUTE_BLOCKS) {
            gpio_set_level(BOARD_PIN_DAC_XSMT, 1);
            if (s_dsp_ok) dsp_engine_startup_ramp(AUDIO_STARTUP_RAMP_MS);
            unmuted = true;
        }

        /* ---- stats / measured fs ---- */
        s_st.blocks++;
        s_st.running = true;
        s_st.clock_ok = true;
        s_st.dac_unmuted = unmuted;
        fs_frames += AUDIO_BLOCK_FRAMES;
        int64_t const now = esp_timer_get_time();
        if (now - fs_t0 >= 1000000) {
            s_st.measured_fs = (float)((double)fs_frames * 1e6 / (double)(now - fs_t0));
            fs_t0 = now;
            fs_frames = 0;
        }
    }
}

static void audio_task(void *arg)
{
    esp_err_t err = i2s_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    audio_loop();
}

esp_err_t audio_start(void)
{
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(audio_task, "audio", AUDIO_TASK_STACK / sizeof(StackType_t), NULL,
                                                   AUDIO_TASK_PRIO, s_stack, &s_tcb, AUDIO_TASK_CORE);
    return h ? ESP_OK : ESP_FAIL;
}
