/*
 * dsp_math.h: fast float32 math for the audio path (no libm calls, no double, no division).
 *
 * Accuracy (verified by host/dsp_tests/test_math):
 *   dsp_log2      |err| <= 7e-7          (=> lin->dB error <= 4e-6 dB)
 *   dsp_exp2      rel err <= 3e-7         (=> dB->lin error <= 3e-6 dB), exactly 1.0f at 0
 *   dsp_sin2pi    |err| <= 2e-7
 * Polynomials are near-minimax fits (IRLS on Chebyshev nodes), evaluated with Horner (madd.s on Xtensa).
 */
#pragma once
#include <stdint.h>
#include <string.h>

#define DSP_PI          3.14159265358979f
#define DSP_TWO_PI      6.28318530717959f
#define DSP_DB2LOG2     0.166096404744368f   /* log2(10)/20 : dB -> log2(gain) */
#define DSP_LOG22DB     6.02059991327962f    /* 20*log10(2) : log2(gain) -> dB */
#define DSP_TINY        1e-18f               /* anti-denormal offset injected into recursive loops */

/* __builtin_memcpy, not memcpy: ESP-IDF builds with -fno-builtin-memcpy, which turned every punning memcpy into a
 * real ROM call (measured: ~100 cycles per sample in the meters alone). The builtin compiles to a register move. */
static inline uint32_t dsp_f2u(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
static inline float    dsp_u2f(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }

static inline float dsp_absf(float x) { return __builtin_fabsf(x); }   /* abs.s on the Xtensa FPU */
static inline float dsp_minf(float a, float b) { return a < b ? a : b; }
static inline float dsp_maxf(float a, float b) { return a > b ? a : b; }
static inline float dsp_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline int   dsp_clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* Flush values that are on their way to the denormal range (Xtensa has no FTZ guarantee). */
static inline float dsp_flush(float x) { return dsp_absf(x) < 1e-20f ? 0.f : x; }

/* log2(x) for x > 0 (normal floats). */
static inline float dsp_log2(float x)
{
    uint32_t u = dsp_f2u(x);
    int e = (int)((u >> 23) & 0xffu) - 127;
    float m = dsp_u2f((u & 0x007fffffu) | 0x3f800000u);   /* [1,2) */
    if (m > 1.41421356f) { m *= 0.5f; e += 1; }             /* [0.707, 1.414) */
    const float t = m - 1.f;
    float p = 1.761664495e-01f;
    p = p * t - 2.709772475e-01f;
    p = p * t + 2.950998454e-01f;
    p = p * t - 3.591848224e-01f;
    p = p * t + 4.806487004e-01f;
    p = p * t - 7.213676932e-01f;
    p = p * t + 1.442696307e+00f;
    return (float)e + t * p;
}

/* 2^x, x clamped to [-125, 125]. Exactly 1.0f for x == 0. */
static inline float dsp_exp2(float x)
{
    x = dsp_clampf(x, -125.f, 125.f);
    const int   i = (int)(x + 127.5f) - 127;    /* round(x); argument > 0 so trunc == floor */
    const float f = x - (float)i;               /* [-0.5, 0.5] */
    float p = 1.340043470e-03f;
    p = p * f + 9.666372955e-03f;
    p = p * f + 5.550333186e-02f;
    p = p * f + 2.402234895e-01f;
    p = p * f + 6.931471955e-01f;
    p = p * f + 1.0f;
    return dsp_u2f(dsp_f2u(p) + ((uint32_t)i << 23));
}

static inline float dsp_db2lin(float db) { return dsp_exp2(db * DSP_DB2LOG2); }
static inline float dsp_lin2db(float g)  { return DSP_LOG22DB * dsp_log2(g + 1e-30f); }

/* sin(2*pi*p) for any finite p (phase in cycles). */
static inline float dsp_sin2pi(float p)
{
    p -= (float)(int)(p + (p >= 0.f ? 0.5f : -0.5f));     /* [-0.5, 0.5] */
    float u = 4.f * p;                                     /* [-2, 2] */
    if (u > 1.f) u = 2.f - u;
    else if (u < -1.f) u = -2.f - u;                      /* [-1, 1], sin(pi/2 u) */
    const float s = u * u;
    float q = 1.516785312e-04f;
    q = q * s - 4.674157495e-03f;
    q = q * s + 7.968992687e-02f;
    q = q * s - 6.459637613e-01f;
    q = q * s + 1.570796320e+00f;
    return u * q;
}
static inline float dsp_cos2pi(float p) { return dsp_sin2pi(p + 0.25f); }

/* Wrap a phase accumulator into [0,1). */
static inline float dsp_wrap01(float p) { float r = p - (float)(int)p; return r < 0.f ? r + 1.f : r; }

/* Round to nearest integer (ties away from zero), no libm. */
static inline int32_t dsp_roundi(float x) { return (int32_t)(x + (x >= 0.f ? 0.5f : -0.5f)); }

/* xorshift32 PRNG (state must be non-zero). */
static inline uint32_t dsp_xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
/* Uniform in [0,1) with 24-bit resolution. */
static inline float dsp_rand01(uint32_t *s) { return (float)(dsp_xorshift32(s) >> 8) * (1.f / 16777216.f); }

/* Equal-power crossfade gains for x in [0,1]: a = cos(pi/2 x), b = sin(pi/2 x). */
static inline void dsp_eqpow(float x, float *a, float *b)
{
    *b = dsp_sin2pi(0.25f * x);
    *a = dsp_sin2pi(0.25f * (1.f - x));
}

/* Cubic Hermite (Catmull-Rom) interpolation between x0 and x1, t in [0,1). */
static inline float dsp_hermite(float xm1, float x0, float x1, float x2, float t)
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
