/*
 * dsp_engine.c: the TWOFOLD chain (see dsp_engine.h, PLAN.md section 8).
 *
 * Threading: dsp_engine_process() runs on the audio task only and is the single writer of every published
 * value. Readers (UI/web, core 0) use seqlocks over relaxed atomics (meters, profiling), a SPSC ring
 * (spectrum) or single atomic words (flags, viz). Requests from other tasks (soft mute, meter take, startup
 * ramp) are atomic counters/words picked up at the next block. No locks, no allocation after init.
 */
#include "dsp_engine.h"
#include "dsp_port.h"
#include "dsp_math.h"
#include "dsp_smooth.h"
#include "dsp_eq.h"
#include "dsp_sat.h"
#include "dsp_chorus.h"
#include "dsp_delay.h"
#include "dsp_reverb.h"
#include "dsp_comp.h"
#include "dsp_limiter.h"
#include "dsp_dither.h"
#include "dsp_loud.h"
#include "dsp_xfeed.h"
#include "dsp_vec.h"
#include "params.h"
#include <string.h>

const char *const dsp_stage_names[DSP_ST_COUNT] = {
    "INPUT", "EQ", "SAT", "CHORUS", "DELAY", "REVERB", "COMP", "OUTPUT", "DITHER",
};

#define TAU_MS          6.f      /* per stage of the 2-stage smoother: ~40 ms to 99 % */
#define FADE_MS         30.f     /* effect on/off, mute */
#define XFADE_MS        50.f     /* delay heads, routing, enum crossfades */
#define GLIDE_MS        ((float)PARAMS_PRESET_GLIDE_MS)
#define GLIDE_TAU_MS    (GLIDE_MS / 6.6f)   /* 2-stage: 99 % at ~6.6 tau */
#define GLIDE_WINDOW    (DSP_FS * 2 / 5)      /* 400 ms after a preset epoch change */
#define SMUTE_SAMPLES   240                   /* 5 ms soft-mute ramp */
#define QUIET_LIN       3.1622777e-5f         /* -90 dBFS */
#define QUIET_SAMPLES   (2 * DSP_FS)
#define CLIP_LIN        0.98855309f           /* -0.1 dBFS */
#define PROF_WINDOW     DSP_FS                /* 1 s */
#define LSB24           (1.f / 8388608.f)

#define ATOMIC_LD(p)       __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ATOMIC_ST(p, v)    __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define RELAXED_LD(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define RELAXED_ST(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELAXED)

/* ---- published (cross-core) state ---- */
enum { MP_IN_PEAK0, MP_IN_PEAK1, MP_OUT_PEAK0, MP_OUT_PEAK1, MP_PLAY_PEAK0, MP_PLAY_PEAK1,
       MP_IN_CLIP0, MP_IN_CLIP1, MP_OUT_OVER0, MP_OUT_OVER1, MP_COMP_GR, MP_LIM_GR, MP_SAT_IN, MP_COUNT };
enum { PP_AVG = 0, PP_MAX = DSP_ST_COUNT, PP_TAVG = 2 * DSP_ST_COUNT, PP_TMAX, PP_BLOCKS, PP_COUNT };

typedef struct {
    uint32_t seq;
    uint32_t w[MP_COUNT];
} pub_meters_t;

typedef struct {
    uint32_t seq;
    uint32_t w[PP_COUNT];
} pub_prof_t;

static struct {
    bool          inited;
    dsp_eq_t      eq;
    dsp_sat_t     sat;
    dsp_chorus_t  cho;
    dsp_delay_t   dly;
    dsp_reverb_t  rev;
    dsp_comp_t    cmp;
    dsp_limiter_t lim;
    dsp_dither_t  dith_dac, dith_rec;
    dsp_loud_t    loud;
    dsp_xfeed_t   xf;

    /* input */
    int        in_mode, in_from;
    float      in_xf;
    dsp_sm_t   in_gain;
    /* monitor */
    dsp_sm_t   mon_mix;           /* 0..1 */
    float      mon_path, mon_path_tgt;
    /* master */
    dsp_sm_t   vol_db;
    dsp_fade_t vol_nz, unmute;
    float      start_g, start_step;
    uint32_t   start_req_seen;
    /* soft mute */
    float      smute_g;
    int        zero_blocks;
    /* glide */
    uint32_t   epoch_seen;
    int        glide_left;
    uint32_t   seq_seen, blk_count;    /* parameter change tracking */
    /* quiet detector */
    uint32_t   quiet_samples;

    /* meter accumulators (audio-owned) */
    float      acc_in[2], acc_out[2], acc_play[2], acc_cgr, acc_lgr, acc_sat;
    float      last_in[2], last_out[2], last_play[2], last_cgr, last_lgr, last_sat;
    uint32_t   in_clip[2], out_over[2];
    uint32_t   take_seen;

    /* profiling accumulators */
    uint32_t   prof_sum[DSP_ST_COUNT], prof_total;
    float      prof_max[DSP_ST_COUNT], prof_tmax;
    uint32_t   prof_samples;
    uint32_t   blocks;
} E;

/* cross-core words */
static pub_meters_t s_meters;
static pub_prof_t   s_prof;
static uint32_t     s_take_cnt, s_take_at;
static int32_t      s_smute_cnt;
static uint32_t     s_silent, s_quiet;
static uint32_t     s_start_req_cnt, s_start_req_ms;
static uint32_t     s_viz[5];
static float       *s_spec;              /* DSP_SPECTRUM_RING floats, PSRAM (sequential writes) */
static uint32_t     s_spec_w;

/* scratch (internal RAM, .bss) */
static float pL[DSP_BLOCK_MAX], pR[DSP_BLOCK_MAX], aL[DSP_BLOCK_MAX], aR[DSP_BLOCK_MAX];
static float xL[DSP_BLOCK_MAX], xR[DSP_BLOCK_MAX];

static inline uint32_t fbits(float f) { return dsp_f2u(f); }
static inline float    bitsf(uint32_t u) { return dsp_u2f(u); }

/* ------------------------------------------------------------------------------------------------ */

static void read_params(bool settle)
{
    /* EQ */
    const bool eq_on = params_get_bool(P_EQ_ON);
    static const param_id_t bon[EQ_BANDS] = { P_EQ_B1_ON, P_EQ_B2_ON, P_EQ_B3_ON, P_EQ_B4_ON, P_EQ_B5_ON };
    for (int b = 0; b < EQ_BANDS; b++) {
        const param_id_t base = (param_id_t)(bon[b] + 1);   /* FREQ, GAIN, Q follow ON in the table */
        const float g = (eq_on && params_get_bool(bon[b])) ? params_get((param_id_t)(base + 1)) : 0.f;
        eq_set_band(&E.eq, b, params_get(base), g, params_get((param_id_t)(base + 2)));
    }
    eq_set_trims(&E.eq, eq_on ? params_get(P_EQ_IN) : 0.f, eq_on ? params_get(P_EQ_OUT) : 0.f);
    /* effects */
    sat_set(&E.sat, params_get_bool(P_SAT_ON), params_get(P_SAT_DRIVE), params_get(P_SAT_WARMTH),
            params_get(P_SAT_TONE), params_get(P_SAT_MIX));
    cho_set(&E.cho, params_get_bool(P_CHO_ON), params_get_int(P_CHO_MODE), params_get(P_CHO_RATE),
            params_get(P_CHO_DEPTH), params_get(P_CHO_MIX));
    dly_set(&E.dly, params_get_bool(P_DLY_ON),
            dly_time_ms(params_get_int(P_DLY_SYNC), params_get(P_TEMPO), params_get(P_DLY_TIME)),
            params_get(P_DLY_FB), params_get_int(P_DLY_MODE), params_get(P_DLY_HICUT), params_get(P_DLY_LOCUT),
            params_get(P_DLY_MIX));
    rev_set(&E.rev, params_get_bool(P_REV_ON), params_get(P_REV_DECAY), params_get(P_REV_PREDLY),
            params_get(P_REV_DAMP), params_get(P_REV_MOD), params_get(P_REV_LOCUT), params_get(P_REV_WIDTH),
            params_get(P_REV_MIX));
    comp_set(&E.cmp, params_get_bool(P_CMP_ON), params_get(P_CMP_THRESH), params_get(P_CMP_RATIO),
             params_get(P_CMP_ATTACK), params_get(P_CMP_RELEASE), params_get(P_CMP_KNEE), params_get(P_CMP_MAKEUP),
             params_get(P_CMP_MIX));
    /* limiter: 2 LSB below the ceiling so TPDF dither (+-1 LSB) can never cross it */
    lim_set(&E.lim, params_get_bool(P_LIM_ON), params_get(P_LIM_CEIL), 2.f * LSB24);
    loud_set(&E.loud, params_get_bool(P_LOUD_ON), params_get(P_LOUD_REF), params_get(P_LOUD_AMT), params_get(P_VOLUME));
    xf_set(&E.xf, params_get_bool(P_XF_ON), params_get(P_XF_FEED), params_get(P_XF_FC));

    /* input / monitor / master */
    const int mode = dsp_clampi(params_get_int(P_IN_MODE), 0, 3);
    if (mode != E.in_mode && E.in_xf >= 1.f) { E.in_from = E.in_mode; E.in_mode = mode; E.in_xf = settle ? 1.f : 0.f; }
    sm_set(&E.in_gain, params_get(P_IN_GAIN));
    sm_set(&E.mon_mix, params_get(P_MON_MIX) * 0.01f);
    E.mon_path_tgt = params_get_int(P_MON_PATH) ? 1.f : 0.f;
    const float vol = params_get(P_VOLUME);
    const param_desc_t *vd = params_desc(P_VOLUME);
    sm_set(&E.vol_db, vol);
    E.vol_nz.tgt = vol <= vd->min + 0.01f ? 0.f : 1.f;
    E.unmute.tgt = params_get_bool(P_MUTE) ? 0.f : 1.f;

    if (settle) {
        eq_settle(&E.eq); sat_settle(&E.sat); cho_settle(&E.cho); dly_settle(&E.dly); rev_settle(&E.rev);
        comp_settle(&E.cmp); lim_settle(&E.lim); loud_settle(&E.loud); xf_settle(&E.xf);
        sm_jump(&E.in_gain, E.in_gain.tgt); sm_jump(&E.mon_mix, E.mon_mix.tgt);
        E.mon_path = E.mon_path_tgt;
        sm_jump(&E.vol_db, E.vol_db.tgt);
        E.vol_nz.v = E.vol_nz.tgt; E.unmute.v = E.unmute.tgt;
        E.in_from = E.in_mode; E.in_xf = 1.f;
    }
}

int dsp_engine_init(const dsp_alloc_t *alloc)
{
    if (E.inited) return 0;
    memset(&E, 0, sizeof E);
    eq_init(&E.eq);
    sat_init(&E.sat);
    s_spec = (float *)alloc->alloc_big(DSP_SPECTRUM_RING * sizeof(float));
    if (!s_spec) return -4;
    if (cho_init(&E.cho, alloc->alloc_hot ? alloc->alloc_hot : alloc->alloc_fast)) return -1;
    if (dly_init(&E.dly, alloc->alloc_big)) return -2;
    if (rev_init(&E.rev, alloc->alloc_fast, alloc->alloc_big)) return -3;
    comp_init(&E.cmp);
    loud_init(&E.loud);
    xf_init(&E.xf);
    lim_init(&E.lim, 60.f);
    dither_init(&E.dith_dac, 0x2545f491u);
    dither_init(&E.dith_rec, 0x9e3779b9u);
    sm_init(&E.in_gain, 0.f, 1e-4f);
    sm_init(&E.mon_mix, 0.f, 1e-5f);
    sm_init(&E.vol_db, -20.f, 1e-4f);
    fade_init(&E.vol_nz, 1.f);
    fade_init(&E.unmute, 1.f);
    E.in_xf = 1.f;
    E.start_g = 1.f;
    E.smute_g = 1.f;
    E.epoch_seen = params_preset_epoch();
    E.start_req_seen = ATOMIC_LD(&s_start_req_cnt);
    read_params(true);
    E.seq_seen = params_seq();
    E.inited = true;
    return 0;
}

void dsp_engine_reset(void)
{
    eq_reset(&E.eq); sat_reset(&E.sat); cho_reset(&E.cho); dly_reset(&E.dly); rev_reset(&E.rev);
    comp_reset(&E.cmp); lim_reset(&E.lim); loud_reset(&E.loud); xf_reset(&E.xf);
}

/* ------------------------------------------------------------------------------------------------ */

static void publish_meters(void)
{
    /* meter "take": reset the accumulators only if the reader saw the latest published values */
    const uint32_t tc = ATOMIC_LD(&s_take_cnt);
    if (tc != E.take_seen) {
        E.take_seen = tc;
        const uint32_t at = RELAXED_LD(&s_take_at);
        const uint32_t cur = RELAXED_LD(&s_meters.seq);
        if (at == cur) {
            /* reader saw everything up to now: start fresh with this block only */
            for (int c = 0; c < 2; c++) { E.acc_in[c] = E.last_in[c]; E.acc_out[c] = E.last_out[c]; E.acc_play[c] = E.last_play[c]; }
            E.acc_cgr = E.last_cgr; E.acc_lgr = E.last_lgr; E.acc_sat = E.last_sat;
        }
        /* else: blocks were published after the reader's copy: keep accumulating (never under-report) */
    }
    uint32_t seq = RELAXED_LD(&s_meters.seq);
    RELAXED_ST(&s_meters.seq, seq + 1);                     /* odd: writing */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t *w = s_meters.w;
    RELAXED_ST(&w[MP_IN_PEAK0], fbits(E.acc_in[0]));   RELAXED_ST(&w[MP_IN_PEAK1], fbits(E.acc_in[1]));
    RELAXED_ST(&w[MP_OUT_PEAK0], fbits(E.acc_out[0])); RELAXED_ST(&w[MP_OUT_PEAK1], fbits(E.acc_out[1]));
    RELAXED_ST(&w[MP_PLAY_PEAK0], fbits(E.acc_play[0])); RELAXED_ST(&w[MP_PLAY_PEAK1], fbits(E.acc_play[1]));
    RELAXED_ST(&w[MP_IN_CLIP0], E.in_clip[0]);         RELAXED_ST(&w[MP_IN_CLIP1], E.in_clip[1]);
    RELAXED_ST(&w[MP_OUT_OVER0], E.out_over[0]);       RELAXED_ST(&w[MP_OUT_OVER1], E.out_over[1]);
    RELAXED_ST(&w[MP_COMP_GR], fbits(E.acc_cgr));
    RELAXED_ST(&w[MP_LIM_GR], fbits(E.acc_lgr));
    RELAXED_ST(&w[MP_SAT_IN], fbits(E.acc_sat));
    ATOMIC_ST(&s_meters.seq, seq + 2);                     /* even: stable */
}

void dsp_engine_get_meters(dsp_meters_t *m, bool take)
{
    uint32_t w[MP_COUNT], s1, s2;
    int tries = 0;
    do {
        s1 = ATOMIC_LD(&s_meters.seq);
        for (int i = 0; i < MP_COUNT; i++) w[i] = RELAXED_LD(&s_meters.w[i]);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = RELAXED_LD(&s_meters.seq);
    } while ((s1 != s2 || (s1 & 1u)) && ++tries < 8);
    m->in_peak[0] = bitsf(w[MP_IN_PEAK0]);   m->in_peak[1] = bitsf(w[MP_IN_PEAK1]);
    m->out_peak[0] = bitsf(w[MP_OUT_PEAK0]); m->out_peak[1] = bitsf(w[MP_OUT_PEAK1]);
    m->play_peak[0] = bitsf(w[MP_PLAY_PEAK0]); m->play_peak[1] = bitsf(w[MP_PLAY_PEAK1]);
    m->in_clip[0] = w[MP_IN_CLIP0];   m->in_clip[1] = w[MP_IN_CLIP1];
    m->out_over[0] = w[MP_OUT_OVER0]; m->out_over[1] = w[MP_OUT_OVER1];
    m->comp_gr_db = bitsf(w[MP_COMP_GR]);
    m->lim_gr_db = bitsf(w[MP_LIM_GR]);
    m->sat_in_level = bitsf(w[MP_SAT_IN]);
    if (take) {
        RELAXED_ST(&s_take_at, s2);
        __atomic_fetch_add(&s_take_cnt, 1u, __ATOMIC_RELEASE);
    }
}

static void prof_block(const uint32_t cyc[DSP_ST_COUNT], int n)
{
    uint32_t tot = 0;
    const float inv = 100.f / ((float)n * (float)(DSP_CPU_HZ / DSP_FS));
    for (int s = 0; s < DSP_ST_COUNT; s++) {
        E.prof_sum[s] += cyc[s];
        tot += cyc[s];
        E.prof_max[s] = dsp_maxf(E.prof_max[s], (float)cyc[s] * inv);
    }
    E.prof_total += tot;
    E.prof_tmax = dsp_maxf(E.prof_tmax, (float)tot * inv);
    E.prof_samples += (uint32_t)n;
    E.blocks++;
    if (E.prof_samples >= PROF_WINDOW) {
        const float winv = 100.f / ((float)E.prof_samples * (float)(DSP_CPU_HZ / DSP_FS));
        uint32_t seq = RELAXED_LD(&s_prof.seq);
        RELAXED_ST(&s_prof.seq, seq + 1);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for (int s = 0; s < DSP_ST_COUNT; s++) {
            RELAXED_ST(&s_prof.w[PP_AVG + s], fbits((float)E.prof_sum[s] * winv));
            RELAXED_ST(&s_prof.w[PP_MAX + s], fbits(E.prof_max[s]));
            E.prof_sum[s] = 0; E.prof_max[s] = 0.f;
        }
        RELAXED_ST(&s_prof.w[PP_TAVG], fbits((float)E.prof_total * winv));
        RELAXED_ST(&s_prof.w[PP_TMAX], fbits(E.prof_tmax));
        RELAXED_ST(&s_prof.w[PP_BLOCKS], E.blocks);
        ATOMIC_ST(&s_prof.seq, seq + 2);
        E.prof_total = 0; E.prof_tmax = 0.f; E.prof_samples = 0;
    }
}

void dsp_engine_get_prof(dsp_prof_t *p)
{
    uint32_t w[PP_COUNT], s1, s2;
    int tries = 0;
    do {
        s1 = ATOMIC_LD(&s_prof.seq);
        for (int i = 0; i < PP_COUNT; i++) w[i] = RELAXED_LD(&s_prof.w[i]);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = RELAXED_LD(&s_prof.seq);
    } while ((s1 != s2 || (s1 & 1u)) && ++tries < 8);
    for (int s = 0; s < DSP_ST_COUNT; s++) { p->avg_pct[s] = bitsf(w[PP_AVG + s]); p->max_pct[s] = bitsf(w[PP_MAX + s]); }
    p->total_avg_pct = bitsf(w[PP_TAVG]);
    p->total_max_pct = bitsf(w[PP_TMAX]);
    p->blocks = RELAXED_LD(&E.blocks);
    p->window_ms = 1000u * PROF_WINDOW / DSP_FS;
}

size_t dsp_engine_spectrum_read(float *dst, size_t n)
{
    if (n > DSP_SPECTRUM_RING) n = DSP_SPECTRUM_RING;
    for (int tries = 0; tries < 3; tries++) {
        const uint32_t w = ATOMIC_LD(&s_spec_w);
        for (size_t i = 0; i < n; i++) dst[i] = s_spec[(w - n + i) & (DSP_SPECTRUM_RING - 1)];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t w2 = RELAXED_LD(&s_spec_w);
        if (w2 - w <= DSP_SPECTRUM_RING - n) return n;     /* nothing we copied was overwritten */
    }
    return n;
}

void dsp_engine_get_viz(dsp_viz_t *v)
{
    for (int i = 0; i < 3; i++) v->chorus_lfo_phase[i] = bitsf(RELAXED_LD(&s_viz[i]));
    v->delay_time_ms = bitsf(RELAXED_LD(&s_viz[3]));
    v->reverb_energy = bitsf(RELAXED_LD(&s_viz[4]));
}

void dsp_engine_soft_mute(bool on)
{
    if (on) __atomic_fetch_add(&s_smute_cnt, 1, __ATOMIC_ACQ_REL);
    else {
        int32_t c = ATOMIC_LD(&s_smute_cnt);
        while (c > 0 && !__atomic_compare_exchange_n(&s_smute_cnt, &c, c - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    }
}
bool dsp_engine_is_silent(void) { return ATOMIC_LD(&s_silent) != 0; }
bool dsp_engine_output_quiet(void) { return ATOMIC_LD(&s_quiet) != 0; }

void dsp_engine_startup_ramp(uint32_t ms)
{
    RELAXED_ST(&s_start_req_ms, ms);
    __atomic_fetch_add(&s_start_req_cnt, 1u, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------------------------------ */

static const float k_in_mtx[4][4] = {     /* {LL, LR, RL, RR}: outL = LL*inL + LR*inR, outR = RL*inL + RR*inR */
    { 1.f, 0.f, 0.f, 1.f },               /* STEREO */
    { .5f, .5f, .5f, .5f },               /* MONO */
    { 1.f, 0.f, 1.f, 0.f },               /* LEFT */
    { 0.f, 1.f, 0.f, 1.f },               /* RIGHT */
};

static DSP_HOT void to_float(const int32_t *src, float *L, float *R, int n)
{
    if (!src) { memset(L, 0, (size_t)n * sizeof(float)); memset(R, 0, (size_t)n * sizeof(float)); return; }
    for (int i = 0; i < n; i++) { L[i] = dsp_i2f(src[2 * i]); R[i] = dsp_i2f(src[2 * i + 1]); }
}

static inline float peak2(const float *L, const float *R, int n, float *pl, float *pr)
{
    float a = 0.f, b = 0.f;
    for (int i = 0; i < n; i++) { a = dsp_maxf(a, dsp_absf(L[i])); b = dsp_maxf(b, dsp_absf(R[i])); }
    *pl = a; *pr = b;
    return dsp_maxf(a, b);
}

/* soft mute gain ramp for this block: returns start gain and per-sample step (towards target) */
static void smute_ramp(float tgt, int n, float *g0, float *d)
{
    *g0 = E.smute_g;
    float e = E.smute_g;
    const float step = (float)n / (float)SMUTE_SAMPLES;
    if (tgt < e) { e -= step; if (e < 0.f) e = 0.f; }
    else if (tgt > e) { e += step; if (e > 1.f) e = 1.f; }
    *d = (e - *g0) / (float)n;
    E.smute_g = e;
}

static DSP_HOT void process_block(const int32_t *play, const int32_t *adc, int32_t *dac, int32_t *rec, int n)
{
    uint32_t cyc[DSP_ST_COUNT];
    uint32_t t0 = dsp_cycles(), t1;

    /* ---------- block setup ---------- */
    const uint32_t ep = params_preset_epoch();
    if (ep != E.epoch_seen) { E.epoch_seen = ep; E.glide_left = GLIDE_WINDOW; }
    const bool glide = E.glide_left > 0;
    if (glide) E.glide_left -= n;
    dsp_blk_t blk;
    dsp_blk_make(&blk, n, glide ? GLIDE_TAU_MS : TAU_MS, glide ? GLIDE_MS : FADE_MS, glide ? GLIDE_MS : XFADE_MS);
    /* Parameters: re-read on any change, plus every 8th block as a sweep (some changes, e.g. the input mode, apply
     * only once a running crossfade has finished). Re-reading all ~80 every block cost ~10 % of a core. */
    {
        const uint32_t seq = params_seq();
        if (seq != E.seq_seen || (E.blk_count & 7u) == 0) { E.seq_seen = seq; read_params(false); }
        E.blk_count++;
    }
    {
        const uint32_t rc = ATOMIC_LD(&s_start_req_cnt);
        if (rc != E.start_req_seen) {
            E.start_req_seen = rc;
            const uint32_t ms = RELAXED_LD(&s_start_req_ms);
            E.start_g = 0.f;
            E.start_step = ms ? 1000.f / ((float)ms * (float)DSP_FS) : 1.f;
        }
    }
    const float smute_tgt = ATOMIC_LD(&s_smute_cnt) > 0 ? 0.f : 1.f;
    float sm0, smd;
    smute_ramp(smute_tgt, n, &sm0, &smd);
    const bool smute_full = sm0 == 0.f && smd == 0.f;      /* whole block is digital zero */
    const bool smute_off = sm0 == 1.f && smd == 0.f;

    DSP_DBG_ADD(6, t0);                                     /* block setup */
    uint32_t td = dsp_cycles();
    /* ---------- INPUT ---------- */
    to_float(play, pL, pR, n);
    to_float(adc, aL, aR, n);
    float pl, pr, il, ir;
    peak2(pL, pR, n, &pl, &pr);
    E.last_play[0] = pl; E.last_play[1] = pr;
    E.acc_play[0] = dsp_maxf(E.acc_play[0], pl); E.acc_play[1] = dsp_maxf(E.acc_play[1], pr);
    float raw_l, raw_r;
    peak2(aL, aR, n, &raw_l, &raw_r);
    DSP_DBG_ADD(7, td); td = dsp_cycles();                  /* int->float + peaks */
    /* input mode matrix (crossfaded on change) */
    if (!(E.in_xf >= 1.f && E.in_mode == 0)) {
        const float *A = k_in_mtx[E.in_from], *B = k_in_mtx[E.in_mode];
        float x = E.in_xf;
        for (int i = 0; i < n; i++) {
            if (x < 1.f) { x += blk.xfade_step; if (x > 1.f) x = 1.f; }
            const float m0 = A[0] + x * (B[0] - A[0]), m1 = A[1] + x * (B[1] - A[1]);
            const float m2 = A[2] + x * (B[2] - A[2]), m3 = A[3] + x * (B[3] - A[3]);
            const float l = aL[i], r = aR[i];
            aL[i] = m0 * l + m1 * r;
            aR[i] = m2 * l + m3 * r;
        }
        E.in_xf = x;
    }
    /* input gain */
    const bool unity_in = E.in_gain.cur == 0.f && E.in_gain.tgt == 0.f;
    if (!unity_in) {
        const float g0 = dsp_db2lin(E.in_gain.cur), g1 = dsp_db2lin(sm_step(&E.in_gain, blk.k));
        const float dg = (g1 - g0) * blk.inv_n;
        float g = g0;
        for (int i = 0; i < n; i++) { g += dg; aL[i] *= g; aR[i] *= g; }
    }
    peak2(aL, aR, n, &il, &ir);
    E.last_in[0] = il; E.last_in[1] = ir;
    E.acc_in[0] = dsp_maxf(E.acc_in[0], il); E.acc_in[1] = dsp_maxf(E.acc_in[1], ir);
    if (raw_l >= CLIP_LIN || il >= CLIP_LIN) E.in_clip[0]++;
    if (raw_r >= CLIP_LIN || ir >= CLIP_LIN) E.in_clip[1]++;
    /* record path */
    if (rec) {
        if (smute_full || !adc) {
            memset(rec, 0, (size_t)n * 2 * sizeof(int32_t));
        } else if (unity_in && E.in_mode == 0 && E.in_xf >= 1.f && smute_off) {
            /* bit-transparent: unity gain, STEREO -> exact copy (24-bit grid), no dither */
            for (int i = 0; i < 2 * n; i++) rec[i] = (int32_t)((uint32_t)adc[i] & 0xffffff00u);
        } else if (smute_off) {
            dither_convert(&E.dith_rec, aL, aR, rec, n);
        } else {
            float g = sm0;
            for (int i = 0; i < n; i++) { g += smd; xL[i] = aL[i] * g; xR[i] = aR[i] * g; }
            dither_convert(&E.dith_rec, xL, xR, rec, n);
        }
    }
    DSP_DBG_ADD(8, td);                                     /* input mode/gain/record */
    t1 = dsp_cycles(); cyc[DSP_ST_INPUT] = t1 - t0; t0 = t1;

    /* ---------- monitor gains (THRU FX part enters before the EQ) ---------- */
    const float mm0 = E.mon_mix.cur, mm1 = sm_step(&E.mon_mix, blk.k);
    const float mp0 = E.mon_path;
    if (E.mon_path != E.mon_path_tgt) {
        const float st = blk.xfade_step * (float)n;
        E.mon_path = E.mon_path_tgt > E.mon_path ? dsp_minf(E.mon_path + st, E.mon_path_tgt)
                                                 : dsp_maxf(E.mon_path - st, E.mon_path_tgt);
    }
    const float mp1 = E.mon_path;
    const bool mon_off = mm0 == 0.f && mm1 == 0.f;
    float cg0, sg0, cg1, sg1;
    dsp_eqpow(mm0, &cg0, &sg0);
    dsp_eqpow(mm1, &cg1, &sg1);
    /* a: play->chain, bp: in->chain, c: chain->out, bq: in->out */
    const float a0 = (1.f - mp0) + mp0 * cg0, a1 = (1.f - mp1) + mp1 * cg1;
    const float bp0 = mp0 * sg0, bp1 = mp1 * sg1;
    const float c0 = (1.f - mp0) * cg0 + mp0, c1 = (1.f - mp1) * cg1 + mp1;
    const float bq0 = (1.f - mp0) * sg0, bq1 = (1.f - mp1) * sg1;
    if (!mon_off) {
        const float da = (a1 - a0) * blk.inv_n, db = (bp1 - bp0) * blk.inv_n;
        float a = a0, bb = bp0;
        for (int i = 0; i < n; i++) {
            a += da; bb += db;
            pL[i] = a * pL[i] + bb * aL[i];
            pR[i] = a * pR[i] + bb * aR[i];
        }
    }

    /* ---------- effects chain ---------- */
    eq_process(&E.eq, pL, pR, &blk);
    t1 = dsp_cycles(); cyc[DSP_ST_EQ] = t1 - t0; t0 = t1;
    sat_process(&E.sat, pL, pR, &blk);
    E.last_sat = E.sat.peak_in; E.acc_sat = dsp_maxf(E.acc_sat, E.sat.peak_in);
    t1 = dsp_cycles(); cyc[DSP_ST_SAT] = t1 - t0; t0 = t1;
    cho_process(&E.cho, pL, pR, &blk);
    t1 = dsp_cycles(); cyc[DSP_ST_CHORUS] = t1 - t0; t0 = t1;
    dly_process(&E.dly, pL, pR, &blk);
    t1 = dsp_cycles(); cyc[DSP_ST_DELAY] = t1 - t0; t0 = t1;
    rev_process(&E.rev, pL, pR, &blk);
    t1 = dsp_cycles(); cyc[DSP_ST_REVERB] = t1 - t0; t0 = t1;
    comp_process(&E.cmp, pL, pR, &blk);
    E.last_cgr = E.cmp.gr_max; E.acc_cgr = dsp_maxf(E.acc_cgr, E.cmp.gr_max);
    t1 = dsp_cycles(); cyc[DSP_ST_COMP] = t1 - t0; t0 = t1;

    /* ---------- OUTPUT: DRY monitor blend, spectrum, master gain, limiter ---------- */
    if (!mon_off) {
        const float dc = (c1 - c0) * blk.inv_n, db = (bq1 - bq0) * blk.inv_n;
        float c = c0, bb = bq0;
        for (int i = 0; i < n; i++) {
            c += dc; bb += db;
            pL[i] = c * pL[i] + bb * aL[i];
            pR[i] = c * pR[i] + bb * aR[i];
        }
    }
    /* listening: loudness compensation (follows the volume target) and headphone crossfeed; both exact bypass when off */
    loud_process(&E.loud, pL, pR, &blk);
    xf_process(&E.xf, pL, pR, &blk);
    {   /* spectrum tap: mono, post monitor blend, pre master volume / limiter */
        uint32_t w = RELAXED_LD(&s_spec_w);
        for (int i = 0; i < n; i++) s_spec[(w + (uint32_t)i) & (DSP_SPECTRUM_RING - 1)] = 0.5f * (pL[i] + pR[i]);
        ATOMIC_ST(&s_spec_w, w + (uint32_t)n);
    }
    {   /* master gain: volume (dB smoothed, -inf at min), mute, startup ramp */
        const float v0 = E.vol_db.cur, v1 = sm_step(&E.vol_db, blk.k);
        const dsp_ramp_t nz = fade_block(&E.vol_nz, blk.fade_step, &blk);
        const dsp_ramp_t um = fade_block(&E.unmute, blk.fade_step, &blk);
        const float s0 = E.start_g;
        if (E.start_g < 1.f) { E.start_g += E.start_step * (float)n; if (E.start_g > 1.f) E.start_g = 1.f; }
        const float s1 = E.start_g;
        const float g0 = dsp_db2lin(v0) * nz.v * um.v * s0 * s0;
        const float g1 = dsp_db2lin(v1) * (nz.v + nz.d * (float)n) * (um.v + um.d * (float)n) * s1 * s1;
        if (!(g0 == 1.f && g1 == 1.f)) {
            if (g0 == g1) dsp_vec_mulc2(pL, pR, g0, n);
            else {
                const float dg = (g1 - g0) * blk.inv_n;
                float g = g0;
                for (int i = 0; i < n; i++) { g += dg; pL[i] *= g; pR[i] *= g; }
            }
        }
    }
    lim_process(&E.lim, pL, pR, &blk);
    if (E.lim.over) { E.out_over[0]++; E.out_over[1]++; }
    E.last_lgr = E.lim.gr_max_db; E.acc_lgr = dsp_maxf(E.acc_lgr, E.lim.gr_max_db);
    float ol, orr;
    const float opk = peak2(pL, pR, n, &ol, &orr);
    t1 = dsp_cycles(); cyc[DSP_ST_OUTPUT] = t1 - t0; t0 = t1;

    /* ---------- DITHER / DAC ---------- */
    const bool transparent = eq_is_identity(&E.eq) && !sat_active(&E.sat) && !cho_active(&E.cho) &&
                             !dly_running(&E.dly) && !rev_running(&E.rev) && !E.cmp.running && mon_off &&
                             E.vol_db.cur == 0.f && E.vol_nz.v == 1.f && E.unmute.v == 1.f && E.start_g == 1.f && !E.loud.active && !E.xf.active &&
                             E.lim.gr_max_db == 0.f;
    if (smute_full) {
        memset(dac, 0, (size_t)n * 2 * sizeof(int32_t));
        ol = orr = 0.f;
    } else {
        if (!smute_off) {
            float g = sm0;
            for (int i = 0; i < n; i++) { g += smd; pL[i] *= g; pR[i] *= g; }
            peak2(pL, pR, n, &ol, &orr);
        }
        if (transparent && smute_off) quantize_convert(pL, pR, dac, n);   /* bit-perfect playback */
        else dither_convert(&E.dith_dac, pL, pR, dac, n);
    }
    E.last_out[0] = ol; E.last_out[1] = orr;
    E.acc_out[0] = dsp_maxf(E.acc_out[0], ol); E.acc_out[1] = dsp_maxf(E.acc_out[1], orr);
    t1 = dsp_cycles(); cyc[DSP_ST_DITHER] = t1 - t0;

    /* ---------- flags, viz, meters, profiling ---------- */
    if (smute_full) { if (++E.zero_blocks >= 1) ATOMIC_ST(&s_silent, 1u); }
    else { E.zero_blocks = 0; ATOMIC_ST(&s_silent, 0u); }
    if (smute_full || dsp_maxf(ol, orr) < QUIET_LIN) {
        if (E.quiet_samples < QUIET_SAMPLES) E.quiet_samples += (uint32_t)n;
    } else E.quiet_samples = 0;
    (void)opk;
    ATOMIC_ST(&s_quiet, E.quiet_samples >= QUIET_SAMPLES ? 1u : 0u);
    for (int v = 0; v < 3; v++) RELAXED_ST(&s_viz[v], fbits(E.cho.viz_phase[v]));
    RELAXED_ST(&s_viz[3], fbits((float)E.dly.d_tgt * (1000.f / (float)DSP_FS)));
    RELAXED_ST(&s_viz[4], fbits(E.rev.energy > 1e-20f ? dsp_exp2(0.5f * dsp_log2(E.rev.energy)) : 0.f));
    publish_meters();
    prof_block(cyc, n);
}

void dsp_engine_dbg_eq_coefs(int band, float out[5])
{
    const biquad_coef_t *c = &E.eq.bq[band].c;
    out[0] = c->b0; out[1] = c->b1; out[2] = c->b2; out[3] = c->a1; out[4] = c->a2;
}

uint32_t g_dsp_dbg[DSP_DBG_N];
void dsp_engine_get_dbg(uint32_t *out)
{
    for (int i = 0; i < DSP_DBG_N; i++) { out[i] = g_dsp_dbg[i]; g_dsp_dbg[i] = 0; }
}

void dsp_engine_process(const int32_t *play, const int32_t *adc, int32_t *dac, int32_t *rec, int n)
{
    while (n > 0) {
        const int k = n > DSP_BLOCK_MAX ? DSP_BLOCK_MAX : n;
        process_block(play, adc, dac, rec, k);
        g_dsp_dbg[DSP_DBG_N - 1] += (uint32_t)k;
        if (play) play += 2 * k;
        if (adc) adc += 2 * k;
        dac += 2 * k;
        if (rec) rec += 2 * k;
        n -= k;
    }
}
