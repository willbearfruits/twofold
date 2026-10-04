/*
 * dsp_comp.h: stereo-linked feed-forward bus compressor.
 *   detector: peak max(|L|,|R|) -> dB (fast log2) -> soft-knee gain computer (Giannoulis, Massberg & Reiss
 *   2012) -> GR smoothed in the dB domain with branching attack/release one-poles -> makeup -> dB->lin
 *   (fast exp2) -> parallel mix  y = x * ((1 - m) + m * g).  On/off fades m; settled off = skipped.
 * Threshold and makeup are interpolated per sample; ratio/knee/attack/release per block (their effect is
 * smoothed by the GR ballistics anyway).
 */
#pragma once
#include <stdbool.h>
#include "dsp_smooth.h"

typedef struct {
    dsp_sm_t   thr, ratio, knee, att, rel, makeup, mix;
    dsp_fade_t on;
    float      gr;           /* smoothed gain reduction, dB >= 0 */
    float      gr_max;       /* max GR of the last block (meter) */
    bool       running;
} dsp_comp_t;

void comp_init(dsp_comp_t *c);
void comp_reset(dsp_comp_t *c);
void comp_set(dsp_comp_t *c, bool on, float thr_db, float ratio, float att_ms, float rel_ms, float knee_db,
              float makeup_db, float mix_pct);
void comp_settle(dsp_comp_t *c);
void comp_process(dsp_comp_t *c, float *L, float *R, const dsp_blk_t *b);
/* Static gain computer: gain reduction (dB, >= 0) for an input level (dB). Shared with tests / UI curve. */
static inline float comp_gr_db(float x_db, float thr, float ratio, float knee)
{
    const float over = x_db - thr, s = 1.f - 1.f / ratio;
    if (knee > 1e-3f) {
        if (2.f * over <= -knee) return 0.f;
        if (2.f * over < knee) { const float t = over + 0.5f * knee; return s * t * t / (2.f * knee); }
    } else if (over <= 0.f) return 0.f;
    return s * over;
}
