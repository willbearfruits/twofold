/* xfeed.c: see dsp_xfeed.h */
#include "dsp_xfeed.h"
#include "dsp_port.h"
#include "dsp_math.h"
#include <string.h>

#define ITD_US 220.f

static float cross_gain(float feed_db)
{
    const float r = dsp_exp2(-feed_db * 0.1660964f);        /* 10^(-feed/20) */
    return r / (1.f + r);
}

void xf_init(dsp_xfeed_t *x)
{
    memset(x, 0, sizeof *x);
    sm_init(&x->g, 0.f, 1e-5f);
    sm_init(&x->lfc, dsp_log2(700.f), 1e-5f);
    /* first-order allpass y = a x + x1 - a y1: LF group delay (1-a)/(1+a) samples */
    const float d = ITD_US * 1e-6f * (float)DSP_FS;
    x->ap_a = (1.f - d) / (1.f + d);
    x->dfc = -1.f;
}

void xf_reset(dsp_xfeed_t *x) { x->lp_z = x->ap_x1 = x->ap_y1 = 0.f; x->active = false; }

void xf_set(dsp_xfeed_t *x, bool on, float feed_db, float fc)
{
    sm_set(&x->g, on ? cross_gain(dsp_clampf(feed_db, 0.5f, 20.f)) : 0.f);
    sm_set(&x->lfc, dsp_log2(dsp_clampf(fc, 100.f, 5000.f)));
}

static void design(dsp_xfeed_t *x, float lfc)
{
    const float fc = dsp_exp2(lfc);
    x->lp_k = 1.f - dsp_exp2(-DSP_TWO_PI * fc / (float)DSP_FS * 1.44269504f);
    x->dfc = lfc;
}

void xf_settle(dsp_xfeed_t *x)
{
    sm_jump(&x->g, x->g.tgt); sm_jump(&x->lfc, x->lfc.tgt);
    design(x, x->lfc.cur);
    x->active = x->g.cur != 0.f;
}

float xf_cross_db(float feed_db, float fc, float f)
{
    const float g = cross_gain(feed_db);
    const float r = f / fc;
    const float lp = 1.f / dsp_exp2(0.5f * dsp_log2(1.f + r * r));   /* |1/(1+j f/fc)| */
    const float cross = g * lp, direct = 1.f - g * lp;
    return 20.f * 0.30102999f * (dsp_log2(cross + 1e-9f) - dsp_log2(direct));
}

DSP_HOT void xf_process(dsp_xfeed_t *x, float *L, float *R, const dsp_blk_t *b)
{
    const dsp_ramp_t g = sm_ramp(&x->g, b);
    const float lfc = sm_step(&x->lfc, b->k);
    if (!x->active) {
        if (g.v == 0.f && g.d == 0.f) return;            /* exact bypass */
        xf_reset(x);
        x->active = true;
    }
    if (lfc != x->dfc) design(x, lfc);
    const float k = x->lp_k, a = x->ap_a;
    float z = x->lp_z, x1 = x->ap_x1, y1 = x->ap_y1, gg = g.v;
    for (int i = 0; i < b->n; i++) {
        gg += g.d;
        const float s = R[i] - L[i];
        z += k * (s - z);                                 /* one-pole LP */
        const float y = a * z + x1 - a * y1;              /* allpass (ITD) */
        x1 = z; y1 = y;
        L[i] += gg * y;
        R[i] -= gg * y;
    }
    x->lp_z = dsp_flush(z); x->ap_x1 = dsp_flush(x1); x->ap_y1 = dsp_flush(y1);
    if (g.v + g.d * (float)b->n == 0.f && x->g.cur == 0.f) x->active = false;
}
