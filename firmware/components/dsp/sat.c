/* sat.c: 4x oversampled saturation (see dsp_sat.h). */
#include "dsp_sat.h"
#include <string.h>

#define PRE_FC      2000.f
#define PRE_DB      12.f     /* emphasis at tone = +-100 % */
#define TILT_DB     6.f      /* net wet tilt (HF vs LF) at tone = +-100 % */
#define DC_FC       5.f      /* wet DC blocker (asymmetric shaper makes DC) */

static float tan_cyc(float p) { return dsp_sin2pi(p) / dsp_cos2pi(p); }   /* tan(2 pi p), init only */

static inline void fo_reset(dsp_fo_t *f) { f->z[0] = f->z[1] = 0.f; }

/* first-order high shelf H(s) = (G s + 1)/(s + 1) at K = tan(pi fc/fs), times `scale` */
static void fo_shelf(dsp_fo_t *f, float K, float G, float scale)
{
    const float n = 1.f / (1.f + K);
    f->b0 = (G + K) * n * scale;
    f->b1 = (K - G) * n * scale;
    f->a1 = (K - 1.f) * n;
}
/* exact inverse of fo_shelf(K, G, 1/scale) */
static void fo_shelf_inv(dsp_fo_t *f, float K, float G, float scale)
{
    const float n = 1.f / (G + K);
    f->b0 = (1.f + K) * n * scale;
    f->b1 = (K - 1.f) * n * scale;
    f->a1 = (K - G) * n;
}

static DSP_HOT void fo_run2(dsp_fo_t *f, float *L, float *R, int n)
{
    const float b0 = f->b0, b1 = f->b1, a1 = f->a1;
    float zl = f->z[0], zr = f->z[1];
    for (int i = 0; i < n; i++) {
        const float yl = b0 * L[i] + zl, yr = b0 * R[i] + zr;
        zl = b1 * L[i] - a1 * yl;
        zr = b1 * R[i] - a1 * yr;
        L[i] = yl; R[i] = yr;
    }
    f->z[0] = dsp_flush(zl); f->z[1] = dsp_flush(zr);
}

/* Same, with the coefficients interpolated linearly from `from` to f's (new) coefficients across the block. */
static DSP_HOT void fo_run2_ramp(dsp_fo_t *f, const dsp_fo_t *from, float *L, float *R, int n, float inv_n)
{
    float b0 = from->b0, b1 = from->b1, a1 = from->a1;
    const float db0 = (f->b0 - b0) * inv_n, db1 = (f->b1 - b1) * inv_n, da1 = (f->a1 - a1) * inv_n;
    float zl = f->z[0], zr = f->z[1];
    for (int i = 0; i < n; i++) {
        b0 += db0; b1 += db1; a1 += da1;
        const float yl = b0 * L[i] + zl, yr = b0 * R[i] + zr;
        zl = b1 * L[i] - a1 * yl;
        zr = b1 * R[i] - a1 * yr;
        L[i] = yl; R[i] = yr;
    }
    f->z[0] = dsp_flush(zl); f->z[1] = dsp_flush(zr);
}

static void design_tone(dsp_sat_t *s, float tone_pct)
{
    const float t = tone_pct * 0.01f;
    const float G = dsp_db2lin(t * PRE_DB), sG = dsp_db2lin(0.5f * t * PRE_DB);
    fo_shelf(&s->pre, s->k_pre, G, 1.f / sG);
    fo_shelf_inv(&s->de, s->k_pre, G, sG);
    const float Gt = dsp_db2lin(t * TILT_DB), sGt = dsp_db2lin(0.5f * t * TILT_DB);
    fo_shelf(&s->tilt, s->k_tilt, Gt, 1.f / sGt);
    s->tone_designed = tone_pct;
}

void sat_reset(dsp_sat_t *s)
{
    hb_reset(&s->up1); hb_reset(&s->up2); hb_reset(&s->dn2); hb_reset(&s->dn1);
    fo_reset(&s->pre); fo_reset(&s->de); fo_reset(&s->tilt); fo_reset(&s->dc);
    memset(s->dly, 0, sizeof s->dly);
    s->dpos = 0;
}

void sat_init(dsp_sat_t *s)
{
    memset(s, 0, sizeof *s);
    s->k_pre = tan_cyc(0.5f * PRE_FC / (float)DSP_FS);
    s->k_tilt = tan_cyc(0.5f * PRE_FC / (float)(DSP_FS * SAT_OS));
    s->r_dc = 1.f - DSP_TWO_PI * DC_FC / (float)(DSP_FS * SAT_OS);
    s->dc.b0 = 1.f; s->dc.b1 = -1.f; s->dc.a1 = -s->r_dc;
    sm_init(&s->drive_db, 6.f, 1e-4f);
    sm_init(&s->warmth, 30.f, 1e-3f);
    sm_init(&s->tone, 0.f, 1e-3f);
    sm_init(&s->mix, 100.f, 1e-3f);
    fade_init(&s->on, 0.f);
    design_tone(s, 0.f);
    /* level compensation table: RMS(in)/RMS(shaped) for a -12 dBFS sine */
    const float A = 0.25f;
    for (int d = 0; d < SAT_COMP_N; d++) {
        const float g = dsp_db2lin((float)d);
        float si = 0.f, so = 0.f;
        for (int k = 0; k < 256; k++) {
            const float x = A * dsp_sin2pi((float)k / 256.f);
            const float y = sat_shape(g * x, 0.f);
            si += x * x; so += y * y;
        }
        s->comp_tab[d] = dsp_exp2(0.5f * (dsp_log2(si) - dsp_log2(so)));
    }
    sat_reset(s);
}

float sat_comp(const dsp_sat_t *s, float drive_db)
{
    drive_db = dsp_clampf(drive_db, 0.f, (float)(SAT_COMP_N - 1));
    int i = (int)drive_db;
    if (i >= SAT_COMP_N - 1) i = SAT_COMP_N - 2;
    const float f = drive_db - (float)i;
    return s->comp_tab[i] + f * (s->comp_tab[i + 1] - s->comp_tab[i]);
}

void sat_set(dsp_sat_t *s, bool on, float drive_db, float warmth_pct, float tone_pct, float mix_pct)
{
    s->on.tgt = on ? 1.f : 0.f;
    sm_set(&s->drive_db, drive_db);
    sm_set(&s->warmth, warmth_pct);
    sm_set(&s->tone, tone_pct);
    sm_set(&s->mix, mix_pct);
}

void sat_settle(dsp_sat_t *s)
{
    sm_jump(&s->drive_db, s->drive_db.tgt); sm_jump(&s->warmth, s->warmth.tgt);
    sm_jump(&s->tone, s->tone.tgt); sm_jump(&s->mix, s->mix.tgt);
    s->on.v = s->on.tgt;
    design_tone(s, s->tone.cur);
}

bool sat_active(const dsp_sat_t *s) { return s->on.v > 0.f || s->on.tgt > 0.f; }

/* delay the dry input by SAT_LATENCY into d[] (always runs: constant latency, no time jumps) */
static DSP_HOT void dry_delay(dsp_sat_t *s, const float *L, const float *R, float *dL, float *dR, int n)
{
    int p = s->dpos;
    for (int i = 0; i < n; i++) {
        dL[i] = s->dly[0][p]; dR[i] = s->dly[1][p];
        s->dly[0][p] = L[i]; s->dly[1][p] = R[i];
        if (++p == SAT_LATENCY) p = 0;
    }
    s->dpos = p;
}

DSP_HOT void sat_process(dsp_sat_t *s, float *L, float *R, const dsp_blk_t *b)
{
    const int n = b->n;
    float dL[DSP_BLOCK_MAX], dR[DSP_BLOCK_MAX];
    float pk = 0.f;
    for (int i = 0; i < n; i++) pk = dsp_maxf(pk, dsp_maxf(dsp_absf(L[i]), dsp_absf(R[i])));
    s->peak_in = pk;
    dry_delay(s, L, R, dL, dR, n);

    if (!sat_active(s)) {
        /* bypass: delayed dry only (bit-exact copy) */
        memcpy(L, dL, (size_t)n * sizeof(float));
        memcpy(R, dR, (size_t)n * sizeof(float));
        s->running = false;
        /* keep smoothers converging so a later enable starts from the right values */
        sm_step(&s->drive_db, 1.f); sm_step(&s->warmth, 1.f); sm_step(&s->tone, 1.f); sm_step(&s->mix, 1.f);
        return;
    }
    if (!s->running) {
        /* (re)start from clean filter states; the on-fade starts at 0 so the start-up transient is masked */
        hb_reset(&s->up1); hb_reset(&s->up2); hb_reset(&s->dn2); hb_reset(&s->dn1);
        fo_reset(&s->pre); fo_reset(&s->de); fo_reset(&s->tilt); fo_reset(&s->dc);
        s->running = true;
    }

    const dsp_ramp_t on = fade_block(&s->on, b->fade_step, b);
    /* smoothed controls -> per-sample ramps */
    const float d0 = s->drive_db.cur, d1 = sm_step(&s->drive_db, b->k);
    const float w0 = s->warmth.cur * (0.01f * SAT_WARM_MAX), w1 = sm_step(&s->warmth, b->k) * (0.01f * SAT_WARM_MAX);
    const float m0 = s->mix.cur * 0.01f, m1 = sm_step(&s->mix, b->k) * 0.01f;
    /* Tone glides 4x slower than other controls (~160 ms): the pre- and de-emphasis filters sit on either side of
     * the ~5-sample oversampling chain, so per-block coefficient updates are briefly mismatched; smaller steps
     * keep that zipper residue far below audibility (host click test). */
    const float t1 = sm_step(&s->tone, 0.25f * b->k);
    const dsp_fo_t pre0 = s->pre, de0 = s->de, tilt0 = s->tilt;     /* previous coefficients for per-sample interpolation */
    const bool tone_moving = t1 != s->tone_designed;
    if (tone_moving) design_tone(s, t1);
    /* The emphasis/tilt filters are identity at tone 0 (and their identity state is zero). Run them whenever the
     * tone is non-zero or still moving, and restart them from the identity state when they engage, so crossing
     * or leaving 0 never resumes a stale state. */
    const bool tone_act = s->tone_designed != 0.f || !sm_settled(&s->tone);
    if (tone_act && !s->tone_run) { fo_reset(&s->pre); fo_reset(&s->de); fo_reset(&s->tilt); }
    s->tone_run = tone_act;

    const float g0 = dsp_db2lin(d0), g1 = dsp_db2lin(d1);
    const float cw0 = m0 * sat_comp(s, d0), cw1 = m1 * sat_comp(s, d1);
    const float cd0 = (1.f - m0) * dsp_db2lin(-d0), cd1 = (1.f - m1) * dsp_db2lin(-d1);

    /* base rate: pre-emphasis, drive */
    float xL[DSP_BLOCK_MAX], xR[DSP_BLOCK_MAX];
    memcpy(xL, L, (size_t)n * sizeof(float));
    memcpy(xR, R, (size_t)n * sizeof(float));
    if (tone_act) {
        if (tone_moving) fo_run2_ramp(&s->pre, &pre0, xL, xR, n, b->inv_n);
        else fo_run2(&s->pre, xL, xR, n);
    }
    {
        const float dg = (g1 - g0) * b->inv_n;
        float g = g0;
        for (int i = 0; i < n; i++) { g += dg; xL[i] *= g; xR[i] *= g; }
    }
    /* up SAT_OS x */
#if SAT_OS == 4
    float aL[2 * DSP_BLOCK_MAX], aR[2 * DSP_BLOCK_MAX];
#endif
    float oL[SAT_OS * DSP_BLOCK_MAX], oR[SAT_OS * DSP_BLOCK_MAX];
    uint32_t td = dsp_cycles();
#if SAT_OS == 4
    hb1_up(&s->up1, xL, xR, aL, aR, n);
    DSP_DBG_ADD(0, td); td = dsp_cycles();
    hb2_up(&s->up2, aL, aR, oL, oR, 2 * n);
    DSP_DBG_ADD(1, td); td = dsp_cycles();
#else
    hb1_up(&s->up1, xL, xR, oL, oR, n);
    DSP_DBG_ADD(0, td); td = dsp_cycles();
#endif

    /* oversampled domain (SAT_OS x 48 kHz): shape, DC block, wet tilt, in-domain mix */
    {
        const int m = SAT_OS * n;
        const float inv_m = b->inv_n * (1.f / (float)SAT_OS);
        const float dw = (w1 - w0) * inv_m, dcw = (cw1 - cw0) * inv_m, dcd = (cd1 - cd0) * inv_m;
        float w = w0, cw = cw0, cd = cd0;
        const float r = s->r_dc;
        float dzl = s->dc.z[0], dzr = s->dc.z[1];
        const bool tilt = tone_act;
        /* tilt coefficients interpolated across the oversampled block while the tone moves */
        float tb0 = tone_moving ? tilt0.b0 : s->tilt.b0, tb1 = tone_moving ? tilt0.b1 : s->tilt.b1;
        float ta1 = tone_moving ? tilt0.a1 : s->tilt.a1;
        const float dtb0 = (s->tilt.b0 - tb0) * inv_m, dtb1 = (s->tilt.b1 - tb1) * inv_m, dta1 = (s->tilt.a1 - ta1) * inv_m;
        float tzl = s->tilt.z[0], tzr = s->tilt.z[1];
        /* Two passes per 64-frame chunk: the shaper (independent per sample, unrolled so several evaluations are
         * in flight on the in-order FPU), then the recursive DC blocker / tilt and the in-domain mix. */
        float shL[64], shR[64];
        for (int j0 = 0; j0 < m; j0 += 64) {
            const int mc = m - j0 < 64 ? m - j0 : 64;
            const float *uL = oL + j0, *uR = oR + j0;
            float wj = w;
            int j = 0;
            for (; j + 1 < mc; j += 2) {
                const float wa = wj + dw, wb = wj + 2.f * dw;
                wj = wb;
                shL[j] = sat_shape(uL[j], wa);         shR[j] = sat_shape(uR[j], wa);
                shL[j + 1] = sat_shape(uL[j + 1], wb); shR[j + 1] = sat_shape(uR[j + 1], wb);
            }
            for (; j < mc; j++) { wj += dw; shL[j] = sat_shape(uL[j], wj); shR[j] = sat_shape(uR[j], wj); }
            w = wj;
            for (j = 0; j < mc; j++) {
                cw += dcw; cd += dcd;
                float sl = shL[j], sr = shR[j];
                /* DC blocker y = x + z; z = r y - x */
                const float hl = sl + dzl, hr = sr + dzr;
                dzl = r * hl - sl; dzr = r * hr - sr;
                sl = hl; sr = hr;
                if (tilt) {
                    tb0 += dtb0; tb1 += dtb1; ta1 += dta1;
                    const float yl = tb0 * sl + tzl, yr = tb0 * sr + tzr;
                    tzl = tb1 * sl - ta1 * yl; tzr = tb1 * sr - ta1 * yr;
                    sl = yl; sr = yr;
                }
                oL[j0 + j] = cw * sl + cd * uL[j];
                oR[j0 + j] = cw * sr + cd * uR[j];
            }
        }
        s->dc.z[0] = dsp_flush(dzl); s->dc.z[1] = dsp_flush(dzr);
        s->tilt.z[0] = dsp_flush(tzl); s->tilt.z[1] = dsp_flush(tzr);
    }
    /* down 4x */
    DSP_DBG_ADD(2, td); td = dsp_cycles();
#if SAT_OS == 4
    hb2_down(&s->dn2, oL, oR, aL, aR, 2 * n);
    DSP_DBG_ADD(3, td); td = dsp_cycles();
    hb1_down(&s->dn1, aL, aR, xL, xR, n);
#else
    hb1_down(&s->dn1, oL, oR, xL, xR, n);
#endif
    DSP_DBG_ADD(4, td);
    if (tone_act) {
        if (tone_moving) fo_run2_ramp(&s->de, &de0, xL, xR, n, b->inv_n);
        else fo_run2(&s->de, xL, xR, n);
    }

    /* on/off crossfade against the latency-matched dry */
    if (on.d == 0.f && on.v == 1.f) {
        memcpy(L, xL, (size_t)n * sizeof(float));
        memcpy(R, xR, (size_t)n * sizeof(float));
    } else {
        float f = on.v;
        for (int i = 0; i < n; i++) {
            f += on.d;
            L[i] = dL[i] + f * (xL[i] - dL[i]);
            R[i] = dR[i] + f * (xR[i] - dR[i]);
        }
    }
    hb_flush(&s->up1); hb_flush(&s->up2); hb_flush(&s->dn2); hb_flush(&s->dn1);
    if (s->on.v == 0.f && s->on.tgt == 0.f) s->running = false;
}
