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

static inline __attribute__((always_inline)) void up_impl(dsp_hb_t *h, const float *c, int nc,
    const float *iL, const float *iR, float *oL, float *oR, int n)
{
    float eL[2 * DSP_BLOCK_MAX], eR[2 * DSP_BLOCK_MAX], dL[2 * DSP_BLOCK_MAX], dR[2 * DSP_BLOCK_MAX];
    for (int k = 0; k < n; k++) { eL[k] = dL[k] = iL[k]; eR[k] = dR[k] = iR[k]; }
    sections(eL, eR, n, c, nc, 0, h->x[0], h->y[0], h->x[1], h->y[1]);     /* path 0: even coefficients */
    sections(dL, dR, n, c, nc, 1, h->x[0], h->y[0], h->x[1], h->y[1]);     /* path 1: odd coefficients */
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
    sections(eL, eR, n, c, nc, 0, h->x[0], h->y[0], h->x[1], h->y[1]);
    sections(dL, dR, n, c, nc, 1, h->x[0], h->y[0], h->x[1], h->y[1]);
    for (int k = 0; k < n; k++) { oL[k] = 0.5f * (eL[k] + dL[k]); oR[k] = 0.5f * (eR[k] + dR[k]); }
}

DSP_HOT void hb1_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ up_impl(h, g_hb1_coef, HB1_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb2_up(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ up_impl(h, g_hb2_coef, HB2_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb1_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ down_impl(h, g_hb1_coef, HB1_NC, iL, iR, oL, oR, n); }
DSP_HOT void hb2_down(dsp_hb_t *h, const float *iL, const float *iR, float *oL, float *oR, int n)
{ down_impl(h, g_hb2_coef, HB2_NC, iL, iR, oL, oR, n); }
