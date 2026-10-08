/*
 * dsp_engine.h: the complete TWOFOLD audio chain as a single portable engine (singleton).
 *
 * Called from exactly one real-time thread (the audio task on core 1): dsp_engine_process().
 * Every other function marked [any] is safe to call from other tasks/cores (lock-free reads of
 * single-writer data).
 *
 * Signal flow (PLAN.md 8):
 *   play (USB) -> EQ -> SAT -> CHORUS -> DELAY -> REVERB -> COMP -> [monitor blend] -> VOLUME -> LIMITER -> dither -> DAC
 *   ADC -> input mode -> input gain -> peak/clip -> dither (skipped when bit-transparent) -> rec (USB)
 *
 * Sample format at the API: interleaved stereo int32, left-justified 24-bit (low 8 bits zero on output),
 * i.e. exactly what both UAC2 (24-bit in 4-byte subslots) and the I2S 32-bit slots carry.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Profiled stages (cycle counters per block). Keep in sync with dsp_stage_names[]. */
typedef enum {
    DSP_ST_INPUT = 0,   /* int->float, input mode/gain, peak/clip, record dither */
    DSP_ST_EQ,
    DSP_ST_SAT,
    DSP_ST_CHORUS,
    DSP_ST_DELAY,
    DSP_ST_REVERB,
    DSP_ST_COMP,
    DSP_ST_OUTPUT,      /* monitor blend, master volume, limiter */
    DSP_ST_DITHER,      /* TPDF + float->int24 for DAC */
    DSP_ST_COUNT
} dsp_stage_t;
extern const char *const dsp_stage_names[DSP_ST_COUNT];   /* "INPUT","EQ","SAT","CHORUS",... */

typedef struct {
    void *(*alloc_fast)(size_t bytes);   /* internal RAM: reverb tank, chorus, state   */
    void *(*alloc_big)(size_t bytes);    /* PSRAM: delay lines, pre-delay (sequential)  */
    void *(*alloc_hot)(size_t bytes);    /* internal RAM, always (random access every sample: chorus lines).
                                          * Optional: NULL = use alloc_fast. */
} dsp_alloc_t;

/* Allocates everything once (call at boot, before radios). Returns 0 on success. */
int  dsp_engine_init(const dsp_alloc_t *alloc);
/* Clears all state (delay/reverb tails), keeps parameters. RT thread only, or before start. */
void dsp_engine_reset(void);

/* Process n frames (n <= DSP_BLOCK_MAX). Reads params (params_get) once per call.
 * play / adc may be NULL (treated as silence). rec may be NULL. dac must be valid. */
void dsp_engine_process(const int32_t *play, const int32_t *adc, int32_t *dac, int32_t *rec, int n);

/* ---- metering [any] ---- */
typedef struct {
    float    in_peak[2];      /* linear, post input gain, max since last take */
    float    out_peak[2];     /* linear, post limiter (what the DAC gets) */
    float    play_peak[2];    /* linear, USB playback before processing */
    uint32_t in_clip[2];      /* count of blocks with ADC/gain overs (>= -0.1 dBFS) since boot */
    uint32_t out_over[2];     /* count of blocks where the limiter input exceeded the ceiling */
    float    comp_gr_db;      /* compressor gain reduction (>= 0), max since last take */
    float    lim_gr_db;       /* limiter gain reduction (>= 0), max since last take */
    float    sat_in_level;    /* saturation input peak (linear) for the transfer-curve dot */
} dsp_meters_t;
/* Copies meters; when take==true, resets the "since last take" maxima (single consumer: UI). */
void dsp_engine_get_meters(dsp_meters_t *m, bool take);

/* ---- profiling [any] ---- */
typedef struct {
    float    avg_pct[DSP_ST_COUNT];   /* percent of one core at DSP_CPU_HZ, averaged over the last window */
    float    max_pct[DSP_ST_COUNT];   /* worst block in the last window */
    float    total_avg_pct, total_max_pct;
    uint32_t blocks;                  /* blocks processed since boot */
    uint32_t window_ms;
} dsp_prof_t;
void dsp_engine_get_prof(dsp_prof_t *p);
/* Test hook: the EQ's live coefficients for band 0..4. */
struct biquad_coef; void dsp_engine_dbg_eq_coefs(int band, float out[5]);
/* Debug sub-stage cycle sums (DSP_DBG_N words, last = frames), reset on read. */
void dsp_engine_get_dbg(uint32_t *out);

/* ---- spectrum tap [any] ----
 * Mono (L+R)/2 of the DAC-bound signal, pre-limiter, written to a lock-free SPSC ring.
 * The consumer (UI task, core 0) pulls the most recent `n` samples (n <= DSP_SPECTRUM_RING). */
#define DSP_SPECTRUM_RING 2048
size_t dsp_engine_spectrum_read(float *dst, size_t n);

/* ---- effect visualisation state [any] ---- */
typedef struct {
    float chorus_lfo_phase[3];   /* 0..1 per voice (for the chorus animation) */
    float delay_time_ms;         /* effective (tempo-synced) delay time */
    float reverb_energy;         /* RMS of the reverb tank output (linear) */
} dsp_viz_t;
void dsp_engine_get_viz(dsp_viz_t *v);

/* ---- soft mute for flash writes [any] ----
 * Requests a 5 ms ramp of the DAC output (and USB record) to digital zero. Ref-counted.
 * dsp_engine_is_silent() becomes true once the ramp has fully reached zero AND at least
 * one full block of zeros has been emitted, so the TX DMA ring holds only zeros. */
void dsp_engine_soft_mute(bool on);
bool dsp_engine_is_silent(void);
/* true when the DAC output has been below -90 dBFS for >= 2 s (storage "audio-safe window") */
bool dsp_engine_output_quiet(void);

/* ---- boot ---- */
/* Start-up volume fade: output starts at silence and ramps to the stored volume over `ms`. */
void dsp_engine_startup_ramp(uint32_t ms);

#ifdef __cplusplus
}
#endif
