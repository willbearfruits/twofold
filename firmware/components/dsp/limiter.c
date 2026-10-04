/* limiter.c: lookahead safety limiter (see dsp_limiter.h). */
#include "dsp_limiter.h"
#include "dsp_port.h"
#include <string.h>

#define RING_MASK (LIM_RING - 1)

void lim_reset(dsp_limiter_t *l)
{
    memset(l->dl, 0, sizeof l->dl);
    memset(l->hist, 0, sizeof l->hist);
    l->dpos = 0;
    l->dq_h = l->dq_n = 0;
    l->g_rel = 1.f;
    l->hpos = 0;
    l->nz = 0;
    l->gr_max_db = 0.f;
    l->over = false;
}

void lim_init(dsp_limiter_t *l, float release_ms)
{
    memset(l, 0, sizeof *l);
    l->a_rel = dsp_exp2(-1.44269504f * 1000.f / (release_ms * (float)DSP_FS));
    sm_init(&l->ceil_db, -0.3f, 1e-4f);
    l->clamp_lin = 0.f;
    l->on = true;
    lim_reset(l);
}

void lim_set(dsp_limiter_t *l, bool on, float ceil_db, float margin_lin)
{
    l->on = on;
    sm_set(&l->ceil_db, ceil_db);
    l->clamp_lin = margin_lin;
}

void lim_settle(dsp_limiter_t *l) { sm_jump(&l->ceil_db, l->ceil_db.tgt); }

static DSP_HOT void delay_only(dsp_limiter_t *l, float *L, float *R, int n)
{
    int p = l->dpos;
    for (int i = 0; i < n; i++) {
        const int r = (p - LIM_DELAY) & RING_MASK;
        const float xl = L[i], xr = R[i];
        L[i] = l->dl[0][r];
        R[i] = l->dl[1][r];
        l->dl[0][p] = xl;
        l->dl[1][p] = xr;
        p = (p + 1) & RING_MASK;
    }
    l->dpos = p;
}

DSP_HOT void lim_process(dsp_limiter_t *l, float *L, float *R, const dsp_blk_t *b)
{
    const int n = b->n;
    const float ceil = dsp_db2lin(sm_step(&l->ceil_db, b->k));
    const float clamp = ceil - l->clamp_lin;
    float pk = 0.f;
    for (int i = 0; i < n; i++) pk = dsp_maxf(pk, dsp_maxf(dsp_absf(L[i]), dsp_absf(R[i])));
    l->over = pk > ceil;

    if (!l->on || (pk <= ceil && l->dq_n == 0 && l->nz == 0)) {
        /* nothing to limit in the window: exact delayed copy (gain 1.0) */
        if (!l->on && (l->dq_n || l->nz)) {
            l->dq_n = 0; l->nz = 0; l->g_rel = 1.f;
            memset(l->hist, 0, sizeof l->hist);
        }
        delay_only(l, L, R, n);
        l->t += (uint32_t)n;
        l->gr_max_db = 0.f;
        return;
    }

    float dsum = 0.f;
    for (int k = 0; k < LIM_LA; k++) dsum += l->hist[k];      /* exact re-sum: no drift across blocks */
    const float inv_la = 1.f / (float)LIM_LA;
    float gmin = 1.f;
    int p = l->dpos;
    for (int i = 0; i < n; i++) {
        const float xl = L[i], xr = R[i];
        const float pkv = dsp_maxf(dsp_absf(xl), dsp_absf(xr));
        const uint32_t t = l->t++;
        /* sliding-window max over [t-LA+1, t], only values above the ceiling are kept */
        if (pkv > ceil) {
            while (l->dq_n && l->dq_v[(l->dq_h + l->dq_n - 1) & RING_MASK] <= pkv) l->dq_n--;
            const uint32_t s = (l->dq_h + l->dq_n) & RING_MASK;
            l->dq_t[s] = t; l->dq_v[s] = pkv; l->dq_n++;
        }
        while (l->dq_n && (uint32_t)(t - l->dq_t[l->dq_h]) >= LIM_LA) { l->dq_h = (l->dq_h + 1) & RING_MASK; l->dq_n--; }
        const float gt = l->dq_n ? ceil / l->dq_v[l->dq_h] : 1.f;
        /* stage 1: instant attack, exponential release of the deficit */
        float g1 = 1.f - (1.f - l->g_rel) * l->a_rel;
        if (1.f - g1 < 1e-7f) g1 = 1.f;
        l->g_rel = dsp_minf(gt, g1);
        /* stage 2: moving average of the deficit over LIM_LA samples */
        const float d = 1.f - l->g_rel, old = l->hist[l->hpos];
        l->hist[l->hpos] = d;
        if (++l->hpos == LIM_LA) l->hpos = 0;
        l->nz += (d != 0.f) - (old != 0.f);
        dsum += d - old;
        if (l->nz == 0) dsum = 0.f;
        const float g = 1.f - dsum * inv_la;
        gmin = dsp_minf(gmin, g);
        /* delayed audio */
        const int r = (p - LIM_DELAY) & RING_MASK;
        float yl = l->dl[0][r] * g, yr = l->dl[1][r] * g;
        l->dl[0][p] = xl; l->dl[1][p] = xr;
        p = (p + 1) & RING_MASK;
        yl = dsp_clampf(yl, -clamp, clamp);
        yr = dsp_clampf(yr, -clamp, clamp);
        L[i] = yl; R[i] = yr;
    }
    l->dpos = p;
    l->gr_max_db = gmin < 1.f ? -dsp_lin2db(gmin) : 0.f;
}
