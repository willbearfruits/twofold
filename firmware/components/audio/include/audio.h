/*
 * audio.h: I2S0 full duplex (PCM1808 ADC + PCM5102A DAC), the audio task (core 1) and DAC soft-mute (XSMT).
 * PLAN.md 4 (clocking), 3 (tasks), 8 (audio path).
 *
 * Boot order: audio_early_init() first thing in app_main (XSMT low), audio_dsp_init() before any radio
 * exists (big DSP buffers), then audio_start() once params/storage are restored.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_BLOCK_FRAMES   32      /* 0.667 ms at 48 kHz: one DMA descriptor, one dsp_engine_process() call */
#define AUDIO_DMA_DESC       4       /* per direction */
#define AUDIO_TX_PRELOAD     2       /* blocks of silence pre-loaded into the TX ring */
#define AUDIO_STARTUP_RAMP_MS 500    /* dsp_engine_startup_ramp() after XSMT goes high */

/* Drive the PCM5102A XSMT pin low (muted). Call as early as possible. */
void      audio_early_init(void);
/* dsp_engine_init() with internal / PSRAM allocators. Returns ESP_OK or ESP_FAIL. */
esp_err_t audio_dsp_init(void);
/* Create the audio task (core 1, prio 22). It configures I2S on core 1 (so the GDMA ISR lives there),
 * pre-fills TX with silence, enables TX then RX and unmutes the DAC ~50 ms after clocks are stable. */
esp_err_t audio_start(void);

typedef struct {
    bool     running;            /* blocks are flowing */
    bool     clock_ok;           /* I2S clock present (always true in INTERNAL mode once started) */
    bool     dac_unmuted;        /* XSMT high */
    uint32_t blocks;             /* processed since boot */
    uint32_t xrun_play;          /* playback blocks padded with silence (USB OUT FIFO underrun) */
    uint32_t xrun_rec;           /* record blocks dropped (USB IN FIFO full) */
    uint32_t i2s_errors;         /* DMA queue overflows (RX/TX) + late TX blocks + read timeouts */
    uint32_t tx_late;            /* TX blocks that missed their slot (subset of i2s_errors) */
    float    measured_fs;        /* I2S frames per second against esp_timer, last 1 s window */
    int8_t   tx_lead;            /* TX blocks queued ahead of the playing one (latency control) */
} audio_stats_t;
void audio_get_stats(audio_stats_t *s);
/* Diagnostic: run the DSP with interrupts masked for `ms` (profile without interrupt/stall time). */
void audio_diag_irqoff(uint32_t ms);

#ifdef __cplusplus
}
#endif
