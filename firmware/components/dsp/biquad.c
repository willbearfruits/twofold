/* biquad.c: stereo TDF-II stages and the fast RBJ designer (see dsp_biquad.h). */
#include "dsp_biquad.h"
#include "dsp_port.h"

void bq_reset(dsp_bq_t *q)
{
    q->z1[0] = q->z1[1] = q->z2[0] = q->z2[1] = 0.f;
}

void bq_identity(biquad_coef_t *c)
{
    c->b0 = 1.f; c->b1 = 0.f; c->b2 = 0.f; c->a1 = 0.f; c->a2 = 0.f;
}

DSP_HOT void bq_process2(dsp_bq_t *q, float *L, float *R, int n)
{
    const float b0 = q->c.b0, b1 = q->c.b1, b2 = q->c.b2, a1 = q->c.a1, a2 = q->c.a2;
    float l1 = q->z1[0], l2 = q->z2[0], r1 = q->z1[1], r2 = q->z2[1];
    for (int i = 0; i < n; i++) {
        const float xl = L[i], xr = R[i];
        const float yl = b0 * xl + l1;
        const float yr = b0 * xr + r1;
        l1 = b1 * xl - a1 * yl + l2;
        r1 = b1 * xr - a1 * yr + r2;
        l2 = b2 * xl - a2 * yl;
        r2 = b2 * xr - a2 * yr;
        L[i] = yl;
        R[i] = yr;
    }
    q->z1[0] = l1; q->z2[0] = l2; q->z1[1] = r1; q->z2[1] = r2;
}

DSP_HOT void bq_process2_ramp(dsp_bq_t *q, const biquad_coef_t *to, float *L, float *R, int n)
{
    const float inv = 1.f / (float)n;
    float b0 = q->c.b0, b1 = q->c.b1, b2 = q->c.b2, a1 = q->c.a1, a2 = q->c.a2;
    const float db0 = (to->b0 - b0) * inv, db1 = (to->b1 - b1) * inv, db2 = (to->b2 - b2) * inv;
    const float da1 = (to->a1 - a1) * inv, da2 = (to->a2 - a2) * inv;
    float l1 = q->z1[0], l2 = q->z2[0], r1 = q->z1[1], r2 = q->z2[1];
    for (int i = 0; i < n; i++) {
        b0 += db0; b1 += db1; b2 += db2; a1 += da1; a2 += da2;
        const float xl = L[i], xr = R[i];
        const float yl = b0 * xl + l1;
        const float yr = b0 * xr + r1;
        l1 = b1 * xl - a1 * yl + l2;
        r1 = b1 * xr - a1 * yr + r2;
        l2 = b2 * xl - a2 * yl;
        r2 = b2 * xr - a2 * yr;
        L[i] = yl;
        R[i] = yr;
    }
    q->c = *to;   /* exact end point, no accumulated rounding */
    q->z1[0] = l1; q->z2[0] = l2; q->z1[1] = r1; q->z2[1] = r2;
}

DSP_HOT void bq_process1(dsp_bq_t *q, float *x, int n)
{
    const float b0 = q->c.b0, b1 = q->c.b1, b2 = q->c.b2, a1 = q->c.a1, a2 = q->c.a2;
    float s1 = q->z1[0], s2 = q->z2[0];
    for (int i = 0; i < n; i++) {
        const float in = x[i];
        const float y = b0 * in + s1;
        s1 = b1 * in - a1 * y + s2;
        s2 = b2 * in - a2 * y;
        x[i] = y;
    }
    q->z1[0] = s1; q->z2[0] = s2;
}

float bq_flush(dsp_bq_t *q)
{
    float m = 0.f;
    for (int c = 0; c < 2; c++) {
        q->z1[c] = dsp_flush(q->z1[c]);
        q->z2[c] = dsp_flush(q->z2[c]);
        m = dsp_maxf(m, dsp_maxf(dsp_absf(q->z1[c]), dsp_absf(q->z2[c])));
    }
    return m;
}

void bq_design_fast(biquad_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q)
{
    f0 = dsp_clampf(f0, 1.f, 0.49f * fs);
    if (q < 0.05f) q = 0.05f;
    /* half angle: h = sin(w0/2), k = cos(w0/2); sin(w0) = 2hk; 1 - cos(w0) = 2h^2 */
    const float ph = 0.5f * f0 / fs;                 /* w0/2 in cycles */
    const float h = dsp_sin2pi(ph), k = dsp_cos2pi(ph);
    const float sw = 2.f * h * k, omc = 2.f * h * h, cw = 1.f - omc;
    const float alpha = sw / (2.f * q);
    float b0, b1, b2, a0, a1, a2;
    switch (type) {
    case RBJ_PEAK: {
        const float A = dsp_exp2(gain_db * (0.5f * DSP_DB2LOG2)), iA = dsp_exp2(-gain_db * (0.5f * DSP_DB2LOG2));
        b0 = 1.f + alpha * A; b1 = -2.f * cw; b2 = 1.f - alpha * A;
        a0 = 1.f + alpha * iA; a1 = -2.f * cw; a2 = 1.f - alpha * iA;
        break;
    }
    case RBJ_LOWSHELF: {
        const float A = dsp_exp2(gain_db * (0.5f * DSP_DB2LOG2)), sA = dsp_exp2(gain_db * (0.25f * DSP_DB2LOG2));
        const float Ap1 = A + 1.f, Am1 = A - 1.f, sa = 2.f * sA * alpha;
        b0 = A * (2.f + Am1 * omc + sa);
        b1 = 2.f * A * (-2.f + Ap1 * omc);
        b2 = A * (2.f + Am1 * omc - sa);
        a0 = 2.f * A - Am1 * omc + sa;
        a1 = -2.f * (2.f * A - Ap1 * omc);
        a2 = 2.f * A - Am1 * omc - sa;
        break;
    }
    case RBJ_HIGHSHELF: {
        const float A = dsp_exp2(gain_db * (0.5f * DSP_DB2LOG2)), sA = dsp_exp2(gain_db * (0.25f * DSP_DB2LOG2));
        const float Ap1 = A + 1.f, Am1 = A - 1.f, sa = 2.f * sA * alpha;
        b0 = A * (2.f * A - Am1 * omc + sa);
        b1 = -2.f * A * (2.f * A - Ap1 * omc);
        b2 = A * (2.f * A - Am1 * omc - sa);
        a0 = 2.f + Am1 * omc + sa;
        a1 = 2.f * (-2.f + Ap1 * omc);
        a2 = 2.f + Am1 * omc - sa;
        break;
    }
    case RBJ_LOWPASS:
        b0 = 0.5f * omc; b1 = omc; b2 = 0.5f * omc;
        a0 = 1.f + alpha; a1 = -2.f * cw; a2 = 1.f - alpha;
        break;
    case RBJ_HIGHPASS:
    default:
        b0 = 0.5f * (2.f - omc); b1 = -(2.f - omc); b2 = 0.5f * (2.f - omc);
        a0 = 1.f + alpha; a1 = -2.f * cw; a2 = 1.f - alpha;
        break;
    }
    const float ia0 = 1.f / a0;
    c->b0 = b0 * ia0; c->b1 = b1 * ia0; c->b2 = b2 * ia0;
    c->a1 = a1 * ia0; c->a2 = a2 * ia0;
}
