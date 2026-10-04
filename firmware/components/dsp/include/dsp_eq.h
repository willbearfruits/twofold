/*
 * dsp_eq.h: 5-band parametric EQ (low shelf, 3 peaks, high shelf), stereo, TDF-II.
 *
 * - Per band the frequency (log2 Hz), gain (dB) and Q (log2) are smoothed separately. While any of them
 *   moves, coefficients are re-designed every block with the fast float designer and interpolated per sample
 *   (click-free). Once settled, the band switches (at most one band per block) to the exact rbj_design()
 *   coefficients, i.e. bit-identical to what the UI draws from rbj.c.
 * - A band whose (effective) gain is settled at 0 dB is skipped as soon as its states have decayed
 *   (< 1e-9), so band on/off and global bypass are gain glides to 0 dB ("analog" bypass, no phase
 *   crossfade) and cost nothing when settled.
 * - In/out trims are smoothed in dB and interpolated per sample; skipped when settled at exactly 0 dB.
 */
#pragma once
#include <stdbool.h>
#include "dsp_biquad.h"
#include "dsp_smooth.h"

typedef struct {
    dsp_bq_t  bq[EQ_BANDS];
    dsp_sm_t  lf[EQ_BANDS], gdb[EQ_BANDS], lq[EQ_BANDS];   /* log2(f), dB, log2(Q) */
    float     tf[EQ_BANDS], tq[EQ_BANDS];                  /* natural-unit targets (for rbj_design) */
    uint8_t   exact[EQ_BANDS];                             /* coefficients == rbj_design(targets) */
    uint8_t   active[EQ_BANDS];
    dsp_sm_t  in_db, out_db;
    int       rr;                                          /* round-robin for exact redesigns */
} dsp_eq_t;

void eq_init(dsp_eq_t *eq);
void eq_reset(dsp_eq_t *eq);                               /* clear filter states */
/* Targets. gain_db is the *effective* gain (0 when the band or the whole EQ is off). */
void eq_set_band(dsp_eq_t *eq, int band, float freq_hz, float gain_db, float q);
void eq_set_trims(dsp_eq_t *eq, float in_db, float out_db);
/* Jump all smoothers to their targets (init / tests). */
void eq_settle(dsp_eq_t *eq);
void eq_process(dsp_eq_t *eq, float *L, float *R, const dsp_blk_t *b);
/* true when the EQ is an exact identity this block (all bands inactive, trims 0 dB). */
bool eq_is_identity(const dsp_eq_t *eq);
