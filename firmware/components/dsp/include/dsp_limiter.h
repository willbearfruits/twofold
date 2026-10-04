/*
 * dsp_limiter.h: stereo-linked safety limiter, 1 ms lookahead, never exceeds the ceiling on sample peaks.
 *
 *  peak[t] = max(|L|,|R|)  ->  sliding-window max over LIM_LA samples (monotonic deque holding only samples
 *  above the ceiling: O(1) amortised, and empty in the normal case)  ->  g_t = ceil / max  (1 if below)
 *  ->  stage 1: instant attack, exponential release (in 1-g)  ->  stage 2: moving average over LIM_LA
 *  (running sum of the deficit 1-g, exact recomputation per block)  ->  applied to the audio delayed by
 *  LIM_LA-1 samples.
 * Guarantee: the moving average at output time spans exactly the LIM_LA gains computed while that sample was
 * inside the window, each <= ceil/|x|, so |y| <= ceil. A final clamp at the ceiling only catches float
 * rounding (never active in tests). Below the ceiling every gain is exactly 1.0f and the output is a
 * bit-exact delayed copy of the input. The delay is constant whether the limiter is on or off.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "dsp_smooth.h"

#define LIM_LA     48              /* lookahead window (1.0 ms) */
#define LIM_DELAY  (LIM_LA - 1)    /* audio latency */
#define LIM_RING   64

typedef struct {
    float    dl[2][LIM_RING];
    int      dpos;
    uint32_t t;                    /* sample counter */
    uint32_t dq_t[LIM_RING];
    float    dq_v[LIM_RING];
    uint32_t dq_h, dq_n;           /* deque head index, count */
    float    g_rel;                /* stage-1 gain */
    float    hist[LIM_LA];         /* stage-2 window of deficits (1 - g) */
    int      hpos, nz;
    float    a_rel;
    dsp_sm_t ceil_db;
    float    clamp_lin;            /* extra margin below the ceiling for the final clamp (see dsp_engine) */
    bool     on;
    float    gr_max_db;            /* max GR (dB) of the last block */
    bool     over;                 /* input exceeded the ceiling in the last block */
} dsp_limiter_t;

void lim_init(dsp_limiter_t *l, float release_ms);
void lim_reset(dsp_limiter_t *l);
/* margin_lin: the output is clamped to ceil - margin_lin (e.g. 2 LSB so that dither never crosses the ceiling) */
void lim_set(dsp_limiter_t *l, bool on, float ceil_db, float margin_lin);
void lim_settle(dsp_limiter_t *l);
void lim_process(dsp_limiter_t *l, float *L, float *R, const dsp_blk_t *b);
