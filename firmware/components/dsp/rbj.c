/* rbj.c: Robert Bristow-Johnson cookbook formulae (shelf Q form). */
#include "rbj.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void rbj_design(biquad_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q)
{
    /* clamp to sane ranges so the UI can call this with anything */
    if (f0 < 1.f) f0 = 1.f;
    if (f0 > 0.49f * fs) f0 = 0.49f * fs;
    if (q < 0.05f) q = 0.05f;

    const double A = pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * M_PI * f0 / fs;
    const double cw = cos(w0), sw = sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;

    switch (type) {
    case RBJ_PEAK:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case RBJ_LOWSHELF: {
        const double sa = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
        break;
    }
    case RBJ_HIGHSHELF: {
        const double sa = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
        break;
    }
    case RBJ_LOWPASS:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case RBJ_HIGHPASS:
    default:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    }
    c->b0 = (float)(b0 / a0); c->b1 = (float)(b1 / a0); c->b2 = (float)(b2 / a0);
    c->a1 = (float)(a1 / a0); c->a2 = (float)(a2 / a0);
}

double biquad_mag2(const biquad_coef_t *c, double f, double fs)
{
    const double w = 2.0 * M_PI * f / fs;
    const double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    const double nr = c->b0 + c->b1 * c1 + c->b2 * c2, ni = -(c->b1 * s1 + c->b2 * s2);
    const double dr = 1.0 + c->a1 * c1 + c->a2 * c2,   di = -(c->a1 * s1 + c->a2 * s2);
    return (nr * nr + ni * ni) / (dr * dr + di * di);
}

double biquad_mag_db(const biquad_coef_t *c, double f, double fs)
{
    double m = biquad_mag2(c, f, fs);
    return 10.0 * log10(m < 1e-30 ? 1e-30 : m);
}
