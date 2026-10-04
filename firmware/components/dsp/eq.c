/* eq.c: 5-band EQ (see dsp_eq.h). */
#include "dsp_eq.h"
#include "dsp_port.h"

#define EQ_SKIP_STATE 1e-9f

void eq_reset(dsp_eq_t *eq)
{
    for (int b = 0; b < EQ_BANDS; b++) bq_reset(&eq->bq[b]);
}

void eq_init(dsp_eq_t *eq)
{
    static const float f_def[EQ_BANDS] = { 80.f, 250.f, 1000.f, 4000.f, 10000.f };
    for (int b = 0; b < EQ_BANDS; b++) {
        sm_init(&eq->lf[b], dsp_log2(f_def[b]), 1e-6f);
        sm_init(&eq->gdb[b], 0.f, 1e-5f);
        sm_init(&eq->lq[b], 0.f, 1e-6f);
        eq->tf[b] = f_def[b];
        eq->tq[b] = 1.f;
        bq_identity(&eq->bq[b].c);
        eq->exact[b] = 1;
        eq->active[b] = 0;
    }
    sm_init(&eq->in_db, 0.f, 1e-5f);
    sm_init(&eq->out_db, 0.f, 1e-5f);
    eq->rr = 0;
    eq_reset(eq);
}

void eq_set_band(dsp_eq_t *eq, int b, float f, float g, float q)
{
    if (f != eq->tf[b]) { eq->tf[b] = f; sm_set(&eq->lf[b], dsp_log2(f)); eq->exact[b] = 0; }
    if (q != eq->tq[b]) { eq->tq[b] = q; sm_set(&eq->lq[b], dsp_log2(q)); eq->exact[b] = 0; }
    if (g != eq->gdb[b].tgt) { sm_set(&eq->gdb[b], g); eq->exact[b] = 0; }
}

void eq_set_trims(dsp_eq_t *eq, float in_db, float out_db)
{
    sm_set(&eq->in_db, in_db);
    sm_set(&eq->out_db, out_db);
}

void eq_settle(dsp_eq_t *eq)
{
    for (int b = 0; b < EQ_BANDS; b++) {
        sm_jump(&eq->lf[b], eq->lf[b].tgt);
        sm_jump(&eq->gdb[b], eq->gdb[b].tgt);
        sm_jump(&eq->lq[b], eq->lq[b].tgt);
        if (eq->gdb[b].cur != 0.f) {
            rbj_design(&eq->bq[b].c, eq_band_type(b), (float)DSP_FS, eq->tf[b], eq->gdb[b].cur, eq->tq[b]);
            eq->active[b] = 1;
        }
        eq->exact[b] = 1;
    }
    sm_jump(&eq->in_db, eq->in_db.tgt);
    sm_jump(&eq->out_db, eq->out_db.tgt);
}

bool eq_is_identity(const dsp_eq_t *eq)
{
    for (int b = 0; b < EQ_BANDS; b++)
        if (eq->active[b] || eq->gdb[b].cur != 0.f || eq->gdb[b].tgt != 0.f) return false;
    return eq->in_db.cur == 0.f && eq->in_db.tgt == 0.f && eq->out_db.cur == 0.f && eq->out_db.tgt == 0.f;
}

static DSP_HOT void apply_gain(dsp_sm_t *s, float *L, float *R, const dsp_blk_t *b)
{
    if (s->cur == 0.f && s->tgt == 0.f) return;     /* exact unity: skip (bit-transparent) */
    const float g0 = dsp_db2lin(s->cur);
    const float g1 = dsp_db2lin(sm_step(s, b->k));
    const float d = (g1 - g0) * b->inv_n;
    float g = g0;
    for (int i = 0; i < b->n; i++) {
        g += d;
        L[i] *= g;
        R[i] *= g;
    }
}

DSP_HOT void eq_process(dsp_eq_t *eq, float *L, float *R, const dsp_blk_t *b)
{
    apply_gain(&eq->in_db, L, R, b);
    bool exact_done = false;
    for (int k = 0; k < EQ_BANDS; k++) {
        const int band = (eq->rr + k) % EQ_BANDS;  /* fair round-robin for the exact redesign slot */
        dsp_bq_t *q = &eq->bq[band];
        if (sm_settled(&eq->gdb[band]) && eq->gdb[band].cur == 0.f) {
            /* 0 dB is an identity filter for every shape: skip once the states have rung out
             * (frequency / Q keep gliding so a later gain move starts from the right place) */
            sm_step(&eq->lf[band], b->k);
            sm_step(&eq->lq[band], b->k);
            if (eq->active[band]) {
                if (bq_flush(q) < EQ_SKIP_STATE) { bq_reset(q); bq_identity(&q->c); eq->active[band] = 0; }
                else bq_process2(q, L, R, b->n);
            }
            eq->exact[band] = 1;
            continue;
        }
        const bool moving = !sm_settled(&eq->lf[band]) || !sm_settled(&eq->gdb[band]) || !sm_settled(&eq->lq[band]);
        if (!eq->active[band]) {
            /* becoming active: start from the identity (== current filter at 0 dB), states are zero */
            bq_identity(&q->c);
            eq->active[band] = 1;
        }
        if (moving) {
            sm_step(&eq->lf[band], b->k);
            sm_step(&eq->gdb[band], b->k);
            sm_step(&eq->lq[band], b->k);
            biquad_coef_t c;
            if (sm_settled(&eq->lf[band]) && sm_settled(&eq->gdb[band]) && sm_settled(&eq->lq[band]) && !exact_done) {
                rbj_design(&c, eq_band_type(band), (float)DSP_FS, eq->tf[band], eq->gdb[band].cur, eq->tq[band]);
                eq->exact[band] = 1;
                exact_done = true;
                eq->rr = band + 1;
            } else {
                bq_design_fast(&c, eq_band_type(band), (float)DSP_FS, dsp_exp2(eq->lf[band].cur),
                               eq->gdb[band].cur, dsp_exp2(eq->lq[band].cur));
                eq->exact[band] = 0;
            }
            bq_process2_ramp(q, &c, L, R, b->n);
        } else if (!eq->exact[band] && !exact_done) {
            biquad_coef_t c;
            rbj_design(&c, eq_band_type(band), (float)DSP_FS, eq->tf[band], eq->gdb[band].cur, eq->tq[band]);
            eq->exact[band] = 1;
            exact_done = true;
            eq->rr = band + 1;
            bq_process2_ramp(q, &c, L, R, b->n);
        } else {
            bq_process2(q, L, R, b->n);
        }
        bq_flush(q);
    }
    if (eq->rr >= EQ_BANDS) eq->rr = 0;
    apply_gain(&eq->out_db, L, R, b);
}
