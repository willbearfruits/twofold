/* comp.c: bus compressor (see dsp_comp.h). */
#include "dsp_comp.h"
#include "dsp_port.h"

void comp_reset(dsp_comp_t *c) { c->gr = 0.f; c->gr_max = 0.f; }

void comp_init(dsp_comp_t *c)
{
    sm_init(&c->thr, -12.f, 1e-4f);
    sm_init(&c->ratio, 2.f, 1e-5f);
    sm_init(&c->knee, 6.f, 1e-4f);
    sm_init(&c->att, 10.f, 1e-4f);
    sm_init(&c->rel, 200.f, 1e-3f);
    sm_init(&c->makeup, 0.f, 1e-4f);
    sm_init(&c->mix, 1.f, 1e-5f);
    fade_init(&c->on, 0.f);
    c->running = false;
    comp_reset(c);
}

void comp_set(dsp_comp_t *c, bool on, float thr_db, float ratio, float att_ms, float rel_ms, float knee_db,
              float makeup_db, float mix_pct)
{
    c->on.tgt = on ? 1.f : 0.f;
    sm_set(&c->thr, thr_db);
    sm_set(&c->ratio, dsp_maxf(ratio, 1.f));
    sm_set(&c->att, dsp_maxf(att_ms, 0.05f));
    sm_set(&c->rel, dsp_maxf(rel_ms, 1.f));
    sm_set(&c->knee, dsp_maxf(knee_db, 0.f));
    sm_set(&c->makeup, makeup_db);
    sm_set(&c->mix, mix_pct * 0.01f);
}

void comp_settle(dsp_comp_t *c)
{
    sm_jump(&c->thr, c->thr.tgt); sm_jump(&c->ratio, c->ratio.tgt); sm_jump(&c->knee, c->knee.tgt);
    sm_jump(&c->att, c->att.tgt); sm_jump(&c->rel, c->rel.tgt); sm_jump(&c->makeup, c->makeup.tgt);
    sm_jump(&c->mix, c->mix.tgt);
    c->on.v = c->on.tgt;
}

DSP_HOT void comp_process(dsp_comp_t *c, float *L, float *R, const dsp_blk_t *b)
{
    const int n = b->n;
    if (c->on.v == 0.f && c->on.tgt == 0.f) {
        /* settled off: bit-transparent, no work */
        sm_jump(&c->thr, c->thr.tgt); sm_jump(&c->makeup, c->makeup.tgt); sm_jump(&c->mix, c->mix.tgt);
        c->gr = 0.f; c->gr_max = 0.f; c->running = false;
        return;
    }
    c->running = true;
    const dsp_ramp_t on = fade_block(&c->on, b->fade_step, b);
    const dsp_ramp_t thr = sm_ramp(&c->thr, b);
    const dsp_ramp_t mk = sm_ramp(&c->makeup, b);
    const float m0 = c->mix.cur, m1 = sm_step(&c->mix, b->k);
    const float ratio = sm_step(&c->ratio, b->k), knee = sm_step(&c->knee, b->k);
    const float att = sm_step(&c->att, b->k), rel = sm_step(&c->rel, b->k);
    /* a = exp(-1 / (t fs)) */
    const float aa = dsp_exp2(-1.44269504f * 1000.f / (att * (float)DSP_FS));
    const float ar = dsp_exp2(-1.44269504f * 1000.f / (rel * (float)DSP_FS));
    const float s = 1.f - 1.f / ratio;
    const bool soft = knee > 1e-3f;
    const float hk = 0.5f * knee, ik = soft ? 1.f / (2.f * knee) : 0.f;
    /* effective parallel mix incl. on-fade, per-sample ramp */
    const float f1 = on.v + on.d * (float)n;
    const float me0 = m0 * on.v, me1 = m1 * f1;
    const float dme = (me1 - me0) * b->inv_n;

    float gr = c->gr, grmax = 0.f, t = thr.v, mu = mk.v, me = me0;
    for (int i = 0; i < n; i++) {
        t += thr.d; mu += mk.d; me += dme;
        const float xl = L[i], xr = R[i];
        const float pk = dsp_maxf(dsp_absf(xl), dsp_absf(xr));
        const float over = DSP_LOG22DB * dsp_log2(pk + 1e-12f) - t;
        float g = 0.f;
        if (soft) {
            if (over >= hk) g = s * over;
            else if (over > -hk) { const float u = over + hk; g = s * u * u * ik; }
        } else if (over > 0.f) g = s * over;
        gr = g + (g > gr ? aa : ar) * (gr - g);
        grmax = dsp_maxf(grmax, gr);
        const float gain = dsp_exp2((mu - gr) * DSP_DB2LOG2);
        const float k = 1.f - me + me * gain;
        L[i] = xl * k;
        R[i] = xr * k;
    }
    c->gr = dsp_flush(gr);
    c->gr_max = grmax;
}
