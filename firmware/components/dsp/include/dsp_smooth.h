/*
 * dsp_smooth.h: per-block context, one-pole parameter smoothers, linear fades.
 *
 * Smoothers run at block rate as TWO cascaded one-poles (critically damped): mid += k (tgt - mid),
 * cur += k (mid - cur), with k = 1 - exp(-n / (tau * fs)) supplied by the caller in dsp_blk_t (normal 6 ms tau
 * => ~40 ms to 99 %, or the preset-glide tau). Unlike a single one-pole, whose slope is maximal (and has a
 * corner) at the start of a jump, the cascade starts with zero slope: a full-range jump (e.g. volume 0 dB ->
 * -inf from MIDI or the web) has no audible onset transient. Measured by the host click test. They snap exactly to the
 * target when within `eps`, so "settled" is an exact equality test and settled gains can be bit-exact.
 * Gains are additionally interpolated linearly per sample between block values (dsp_ramp_t).
 */
#pragma once
#include <stdbool.h>
#include "dsp_math.h"

typedef struct {
    int   n;          /* frames in this block (1..DSP_BLOCK_MAX) */
    float inv_n;      /* 1/n */
    float k;          /* one-pole coefficient for this block (normal or preset glide) */
    float fade_step;  /* per-sample increment of 0..1 on/off fades (30 ms, or the glide time) */
    float xfade_step; /* per-sample increment of discrete-change crossfades (delay heads, enums) */
} dsp_blk_t;

/* Fill a block context. tau_ms: smoother time constant, fade_ms / xfade_ms: linear fade durations. */
void dsp_blk_make(dsp_blk_t *b, int n, float tau_ms, float fade_ms, float xfade_ms);
/* 1 - exp(-n / (tau * fs)) using the fast exp (init / per-block only). */
float dsp_onepole_k(float tau_ms, int n);

/* ---- one-pole smoother ---- */
typedef struct { float cur, mid, tgt, eps; } dsp_sm_t;

static inline void  sm_init(dsp_sm_t *s, float v, float eps) { s->cur = s->mid = s->tgt = v; s->eps = eps; }
static inline void  sm_set(dsp_sm_t *s, float tgt) { s->tgt = tgt; }
static inline void  sm_jump(dsp_sm_t *s, float v) { s->cur = s->mid = s->tgt = v; }
static inline bool  sm_settled(const dsp_sm_t *s) { return s->cur == s->tgt && s->mid == s->tgt; }
static inline float sm_step(dsp_sm_t *s, float k)
{
    if (s->cur != s->tgt || s->mid != s->tgt) {
        s->mid += k * (s->tgt - s->mid);
        s->cur += k * (s->mid - s->cur);
        if (dsp_absf(s->tgt - s->mid) <= s->eps && dsp_absf(s->tgt - s->cur) <= s->eps) s->cur = s->mid = s->tgt;
    }
    return s->cur;
}

/* ---- per-sample linear ramp between two block values: v_i = v + d * (i + 1) ---- */
typedef struct { float v, d; } dsp_ramp_t;
static inline dsp_ramp_t ramp_make(float a, float b, float inv_n) { dsp_ramp_t r = { a, (b - a) * inv_n }; return r; }
static inline bool ramp_const(const dsp_ramp_t *r) { return r->d == 0.f; }
/* Step a smoother and return the ramp of f(value) across the block (f = identity). */
static inline dsp_ramp_t sm_ramp(dsp_sm_t *s, const dsp_blk_t *b)
{
    const float a = s->cur;
    return ramp_make(a, sm_step(s, b->k), b->inv_n);
}

/* ---- linear 0..1 fade (effect on/off, crossfades) ---- */
typedef struct { float v, tgt; } dsp_fade_t;
static inline void fade_init(dsp_fade_t *f, float v) { f->v = f->tgt = v; }
static inline bool fade_settled(const dsp_fade_t *f) { return f->v == f->tgt; }
/* Advance by n samples of `step`; returns the per-sample ramp from the old to the new value. */
static inline dsp_ramp_t fade_block(dsp_fade_t *f, float step, const dsp_blk_t *b)
{
    const float a = f->v;
    float e = a;
    if (f->tgt > a) { e = a + step * (float)b->n; if (e > f->tgt) e = f->tgt; }
    else if (f->tgt < a) { e = a - step * (float)b->n; if (e < f->tgt) e = f->tgt; }
    f->v = e;
    return ramp_make(a, e, b->inv_n);
}
