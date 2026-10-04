/*
 * dsp_delay.h: stereo / ping-pong delay with filtered, soft-clipped feedback.
 *
 * - Lines (2 x 1.5 s) in PSRAM via alloc_big; every access is a contiguous block read/write.
 * - Integer-sample read heads (tap times are exact: 375 ms = 18000 samples, no interpolation loss in the
 *   repeats). A time change (knob, tempo, sync division) starts an equal-power crossfade (50 ms) from the
 *   old head to the new one: no pitch zipper, no click. Changes arriving during a crossfade are picked up
 *   when it ends.
 * - Line input = route(input * on_fade, feedback * y) -> HP (lo cut) -> LP (hi cut) -> soft clip:
 *   every repeat passes the filters once more (progressively darker / thinner echoes); the soft clip is
 *   exactly linear below -1.9 dBFS and saturates smoothly towards +1.9 dBFS, so 95 % feedback stays musical.
 * - STEREO <-> PING-PONG changes crossfade the routing matrix (50 ms), the lines keep their content.
 * - Off = the input fades out, the tail keeps ringing (spill-over); processing stops once the line contents
 *   and the output have been below -100 dBFS for a whole delay time.
 */
#pragma once
#include <stdbool.h>
#include "dsp_biquad.h"
#include "dsp_dline.h"
#include "dsp_smooth.h"

#define DLY_MAX_MS     1500.f
#define DLY_MIN_MS     10.f
#define DLY_SC_KNEE    0.8f
#define DLY_SC_CEIL    1.25f
enum { DLY_STEREO = 0, DLY_PINGPONG = 1 };

typedef struct {
    dsp_dline_t line[2];
    int        d_cur, d_next, d_tgt;
    float      xf;
    bool       xfading;
    float      route, route_tgt;          /* 0 = stereo, 1 = ping-pong */
    dsp_bq_t   hp, lp;
    dsp_sm_t   fb, mix, lhi, llo;         /* fb 0..0.95, mix 0..1, log2 Hz */
    dsp_fade_t on;
    int        quiet;
    bool       running;
    float      out_peak;                  /* wet peak of the last block */
} dsp_delay_t;

/* alloc_big: 2 x (1.5 s + block) floats. Returns 0 on success. */
int   dly_init(dsp_delay_t *d, void *(*alloc_big)(size_t));
void  dly_reset(dsp_delay_t *d);
/* Effective time for sync index (0 = FREE, 1..7 = 1/2 .. 1/16) at bpm. Synced values longer than the line
 * are halved until they fit (stay on the beat grid); free times are clamped to [10, 1500] ms. */
float dly_time_ms(int sync, float bpm, float free_ms);
void  dly_set(dsp_delay_t *d, bool on, float time_ms, float fb_pct, int mode, float hicut_hz, float locut_hz,
              float mix_pct);
void  dly_settle(dsp_delay_t *d);
void  dly_process(dsp_delay_t *d, float *L, float *R, const dsp_blk_t *b);
bool  dly_running(const dsp_delay_t *d);
static inline float dly_softclip(float x)
{
    const float a = dsp_absf(x);
    if (a <= DLY_SC_KNEE) return x;
    float t = (a - DLY_SC_KNEE) * (1.f / (2.f * (DLY_SC_CEIL - DLY_SC_KNEE)));
    if (t > 1.f) t = 1.f;
    const float y = DLY_SC_KNEE + 2.f * (DLY_SC_CEIL - DLY_SC_KNEE) * (t - 0.5f * t * t);
    return x < 0.f ? -y : y;
}
