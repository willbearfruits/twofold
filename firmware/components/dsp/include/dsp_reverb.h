/*
 * dsp_reverb.h: plate reverb after J. Dattorro, "Effect Design Part 1" (JAES 1997).
 *
 * The tank runs at 24 kHz (REV_TANK_FS): the mono input is decimated by 2 and the stereo taps are
 * interpolated by 2 with the HB1 polyphase IIR half-band (passband to 10 kHz, >= 112 dB from 14 kHz at 48 kHz).
 * Delay lengths are scaled from Dattorro's 29761 Hz to 24 kHz. This halves the tank memory (~73 KB) and the
 * tank CPU, and limits the wet signal to ~10 kHz, like the classic hardware plates/reverbs (EMT 250, Lexicon 224)
 * and well above where the damping filter already rolls the tail off. The dry path is untouched.
 * Arbitrary block sizes are supported: an odd 48 kHz sample is carried over, giving a constant 1-sample latency.
 *
 *  mono in (on-fade) -> pre-delay (PSRAM, two-head crossfade on change) -> bandwidth LP (12 kHz) -> lo-cut HP
 *  -> 4 input diffusers (142/107/379/277, g .75/.75/.625/.625)
 *  -> figure-eight tank, each half: modulated allpass (672/908, g -0.70, +-0..48 samples, 0.9 Hz quadrature
 *     LFO, cubic Hermite reads) -> delay (4453/4217) -> damping LP (P_REV_DAMP) -> x decay -> allpass
 *     (1800/2656, g = clamp(decay + .15, .25, .5)) -> delay (3720/3163) -> x decay -> other half
 *  -> Dattorro's 7 + 7 output taps (x0.6) -> width (M/S) -> dry/wet.
 * decay = 10^(-3 * L_loop / (4 fs RT60)), L_loop = sum of the 8 tank lengths (4 decay gains per loop), so
 * P_REV_DECAY is the RT60 of the tank loop in seconds.
 * Each tank line is a separate alloc_fast allocation, shortest first: the allocator may place the long
 * delay lines in PSRAM when internal RAM is scarce (they are sequential streams; the hot short allpasses
 * stay internal). Every stage runs block-wise: all lines are longer than a block, so each stage reads only data
 * written by earlier blocks (contiguous copies, no per-sample wrap checks).
 * Off = input fades out, tail rings out; processing stops after 1 s below -100 dBFS.
 */
#pragma once
#include <stdbool.h>
#include "dsp_biquad.h"
#include "dsp_dline.h"
#include "dsp_halfband.h"
#include "dsp_smooth.h"

#define REV_PREDLY_MAX_MS 150.f
#define REV_TANK_FS       24000    /* tank sample rate (decimation by 2) */
#define REV_MOD_MAX       24.f     /* modulation excursion (tank samples, 1 ms) at 100 % */

enum { RV_DIF1, RV_DIF2, RV_DIF3, RV_DIF4,
       RV_LMAP, RV_LD1, RV_LAP2, RV_LD2,
       RV_RMAP, RV_RD1, RV_RAP2, RV_RD2, RV_NLINES };

typedef struct {
    dsp_dline_t ln[RV_NLINES];
    int         nom[RV_NLINES];        /* nominal lengths (modulated APs: centre delay) */
    int         taps[2][7];            /* output tap offsets */
    dsp_dline_t pre;
    int         pd_cur, pd_next, pd_tgt;
    float       pd_xf;
    bool        pd_xfading;
    float       bw_z, damp_z[2];
    dsp_bq_t    locut;                 /* mono, channel 0 state */
    float       lfo_ph, exc_prev[2];   /* modulated-AP delay at the end of the previous block */
    dsp_sm_t    ldecay, damp, mod, llocut, width, mix;
    float       dec_cur;
    dsp_fade_t  on;
    float       energy;
    int         quiet;
    bool        running;
    int         loop_len;
    dsp_hb_t    dn, up;                /* 48k -> 24k decimator (ch 0 used), 24k -> 48k interpolator (L/R) */
    float       pend;                  /* odd input sample carried to the next block */
    int         has_pend;
    float       oq[2];                 /* 48 kHz output carried to the next block (0 or 1 sample) */
    int         oq_n;
    size_t      fast_bytes, big_bytes;
} dsp_reverb_t;

int   rev_init(dsp_reverb_t *r, void *(*alloc_fast)(size_t), void *(*alloc_big)(size_t));
void  rev_reset(dsp_reverb_t *r);
void  rev_set(dsp_reverb_t *r, bool on, float decay_s, float predelay_ms, float damp_pct, float mod_pct,
              float locut_hz, float width_pct, float mix_pct);
void  rev_settle(dsp_reverb_t *r);
void  rev_process(dsp_reverb_t *r, float *L, float *R, const dsp_blk_t *b);
bool  rev_running(const dsp_reverb_t *r);
/* tank decay gain for an RT60 (s) */
float rev_decay_gain(const dsp_reverb_t *r, float rt60_s);
