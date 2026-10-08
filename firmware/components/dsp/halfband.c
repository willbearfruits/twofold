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

/* Each section is a first-order allpass in z^2 run at the low rate: y = c * (u - y[-1]) + u[-1].
 * Processed section-major: one section sweeps the whole block for L and R together, with its state and
 * coefficient in registers. The two channels are independent chains, which hides the FPU latency on the
 * in-order Xtensa core (the sample-major version reloaded/stored all state every sample). Same arithmetic per
 * sample as before, so the output is bit-identical. */
static DSP_HOT void sections(float *restrict a, float *restrict b, int n, const float *c, int nc, int i0,
                             float *xa, float *ya, float *xb, float *yb)
{
    for (int i = i0; i < nc; i += 2) {
        const float k = c[i];
        float ux = xa[i], uy = ya[i], vx = xb[i], vy = yb[i];
        for (int j = 0; j < n; j++) {
            const float u = a[j], v = b[j];
            uy = (u - uy) * k + ux;
            vy = (v - vy) * k + vx;
            ux = u; vx = v;
            a[j] = uy; b[j] = vy;
        }
        xa[i] = ux; ya[i] = uy; xb[i] = vx; yb[i] = vy;
    }
}

/* Paths 0 and 1 together: section 2p of path 0 and section 2p+1 of path 1 run in the same loop, so four
 * independent recurrences (L/R x path) share the FPU pipeline. Same per-sample arithmetic as sections(). */
static DSP_HOT void sections2(float *restrict a, float *restrict b, float *restrict p, float *restrict q, int n,
                              const float *c, int nc, float *xa, float *ya, float *xb, float *yb)
{
    int i = 0;
    for (; i + 1 < nc; i += 2) {
        const float k0 = c[i], k1 = c[i + 1];
        float ux = xa[i], uy = ya[i], vx = xb[i], vy = yb[i];
        float sx = xa[i + 1], sy = ya[i + 1], tx = xb[i + 1], ty = yb[i + 1];
        for (int j = 0; j < n; j++) {
            const float u = a[j], v = b[j], s = p[j], t = q[j];
            uy = (u - uy) * k0 + ux;
            vy = (v - vy) * k0 + vx;
            sy = (s - sy) * k1 + sx;
            ty = (t - ty) * k1 + tx;
            ux = u; vx = v; sx = s; tx = t;
            a[j] = uy; b[j] = vy; p[j] = sy; q[j] = ty;
        }
        xa[i] = ux; ya[i] = uy; xb[i] = vx; yb[i] = vy;
        xa[i + 1] = sx; ya[i + 1] = sy; xb[i + 1] = tx; yb[i + 1] = ty;
    }
    if (i < nc) sections(a, b, n, c, nc, i, xa, ya, xb, yb);      /* odd count: last path-0 section alone */
}

static inline __attribute__((always_inline)) void up_impl(dsp_hb_t *h, const float *c, int nc,
    const float *iL, const float *iR, float *oL, float *oR, int n)
{
    float eL[2 * DSP_BLOCK_MAX], eR[2 * DSP_BLOCK_MAX], dL[2 * DSP_BLOCK_MAX], dR[2 * DSP_BLOCK_MAX];
    for (int k = 0; k < n; k++) { eL[k] = dL[k] = iL[k]; eR[k] = dR[k] = iR[k]; }
    sections2(eL, eR, dL, dR, n, c, nc, h->x[0], h->y[0], h->x[1], h->y[1]);   /* path 0 even, path 1 odd coefs */
    for (int k = 0; k < n; k++) {
        oL[2 * k] = eL[k]; oL[2 * k + 1] = dL[k];
        oR[2 * k] = eR[k]; oR[2 * k + 1] = dR[k];
    }
}

static inline __attribute__((always_inline)) void down_impl(dsp_hb_t *h, const float *c, int nc,
    const float *iL, const float *iR, float *oL, float *oR, int n)
{
    float eL[2 * DSP_BLOCK_MAX], eR[2 * DSP_BLOCK_MAX], dL[2 * DSP_BLOCK_MAX], dR[2 * DSP_BLOCK_MAX];
    for (int k = 0; k < n; k++) {
        eL[k] = iL[2 * k + 1]; dL[k] = iL[2 * k];
        eR[k] = iR[2 * k + 1]; dR[k] = iR[2 * k];
    }
    sections2(eL, eR, dL, dR, n, c, nc, h->x[0], h->y[0], h->x[1], h->y[1]);
    for (int k = 0; k < n; k++) { oL[k] = 0.5f * (eL[k] + dL[k]); oR[k] = 0.5f * (eR[k] + dR[k]); }
}

/* Mono 2x down: path 0 (even coefficients) on the odd input samples and path 1 (odd coefficients) on the even
 * ones run as the two chains of one loop. State: channel 0 of h. */
DSP_HOT void hb1_down_mono(dsp_hb_t *h, const float *in, float *out, int n)
{
    float e[DSP_BLOCK_MAX], d[DSP_BLOCK_MAX];
    for (int k = 0; k < n; k++) { e[k] = in[2 * k + 1]; d[k] = in[2 * k]; }
    const float *c = g_hb1_coef;
    float *x = h->x[0], *y = h->y[0];
    int i = 0;
    for (; i + 1 < HB1_NC; i += 2) {
        const float k0 = c[i], k1 = c[i + 1];
        float ux = x[i], uy = y[i], sx = x[i + 1], sy = y[i + 1];
        for (int j = 0; j < n; j++) {
            const float u = e[j], s = d[j];
            uy = (u - uy) * k0 + ux;
            sy = (s - sy) * k1 + sx;
            ux = u; sx = s;
            e[j] = uy; d[j] = sy;
        }
        x[i] = ux; y[i] = uy; x[i + 1] = sx; y[i + 1] = sy;
    }
    for (; i < HB1_NC; i += 2) {                           /* remaining path-0 section */
        const float k0 = c[i];
        float ux = x[i], uy = y[i];
        for (int j = 0; j < n; j++) { const float u = e[j]; uy = (u - uy) * k0 + ux; ux = u; e[j] = uy; }
        x[i] = ux; y[i] = uy;
    }
    for (int k = 0; k < n; k++) out[k] = 0.5f * (e[k] + d[k]);
}

DSP_HOT void hb1_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ up_impl(h, g_hb1_coef, HB1_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb2_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ up_impl(h, g_hb2_coef, HB2_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb1_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ down_impl(h, g_hb1_coef, HB1_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb2_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ down_impl(h, g_hb2_coef, HB2_NC, iL, iR, oL, oR, n); }
