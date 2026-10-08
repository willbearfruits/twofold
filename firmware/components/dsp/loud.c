/* loud.c: see dsp_loud.h */
#include "dsp_loud.h"
#include "dsp_port.h"
#include "dsp_math.h"
#include <string.h>

#define LO_F    50.f
#define LO_FC   125.f
#define HI_F    10000.f
#define HI_FC   6000.f
#define G_MIN   (-6.f)
#define G_MAX   24.f

/* ISO 226:2003 table 1: frequency, alpha_f, L_U, T_f (29 rows, 20 Hz .. 12.5 kHz) */
static const float k_f[29]  = { 20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
                                1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500 };
static const float k_af[29] = { 0.532f, 0.506f, 0.480f, 0.455f, 0.432f, 0.409f, 0.387f, 0.367f, 0.349f, 0.330f, 0.315f,
                                0.301f, 0.288f, 0.276f, 0.267f, 0.259f, 0.253f, 0.250f, 0.246f, 0.244f, 0.243f, 0.243f,
                                0.243f, 0.242f, 0.242f, 0.245f, 0.254f, 0.271f, 0.301f };
static const float k_lu[29] = { -31.6f, -27.2f, -23.0f, -19.1f, -16.1f, -13.4f, -10.9f, -8.6f, -6.6f, -4.8f, -3.4f,
                                -2.2f, -1.2f, -0.4f, 0.0f, 0.3f, 0.5f, 0.0f, -2.7f, -4.1f, -1.0f, 1.7f, 2.5f, 1.2f,
                                -2.1f, -7.1f, -11.2f, -10.7f, -3.1f };
static const float k_tf[29] = { 78.5f, 68.7f, 59.5f, 51.1f, 44.0f, 37.5f, 31.5f, 26.5f, 22.1f, 17.9f, 14.4f, 11.4f,
                                8.6f, 6.2f, 4.4f, 3.0f, 2.2f, 2.4f, 3.5f, 1.7f, -1.3f, -4.2f, -6.0f, -5.4f, -1.5f,
                                6.0f, 12.6f, 13.9f, 12.3f };

static inline float pow10f_(float x) { return dsp_exp2(x * 3.32192809f); }
static inline float log10f_(float x) { return dsp_log2(x) * 0.30102999f; }
static inline float powf_(float x, float y) { return dsp_exp2(y * dsp_log2(x)); }

static float spl_row(int i, float phon)
{
    const float af = k_af[i];
    const float cf = powf_(0.4f * pow10f_((k_tf[i] + k_lu[i]) * 0.1f - 9.f), af);
    const float Af = 4.47e-3f * (pow10f_(0.025f * phon) - 1.15f) + cf;
    return (10.f / af) * log10f_(Af) - k_lu[i] + 94.f;
}

float loud_iso226_spl(float f, float phon)
{
    phon = dsp_clampf(phon, 20.f, 90.f);
    f = dsp_clampf(f, k_f[0], k_f[28]);
    int i = 0;
    while (i < 27 && k_f[i + 1] < f) i++;
    const float t = dsp_log2(f / k_f[i]) / dsp_log2(k_f[i + 1] / k_f[i]);   /* log-frequency interpolation */
    const float a = spl_row(i, phon), b = spl_row(i + 1, phon);
    return a + t * (b - a);
}

static float gain_at(float f, float ref, float amt, float vol_db)
{
    const float A = vol_db < 0.f ? -vol_db : 0.f;          /* attenuation below the reference */
    if (A <= 0.f || amt <= 0.f) return 0.f;
    const float pr = dsp_clampf(ref, 20.f, 90.f), pl = dsp_clampf(ref - A, 20.f, 90.f);
    const float g = loud_iso226_spl(f, pl) - loud_iso226_spl(f, pr) + (pr - pl);
    return dsp_clampf(g * amt * 0.01f, G_MIN, G_MAX);
}

float loud_gain_db(float f, float ref, float amt, float vol_db) { return gain_at(f, ref, amt, vol_db); }

void loud_init(dsp_loud_t *l)
{
    memset(l, 0, sizeof *l);
    sm_init(&l->glo, 0.f, 1e-3f);
    sm_init(&l->ghi, 0.f, 1e-3f);
    bq_identity(&l->lo.c); bq_identity(&l->hi.c);
    l->ref = 83.f; l->amt = 100.f;
}

void loud_reset(dsp_loud_t *l) { bq_reset(&l->lo); bq_reset(&l->hi); l->active = false; }

void loud_set(dsp_loud_t *l, bool on, float ref, float amt, float vol_db)
{
    l->on = on; l->ref = ref; l->amt = amt;
    const float a = on ? amt : 0.f;
    sm_set(&l->glo, gain_at(LO_F, ref, a, vol_db));
    sm_set(&l->ghi, gain_at(HI_F, ref, a, vol_db));
}

static void design(dsp_loud_t *l, float glo, float ghi)
{
    bq_design_fast(&l->lo.c, RBJ_LOWSHELF, (float)DSP_FS, LO_FC, glo, 0.7071f);
    bq_design_fast(&l->hi.c, RBJ_HIGHSHELF, (float)DSP_FS, HI_FC, ghi, 0.7071f);
    l->dlo = glo; l->dhi = ghi;
}

void loud_settle(dsp_loud_t *l)
{
    sm_jump(&l->glo, l->glo.tgt); sm_jump(&l->ghi, l->ghi.tgt);
    design(l, l->glo.cur, l->ghi.cur);
    l->active = l->glo.cur != 0.f || l->ghi.cur != 0.f;
}

DSP_HOT void loud_process(dsp_loud_t *l, float *L, float *R, const dsp_blk_t *b)
{
    const float g0 = sm_step(&l->glo, b->k), g1 = sm_step(&l->ghi, b->k);
    if (!l->active) {
        if (g0 == 0.f && g1 == 0.f) return;              /* exact bypass */
        bq_reset(&l->lo); bq_reset(&l->hi);
        l->active = true;
    }
    if (g0 != l->dlo || g1 != l->dhi) {
        biquad_coef_t lo, hi;
        bq_design_fast(&lo, RBJ_LOWSHELF, (float)DSP_FS, LO_FC, g0, 0.7071f);
        bq_design_fast(&hi, RBJ_HIGHSHELF, (float)DSP_FS, HI_FC, g1, 0.7071f);
        bq_process2_ramp(&l->lo, &lo, L, R, b->n);
        bq_process2_ramp(&l->hi, &hi, L, R, b->n);
        l->dlo = g0; l->dhi = g1;
    } else {
        bq_process2(&l->lo, L, R, b->n);
        bq_process2(&l->hi, L, R, b->n);
    }
    /* once both gains have settled at 0, let the states ring out, then stop */
    if (g0 == 0.f && g1 == 0.f && bq_flush(&l->lo) < 1e-9f && bq_flush(&l->hi) < 1e-9f) {
        bq_reset(&l->lo); bq_reset(&l->hi);
        l->active = false;
    }
}
