/* reverb.c: Dattorro plate (see dsp_reverb.h). */
#include "dsp_reverb.h"
#include "dsp_port.h"

#define SCALE       ((float)REV_TANK_FS / 29761.f)
#define BW_HZ       12000.f
#define LFO_HZ      0.9f
#define QUIET_LIN   1e-5f
#define DAMP_HI_HZ  10000.f       /* tank Nyquist is 12 kHz */
#define DAMP_LO_HZ  1200.f
#define GAP1        (-0.70f)           /* decay diffusion 1 */
#define OUT_GAIN    0.6f

static const int k_len29[RV_NLINES] = { 142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163 };
static const float k_dif_g[4] = { 0.75f, 0.75f, 0.625f, 0.625f };
/* Dattorro table 2: {line, offset @29761, sign} for left / right outputs */
typedef struct { int line, off; float sign; } rv_tap_t;
static const rv_tap_t k_taps[2][7] = {
    { { RV_RD1, 266, 1.f }, { RV_RD1, 2974, 1.f }, { RV_RAP2, 1913, -1.f }, { RV_RD2, 1996, 1.f },
      { RV_LD1, 1990, -1.f }, { RV_LAP2, 187, -1.f }, { RV_LD2, 1066, -1.f } },
    { { RV_LD1, 353, 1.f }, { RV_LD1, 3627, 1.f }, { RV_LAP2, 1228, -1.f }, { RV_LD2, 2673, 1.f },
      { RV_RD1, 2111, -1.f }, { RV_RAP2, 335, -1.f }, { RV_RD2, 121, -1.f } },
};

static int sc(int n29) { return dsp_roundi((float)n29 * SCALE); }

int rev_init(dsp_reverb_t *r, void *(*alloc_fast)(size_t), void *(*alloc_big)(size_t))
{
    memset(r, 0, sizeof *r);
    int total = 0;
    int len[RV_NLINES];
    for (int i = 0; i < RV_NLINES; i++) {
        r->nom[i] = sc(k_len29[i]);
        len[i] = r->nom[i];
        if (i == RV_LMAP || i == RV_RMAP) len[i] += (int)REV_MOD_MAX + 4;
        total += len[i];
    }
    /* one allocation per line, shortest first, so the allocator can keep the hot short allpasses internal
     * and spill only the long sequential delay lines to PSRAM when internal RAM is scarce */
    bool done[RV_NLINES] = { false };
    for (int k = 0; k < RV_NLINES; k++) {
        int best = -1;
        for (int i = 0; i < RV_NLINES; i++)
            if (!done[i] && (best < 0 || len[i] < len[best])) best = i;
        done[best] = true;
        float *buf = (float *)alloc_fast((size_t)len[best] * sizeof(float));
        if (!buf) return -1;
        dl_init(&r->ln[best], buf, len[best]);
    }
    r->fast_bytes = (size_t)total * sizeof(float);
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 7; t++) r->taps[c][t] = sc(k_taps[c][t].off);
    r->loop_len = 0;
    for (int i = RV_LMAP; i < RV_NLINES; i++) r->loop_len += r->nom[i];

    const int plen = (int)(REV_PREDLY_MAX_MS * DSP_FS / 1000) + 2 * DSP_BLOCK_MAX + 4;
    float *pbuf = (float *)alloc_big((size_t)plen * sizeof(float));
    if (!pbuf) return -1;
    r->big_bytes = (size_t)plen * sizeof(float);
    dl_init(&r->pre, pbuf, plen);

    sm_init(&r->ldecay, dsp_log2(2.2f), 1e-5f);
    sm_init(&r->damp, 45.f, 1e-3f);
    sm_init(&r->mod, 30.f, 1e-3f);
    sm_init(&r->llocut, dsp_log2(120.f), 1e-5f);
    sm_init(&r->width, 1.f, 1e-5f);
    sm_init(&r->mix, 0.2f, 1e-5f);
    fade_init(&r->on, 0.f);
    r->pd_cur = r->pd_next = r->pd_tgt = 960;
    bq_design_fast(&r->locut.c, RBJ_HIGHPASS, (float)DSP_FS, 120.f, 0.f, 0.70710678f);
    rev_reset(r);
    return 0;
}

void rev_reset(dsp_reverb_t *r)
{
    for (int i = 0; i < RV_NLINES; i++) dl_clear(&r->ln[i]);
    dl_clear(&r->pre);
    r->bw_z = r->damp_z[0] = r->damp_z[1] = 0.f;
    bq_reset(&r->locut);
    r->exc_prev[0] = r->exc_prev[1] = 0.f;
    hb_reset(&r->dn); hb_reset(&r->up);
    r->pend = 0.f; r->has_pend = 0;
    r->oq[0] = r->oq[1] = 0.f; r->oq_n = 1;     /* primed: constant 1-sample latency for any block size */
    r->dec_cur = rev_decay_gain(r, dsp_exp2(r->ldecay.cur));
    r->energy = 0.f;
    r->quiet = 0;
    r->running = false;
    r->pd_xfading = false;
    r->pd_cur = r->pd_next = r->pd_tgt;
}

float rev_decay_gain(const dsp_reverb_t *r, float rt60_s)
{
    /* 10^(-3 L / (4 fs T)) = 2^(-3 log2(10) L / (4 fs T)) */
    return dsp_exp2(-3.f * 3.32192809f * (float)r->loop_len / (4.f * (float)REV_TANK_FS * rt60_s));
}

void rev_set(dsp_reverb_t *r, bool on, float decay_s, float predelay_ms, float damp_pct, float mod_pct,
             float locut_hz, float width_pct, float mix_pct)
{
    r->on.tgt = on ? 1.f : 0.f;
    sm_set(&r->ldecay, dsp_log2(dsp_clampf(decay_s, 0.1f, 30.f)));
    r->pd_tgt = dsp_roundi(dsp_clampf(predelay_ms, 0.f, REV_PREDLY_MAX_MS) * (float)DSP_FS * 1e-3f);
    sm_set(&r->damp, damp_pct);
    sm_set(&r->mod, mod_pct);
    sm_set(&r->llocut, dsp_log2(locut_hz));
    sm_set(&r->width, width_pct * 0.01f);
    sm_set(&r->mix, mix_pct * 0.01f);
}

void rev_settle(dsp_reverb_t *r)
{
    sm_jump(&r->ldecay, r->ldecay.tgt); sm_jump(&r->damp, r->damp.tgt); sm_jump(&r->mod, r->mod.tgt);
    sm_jump(&r->llocut, r->llocut.tgt); sm_jump(&r->width, r->width.tgt); sm_jump(&r->mix, r->mix.tgt);
    r->dec_cur = rev_decay_gain(r, dsp_exp2(r->ldecay.cur));
    bq_design_fast(&r->locut.c, RBJ_HIGHPASS, (float)DSP_FS, dsp_exp2(r->llocut.cur), 0.f, 0.70710678f);
    r->pd_cur = r->pd_next = r->pd_tgt;
    r->pd_xfading = false;
    r->on.v = r->on.tgt;
}

bool rev_running(const dsp_reverb_t *r) { return r->running; }

static inline float mix_dry(float m) { return m < 0.5f ? 1.f : 2.f * (1.f - m); }
static inline float mix_wet(float m) { return m > 0.5f ? 1.f : 2.f * m; }

/* fixed allpass, block-wise: v = x + g vN, y = vN - g v */
static DSP_HOT void ap_block(dsp_dline_t *l, float g, float *x, int n)
{
    float vN[DSP_BLOCK_MAX], v[DSP_BLOCK_MAX];
    dl_read(l, l->len, vN, n);
    for (int i = 0; i < n; i++) {
        v[i] = x[i] + g * vN[i];
        x[i] = vN[i] - g * v[i];
    }
    dl_write(l, v, n);
}

/* modulated allpass: delay d0 -> d1 (samples) across the block, Hermite-interpolated reads */
static DSP_HOT void map_block(dsp_dline_t *l, float g, float d0, float d1, float *x, int n, float inv_n)
{
    const float dmax = dsp_maxf(d0, d1), dmin = dsp_minf(d0, d1);
    const int Ds = (int)dmax + 2;
    const int W = Ds - (int)dmin + n + 3;
    float win[DSP_BLOCK_MAX + 16], v[DSP_BLOCK_MAX];
    dl_read(l, Ds, win, W);
    const float dd = (d1 - d0) * inv_n;
    float d = d0;
    for (int i = 0; i < n; i++) {
        d += dd;
        const float loc = (float)(Ds + i) - d;
        const int ip = (int)loc;
        const float t = loc - (float)ip;
        const float vd = dsp_hermite(win[ip - 1], win[ip], win[ip + 1], win[ip + 2], t);
        v[i] = x[i] + g * vd;
        x[i] = vd - g * v[i];
    }
    dl_write(l, v, n);
}

DSP_HOT void rev_process(dsp_reverb_t *r, float *L, float *R, const dsp_blk_t *b)
{
    const int n = b->n;
    if (!r->running) {
        if (r->on.tgt == 0.f) {
            sm_jump(&r->width, r->width.tgt); sm_jump(&r->mix, r->mix.tgt);
            r->energy = 0.f;
            return;
        }
        r->running = true;
        r->quiet = 0;
        if (!r->pd_xfading) r->pd_cur = r->pd_next = r->pd_tgt;
    }
    const dsp_ramp_t on = fade_block(&r->on, b->fade_step, b);

    /* ---- control (block rate) ---- */
    const float dec0 = r->dec_cur;
    const float dec1 = rev_decay_gain(r, dsp_exp2(sm_step(&r->ldecay, b->k)));
    r->dec_cur = dec1;
    const float dd2 = dsp_clampf(dec1 + 0.15f, 0.25f, 0.5f);
    const float damp = sm_step(&r->damp, b->k) * 0.01f;
    const float fc = dsp_exp2(13.2877123795f + damp * (10.2288186905f - 13.2877123795f));   /* 10k..1.2k */
    const float dk = dsp_exp2(-DSP_TWO_PI * fc / (float)REV_TANK_FS * 1.44269504f);
    const float exc = sm_step(&r->mod, b->k) * 0.01f * REV_MOD_MAX;
    if (!sm_settled(&r->llocut)) {
        biquad_coef_t c;
        bq_design_fast(&c, RBJ_HIGHPASS, (float)DSP_FS, dsp_exp2(sm_step(&r->llocut, b->k)), 0.f, 0.70710678f);
        r->locut.c = c;
    }
    r->lfo_ph = dsp_wrap01(r->lfo_ph + LFO_HZ * (float)n * (1.f / (float)DSP_FS));
    const float e1[2] = { exc * dsp_sin2pi(r->lfo_ph), exc * dsp_cos2pi(r->lfo_ph) };

    /* ---- input: mono, on-fade, pre-delay ---- */
    float x[DSP_BLOCK_MAX];
    {
        float f = on.v;
        for (int i = 0; i < n; i++) { f += on.d; x[i] = 0.5f * (L[i] + R[i]) * f; }
    }
    if (!r->pd_xfading && r->pd_tgt != r->pd_cur) { r->pd_next = r->pd_tgt; r->pd_xf = 0.f; r->pd_xfading = true; }
    dl_write(&r->pre, x, n);
    /* the line now holds this block: read at delay (pd + n) == pd samples behind each input sample */
    dl_read(&r->pre, r->pd_cur + n, x, n);
    if (r->pd_xfading) {
        float y2[DSP_BLOCK_MAX];
        dl_read(&r->pre, r->pd_next + n, y2, n);
        float xf = r->pd_xf;
        for (int i = 0; i < n; i++) {
            xf += b->xfade_step;
            if (xf > 1.f) xf = 1.f;
            float a, c;
            dsp_eqpow(xf, &a, &c);
            x[i] = a * x[i] + c * y2[i];
        }
        r->pd_xf = xf;
        if (xf >= 1.f) { r->pd_cur = r->pd_next; r->pd_xfading = false; }
    }
    /* bandwidth one-pole LP + lo-cut */
    {
        const float p = 0.20857f;   /* exp(-2 pi 12k / 48k) */
        float z = r->bw_z;
        for (int i = 0; i < n; i++) { z = x[i] + p * (z - x[i]); x[i] = z; }
        r->bw_z = dsp_flush(z);
    }
    bq_process1(&r->locut, x, n);
    r->locut.z1[0] = dsp_flush(r->locut.z1[0]); r->locut.z2[0] = dsp_flush(r->locut.z2[0]);
    for (int i = 0; i < n; i++) x[i] += DSP_TINY;

    /* ---- 48 kHz -> 24 kHz (tank rate); m = tank samples this block ---- */
    float xin[DSP_BLOCK_MAX + 2], xd[DSP_BLOCK_MAX / 2 + 2], xscr[DSP_BLOCK_MAX / 2 + 2];
    int nin = 0;
    if (r->has_pend) xin[nin++] = r->pend;
    memcpy(xin + nin, x, (size_t)n * sizeof(float));
    nin += n;
    const int m = nin >> 1;
    r->has_pend = nin & 1;
    if (r->has_pend) r->pend = xin[nin - 1];
    hb1_down(&r->dn, xin, xin, xd, xscr /* second channel unused */, m);
    const float inv_m = m ? 1.f / (float)m : 0.f;

    /* input diffusers */
    for (int k = 0; k < 4; k++) ap_block(&r->ln[RV_DIF1 + k], k_dif_g[k], xd, m);

    /* ---- output taps (read before this block's tank writes: offsets >= 195 > m) ---- */
    float wl[DSP_BLOCK_MAX], wr[DSP_BLOCK_MAX], tmp[DSP_BLOCK_MAX];   /* tank-rate taps (m samples) */
    for (int c = 0; c < 2; c++) {
        float *acc = c ? wr : wl;
        memset(acc, 0, (size_t)m * sizeof(float));
        for (int t = 0; t < 7; t++) {
            const rv_tap_t *tp = &k_taps[c][t];
            dl_read(&r->ln[tp->line], r->taps[c][t], tmp, m);
            const float g = tp->sign * OUT_GAIN;
            for (int i = 0; i < m; i++) acc[i] += g * tmp[i];
        }
    }

    /* ---- tank ---- */
    float lend[DSP_BLOCK_MAX], rend[DSP_BLOCK_MAX];
    dl_read(&r->ln[RV_LD2], r->ln[RV_LD2].len, lend, m);
    dl_read(&r->ln[RV_RD2], r->ln[RV_RD2].len, rend, m);
    const float ddec = (dec1 - dec0) * inv_m;
    for (int half = 0; half < 2; half++) {
        const int map = half ? RV_RMAP : RV_LMAP, d1 = half ? RV_RD1 : RV_LD1;
        const int ap2 = half ? RV_RAP2 : RV_LAP2, d2 = half ? RV_RD2 : RV_LD2;
        const float *cross = half ? lend : rend;
        float t[DSP_BLOCK_MAX];
        float g = dec0;
        for (int i = 0; i < m; i++) { g += ddec; t[i] = xd[i] + g * cross[i]; }
        const float nomd = (float)r->nom[map];
        map_block(&r->ln[map], GAP1, nomd + r->exc_prev[half], nomd + e1[half], t, m, inv_m);
        r->exc_prev[half] = e1[half];
        float u[DSP_BLOCK_MAX];
        dl_read(&r->ln[d1], r->ln[d1].len, u, m);      /* read before write: delay == len */
        dl_write(&r->ln[d1], t, m);
        float z = r->damp_z[half];
        g = dec0;
        for (int i = 0; i < m; i++) {
            g += ddec;
            z = u[i] + dk * (z - u[i]);
            t[i] = z * g;
        }
        r->damp_z[half] = dsp_flush(z);
        ap_block(&r->ln[ap2], dd2, t, m);
        dl_write(&r->ln[d2], t, m);
    }
    /* d2 lines: their outputs (lend/rend) were read at the start of the block, before these writes, so the
     * delay is exactly `len` (a dl_read at delay == len returns the samples written `len` samples ago). */

    /* ---- 24 kHz -> 48 kHz: queue (0..1 carried samples) + 2m new samples, emit exactly n ---- */
    {
        float uL[DSP_BLOCK_MAX + 4], uR[DSP_BLOCK_MAX + 4];
        int q = r->oq_n;
        if (q) { uL[0] = r->oq[0]; uR[0] = r->oq[1]; }
        hb1_up(&r->up, wl, wr, uL + q, uR + q, m);
        const int avail = q + 2 * m;            /* == n + 1 - has_pend, always >= n */
        memcpy(wl, uL, (size_t)n * sizeof(float));
        memcpy(wr, uR, (size_t)n * sizeof(float));
        r->oq_n = avail - n;
        if (r->oq_n) { r->oq[0] = uL[n]; r->oq[1] = uR[n]; }
    }

    /* ---- width, energy, dry/wet ---- */
    const dsp_ramp_t wd = sm_ramp(&r->width, b);
    const float m0 = r->mix.cur, m1 = sm_step(&r->mix, b->k);
    const float f1 = on.v + on.d * (float)n;
    const float dry0 = 1.f - on.v * (1.f - mix_dry(m0)), dry1 = 1.f - f1 * (1.f - mix_dry(m1));
    const float wet0 = mix_wet(m0), wet1 = mix_wet(m1);
    const float ddry = (dry1 - dry0) * b->inv_n, dwet = (wet1 - wet0) * b->inv_n;
    float dr = dry0, we = wet0, w = wd.v, e = 0.f, pk = 0.f;
    for (int i = 0; i < n; i++) {
        dr += ddry; we += dwet; w += wd.d;
        const float mid = 0.5f * (wl[i] + wr[i]), side = 0.5f * (wl[i] - wr[i]) * w;
        const float ol = mid + side, orr = mid - side;
        e += ol * ol + orr * orr;
        pk = dsp_maxf(pk, dsp_maxf(dsp_absf(ol), dsp_absf(orr)));
        L[i] = dr * L[i] + we * ol;
        R[i] = dr * R[i] + we * orr;
    }
    r->energy += 0.05f * (e * 0.5f * b->inv_n - r->energy);
    if (pk < QUIET_LIN) r->quiet += n; else r->quiet = 0;
    if (r->on.tgt == 0.f && r->on.v == 0.f && r->quiet > DSP_FS) r->running = false;
}
