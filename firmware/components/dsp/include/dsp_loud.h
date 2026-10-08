/*
 * dsp_loud.h: loudness compensation after ISO 226:2003 equal-loudness contours.
 *
 * At the reference listening level (P_LOUD_REF dB SPL for a 0 dBFS... i.e. at master volume 0 dB) the mix is
 * heard as intended. Turning the volume down by A dB moves a 1 kHz tone from phon_ref to phon_ref - A, but low
 * (and very high) frequencies lose loudness faster, so they get the extra gain
 *     G(f) = Lp(f, phon_ref - A) - Lp(f, phon_ref) + A        (Lp from the ISO 226 formula, 20..90 phon)
 * scaled by AMOUNT (0..200 %). Realised as a low shelf (G at 50 Hz, corner 125 Hz) and a high shelf (G at 10 kHz,
 * corner 6 kHz), re-designed at block rate while the volume moves; exact bypass at A = 0 or AMOUNT = 0.
 * It follows the master-volume *target*, so a fade-out is compensated continuously.
 */
#pragma once
#include <stdbool.h>
#include "dsp_biquad.h"
#include "dsp_smooth.h"

typedef struct {
    dsp_bq_t  lo, hi;
    dsp_sm_t  glo, ghi;         /* shelf gains, dB */
    float     dlo, dhi;         /* gains the filters are currently designed for */
    bool      active;           /* filters running (gains or states non-zero) */
    float     ref, amt;         /* targets */
    bool      on;
} dsp_loud_t;

void  loud_init(dsp_loud_t *l);
void  loud_reset(dsp_loud_t *l);
void  loud_set(dsp_loud_t *l, bool on, float ref_spl, float amount_pct, float volume_db);
void  loud_settle(dsp_loud_t *l);
void  loud_process(dsp_loud_t *l, float *L, float *R, const dsp_blk_t *b);
/* ISO 226:2003 sound pressure level (dB SPL) of a pure tone at f Hz perceived at `phon` (20..90). */
float loud_iso226_spl(float f_hz, float phon);
/* Compensation gain (dB) at f for the given settings; what the UI draws. */
float loud_gain_db(float f_hz, float ref_spl, float amount_pct, float volume_db);
