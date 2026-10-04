/*
 * dsp_biquad.h: stereo TDF-II biquad stages + a fast float RBJ designer for moving parameters.
 *
 * Why own TDF-II instead of esp-dsp's dsps_biquad_f32: that one is DF-II (poles before zeros), whose
 * internal state gains up to 1/|A(e^jw)| (~+100 dB for a 20 Hz shelf at 48 kHz), so its float rounding
 * noise is amplified at low frequencies. TDF-II keeps state magnitudes near the signal level. Stereo pairs
 * are processed in one loop (two independent dependency chains keep the Xtensa FPU pipeline busy).
 */
#pragma once
#include "rbj.h"
#include "dsp_math.h"

typedef struct {
    biquad_coef_t c;          /* current coefficients */
    float z1[2], z2[2];       /* TDF-II states, per channel */
} dsp_bq_t;

void bq_reset(dsp_bq_t *q);
/* Identity coefficients (b = a = {1,0,0}). */
void bq_identity(biquad_coef_t *c);
/* In-place stereo processing with constant coefficients. */
void bq_process2(dsp_bq_t *q, float *L, float *R, int n);
/* In-place stereo processing, coefficients interpolated linearly per sample from q->c to `to`
 * (q->c == *to afterwards). Linear interpolation of (a1, a2) stays inside the stability triangle. */
void bq_process2_ramp(dsp_bq_t *q, const biquad_coef_t *to, float *L, float *R, int n);
/* Mono (channel 0 state). */
void bq_process1(dsp_bq_t *q, float *x, int n);
/* Flush denormal-bound states; returns max |state| (used to decide when a 0 dB band may be skipped). */
float bq_flush(dsp_bq_t *q);

/* Fast float RBJ design (same formulas as rbj_design(), no libm/double; one division).
 * f0 in Hz, gain in dB, q > 0. 1-cos(w0) is formed as 2 sin^2(w0/2) to keep low-frequency precision.
 * Used while parameters move; settled EQ bands switch to the exact rbj_design() coefficients. */
void bq_design_fast(biquad_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q);
