/* delay.c: stereo / ping-pong delay (see dsp_delay.h). */
#include "dsp_delay.h"
#include "dsp_port.h"

#define DLY_LEN     ((int)(DLY_MAX_MS * DSP_FS / 1000) + DSP_BLOCK_MAX + 4)
#define QUIET_LIN   1e-5f        /* -100 dBFS */
#define FILT_Q      0.70710678f

int dly_init(dsp_delay_t *d, void *(*alloc_big)(size_t))
{
    memset(d, 0, sizeof *d);
    for (int c = 0; c < 2; c++) {
        float *buf = (float *)alloc_big((size_t)DLY_LEN * sizeof(float));
        if (!buf) return -1;
        dl_init(&d->line[c], buf, DLY_LEN);
    }
    d->d_cur = d->d_next = d->d_tgt = 18000;
    d->xf = 1.f;
    d->route = d->route_tgt = 1.f;
    sm_init(&d->fb, 0.35f, 1e-5f);
    sm_init(&d->mix, 0.25f, 1e-5f);
    sm_init(&d->lhi, dsp_log2(6000.f), 1e-5f);
    sm_init(&d->llo, dsp_log2(120.f), 1e-5f);
    fade_init(&d->on, 0.f);
    bq_design_fast(&d->lp.c, RBJ_LOWPASS, (float)DSP_FS, 6000.f, 0.f, FILT_Q);
    bq_design_fast(&d->hp.c, RBJ_HIGHPASS, (float)DSP_FS, 120.f, 0.f, FILT_Q);
    dly_reset(d);
    return 0;
}

void dly_reset(dsp_delay_t *d)
{
    for (int c = 0; c < 2; c++) dl_clear(&d->line[c]);
    bq_reset(&d->hp);
    bq_reset(&d->lp);
    d->quiet = 0;
    d->running = false;
    d->xfading = false;
    d->d_cur = d->d_next = d->d_tgt;
}

float dly_time_ms(int sync, float bpm, float free_ms)
{
    static const float beats[8] = { 0.f, 2.f, 1.f, 1.5f, 2.f / 3.f, 0.5f, 0.75f, 0.25f };
    if (sync <= 0 || sync > 7) return dsp_clampf(free_ms, DLY_MIN_MS, DLY_MAX_MS);
    if (bpm < 1.f) bpm = 1.f;
    float ms = beats[sync] * 60000.f / bpm;
    while (ms > DLY_MAX_MS) ms *= 0.5f;
    return dsp_maxf(ms, DLY_MIN_MS);
}

void dly_set(dsp_delay_t *d, bool on, float time_ms, float fb_pct, int mode, float hicut_hz, float locut_hz,
             float mix_pct)
{
    d->on.tgt = on ? 1.f : 0.f;
    d->d_tgt = dsp_clampi(dsp_roundi(time_ms * (float)DSP_FS * 1e-3f), DSP_BLOCK_MAX, DLY_LEN - DSP_BLOCK_MAX - 2);
    sm_set(&d->fb, dsp_clampf(fb_pct, 0.f, 95.f) * 0.01f);
    sm_set(&d->mix, dsp_clampf(mix_pct, 0.f, 100.f) * 0.01f);
    sm_set(&d->lhi, dsp_log2(hicut_hz));
    sm_set(&d->llo, dsp_log2(locut_hz));
    d->route_tgt = mode == DLY_PINGPONG ? 1.f : 0.f;
}

void dly_settle(dsp_delay_t *d)
{
    sm_jump(&d->fb, d->fb.tgt); sm_jump(&d->mix, d->mix.tgt);
    sm_jump(&d->lhi, d->lhi.tgt); sm_jump(&d->llo, d->llo.tgt);
    bq_design_fast(&d->lp.c, RBJ_LOWPASS, (float)DSP_FS, dsp_exp2(d->lhi.cur), 0.f, FILT_Q);
    bq_design_fast(&d->hp.c, RBJ_HIGHPASS, (float)DSP_FS, dsp_exp2(d->llo.cur), 0.f, FILT_Q);
    d->route = d->route_tgt;
    d->d_cur = d->d_next = d->d_tgt;
    d->xfading = false;
    d->on.v = d->on.tgt;
}

bool dly_running(const dsp_delay_t *d) { return d->running; }

static inline float mix_dry(float m) { return m < 0.5f ? 1.f : 2.f * (1.f - m); }
static inline float mix_wet(float m) { return m > 0.5f ? 1.f : 2.f * m; }

DSP_HOT void dly_process(dsp_delay_t *d, float *L, float *R, const dsp_blk_t *b)
{
    const int n = b->n;
    if (!d->running) {
        if (d->on.tgt == 0.f) {
            /* idle: keep the smoothers converged, nothing else */
            sm_jump(&d->fb, d->fb.tgt); sm_jump(&d->mix, d->mix.tgt);
            d->route = d->route_tgt;
            d->out_peak = 0.f;
            return;
        }
        d->running = true;
        d->quiet = 0;
        if (!d->xfading) d->d_cur = d->d_next = d->d_tgt;   /* stale line content is < -100 dBFS */
        sm_jump(&d->lhi, d->lhi.tgt); sm_jump(&d->llo, d->llo.tgt);
        bq_design_fast(&d->lp.c, RBJ_LOWPASS, (float)DSP_FS, dsp_exp2(d->lhi.cur), 0.f, FILT_Q);
        bq_design_fast(&d->hp.c, RBJ_HIGHPASS, (float)DSP_FS, dsp_exp2(d->llo.cur), 0.f, FILT_Q);
    }

    const dsp_ramp_t on = fade_block(&d->on, b->fade_step, b);
    const dsp_ramp_t fb = sm_ramp(&d->fb, b);
    const float m0 = d->mix.cur, m1 = sm_step(&d->mix, b->k);
    const float f1 = on.v + on.d * (float)n;
    const float dry0 = 1.f - on.v * (1.f - mix_dry(m0)), dry1 = 1.f - f1 * (1.f - mix_dry(m1));
    const float wet0 = mix_wet(m0), wet1 = mix_wet(m1);

    /* read heads (old, and new while crossfading) */
    if (!d->xfading && d->d_tgt != d->d_cur) {
        d->d_next = d->d_tgt;
        d->xf = 0.f;
        d->xfading = true;
    }
    float yl[DSP_BLOCK_MAX], yr[DSP_BLOCK_MAX];
    dl_read(&d->line[0], d->d_cur, yl, n);
    dl_read(&d->line[1], d->d_cur, yr, n);
    if (d->xfading) {
        float nl[DSP_BLOCK_MAX], nr[DSP_BLOCK_MAX];
        dl_read(&d->line[0], d->d_next, nl, n);
        dl_read(&d->line[1], d->d_next, nr, n);
        float x = d->xf;
        for (int i = 0; i < n; i++) {
            x += b->xfade_step;
            if (x > 1.f) x = 1.f;
            float go, gn;
            dsp_eqpow(x, &go, &gn);
            yl[i] = go * yl[i] + gn * nl[i];
            yr[i] = go * yr[i] + gn * nr[i];
        }
        d->xf = x;
        if (x >= 1.f) { d->d_cur = d->d_next; d->xfading = false; }
    }

    /* line inputs: routing matrix crossfaded between stereo and ping-pong */
    float il[DSP_BLOCK_MAX], ir[DSP_BLOCK_MAX];
    {
        float p = d->route;
        const float pstep = d->route_tgt > p ? b->xfade_step : -b->xfade_step;
        float f = on.v, g = fb.v;
        for (int i = 0; i < n; i++) {
            if (p != d->route_tgt) { p += pstep; if ((pstep > 0.f && p > d->route_tgt) || (pstep < 0.f && p < d->route_tgt)) p = d->route_tgt; }
            f += on.d; g += fb.d;
            const float xl = L[i] * f, xr = R[i] * f;
            const float q = 1.f - p;
            il[i] = q * xl + p * 0.5f * (xl + xr) + g * (q * yl[i] + p * yr[i]);
            ir[i] = q * xr + g * (q * yr[i] + p * yl[i]);
        }
        d->route = p;
    }
    /* feedback / input filters (coefficients re-designed while the cutoffs move) */
    if (!sm_settled(&d->lhi)) {
        biquad_coef_t c;
        bq_design_fast(&c, RBJ_LOWPASS, (float)DSP_FS, dsp_exp2(sm_step(&d->lhi, b->k)), 0.f, FILT_Q);
        bq_process2_ramp(&d->lp, &c, il, ir, n);
    } else {
        bq_process2(&d->lp, il, ir, n);
    }
    if (!sm_settled(&d->llo)) {
        biquad_coef_t c;
        bq_design_fast(&c, RBJ_HIGHPASS, (float)DSP_FS, dsp_exp2(sm_step(&d->llo, b->k)), 0.f, FILT_Q);
        bq_process2_ramp(&d->hp, &c, il, ir, n);
    } else {
        bq_process2(&d->hp, il, ir, n);
    }
    bq_flush(&d->lp);
    bq_flush(&d->hp);
    float pin = 0.f;
    for (int i = 0; i < n; i++) {
        il[i] = dly_softclip(il[i]) + DSP_TINY;
        ir[i] = dly_softclip(ir[i]) + DSP_TINY;
        pin = dsp_maxf(pin, dsp_maxf(dsp_absf(il[i]), dsp_absf(ir[i])));
    }
    dl_write(&d->line[0], il, n);
    dl_write(&d->line[1], ir, n);

    /* output: dry / wet (the wet gain does not follow the on-fade: tails spill over) */
    float pout = 0.f;
    {
        const float ddry = (dry1 - dry0) * b->inv_n, dwet = (wet1 - wet0) * b->inv_n;
        float dr = dry0, we = wet0;
        for (int i = 0; i < n; i++) {
            dr += ddry; we += dwet;
            pout = dsp_maxf(pout, dsp_maxf(dsp_absf(yl[i]), dsp_absf(yr[i])));
            L[i] = dr * L[i] + we * yl[i];
            R[i] = dr * R[i] + we * yr[i];
        }
    }
    d->out_peak = pout;
    /* spill-over end detection */
    if (pin < QUIET_LIN && pout < QUIET_LIN) d->quiet += n; else d->quiet = 0;
    if (d->on.tgt == 0.f && d->on.v == 0.f && !d->xfading && d->quiet > d->d_cur + 2 * DSP_BLOCK_MAX)
        d->running = false;
}
