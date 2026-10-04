/* halfband.c: polyphase IIR half-band up/down samplers (see dsp_halfband.h). */
#include "dsp_halfband.h"
#include "dsp_math.h"
#include "dsp_port.h"
#include <string.h>

/* designed with host/dsp_tests/tools/hiir_design.py (transition 0.0833 / 0.29 of the high rate) */
const float g_hb1_coef[HB1_NC] = {
    0.033406262742f, 0.126323304631f, 0.260636989050f, 0.415955120793f,
    0.577420095025f, 0.739761478630f, 0.908425363017f,
};
const float g_hb2_coef[HB2_NC] = {
    0.038540543221f, 0.157814819878f, 0.373658507345f, 0.732566622455f,
};

void hb_reset(dsp_hb_t *h) { memset(h, 0, sizeof *h); }

void hb_flush(dsp_hb_t *h)
{
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < HB_MAXC; i++) { h->x[c][i] = dsp_flush(h->x[c][i]); h->y[c][i] = dsp_flush(h->y[c][i]); }
}

/* One polyphase step: path 0 (even coefs) gets s0, path 1 (odd coefs) gets s1. NC is a compile-time
 * constant at every call site, so the section loop is fully unrolled. Each section:
 * y = c * (x - y[-1]) + x[-1]  (first-order allpass in z^2, run at the low rate). */
static inline __attribute__((always_inline)) void hb_step(const float *c, int nc, float *xs, float *ys, float *s0, float *s1)
{
    float e = *s0, o = *s1;
    for (int i = 0; i < nc; i += 2) {
        const float te = e;
        e = (e - ys[i]) * c[i] + xs[i];
        xs[i] = te; ys[i] = e;
        if (i + 1 < nc) {
            const float to = o;
            o = (o - ys[i + 1]) * c[i + 1] + xs[i + 1];
            xs[i + 1] = to; ys[i + 1] = o;
        }
    }
    *s0 = e; *s1 = o;
}

static inline __attribute__((always_inline)) void up_impl(dsp_hb_t *h, const float *c, int nc,
    const float *iL, const float *iR, float *oL, float *oR, int n)
{
    float xl[HB_MAXC], yl[HB_MAXC], xr[HB_MAXC], yr[HB_MAXC];
    memcpy(xl, h->x[0], sizeof xl); memcpy(yl, h->y[0], sizeof yl);
    memcpy(xr, h->x[1], sizeof xr); memcpy(yr, h->y[1], sizeof yr);
    for (int k = 0; k < n; k++) {
        float le = iL[k], lo = iL[k], re = iR[k], ro = iR[k];
        hb_step(c, nc, xl, yl, &le, &lo);
        hb_step(c, nc, xr, yr, &re, &ro);
        oL[2 * k] = le; oL[2 * k + 1] = lo;
        oR[2 * k] = re; oR[2 * k + 1] = ro;
    }
    memcpy(h->x[0], xl, sizeof xl); memcpy(h->y[0], yl, sizeof yl);
    memcpy(h->x[1], xr, sizeof xr); memcpy(h->y[1], yr, sizeof yr);
}

static inline __attribute__((always_inline)) void down_impl(dsp_hb_t *h, const float *c, int nc,
    const float *iL, const float *iR, float *oL, float *oR, int n)
{
    float xl[HB_MAXC], yl[HB_MAXC], xr[HB_MAXC], yr[HB_MAXC];
    memcpy(xl, h->x[0], sizeof xl); memcpy(yl, h->y[0], sizeof yl);
    memcpy(xr, h->x[1], sizeof xr); memcpy(yr, h->y[1], sizeof yr);
    for (int k = 0; k < n; k++) {
        float le = iL[2 * k + 1], lo = iL[2 * k], re = iR[2 * k + 1], ro = iR[2 * k];
        hb_step(c, nc, xl, yl, &le, &lo);
        hb_step(c, nc, xr, yr, &re, &ro);
        oL[k] = 0.5f * (le + lo);
        oR[k] = 0.5f * (re + ro);
    }
    memcpy(h->x[0], xl, sizeof xl); memcpy(h->y[0], yl, sizeof yl);
    memcpy(h->x[1], xr, sizeof xr); memcpy(h->y[1], yr, sizeof yr);
}

DSP_HOT void hb1_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ up_impl(h, g_hb1_coef, HB1_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb2_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ up_impl(h, g_hb2_coef, HB2_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb1_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ down_impl(h, g_hb1_coef, HB1_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb2_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ down_impl(h, g_hb2_coef, HB2_NC, iL, iR, oL, oR, n); }
