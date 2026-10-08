/* svf.c: see dsp_svf.h */
#include "dsp_svf.h"
#include "dsp_math.h"
#include "dsp_port.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void svf_identity(svf_coef_t *c) { c->a1 = c->a2 = c->a3 = 0.f; c->m0 = 1.f; c->m1 = c->m2 = 0.f; }

/* Exact designer (double, libm): used for settled bands, like rbj_design(). Called at most once per block. */
void svf_design(svf_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q)
{
    if (f0 < 1.f) f0 = 1.f;
    if (f0 > 0.49f * fs) f0 = 0.49f * fs;
    if (q < 0.05f) q = 0.05f;
    const double A = pow(10.0, gain_db / 40.0);
    double g = tan(M_PI * (double)f0 / fs);
    double k = 1.0 / q, m0, m1, m2;
    switch (type) {
    case RBJ_PEAK:      k = 1.0 / (q * A); m0 = 1; m1 = k * (A * A - 1); m2 = 0; break;
    case RBJ_LOWSHELF:  g /= sqrt(A); m0 = 1; m1 = k * (A - 1); m2 = A * A - 1; break;
    case RBJ_HIGHSHELF: g *= sqrt(A); m0 = A * A; m1 = k * (1 - A) * A; m2 = 1 - A * A; break;
    case RBJ_LOWPASS:   m0 = 0; m1 = 0; m2 = 1; break;
    case RBJ_HIGHPASS:
    default:            m0 = 1; m1 = -k; m2 = -1; break;
    }
    const double a1 = 1.0 / (1.0 + g * (g + k));
    c->a1 = (float)a1; c->a2 = (float)(g * a1); c->a3 = (float)(g * g * a1);
    c->m0 = (float)m0; c->m1 = (float)m1; c->m2 = (float)m2;
}

/* Fast float designer (no libm, no double): used every block while a band moves. */
void svf_design_fast(svf_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q)
{
    f0 = dsp_clampf(f0, 1.f, 0.49f * fs);
    if (q < 0.05f) q = 0.05f;
    const float A = dsp_exp2(gain_db * (3.32192809f / 40.f));        /* 10^(dB/40) */
    const float ph = f0 / fs;                                          /* cycles: tan(pi f/fs) = sin/cos of half a turn */
    float g = dsp_sin2pi(0.5f * ph) / dsp_cos2pi(0.5f * ph);
    float k = 1.f / q, m0, m1, m2;
    switch (type) {
    case RBJ_PEAK:      k = 1.f / (q * A); m0 = 1.f; m1 = k * (A * A - 1.f); m2 = 0.f; break;
    case RBJ_LOWSHELF:  g /= dsp_exp2(0.5f * dsp_log2(A)); m0 = 1.f; m1 = k * (A - 1.f); m2 = A * A - 1.f; break;
    case RBJ_HIGHSHELF: g *= dsp_exp2(0.5f * dsp_log2(A)); m0 = A * A; m1 = k * (1.f - A) * A; m2 = 1.f - A * A; break;
    case RBJ_LOWPASS:   m0 = 0.f; m1 = 0.f; m2 = 1.f; break;
    case RBJ_HIGHPASS:
    default:            m0 = 1.f; m1 = -k; m2 = -1.f; break;
    }
    c->a1 = 1.f / (1.f + g * (g + k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
    c->m0 = m0; c->m1 = m1; c->m2 = m2;
}

void svf_reset(dsp_svf_t *s) { s->ic1[0] = s->ic1[1] = s->ic2[0] = s->ic2[1] = 0.f; }

/* L and R interleaved statement by statement: two independent dependency chains for the in-order FPU. */
#define SVF_STEP2(xl, xr, l1, l2, r1, r2, ol, orr)                     \
    do {                                                               \
        const float v0l = (xl), v0r = (xr);                            \
        const float v3l = v0l - (l2), v3r = v0r - (r2);                \
        const float v1l = a1 * (l1) + a2 * v3l;                        \
        const float v1r = a1 * (r1) + a2 * v3r;                        \
        const float v2l = (l2) + a2 * (l1) + a3 * v3l;                 \
        const float v2r = (r2) + a2 * (r1) + a3 * v3r;                 \
        (l1) = 2.f * v1l - (l1); (r1) = 2.f * v1r - (r1);              \
        (l2) = 2.f * v2l - (l2); (r2) = 2.f * v2r - (r2);              \
        (ol) = m0 * v0l + m1 * v1l + m2 * v2l;                         \
        (orr) = m0 * v0r + m1 * v1r + m2 * v2r;                        \
    } while (0)

DSP_HOT void svf_process2(dsp_svf_t *s, float *L, float *R, int n)
{
    const float a1 = s->c.a1, a2 = s->c.a2, a3 = s->c.a3, m0 = s->c.m0, m1 = s->c.m1, m2 = s->c.m2;
    float l1 = s->ic1[0], l2 = s->ic2[0], r1 = s->ic1[1], r2 = s->ic2[1];
    for (int i = 0; i < n; i++) SVF_STEP2(L[i], R[i], l1, l2, r1, r2, L[i], R[i]);
    s->ic1[0] = l1; s->ic2[0] = l2; s->ic1[1] = r1; s->ic2[1] = r2;
}

DSP_HOT void svf_process2_ramp(dsp_svf_t *s, const svf_coef_t *to, float *L, float *R, int n)
{
    const float inv = 1.f / (float)n;
    float a1 = s->c.a1, a2 = s->c.a2, a3 = s->c.a3, m0 = s->c.m0, m1 = s->c.m1, m2 = s->c.m2;
    const float da1 = (to->a1 - a1) * inv, da2 = (to->a2 - a2) * inv, da3 = (to->a3 - a3) * inv;
    const float dm0 = (to->m0 - m0) * inv, dm1 = (to->m1 - m1) * inv, dm2 = (to->m2 - m2) * inv;
    float l1 = s->ic1[0], l2 = s->ic2[0], r1 = s->ic1[1], r2 = s->ic2[1];
    for (int i = 0; i < n; i++) {
        a1 += da1; a2 += da2; a3 += da3; m0 += dm0; m1 += dm1; m2 += dm2;
        SVF_STEP2(L[i], R[i], l1, l2, r1, r2, L[i], R[i]);
    }
    s->ic1[0] = l1; s->ic2[0] = l2; s->ic1[1] = r1; s->ic2[1] = r2;
    s->c = *to;
}

float svf_flush(dsp_svf_t *s)
{
    float m = 0.f;
    for (int c = 0; c < 2; c++) {
        s->ic1[c] = dsp_flush(s->ic1[c]); s->ic2[c] = dsp_flush(s->ic2[c]);
        m = dsp_maxf(m, dsp_maxf(dsp_absf(s->ic1[c]), dsp_absf(s->ic2[c])));
    }
    return m;
}
