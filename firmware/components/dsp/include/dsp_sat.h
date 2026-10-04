/*
 * dsp_sat.h: 4x oversampled stereo saturation ("warmth").
 *
 *  x -> pre-emphasis(TONE) -> x DRIVE -> up 2x (HB1) -> up 2x (HB2)                         [192 kHz]
 *    -> shaper (odd C3 polynomial + even "warmth" term) -> DC block -> wet tilt(TONE)
 *    -> MIX inside the oversampled domain: y = c_wet * shaped + c_dry * x_os   (phase coherent)
 *    -> down (HB2) -> down (HB1) -> de-emphasis (exact inverse of the pre-emphasis)          [48 kHz]
 *    -> on/off crossfade against the dry input delayed by SAT_LATENCY (the chain's LF group delay)
 *
 * Shaper: u = clamp(x / X0, -1, 1);  y = G(u) + w H(u)
 *   G(u) = 35/16 * integral (1-u^2)^3  = 2.1875u - 2.1875u^3 + 1.3125u^5 - 0.3125u^7   (G(1) = 1)
 *   H(u) = 1 - (1-u^2)^4                                                              (even, H(0)=H'(0)=0)
 *   y'(u) = (1-u^2)^3 (2.1875 + 8 w u)  => monotonic for w < 0.273 (we use w <= 0.25), C3 at the clamp,
 *   unity small-signal gain (X0 = 2.1875). Because y is a polynomial of degree <= 8 inside the clamp, the
 *   shaped signal has no harmonics above 8 f0, which at 4x oversampling (192 kHz) never fold back below
 *   24 kHz for f0 <= 21 kHz: aliasing is structurally absent until the input exceeds the clamp point
 *   (+6.8 dB above full scale at 0 dB drive). Beyond it the C3 junction keeps the remaining aliases low.
 * Level compensation: c_wet = mix * comp(drive), comp = RMS ratio for a -12 dBFS sine (table at init),
 *   so turning DRIVE changes the character, not (much) the loudness.
 * TONE (+-100 %): first-order 2 kHz shelf pre-emphasis +-12 dB (more/less HF into the shaper, de-emphasised
 *   exactly afterwards) plus a +-3 dB tilt of the wet signal only.
 */
#pragma once
#include <stdbool.h>
#include "dsp_halfband.h"
#include "dsp_smooth.h"
#include "dsp_port.h"

#define SAT_LATENCY   5                /* samples @48k, constant (also while bypassed: no time jumps) */
#define SAT_OS        4
#define SAT_X0        2.1875f
#define SAT_WARM_MAX  0.25f
#define SAT_COMP_N    25               /* comp table: 0..24 dB in 1 dB steps */

typedef struct { float b0, b1, a1; float z[2]; } dsp_fo_t;   /* first-order TDF section, stereo */

typedef struct {
    dsp_hb_t   up1, up2, dn2, dn1;
    dsp_fo_t   pre, de, tilt, dc;
    dsp_sm_t   drive_db, warmth, tone, mix;
    dsp_fade_t on;
    float      dly[2][SAT_LATENCY];
    int        dpos;
    float      comp_tab[SAT_COMP_N];
    float      k_pre, k_tilt, r_dc;     /* design constants */
    float      tone_designed;
    bool       tone_run;      /* emphasis/tilt filters currently running */
    float      peak_in;                 /* max |x| of the last block (meter) */
    bool       running;
} dsp_sat_t;

/* The waveshaper (unity small-signal slope), w in [0, SAT_WARM_MAX]. */
static inline float sat_shape(float x, float w)
{
    float u = x * (1.f / SAT_X0);
    u = u > 1.f ? 1.f : (u < -1.f ? -1.f : u);
    const float s = u * u;
    const float g = u * (2.1875f + s * (-2.1875f + s * (1.3125f - 0.3125f * s)));
    const float v = 1.f - s, v2 = v * v;
    return g + w * (1.f - v2 * v2);
}

void  sat_init(dsp_sat_t *s);
void  sat_reset(dsp_sat_t *s);
/* Targets: drive 0..24 dB, warmth/mix 0..100 %, tone -100..100 %. */
void  sat_set(dsp_sat_t *s, bool on, float drive_db, float warmth_pct, float tone_pct, float mix_pct);
void  sat_settle(dsp_sat_t *s);
void  sat_process(dsp_sat_t *s, float *L, float *R, const dsp_blk_t *b);
/* Level compensation for a drive (dB), interpolated from the table. */
float sat_comp(const dsp_sat_t *s, float drive_db);
bool  sat_active(const dsp_sat_t *s);
