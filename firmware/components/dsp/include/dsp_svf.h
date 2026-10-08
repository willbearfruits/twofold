/*
 * dsp_svf.h: linear trapezoidal state-variable filter (A. Simper, Cytomic 2013, "SvfLinearTrapOptimised2"),
 * stereo, with bell / low-shelf / high-shelf responses matched to the RBJ cookbook designs in rbj.c.
 *
 * Why, next to the TDF-II biquad: a biquad's poles live in a2 = r^2 ~ 0.9998 for a low-frequency, high-Q band,
 * so a float coefficient keeps only ~12 significant bits of the pole position and the recursion's rounding is
 * amplified by 1/|A| (~70 dB at a 30 Hz Q=10 band). The SVF's coefficients are g = tan(pi f/fs) ~ 0.002 and
 * k = 1/Q, both with full float precision, and its integrator states carry the signal, not a near-cancelling
 * difference. Measured (host/dsp_tests eqlf): residual vs the double-precision ideal -79 dBFS (TDF-II) ->
 * see docs/test-results for the SVF figure. The EQ uses the SVF below fs/7.5 and the biquad above (the
 * biquad is fine there and the SVF's g grows large near Nyquist).
 */
#pragma once
#include "rbj.h"

typedef struct { float a1, a2, a3, m0, m1, m2; } svf_coef_t;
typedef struct { svf_coef_t c; float ic1[2], ic2[2]; } dsp_svf_t;

void svf_design(svf_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q);        /* exact (double) */
void svf_design_fast(svf_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q);   /* float, while moving */
void svf_identity(svf_coef_t *c);
void svf_reset(dsp_svf_t *s);
void svf_process2(dsp_svf_t *s, float *L, float *R, int n);
/* coefficients interpolated linearly per sample from s->c to `to` (s->c == *to afterwards) */
void svf_process2_ramp(dsp_svf_t *s, const svf_coef_t *to, float *L, float *R, int n);
float svf_flush(dsp_svf_t *s);          /* denormal flush; returns max |state| */
