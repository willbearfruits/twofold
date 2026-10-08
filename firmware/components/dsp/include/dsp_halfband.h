/*
 * dsp_halfband.h: polyphase IIR half-band 2x up/down samplers (HIIR-style, after Laurent de Soras),
 * stereo. H(z) = 1/2 (A0(z^2) + z^-1 A1(z^2)), A0/A1 = cascades of first-order allpasses in z^2,
 * coefficients from the elliptic design (Valenzuela & Constantinides). Each polyphase branch runs at the
 * low rate, so a 7-coefficient stage costs 7 multiplies per low-rate sample.
 *
 * Why IIR rather than a linear-phase FIR for TWOFOLD's 4x saturation:
 *   - stage 1 (48k<->96k, pass 20 kHz / stop 28 kHz, 112 dB) needs 7 multiplies per sample; a FIR with the
 *     same spec needs ~78 taps (~39 non-zero, ~20 with symmetry): ~4x the work, per stage, per direction.
 *   - latency: the whole up+down chain has 5.06 samples group delay at LF (~0.1 ms); a FIR chain ~45 samples.
 *     This matters because THRU-FX direct monitoring runs through the chain.
 *   - the phase non-linearity is confined to the top octave; the wet/dry MIX is done inside the oversampled
 *     domain (dry and wet see the same filters) so mixing never comb-filters.
 *
 * Specs (verified in host tests): HB1 7 coefs: passband ripple < 1e-10 dB to 20 kHz, stopband >= 112 dB from
 * 28 kHz (at 96 kHz). HB2 4 coefs: >= 131 dB from 76 kHz (at 192 kHz).
 */
#pragma once

#define HB_MAXC 8
#define HB1_NC  7
#define HB2_NC  4
extern const float g_hb1_coef[HB1_NC];
extern const float g_hb2_coef[HB2_NC];

typedef struct { float x[2][HB_MAXC], y[2][HB_MAXC]; } dsp_hb_t;   /* [channel][section] */

void hb_reset(dsp_hb_t *h);
void hb_flush(dsp_hb_t *h);
/* 2x up: n input frames -> 2n output frames. Unity passband gain. */
void hb1_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n);
void hb2_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n);
/* 2x down: 2n input frames -> n output frames. */
void hb1_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n);
void hb2_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n);
/* mono 2x down with HB1 (channel 0 state): 2n input samples -> n output */
void hb1_down_mono(dsp_hb_t *h, const float *in, float *out, int n);
