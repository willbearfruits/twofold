/*
 * dsp_chorus.h: 3-voice stereo chorus / ensemble / vibrato.
 *
 * Every voice reads both channel lines at a modulated delay with cubic Hermite interpolation. Delays are
 * computed from the LFOs once per block and interpolated linearly per sample (delay error < 0.005 samples
 * at 6 Hz, i.e. inaudible), so the LFOs cost nothing per sample.
 *   CHORUS   : voices at 7.5 / 10.5 / 13.5 ms, sine LFOs 120 deg apart, right channel in quadrature,
 *              voices panned L / C / R, depth up to +-4 ms.
 *   ENSEMBLE : Solina/Juno-style: three lines with the same 5 ms base delay, slow LFOs 120 deg apart (RATE)
 *              plus a fixed 6.1 Hz "vibrato" LFO (also 120 deg apart) plus slow random drift per voice;
 *              right channel uses the opposite LFO phases.
 *   VIBRATO  : one voice per channel, 100 % wet (MIX ignored), depth up to +-3 ms.
 * Mode changes crossfade between the outgoing and incoming mode (both rendered for 40 ms); on/off is a
 * dry/wet fade. Lines (2 x 2048 floats) live in internal RAM; they keep being written while the effect is
 * off so enabling it never replays stale audio.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "dsp_smooth.h"

#define CHO_LEN   2048
#define CHO_MODES 3
enum { CHO_CHORUS = 0, CHO_ENSEMBLE, CHO_VIBRATO };

typedef struct {
    float   *line[2];
    int      w;
    float    ph, fph;                       /* slow / fast LFO phase (cycles) */
    float    drift_tgt[3], drift1[3], drift2[3];
    int      drift_cnt;
    uint32_t rng;
    float    dprev[CHO_MODES][3][2];        /* delay (samples) at the end of the previous block */
    int      mode, from_mode;
    float    xf;                            /* mode crossfade 0..1 (1 = done) */
    dsp_sm_t rate, depth, mix;
    dsp_fade_t on;
    float    viz_phase[3];
    bool     running;
} dsp_chorus_t;

/* alloc_fast: internal RAM for 2 x CHO_LEN floats. Returns 0 on success. */
int  cho_init(dsp_chorus_t *c, void *(*alloc_fast)(size_t));
void cho_reset(dsp_chorus_t *c);
void cho_set(dsp_chorus_t *c, bool on, int mode, float rate_hz, float depth_pct, float mix_pct);
void cho_settle(dsp_chorus_t *c);
void cho_process(dsp_chorus_t *c, float *L, float *R, const dsp_blk_t *b);
bool cho_active(const dsp_chorus_t *c);
