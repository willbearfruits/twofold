/*
 * dsp_dline.h: block-oriented circular delay lines.
 *
 * All access is in contiguous runs of n samples (at most two memcpy-like segments at the wrap), which is
 * what makes PSRAM-resident lines cheap: each block touches a few sequential cache lines per stream.
 * Invariant used by the whole library: a read of delay D >= n for a block of n samples only touches data
 * written by earlier blocks, so reads and writes of one block may happen in any order.
 */
#pragma once
#include <string.h>

typedef struct { float *buf; int len; int w; } dsp_dline_t;

static inline void dl_init(dsp_dline_t *d, float *buf, int len) { d->buf = buf; d->len = len; d->w = 0; }
static inline void dl_clear(dsp_dline_t *d) { memset(d->buf, 0, (size_t)d->len * sizeof(float)); d->w = 0; }

/* dst[i] = x[t0 + i - delay], t0 = time of the next write. Requires n <= delay <= len. */
static inline void dl_read(const dsp_dline_t *d, int delay, float *dst, int n)
{
    int r = d->w - delay;
    if (r < 0) r += d->len;
    int first = d->len - r;
    if (first > n) first = n;
    memcpy(dst, d->buf + r, (size_t)first * sizeof(float));
    if (first < n) memcpy(dst + first, d->buf, (size_t)(n - first) * sizeof(float));
}

/* Append n samples. */
static inline void dl_write(dsp_dline_t *d, const float *src, int n)
{
    int first = d->len - d->w;
    if (first > n) first = n;
    memcpy(d->buf + d->w, src, (size_t)first * sizeof(float));
    if (first < n) memcpy(d->buf, src + first, (size_t)(n - first) * sizeof(float));
    d->w += n;
    if (d->w >= d->len) d->w -= d->len;
}
