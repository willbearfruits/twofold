/*
 * dsp_xfeed.h: headphone crossfeed (Bauer BS2B style, mid/side form).
 *
 *   s = R - L;  y = AP(LP(s));  L' = L + g y;  R' = R - g y
 * LP: one-pole at FC (300..2000 Hz, default 700). AP: first-order allpass with ~220 us low-frequency group delay
 * (the interaural time difference). FEED (dB) is the cross-feed level relative to the direct signal at low
 * frequencies: g/(1-g) = 10^(-FEED/20). Mono input passes through bit-exactly (y == 0), and off = g -> 0,
 * then exact bypass. ~5 multiplies per frame.
 */
#pragma once
#include <stdbool.h>
#include "dsp_smooth.h"

typedef struct {
    dsp_sm_t g, lfc;            /* cross gain 0..0.5, log2(fc) */
    float    lp_z, ap_x1, ap_y1;
    float    lp_k, ap_a, dfc;   /* coefficients, designed-for fc */
    bool     active;
} dsp_xfeed_t;

void xf_init(dsp_xfeed_t *x);
void xf_reset(dsp_xfeed_t *x);
void xf_set(dsp_xfeed_t *x, bool on, float feed_db, float fc_hz);
void xf_settle(dsp_xfeed_t *x);
void xf_process(dsp_xfeed_t *x, float *L, float *R, const dsp_blk_t *b);
/* magnitude (dB) of the cross path g*LP relative to the direct path, for the UI */
float xf_cross_db(float feed_db, float fc_hz, float f_hz);
