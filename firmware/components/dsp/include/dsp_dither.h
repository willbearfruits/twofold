/*
 * dsp_dither.h: TPDF dither and float -> 24-bit (left-justified in int32) conversion with saturation.
 * TPDF = u1 - u2, u = 24-bit uniforms from two xorshift32 draws per sample: triangular on (-1, +1) LSB,
 * zero mean, independent of the signal (first two error moments signal-independent). One generator state
 * per channel (independent channels).
 */
#pragma once
#include <stdint.h>
#include "dsp_math.h"

#define DSP_I24_SCALE  8388608.f          /* 2^23: float 1.0 == full scale */
#define DSP_I24_MAX    8388607
#define DSP_I24_MIN    (-8388608)

typedef struct { uint32_t s[2]; } dsp_dither_t;

void dither_init(dsp_dither_t *d, uint32_t seed);
/* TPDF value in LSB units for channel ch (two draws). */
static inline float dither_tpdf(dsp_dither_t *d, int ch) { return dsp_rand01(&d->s[ch]) - dsp_rand01(&d->s[ch]); }

static inline int32_t dsp_f2i24(float v)   /* v already in LSB units, saturating, returns left-justified */
{
    v = dsp_clampf(v, (float)DSP_I24_MIN, (float)DSP_I24_MAX);
    return (int32_t)((uint32_t)dsp_roundi(v) << 8);
}
static inline float dsp_i2f(int32_t s) { return (float)s * (1.f / 2147483648.f); }

/* interleaved stereo out[2n]; gain-free conversion with TPDF dither */
void dither_convert(dsp_dither_t *d, const float *L, const float *R, int32_t *out, int n);
/* without dither (exact for values that are already on the 24-bit grid) */
void quantize_convert(const float *L, const float *R, int32_t *out, int n);
