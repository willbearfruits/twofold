/* chorus.c: chorus / ensemble / vibrato (see dsp_chorus.h). */
#include "dsp_chorus.h"
#include "dsp_port.h"
#include <string.h>

#define CHO_MASK        (CHO_LEN - 1)
#define ENS_FAST_HZ     6.1f
#define DRIFT_PERIOD    12000          /* new random drift target every 0.25 s */
#define DRIFT_TAU_S     0.35f

typedef struct {
    float base_ms[3];
    float depth_ms, fast_ms, drift_ms;   /* at 100 % DEPTH */
    float rphase;                        /* right-channel LFO phase offset (cycles) */
    float pan[3][2];
    float wet_gain;
    int   nv;
    bool  wet_only;
} cho_cfg_t;

static const cho_cfg_t k_cfg[CHO_MODES] = {
    [CHO_CHORUS]   = { { 7.5f, 10.5f, 13.5f }, 4.0f, 0.0f,  0.08f, 0.25f,
                       { { 0.95f, 0.30f }, { 0.65f, 0.65f }, { 0.30f, 0.95f } }, 0.55f, 3, false },
    [CHO_ENSEMBLE] = { { 5.0f, 5.0f, 5.0f },    2.2f, 0.35f, 0.25f, 0.50f,
                       { { 0.85f, 0.45f }, { 0.65f, 0.65f }, { 0.45f, 0.85f } }, 0.55f, 3, false },
    [CHO_VIBRATO]  = { { 6.0f, 6.0f, 6.0f },    3.0f, 0.0f,  0.0f,  0.0f,
                       { { 1.0f, 1.0f }, { 0.f, 0.f }, { 0.f, 0.f } },               1.0f,  1, true },
};

int cho_init(dsp_chorus_t *c, void *(*alloc_hot)(size_t))
{
    memset(c, 0, sizeof *c);
    for (int ch = 0; ch < 2; ch++) {
        c->line[ch] = (float *)alloc_hot(CHO_LEN * sizeof(float));
        if (!c->line[ch]) return -1;
    }
    c->rng = 0x9e3779b9u;
    c->mode = c->from_mode = CHO_ENSEMBLE;
    c->xf = 1.f;
    sm_init(&c->rate, 0.6f, 1e-5f);
    sm_init(&c->depth, 40.f, 1e-3f);
    sm_init(&c->mix, 35.f, 1e-3f);
    fade_init(&c->on, 0.f);
    cho_reset(c);
    return 0;
}

static void delays_mode(dsp_chorus_t *c, int m, float out[CHO_MODES][3][2])
{
    const float depth = c->depth.cur * 0.01f;
    {
        const cho_cfg_t *k = &k_cfg[m];
        for (int v = 0; v < 3; v++) {
            for (int ch = 0; ch < 2; ch++) {
                const float off = (float)v * (1.f / 3.f) + (float)ch * k->rphase;
                float ms = k->base_ms[v] + depth * (k->depth_ms * dsp_sin2pi(c->ph + off) +
                                                    k->fast_ms * dsp_sin2pi(c->fph + off) +
                                                    k->drift_ms * c->drift2[v] * (ch ? -1.f : 1.f));
                out[m][v][ch] = ms * (float)DSP_FS * 1e-3f;
            }
        }
    }
}

static void delays_now(dsp_chorus_t *c, float out[CHO_MODES][3][2])
{
    for (int m = 0; m < CHO_MODES; m++) delays_mode(c, m, out);
}

void cho_reset(dsp_chorus_t *c)
{
    for (int ch = 0; ch < 2; ch++) memset(c->line[ch], 0, CHO_LEN * sizeof(float));
    c->w = 0;
    delays_now(c, c->dprev);
}

void cho_set(dsp_chorus_t *c, bool on, int mode, float rate_hz, float depth_pct, float mix_pct)
{
    c->on.tgt = on ? 1.f : 0.f;
    mode = dsp_clampi(mode, 0, CHO_MODES - 1);
    if (mode != c->mode && c->xf >= 1.f) {          /* a change during a crossfade waits for it to end */
        c->from_mode = c->mode;
        c->mode = mode;
        c->xf = (c->on.v > 0.f || c->on.tgt > 0.f) ? 0.f : 1.f;
    }
    sm_set(&c->rate, rate_hz);
    sm_set(&c->depth, depth_pct);
    sm_set(&c->mix, mix_pct);
}

void cho_settle(dsp_chorus_t *c)
{
    sm_jump(&c->rate, c->rate.tgt); sm_jump(&c->depth, c->depth.tgt); sm_jump(&c->mix, c->mix.tgt);
    c->on.v = c->on.tgt; c->xf = 1.f;
    delays_now(c, c->dprev);
}

bool cho_active(const dsp_chorus_t *c) { return c->on.v > 0.f || c->on.tgt > 0.f; }

/* Accumulate the wet signal of `mode` into wl/wr. */
static DSP_HOT void render(const dsp_chorus_t *c, int mode, const float dnew[CHO_MODES][3][2],
                           float *wl, float *wr, int n, float inv_n)
{
    const cho_cfg_t *k = &k_cfg[mode];
    const float *bm = c->line[0], *bs = c->line[1];
    const float base = (float)(c->w + CHO_LEN);
    for (int v = 0; v < k->nv; v++) {
        /* Read position = (w + CHO_LEN + i) - d, kept as integer index + fraction and advanced by (1 - dd) per
         * sample (no float<->int conversion per sample); |dd| < 1, so one carry per step suffices. */
        const float dl0 = c->dprev[mode][v][0], ddl = (dnew[mode][v][0] - dl0) * inv_n;
        const float pl0 = base - (dl0 + ddl);
        int il = (int)pl0;
        float tl = pl0 - (float)il;
        if (!k->wet_only) {
            /* chorus / ensemble: mono in, one tap per voice on the mid line, panned (Juno / string-ensemble style) */
            const float gl = k->pan[v][0] * k->wet_gain, gr = k->pan[v][1] * k->wet_gain;
            for (int i = 0; i < n; i++) {
                const float y = dsp_hermite(bm[(il - 1) & CHO_MASK], bm[il & CHO_MASK], bm[(il + 1) & CHO_MASK],
                                            bm[(il + 2) & CHO_MASK], tl);
                wl[i] += gl * y;
                wr[i] += gr * y;
                tl -= ddl; il++;
                if (tl < 0.f) { tl += 1.f; il--; } else if (tl >= 1.f) { tl -= 1.f; il++; }
            }
        } else {
            /* vibrato: stays stereo. L = M + S read at the left delay, R = M - S at the right delay. */
            const float dr0 = c->dprev[mode][v][1], ddr = (dnew[mode][v][1] - dr0) * inv_n;
            const float pr0 = base - (dr0 + ddr);
            int ir = (int)pr0;
            float tr = pr0 - (float)ir;
            const float gl = k->pan[v][0] * k->wet_gain, gr = k->pan[v][1] * k->wet_gain;
            for (int i = 0; i < n; i++) {
                const int l0 = il & CHO_MASK, l1 = (il - 1) & CHO_MASK, l2 = (il + 1) & CHO_MASK, l3 = (il + 2) & CHO_MASK;
                const int r0 = ir & CHO_MASK, r1 = (ir - 1) & CHO_MASK, r2 = (ir + 1) & CHO_MASK, r3 = (ir + 2) & CHO_MASK;
                const float yl = dsp_hermite(bm[l1] + bs[l1], bm[l0] + bs[l0], bm[l2] + bs[l2], bm[l3] + bs[l3], tl);
                const float yr = dsp_hermite(bm[r1] - bs[r1], bm[r0] - bs[r0], bm[r2] - bs[r2], bm[r3] - bs[r3], tr);
                wl[i] += gl * yl;
                wr[i] += gr * yr;
                tl -= ddl; tr -= ddr; il++; ir++;
                if (tl < 0.f) { tl += 1.f; il--; } else if (tl >= 1.f) { tl -= 1.f; il++; }
                if (tr < 0.f) { tr += 1.f; ir--; } else if (tr >= 1.f) { tr -= 1.f; ir++; }
            }
        }
    }
}

static void write_line(dsp_chorus_t *c, const float *L, const float *R, int n)
{
    int w = c->w;
    for (int i = 0; i < n; i++) {
        c->line[0][w] = 0.5f * (L[i] + R[i]);       /* mid  */
        c->line[1][w] = 0.5f * (L[i] - R[i]);       /* side */
        w = (w + 1) & CHO_MASK;
    }
    c->w = w;
}

static void mode_gains(int mode, float f, float mix, float *dg, float *wg)
{
    const cho_cfg_t *k = &k_cfg[mode];
    const float dry = k->wet_only ? 0.f : 1.f - mix;
    const float wet = k->wet_only ? 1.f : mix;
    *dg = 1.f - f * (1.f - dry);
    *wg = f * wet;
}

DSP_HOT void cho_process(dsp_chorus_t *c, float *L, float *R, const dsp_blk_t *b)
{
    uint32_t tq = dsp_cycles();
    const int n = b->n;
    /* LFOs and drift (block rate) */
    const float rate = sm_step(&c->rate, b->k);
    sm_step(&c->depth, b->k);
    c->ph = dsp_wrap01(c->ph + rate * (float)n * (1.f / (float)DSP_FS));
    c->fph = dsp_wrap01(c->fph + ENS_FAST_HZ * (float)n * (1.f / (float)DSP_FS));
    c->drift_cnt += n;
    if (c->drift_cnt >= DRIFT_PERIOD) {
        c->drift_cnt -= DRIFT_PERIOD;
        for (int v = 0; v < 3; v++) c->drift_tgt[v] = 2.f * dsp_rand01(&c->rng) - 1.f;
    }
    const float kd = (float)n / (DRIFT_TAU_S * (float)DSP_FS);
    for (int v = 0; v < 3; v++) {               /* two cascaded one-poles: continuous pitch */
        c->drift1[v] += kd * (c->drift_tgt[v] - c->drift1[v]);
        c->drift2[v] += kd * (c->drift1[v] - c->drift2[v]);
    }
    for (int v = 0; v < 3; v++) c->viz_phase[v] = dsp_wrap01(c->ph + (float)v * (1.f / 3.f));
    float dnew[CHO_MODES][3][2];
    memcpy(dnew, c->dprev, sizeof dnew);            /* untouched modes keep their last delays */
    uint8_t mask;
    if (!cho_active(c)) { delays_now(c, dnew); mask = (uint8_t)((1u << CHO_MODES) - 1u); }   /* idle: all current */
    else {
        delays_mode(c, c->mode, dnew);
        mask = (uint8_t)(1u << c->mode);
        if (c->xf < 1.f && c->from_mode != c->mode) { delays_mode(c, c->from_mode, dnew); mask |= (uint8_t)(1u << c->from_mode); }
    }
    /* a mode that was not computed last block starts from its current delays (no sweep from stale values) */
    for (int m = 0; m < CHO_MODES; m++)
        if ((mask & (1u << m)) && !(c->dmask & (1u << m))) memcpy(c->dprev[m], dnew[m], sizeof dnew[m]);
    c->dmask = mask;
    DSP_DBG_ADD(9, tq); tq = dsp_cycles();

    if (!cho_active(c)) {
        write_line(c, L, R, n);
        memcpy(c->dprev, dnew, sizeof dnew);
        sm_step(&c->mix, 1.f);
        c->running = false;
        return;
    }
    c->running = true;
    const dsp_ramp_t on = fade_block(&c->on, b->fade_step, b);
    const float m0 = c->mix.cur * 0.01f, m1 = sm_step(&c->mix, b->k) * 0.01f;
    const float f0 = on.v, f1 = on.v + on.d * (float)n;

    float wl[DSP_BLOCK_MAX], wr[DSP_BLOCK_MAX];
    float dg0, wg0, dg1, wg1;
    if (c->xf >= 1.f) {
        memset(wl, 0, (size_t)n * sizeof(float));
        memset(wr, 0, (size_t)n * sizeof(float));
        render(c, c->mode, (const float (*)[3][2])dnew, wl, wr, n, b->inv_n);
        DSP_DBG_ADD(10, tq); tq = dsp_cycles();
        write_line(c, L, R, n);                         /* reads above only touched older samples */
        mode_gains(c->mode, f0, m0, &dg0, &wg0);
        mode_gains(c->mode, f1, m1, &dg1, &wg1);
        const float ddg = (dg1 - dg0) * b->inv_n, dwg = (wg1 - wg0) * b->inv_n;
        float dg = dg0, wg = wg0;
        for (int i = 0; i < n; i++) {
            dg += ddg; wg += dwg;
            const float xl = L[i], xr = R[i];
            L[i] = dg * xl + wg * wl[i];
            R[i] = dg * xr + wg * wr[i];
        }
    } else {
        /* mode crossfade: render outgoing and incoming modes, blend linearly (both contain the dry) */
        float ol[DSP_BLOCK_MAX], orr[DSP_BLOCK_MAX];
        memset(wl, 0, (size_t)n * sizeof(float)); memset(wr, 0, (size_t)n * sizeof(float));
        memset(ol, 0, (size_t)n * sizeof(float)); memset(orr, 0, (size_t)n * sizeof(float));
        render(c, c->mode, (const float (*)[3][2])dnew, wl, wr, n, b->inv_n);
        render(c, c->from_mode, (const float (*)[3][2])dnew, ol, orr, n, b->inv_n);
        write_line(c, L, R, n);
        float adg0, awg0, adg1, awg1;
        mode_gains(c->mode, f0, m0, &dg0, &wg0);       mode_gains(c->mode, f1, m1, &dg1, &wg1);
        mode_gains(c->from_mode, f0, m0, &adg0, &awg0); mode_gains(c->from_mode, f1, m1, &adg1, &awg1);
        float x = c->xf;
        for (int i = 0; i < n; i++) {
            const float t = (float)(i + 1) * b->inv_n;
            x += b->xfade_step;
            if (x > 1.f) x = 1.f;
            const float dg = dg0 + t * (dg1 - dg0), wg = wg0 + t * (wg1 - wg0);
            const float adg = adg0 + t * (adg1 - adg0), awg = awg0 + t * (awg1 - awg0);
            const float xl = L[i], xr = R[i];
            const float nl = dg * xl + wg * wl[i], nr = dg * xr + wg * wr[i];
            const float pl = adg * xl + awg * ol[i], pr = adg * xr + awg * orr[i];
            L[i] = pl + x * (nl - pl);
            R[i] = pr + x * (nr - pr);
        }
        c->xf = x;
    }
    DSP_DBG_ADD(12, tq);
    memcpy(c->dprev, dnew, sizeof dnew);
}
