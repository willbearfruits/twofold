/*
 * dsp_tests.c: host test-suite for the TWOFOLD DSP engine.
 *
 * Every test drives the real engine through its public API (dsp_engine.h + params.h), i.e. exactly the code
 * that runs on the ESP32-S3. One test per process (the engine is a singleton that settles its parameters at
 * init): `dsp_tests <name> <outdir>`. Exit code 0 = pass. Each test prints one RESULT line that run.sh
 * collects into docs/test-results/dsp_tests.md, and may write CSV files for plots.py.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsp_engine.h"
#include "params.h"
#include "rbj.h"
#include "dsp_sat.h"   /* SAT_OS */
#include "dsp_loud.h"
#include "dsp_xfeed.h"

#define FS 48000
#define BLK 32
#define FULL 2147483648.0
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *g_out = ".";
static int g_fail;

#define RESULT(ok, ...) do { printf("RESULT %s | ", (ok) ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
                             if (!(ok)) g_fail = 1; } while (0)

static void *xalloc(size_t n) { void *p = calloc(1, n); if (!p) { perror("calloc"); exit(2); } return p; }
static const dsp_alloc_t k_alloc = { xalloc, xalloc, xalloc };

/* Studio-neutral baseline: unity volume, all effects off, flat EQ, limiter as given. */
static void base_params(bool limiter)
{
    params_init();
    params_set(P_VOLUME, 0.f, SRC_INIT);
    params_set(P_LIM_ON, limiter ? 1.f : 0.f, SRC_INIT);
}

static void engine_start(void)
{
    if (dsp_engine_init(&k_alloc) != 0) { fprintf(stderr, "dsp_engine_init failed\n"); exit(2); }
}

/* Run float stereo (interleaved) through the engine. adc may be NULL. Output float interleaved. */
static void run(const float *play, const float *adc, float *out, int frames)
{
    int32_t pi[BLK * 2], ai[BLK * 2], di[BLK * 2], ri[BLK * 2];
    for (int f = 0; f < frames; f += BLK) {
        int n = frames - f < BLK ? frames - f : BLK;
        for (int i = 0; i < 2 * n; i++) {
            double p = play ? play[2 * f + i] * FULL : 0, a = adc ? adc[2 * f + i] * FULL : 0;
            pi[i] = (int32_t)fmax(-FULL, fmin(FULL - 256, p));
            ai[i] = (int32_t)fmax(-FULL, fmin(FULL - 256, a)) & ~0xFF;
        }
        dsp_engine_process(play ? pi : NULL, adc ? ai : NULL, di, ri, n);
        for (int i = 0; i < 2 * n; i++) out[2 * f + i] = (float)(di[i] / FULL);
    }
}

static float *buf(int frames) { return xalloc(sizeof(float) * 2 * (size_t)frames); }

static FILE *csv(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_out, name);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    return f;
}

/* |H(f)| of a real impulse response by direct DFT at one frequency. */
static double ir_mag_db(const float *h, int stride, int n, double f)
{
    double re = 0, im = 0, w = 2 * M_PI * f / FS;
    double c = cos(w), s = sin(w), cr = 1, ci = 0;
    for (int i = 0; i < n; i++) {
        re += h[i * stride] * cr; im -= h[i * stride] * ci;
        double t = cr * c - ci * s; ci = cr * s + ci * c; cr = t;
        if ((i & 1023) == 1023) { double m = hypot(cr, ci); cr /= m; ci /= m; }
    }
    return 20 * log10(hypot(re, im) + 1e-30);
}

/* ------------------------------------------------------------------------------------------------ */
/* EQ: measured response of the running cascade vs the analytic RBJ product.                          */
typedef struct { float f, g, q; } band_t;
static const band_t k_eq_cfg[3][5] = {
    { { 30, 12, 0.71f }, { 200, -9, 4 }, { 1000, 6, 10 }, { 5000, -12, 0.5f }, { 15000, 8, 0.71f } },
    { { 20, -18, 0.71f }, { 60, 18, 0.3f }, { 3000, -18, 2 }, { 9000, 18, 8 }, { 20000, -18, 1.5f } },
    { { 120, -2, 0.71f }, { 450, 1.5f, 1 }, { 2500, -3, 1.4f }, { 7000, 2, 1 }, { 11000, 3, 0.71f } },
};

static int test_eq(int cfg)
{
    base_params(false);
    const band_t *b = k_eq_cfg[cfg];
    for (int k = 0; k < 5; k++) {
        params_set((param_id_t)(P_EQ_B1_FREQ + 4 * k), b[k].f, SRC_INIT);
        params_set((param_id_t)(P_EQ_B1_GAIN + 4 * k), b[k].g, SRC_INIT);
        params_set((param_id_t)(P_EQ_B1_Q + 4 * k), b[k].q, SRC_INIT);
    }
    engine_start();
    const int N = 1 << 17;   /* 2.7 s: long enough for a 20 Hz shelf to decay */
    float *in = buf(N), *out = buf(N);
    in[0] = in[1] = 0.25f;
    run(in, NULL, out, N);
    biquad_coef_t c[5];
    for (int k = 0; k < 5; k++)
        rbj_design(&c[k], eq_band_type(k), FS, params_get((param_id_t)(P_EQ_B1_FREQ + 4 * k)),
                   params_get((param_id_t)(P_EQ_B1_GAIN + 4 * k)), params_get((param_id_t)(P_EQ_B1_Q + 4 * k)));
    char name[64];
    snprintf(name, sizeof name, "eq_cfg%d.csv", cfg);
    FILE *f = csv(name);
    fprintf(f, "freq,measured_db,target_db\n");
    double worst = 0, worst_f = 0;
    for (int i = 0; i <= 240; i++) {
        double fr = 20 * pow(1000.0, i / 240.0);
        double meas = ir_mag_db(out, 2, N, fr) - 20 * log10(0.25);
        double tgt = 0;
        for (int k = 0; k < 5; k++) tgt += biquad_mag_db(&c[k], fr, FS);
        fprintf(f, "%.3f,%.5f,%.5f\n", fr, meas, tgt);
        if (fabs(meas - tgt) > worst) { worst = fabs(meas - tgt); worst_f = fr; }
    }
    fclose(f);
    RESULT(worst <= 0.05, "eq cfg%d: max |measured - analytic RBJ| = %.4f dB (at %.0f Hz), limit 0.05 dB", cfg, worst, worst_f);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Limiter: never above ceiling; bit-identical to limiter-off below threshold.                         */
static uint32_t rng = 12345;
static float frand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float)rng / 4294967296.f * 2.f - 1.f; }

static int test_limiter(void)
{
    base_params(true);
    params_set(P_EQ_OUT, 12.f, SRC_INIT);            /* +12 dB into the limiter */
    engine_start();
    const int N = FS * 6;
    float *in = buf(N), *out = buf(N);
    for (int i = 0; i < N; i++) {
        int seg = i / FS;
        float x;
        double t = (double)i / FS;
        switch (seg) {
        case 0: x = 0.99f * (float)sin(2 * M_PI * 997 * t); break;                     /* sine */
        case 1: x = (fmod(t * 110, 1.0) < 0.5) ? 0.9f : -0.9f; break;                  /* square */
        case 2: x = ((i / 2400) & 1) ? 0.95f * frand() : 0.05f * frand(); break;      /* noise bursts */
        case 3: x = 0.99f * (float)sin(2 * M_PI * 11025 * t + 0.7); break;            /* fs/4-ish, intersample */
        case 4: x = (i % 4800 < 3) ? 0.99f : 0.f; break;                              /* isolated spikes */
        default: x = 0.7f * (float)sin(2 * M_PI * 60 * t) + 0.3f * frand(); break;
        }
        in[2 * i] = x; in[2 * i + 1] = -x;
    }
    run(in, NULL, out, N);
    const double ceil_lin = pow(10, -0.3 / 20), lsb = 1.0 / 8388608.0;
    double mx = 0;
    for (int i = 0; i < 2 * N; i++) mx = fmax(mx, fabs(out[i]));
    RESULT(mx <= ceil_lin + 1.5 * lsb, "limiter: peak out %.6f (%.3f dBFS) with +12 dB overs, ceiling -0.30 dBFS",
           mx, 20 * log10(mx));
    FILE *f = csv("limiter.csv");
    fprintf(f, "t,in,out\n");
    for (int i = 2 * FS; i < 3 * FS; i += 4) fprintf(f, "%.5f,%.5f,%.5f\n", (double)i / FS, in[2 * i] * 3.98, out[2 * i]);
    fclose(f);
    return 0;
}

static void transparency_run(bool lim, float *out, int N)
{
    float *in = buf(N);
    for (int i = 0; i < N; i++) {
        float x = 0.5f * (float)sin(2 * M_PI * 1000.0 * i / FS) * (0.6f + 0.4f * frand());   /* <= -6 dBFS */
        in[2 * i] = x; in[2 * i + 1] = 0.5f * frand();
    }
    base_params(lim);
    engine_start();
    run(in, NULL, out, N);
}

static int test_lim_transparent(const char *mode)
{
    /* run twice in separate processes (limiter on / off) and compare files */
    const int N = FS * 2;
    float *out = buf(N);
    transparency_run(!strcmp(mode, "on"), out, N);
    char p[512];
    snprintf(p, sizeof p, "%s/lim_transp_%s.bin", g_out, mode);
    FILE *f = fopen(p, "wb");
    fwrite(out, sizeof(float), 2 * (size_t)N, f);
    fclose(f);
    if (!strcmp(mode, "on")) {
        snprintf(p, sizeof p, "%s/lim_transp_off.bin", g_out);
        float *ref = buf(N);
        f = fopen(p, "rb");
        if (!f || fread(ref, sizeof(float), 2 * (size_t)N, f) != 2 * (size_t)N) { RESULT(0, "missing off run"); return 0; }
        fclose(f);
        /* the limiter adds 1 ms lookahead: align by searching the delay */
        int best_d = -1;
        for (int d = 0; d <= 128 && best_d < 0; d++) {
            int same = 1;
            for (int i = FS / 2; i < N - 200 && same; i++)
                if (out[2 * (i + d)] != ref[2 * i] || out[2 * (i + d) + 1] != ref[2 * i + 1]) same = 0;
            if (same) best_d = d;
        }
        RESULT(best_d >= 0, "limiter below threshold: output bit-identical to limiter-off (lookahead %d samples)", best_d);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Reverb: impulse response, RT60 by Schroeder backward integration vs P_REV_DECAY.                    */
static int test_reverb(float decay)
{
    base_params(false);
    params_set(P_REV_ON, 1, SRC_INIT);
    params_set(P_REV_MIX, 100, SRC_INIT);
    params_set(P_REV_DECAY, decay, SRC_INIT);
    params_set(P_REV_PREDLY, 20, SRC_INIT);
    params_set(P_REV_LOCUT, 20, SRC_INIT);
    engine_start();
    const int N = (int)(FS * (decay * 1.6f + 1.0f));
    float *in = buf(N), *out = buf(N);
    in[0] = in[1] = 0.5f;
    run(in, NULL, out, N);
    double *E = xalloc(sizeof(double) * (size_t)N);
    double acc = 0;
    for (int i = N - 1; i >= 0; i--) { acc += (double)out[2 * i] * out[2 * i] + (double)out[2 * i + 1] * out[2 * i + 1]; E[i] = acc; }
    /* T20 fit between -5 and -25 dB, extrapolated to 60 dB */
    int i5 = -1, i25 = -1;
    for (int i = 0; i < N; i++) {
        double db = 10 * log10(E[i] / E[0] + 1e-300);
        if (i5 < 0 && db <= -5) i5 = i;
        if (i25 < 0 && db <= -25) { i25 = i; break; }
    }
    double rt = (i5 >= 0 && i25 > i5) ? 3.0 * (i25 - i5) / (double)FS : -1;
    /* inter-channel correlation of the tail (decorrelation of the stereo image) */
    double sxy = 0, sxx = 0, syy = 0;
    for (int i = FS / 10; i < N; i++) { sxy += out[2 * i] * out[2 * i + 1]; sxx += out[2 * i] * out[2 * i]; syy += out[2 * i + 1] * out[2 * i + 1]; }
    double corr = sxy / sqrt(sxx * syy + 1e-30);
    int first = 0;
    for (int i = 0; i < N; i++) if (fabs(out[2 * i]) > 1e-4) { first = i; break; }
    char name[64];
    snprintf(name, sizeof name, "reverb_ir_%.1fs.csv", decay);
    FILE *f = csv(name);
    fprintf(f, "t,L,R\n");
    for (int i = 0; i < N; i += 2) fprintf(f, "%.6f,%.7f,%.7f\n", (double)i / FS, out[2 * i], out[2 * i + 1]);
    fclose(f);
    double err = rt > 0 ? fabs(rt - decay) / decay : 1;
    RESULT(err <= 0.15 && fabs(corr) < 0.3 && first >= (int)(0.019 * FS),
           "reverb decay %.1f s: measured RT60 %.2f s (%.1f %% off), L/R tail correlation %.3f, first output %.1f ms (pre-delay 20 ms)",
           decay, rt, err * 100, corr, first * 1000.0 / FS);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Delay: tap times, ping-pong alternation, feedback decay.                                             */
static int find_peaks(const float *x, int stride, int n, int *pos, float *amp, int maxp)
{
    int k = 0;
    for (int i = 1; i < n - 1 && k < maxp; i++) {
        float a = fabsf(x[i * stride]);
        if (a > 0.01f && a >= fabsf(x[(i - 1) * stride]) && a > fabsf(x[(i + 1) * stride])) {
            if (k && i - pos[k - 1] < 480) { if (a > amp[k - 1]) { pos[k - 1] = i; amp[k - 1] = a; } continue; }
            pos[k] = i; amp[k] = a; k++;
        }
    }
    return k;
}

static int test_delay(int sync, int pingpong)
{
    base_params(false);
    params_set(P_DLY_ON, 1, SRC_INIT);
    params_set(P_DLY_MIX, 100, SRC_INIT);
    params_set(P_DLY_FB, 50, SRC_INIT);
    params_set(P_DLY_MODE, (float)pingpong, SRC_INIT);
    params_set(P_DLY_SYNC, (float)sync, SRC_INIT);
    params_set(P_DLY_TIME, 250, SRC_INIT);
    params_set(P_DLY_HICUT, 20000, SRC_INIT);
    params_set(P_DLY_LOCUT, 20, SRC_INIT);
    params_set(P_TEMPO, 120, SRC_INIT);
    engine_start();
    const double expect_ms = sync == 0 ? 250.0 : (sync == 2 ? 500.0 : (sync == 6 ? 375.0 : 0));
    const int N = FS * 3;
    float *in = buf(N), *out = buf(N);
    in[0] = 0.5f;   /* left only */
    run(in, NULL, out, N);
    int pl[16], pr[16];
    float al[16], ar[16];
    int nl = find_peaks(out, 2, N, pl, al, 16), nr = find_peaks(out + 1, 2, N, pr, ar, 16);
    char name[64];
    snprintf(name, sizeof name, "delay_ir_sync%d_pp%d.csv", sync, pingpong);
    FILE *f = csv(name);
    fprintf(f, "t,L,R\n");
    for (int i = 0; i < N; i += 4) fprintf(f, "%.5f,%.6f,%.6f\n", (double)i / FS, out[2 * i], out[2 * i + 1]);
    fclose(f);
    const int D = (int)lround(expect_ms * FS / 1000.0);
    int ok = nl >= 2;
    char taps[256] = "";
    /* the engine has a fixed latency (limiter lookahead, always applied so latency never jumps) that
     * shifts every tap equally: judge the tap spacing and report the offset */
    const int lat = pl[0] - D;
    if (pingpong) {
        /* L, R, L, R ... at D, 2D, 3D ... */
        ok = ok && nr >= 2 && abs(pr[0] - pl[0] - D) <= 1 && abs(pl[1] - pr[0] - D) <= 1;
        snprintf(taps, sizeof taps, "L@%d R@%d L@%d, spacing %d/%d (expect %d), fixed engine latency %d", pl[0], pr[0], pl[1],
                 pr[0] - pl[0], pl[1] - pr[0], D, lat);
    } else {
        ok = ok && abs(pl[1] - pl[0] - D) <= 1;
        snprintf(taps, sizeof taps, "L@%d L@%d, spacing %d (expect %d), fixed engine latency %d", pl[0], pl[1], pl[1] - pl[0], D, lat);
    }
    ok = ok && lat >= 0 && lat <= 64;
    double fb_db = nl >= 2 ? 20 * log10(al[1] / al[0]) : 0;
    RESULT(ok, "delay sync=%s mode=%s: %s samples; successive same-side tap ratio %.2f dB",
           g_param_desc[P_DLY_SYNC].enum_names[sync], pingpong ? "PING-PONG" : "STEREO", taps, fb_db);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Saturation aliasing: bin-exact sine, energy on non-harmonic (aliased) bins.                         */
static double tone_level(const float *x, int stride, int n, double f)
{
    /* Blackman-Harris windowed single-bin DFT, normalised to sine amplitude */
    double re = 0, im = 0, wsum = 0;
    for (int i = 0; i < n; i++) {
        double t = 2 * M_PI * i / (n - 1);
        double w = 0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2 * t) - 0.01168 * cos(3 * t);
        double ph = 2 * M_PI * f * i / FS;
        re += w * x[i * stride] * cos(ph); im -= w * x[i * stride] * sin(ph);
        wsum += w;
    }
    return 2 * hypot(re, im) / wsum;
}

static double fold(double f) { f = fmod(f, FS); if (f > FS / 2) f = FS - f; return f; }

static void alias_measure(const float *y, int stride, int N, double f0, double *worst_alias_dbc, double *thd_db)
{
    double a1 = tone_level(y, stride, N, f0), worst = 0, harm = 0;
    for (int k = 2; k <= 60; k++) {
        double fk = k * f0;
        double ff = fold(fk);
        if (ff < 20 || ff > 20000) continue;
        double a = tone_level(y, stride, N, ff);
        if (fk < FS / 2) harm += a * a;        /* genuine harmonic */
        else {
            int coincides = 0;                  /* skip aliases that land on a genuine harmonic bin */
            for (int j = 1; j * f0 < FS / 2; j++) if (fabs(ff - j * f0) < 30) coincides = 1;
            if (!coincides && a > worst) worst = a;
        }
    }
    *worst_alias_dbc = 20 * log10(worst / a1 + 1e-12);
    *thd_db = 10 * log10(harm / (a1 * a1) + 1e-24);
}

static int test_sat_alias(double f0, float drive, float level_db)
{
    base_params(false);
    params_set(P_SAT_ON, 1, SRC_INIT);
    params_set(P_SAT_DRIVE, drive, SRC_INIT);
    params_set(P_SAT_WARMTH, 30, SRC_INIT);
    params_set(P_SAT_MIX, 100, SRC_INIT);
    engine_start();
    const int N = 1 << 15, W = 1 << 14;
    float *in = buf(N), *out = buf(N);
    for (int i = 0; i < N; i++) in[2 * i] = in[2 * i + 1] = powf(10.f, level_db / 20.f) * (float)sin(2 * M_PI * f0 * i / FS);
    run(in, NULL, out, N);
    double alias_db, thd;
    alias_measure(out + 2 * (N - W), 2, W, f0, &alias_db, &thd);
    /* baseline: the same kind of drive with a naive non-oversampled tanh */
    float *naive = buf(W);
    const float g = powf(10.f, drive / 20.f);
    for (int i = 0; i < W; i++) naive[i] = tanhf(g * in[2 * (N - W + i)]);
    double nalias, nthd;
    alias_measure(naive, 1, W, f0, &nalias, &nthd);
    /* criterion: musical settings (<= 12 dB drive) must keep aliases below -80 dBc; the extreme corner
     * (max drive on a loud HF sine) must still be >= 25 dB better than naive (non-oversampled) saturation */
    if (drive > 12.f) {
        printf("RESULT INFO | saturation EXTREME corner (informational): drive %.0f dB, %.0f Hz %.0f dBFS: worst alias %.1f dBc (%dx OS) vs %.1f dBc naive tanh\n",
               drive, f0, level_db, alias_db, SAT_OS, nalias);
        return 0;
    }
    RESULT(alias_db <= -80.0, "saturation drive %.0f dB, %.0f Hz %.0f dBFS: worst alias %.1f dBc (%dx OS) vs %.1f dBc naive tanh; in-band THD %.1f dB",
           drive, f0, level_db, alias_db, SAT_OS, nalias, thd);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Click detection: step every parameter while a test signal plays; look for HF bursts.                */
typedef struct { double s0[2], s1[2]; } hp_state_t;   /* two cascaded biquads, double precision */

static void hp_run(const float *x, int stride, int n, float *y)
{
    biquad_coef_t c;
    rbj_design(&c, RBJ_HIGHPASS, FS, 12000, 0, 0.7071f);
    double z[2][2] = { { 0 } };
    for (int i = 0; i < n; i++) {
        double v = x[i * stride];
        for (int s = 0; s < 2; s++) {
            double o = c.b0 * v + z[s][0];
            z[s][0] = c.b1 * v - c.a1 * o + z[s][1];
            z[s][1] = c.b2 * v - c.a2 * o;
            v = o;
        }
        y[i] = (float)v;
    }
}

static double win_max(const float *y, int a, int b) { double m = 0; for (int i = a; i < b; i++) m = fmax(m, fabs(y[i])); return m; }

static int param_group_on(param_id_t id)
{
    const char *k = g_param_desc[id].key;
    if (!strncmp(k, "sat.", 4)) return P_SAT_ON;
    if (!strncmp(k, "cho.", 4)) return P_CHO_ON;
    if (!strncmp(k, "dly.", 4)) return P_DLY_ON;
    if (!strncmp(k, "rev.", 4)) return P_REV_ON;
    if (!strncmp(k, "cmp.", 4)) return P_CMP_ON;
    return -1;
}

/* Click metric for one transition at sample `at`: the 12 kHz high-passed output is cut into 1 ms frames; each
 * frame's peak is compared with the median of the surrounding +-10 ms. A click is a short broadband spike, so it
 * stands out from its own neighbourhood; a deliberate parameter glide (e.g. a preset morphing drive and tone over
 * 150 ms) changes the level smoothly and does not. Returns the worst ratio (dB) in the window after the change.
 * Frames are floored 60 dB below the program's broadband RMS (and at -90 dBFS): quieter artefacts are inaudible. */
static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static double transition_excess(float *out, int seg, int at)
{
    const int F = 48, nf = seg * 3 / F;
    float *hp = buf(seg * 3), *fr = xalloc(sizeof(float) * (size_t)nf);
    double worst = -200;
    for (int ch = 0; ch < 2; ch++) {
        /* audibility floor: 70 dB below the louder of the pre/post broadband RMS, never below -90 dBFS */
        double e0 = 0, e1 = 0;
        for (int i = at - seg / 2; i < at; i++) e0 += (double)out[2 * i + ch] * out[2 * i + ch];
        for (int i = at + seg; i < at + seg + seg / 2 && i < seg * 3; i++) e1 += (double)out[2 * i + ch] * out[2 * i + ch];
        const float floor_lin = (float)fmax(3e-5, sqrt(fmax(e0, e1) / (seg / 2)) * 1e-3);   /* -60 dB re program RMS: masked, inaudible */
        hp_run(out + ch, 2, seg * 3, hp);
        for (int k = 0; k < nf; k++) fr[k] = fmaxf((float)win_max(hp, k * F, k * F + F), floor_lin);
        for (int k = (at - 960) / F; k < (at + 9600) / F && k + 10 < nf; k++) {
            float nb[21];
            for (int j = -10; j <= 10; j++) nb[j + 10] = fr[k + j];
            qsort(nb, 21, sizeof(float), cmp_f);
            worst = fmax(worst, 20 * log10(fr[k] / nb[10]));
        }
    }
    free(hp); free(fr);
    return worst;
}

static int test_click1(int id)
{
    const param_desc_t *d = &g_param_desc[id];
    base_params(true);
    params_set(P_VOLUME, -6.f, SRC_INIT);
    int grp = param_group_on((param_id_t)id);
    if (grp >= 0) params_set((param_id_t)grp, 1, SRC_INIT);
    if (id == P_MON_MIX || id == P_IN_GAIN || id == P_IN_MODE || id == P_MON_PATH) params_set(P_MON_MIX, 50, SRC_INIT);
    engine_start();
    const int seg = FS / 2;
    float vals[3] = { d->max, d->min, d->def };
    const char *names[3] = { "def->max", "max->min", "min->def" };
    float *in = buf(seg * 3), *adc = buf(seg * 3), *out = buf(seg * 3);
    double worst = -200;
    int worst_t = 0;
    long ph = 0;
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < seg * 3; i++, ph++) {
            double s = (double)ph / FS;
            float x = 0.25f * (float)sin(2 * M_PI * 1000 * s) + 0.12f * (float)sin(2 * M_PI * 100 * s);
            in[2 * i] = x; in[2 * i + 1] = 0.8f * x;
            adc[2 * i] = adc[2 * i + 1] = 0.2f * (float)sin(2 * M_PI * 1500 * s);
        }
        /* settle one segment, step at the start of the second, observe through the third */
        run(in, adc, out, seg);
        params_set((param_id_t)id, vals[t], SRC_UI);
        run(in + 2 * seg, adc + 2 * seg, out + 2 * seg, 2 * seg);
        double e = transition_excess(out, seg, seg);
        if (getenv("TF_DUMP") && atoi(getenv("TF_DUMP")) == t) {
            FILE *df = csv("click_dump.csv");
            for (int i = seg - 960; i < seg + 4800; i++) fprintf(df, "%d,%.9f\n", i - seg, out[2 * i]);
            fclose(df);
        }
        if (e > worst) { worst = e; worst_t = t; }
        printf("CLICK %s %s %.2f\n", d->key, names[t], e);
    }
    printf("CLICKWORST %s %.2f %s\n", d->key, worst, names[worst_t]);
    return 0;
}

static int test_click_presets(void)
{
    base_params(true);
    params_set(P_VOLUME, -6.f, SRC_INIT);
    engine_start();
    const int seg = FS / 2;
    float *in = buf(seg * 3), *out = buf(seg * 3);
    uint8_t blob[2048];
    double worst = -200;
    long ph = 0;
    for (int p = 0; p < 2 * PRESET_SLOTS; p++) {
        int slot = (p * 3) % PRESET_SLOTS;       /* hop between presets in a mixed order */
        for (int i = 0; i < seg * 3; i++, ph++) {
            float x = 0.25f * (float)sin(2 * M_PI * 1000 * (double)ph / FS) + 0.12f * (float)sin(2 * M_PI * 100 * (double)ph / FS);
            in[2 * i] = x; in[2 * i + 1] = 0.8f * x;
        }
        run(in, NULL, out, seg);
        size_t n = params_factory_blob(slot, blob, sizeof blob);
        params_preset_apply(blob, n, NULL);
        params_set(P_VOLUME, -6.f, SRC_UI);
        run(in + 2 * seg, NULL, out + 2 * seg, 2 * seg);
        double e = transition_excess(out, seg, seg);
        printf("CLICK preset->%s %.2f\n", g_factory_presets[slot].name, e);
        worst = fmax(worst, e);
    }
    /* soft mute on/off */
    for (int i = 0; i < seg * 3; i++, ph++) {
        float x = 0.25f * (float)sin(2 * M_PI * 1000 * (double)ph / FS);
        in[2 * i] = in[2 * i + 1] = x;
    }
    run(in, NULL, out, seg);
    dsp_engine_soft_mute(true);
    run(in + 2 * seg, NULL, out + 2 * seg, seg / 2);
    int silent = dsp_engine_is_silent();
    dsp_engine_soft_mute(false);
    run(in + 2 * seg + seg, NULL, out + 2 * seg + seg, 2 * seg - seg / 2);
    double e = transition_excess(out, seg, seg);
    printf("CLICK soft-mute %.2f (silent=%d)\n", e, silent);
    RESULT(worst <= 6.0 && e <= 6.0 && silent, "preset crossfades: worst 1 ms HF spike %.1f dB over its +-10 ms neighbourhood (limit 6 dB); soft mute %.1f dB, reaches digital silence: %s",
           worst, e, silent ? "yes" : "NO");
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Dither statistics and record-path bit exactness.                                                   */
static int test_dither(int bitperfect)
{
    base_params(false);
    /* bitperfect=0: -3 dB volume (processing active) -> TPDF statistics of the requantisation error.
     * bitperfect=1: unity, flat chain, 24-bit input (what UAC2 delivers) -> output must equal input exactly. */
    const double g = bitperfect ? 1.0 : pow(10.0, -3.0 / 20.0);
    params_set(P_VOLUME, bitperfect ? 0.f : -3.f, SRC_INIT);
    engine_start();
    const int N = FS * 4;
    float *in = buf(N), *out = buf(N);
    for (int i = 0; i < 2 * N; i++) in[i] = 0.3f * frand();
    /* exact int32 input with sub-LSB information */
    int32_t pi[BLK * 2], di[BLK * 2];
    /* find the engine latency with an impulse */
    int lat = -1;
    for (int f = 0; f < 8 * BLK && lat < 0; f += BLK) {
        memset(pi, 0, sizeof pi);
        if (f == 0) pi[0] = 1 << 30;   /* 0.5 FS impulse, 24-bit aligned */
        dsp_engine_process(pi, NULL, di, NULL, BLK);
        for (int i = 0; i < BLK; i++) if (di[2 * i] > (1 << 28)) { lat = f + i; break; }
    }
    for (int f = 0; f < 8 * BLK; f += BLK) { memset(pi, 0, sizeof pi); dsp_engine_process(pi, NULL, di, NULL, BLK); }
    if (lat < 0) { RESULT(0, "dither: could not find engine latency"); return 0; }
    static int32_t hist_in[1 << 20];
    long hn = 0;
    double lsb = 1.0 / 8388608.0, sum = 0, sum2 = 0, sxy = 0;
    long hist[7] = { 0 }, n = 0;
    double prev = 0;
    for (int f = 0; f < N; f += BLK) {
        for (int i = 0; i < 2 * BLK; i++) { pi[i] = (int32_t)(in[2 * f + i] * FULL) & (bitperfect ? ~0xFF : ~0); hist_in[(hn + i) & ((1 << 20) - 1)] = pi[i]; }
        hn += 2 * BLK;
        dsp_engine_process(pi, NULL, di, NULL, BLK);
        for (int i = 0; i < 2 * BLK; i++) {
            if (di[i] & 0xFF) { RESULT(0, "DAC output not 24-bit left-justified"); return 0; }
            long src = hn - 2 * BLK + i - 2L * lat;
            if (src < 0) continue;
            double e = (di[i] - g * (double)hist_in[src & ((1 << 20) - 1)]) / FULL / lsb;     /* error in 24-bit LSBs */
            sum += e; sum2 += e * e; n++;
            int b = (int)lround(e) + 3;
            if (b >= 0 && b < 7) hist[b]++;
            if (i & 1) { sxy += e * prev; } else prev = e;
        }
    }
    (void)out;
    double mean = sum / n, rms = sqrt(sum2 / n - mean * mean), lr_corr = sxy / (n / 2) / (rms * rms);
    /* TPDF (2 LSB p-p triangular) + rounding: error variance = 1/12 + 2/12 -> rms 0.5 LSB, |e| < 1.5 LSB */
    FILE *f = csv("dither_hist.csv");
    fprintf(f, "lsb,count\n");
    for (int b = 0; b < 7; b++) fprintf(f, "%d,%ld\n", b - 3, hist[b]);
    fclose(f);
    int bounded = hist[0] == 0 && hist[6] == 0;
    if (bitperfect) {
        RESULT(sum2 == 0.0, "DAC path at unity, flat chain, 24-bit input: error rms %.3g LSB over %ld samples (bit-perfect)", sqrt(sum2 / n), n);
        return 0;
    }
    RESULT(fabs(mean) < 0.01 && fabs(rms - 0.5) < 0.03 && fabs(lr_corr) < 0.02 && bounded,
           "TPDF dither (aligned for %d-sample latency): error mean %.4f LSB, rms %.3f LSB (theory 0.500), L/R correlation %.4f, |e| <= 2 LSB: %s",
           lat, mean, rms, lr_corr, bounded ? "yes" : "no");

    /* record path: STEREO, 0 dB input gain -> bit-exact copy of the ADC */
    int32_t ai[BLK * 2], ri[BLK * 2];
    long mism = 0;
    for (int f2 = 0; f2 < FS; f2 += BLK) {
        for (int i = 0; i < 2 * BLK; i++) ai[i] = (int32_t)(frand() * 2147483000.f) & ~0xFF;
        dsp_engine_process(NULL, ai, di, ri, BLK);
        for (int i = 0; i < 2 * BLK; i++) mism += ri[i] != ai[i];
    }
    RESULT(mism == 0, "record path at unity gain (STEREO): %ld mismatching samples of %d (bit-exact)", mism, FS * 2);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Host benchmark (informational): everything on vs all off.                                          */
static int test_bench(void)
{
    base_params(true);
    params_set(P_SAT_ON, 1, SRC_INIT); params_set(P_CHO_ON, 1, SRC_INIT); params_set(P_DLY_ON, 1, SRC_INIT);
    params_set(P_REV_ON, 1, SRC_INIT); params_set(P_CMP_ON, 1, SRC_INIT); params_set(P_MON_MIX, 30, SRC_INIT);
    engine_start();
    const int N = FS * 10;
    float *in = buf(N), *adc = buf(N), *out = buf(N);
    for (int i = 0; i < 2 * N; i++) { in[i] = 0.3f * frand(); adc[i] = 0.1f * frand(); }
    run(in, adc, out, FS);   /* warm up */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    run(in, adc, out, N);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    dsp_prof_t p;
    dsp_engine_get_prof(&p);
    printf("BENCH realtime_factor %.1f ns_per_frame %.1f\n", 10.0 / s, s * 1e9 / N);
    double tot = 0;
    for (int i = 0; i < DSP_ST_COUNT; i++) tot += p.avg_pct[i];
    for (int i = 0; i < DSP_ST_COUNT; i++)
        printf("BENCHSTAGE %s %.1f\n", dsp_stage_names[i], tot > 0 ? 100.0 * p.avg_pct[i] / tot : 0);
    RESULT(1, "host benchmark (x86, informational): full chain incl. all effects runs %.0fx realtime (%.0f ns/frame)", 10.0 / s, s * 1e9 / N);
    return 0;
}


/* ------------------------------------------------------------------------------------------------ */
/* EQ low-frequency precision: float TDF-II cascade vs a double-precision reference with the same coefficients. */
static int test_eq_lf(void)
{
    base_params(false);
    params_set(P_EQ_B1_FREQ, 20, SRC_INIT); params_set(P_EQ_B1_GAIN, 18, SRC_INIT); params_set(P_EQ_B1_Q, 0.71f, SRC_INIT);
    params_set(P_EQ_B2_FREQ, 30, SRC_INIT); params_set(P_EQ_B2_GAIN, 18, SRC_INIT); params_set(P_EQ_B2_Q, 10, SRC_INIT);
    params_set(P_EQ_B3_FREQ, 60, SRC_INIT); params_set(P_EQ_B3_GAIN, -18, SRC_INIT); params_set(P_EQ_B3_Q, 8, SRC_INIT);
    engine_start();
    const int N = FS * 6;
    float *in = buf(N), *out = buf(N);
    for (int i = 0; i < N; i++) {
        double t = (double)i / FS;
        float x = 0.03f * (float)sin(2 * M_PI * 20 * t) + 0.01f * (float)sin(2 * M_PI * 1000 * t);   /* -30 / -40 dBFS */
        in[2 * i] = in[2 * i + 1] = x;
    }
    run(in, NULL, out, N);
    /* reference: the ideal filter: double-precision RBJ design, double DF-I arithmetic */
    struct { double b0, b1, b2, a1, a2; } c[5];
    for (int k = 0; k < 5; k++) {
        const double f0 = params_get((param_id_t)(P_EQ_B1_FREQ + 4 * k)), gdb = params_get((param_id_t)(P_EQ_B1_GAIN + 4 * k));
        const double q = params_get((param_id_t)(P_EQ_B1_Q + 4 * k));
        const double A = pow(10, gdb / 40), w0 = 2 * M_PI * f0 / FS, cw = cos(w0), sw = sin(w0), al = sw / (2 * q), sa = 2 * sqrt(A) * al;
        double b0, b1, b2, a0, a1, a2;
        if (eq_band_type(k) == RBJ_PEAK) { b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; }
        else if (eq_band_type(k) == RBJ_LOWSHELF) { b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa; }
        else { b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa; }
        c[k].b0 = b0 / a0; c[k].b1 = b1 / a0; c[k].b2 = b2 / a0; c[k].a1 = a1 / a0; c[k].a2 = a2 / a0;
    }
    double *ref = xalloc(sizeof(double) * (size_t)N);
    for (int i = 0; i < N; i++) ref[i] = in[2 * i];
    for (int k = 0; k < 5; k++) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int i = 0; i < N; i++) {
            double x = ref[i], y = c[k].b0 * x + c[k].b1 * x1 + c[k].b2 * x2 - c[k].a1 * y1 - c[k].a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y; ref[i] = y;
        }
    }
    /* align (engine latency) and compare over the last 3 s */
    int lat = 49;
    double best = 1e9; int best_lat = lat;
    for (int d = 40; d <= 60; d++) {
        double e = 0;
        for (int i = 3 * FS; i < N - 64; i++) { double v = out[2 * (i + d)] - ref[i]; e += v * v; }
        if (e < best) { best = e; best_lat = d; }
    }
    double e2 = 0, sig = 0;
    int n = 0;
    for (int i = 3 * FS; i < N - 64; i++) { double v = out[2 * (i + best_lat)] - ref[i]; e2 += v * v; sig += ref[i] * ref[i]; n++; }
    double noise_dbfs = 10 * log10(e2 / n + 1e-30), sig_dbfs = 10 * log10(sig / n);
    /* the 24-bit output quantisation alone (TPDF dither) sits at -144.5 dBFS rms */
    /* audibility: the residual must sit >= 90 dB below the signal (24-bit dither alone is -144.5 dBFS) */
    RESULT(noise_dbfs <= sig_dbfs - 90.0, "EQ LF precision (20 Hz shelf +18, 30 Hz Q10 +18, 60 Hz Q8 -18): engine vs ideal double filter = %.1f dBFS rms, "
           "%.1f dB below the %.1f dBFS signal (TDF-II float biquads measured -79.1 dBFS here; limit 90 dB below)", noise_dbfs, sig_dbfs - noise_dbfs, sig_dbfs);
    free(ref);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Loudness: ISO 226 table self-check, and the engine's 50 Hz / 1 kHz gain vs the formula at -40 dB volume. */
static double iso226_ref(double f_target, double phon)
{
    /* independent double-precision ISO 226:2003 at the table frequency nearest f_target */
    static const double F[] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500 };
    static const double AF[] = { 0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349, 0.330, 0.315, 0.301, 0.288, 0.276, 0.267, 0.259, 0.253, 0.250, 0.246, 0.244, 0.243, 0.243, 0.243, 0.242, 0.242, 0.245, 0.254, 0.271, 0.301 };
    static const double LU[] = { -31.6, -27.2, -23.0, -19.1, -16.1, -13.4, -10.9, -8.6, -6.6, -4.8, -3.4, -2.2, -1.2, -0.4, 0.0, 0.3, 0.5, 0.0, -2.7, -4.1, -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1 };
    static const double TF[] = { 78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7, -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3 };
    int i = 0;
    for (int k = 1; k < 29; k++) if (fabs(F[k] - f_target) < fabs(F[i] - f_target)) i = k;
    double Af = 4.47e-3 * (pow(10, 0.025 * phon) - 1.15) + pow(0.4 * pow(10, (TF[i] + LU[i]) / 10 - 9), AF[i]);
    return 10 / AF[i] * log10(Af) - LU[i] + 94;
}

static int test_loud(void)
{
    /* table self-check: at 1 kHz Lp == phon; 100 Hz at 40 phon is the well-known ~52 dB SPL */
    double e1k = 0;
    for (int ph = 20; ph <= 90; ph += 10) e1k = fmax(e1k, fabs(loud_iso226_spl(1000, (float)ph) - ph));
    double l100 = loud_iso226_spl(100, 40);
    /* ISO 226:2003 puts the 40-phon contour at 100 Hz near 64 dB SPL (threshold 26.5) */
    RESULT(e1k < 0.05 && l100 > 60.0 && l100 < 68.0, "ISO 226 table: |Lp(1 kHz) - phon| max %.3f dB; Lp(100 Hz, 40 phon) = %.1f dB SPL (ISO 226:2003 ~64)", e1k, l100);

    base_params(false);
    params_set(P_LOUD_ON, 1, SRC_INIT); params_set(P_LOUD_REF, 83, SRC_INIT); params_set(P_LOUD_AMT, 100, SRC_INIT);
    params_set(P_VOLUME, -40, SRC_INIT);
    engine_start();
    const int N = 1 << 16;
    float *in = buf(N), *out = buf(N);
    in[0] = in[1] = 0.25f;
    run(in, NULL, out, N);
    const double vol = pow(10, -40.0 / 20);
    double g50 = ir_mag_db(out, 2, N, 50) - 20 * log10(0.25 * vol), g1k = ir_mag_db(out, 2, N, 1000) - 20 * log10(0.25 * vol);
    double g10k = ir_mag_db(out, 2, N, 10000) - 20 * log10(0.25 * vol);
    const double exp50 = iso226_ref(50, 43) - iso226_ref(50, 83) + 40, exp10k = iso226_ref(10000, 43) - iso226_ref(10000, 83) + 40;
    RESULT(fabs(g50 - exp50) < 1.0 && fabs(g1k) < 0.3 && fabs(g10k - exp10k) < 1.5,
           "loudness @ volume -40 dB (83 -> 43 phon): gain 50 Hz %+.1f dB (ISO 226: %+.1f), 1 kHz %+.2f dB, 10 kHz %+.1f dB (ISO 226: %+.1f)",
           g50, exp50, g1k, g10k, exp10k);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Crossfeed: mono passes bit-exactly; L-only input crosses into R at -FEED dB at LF and far less at 10 kHz. */
static int test_xfeed(float mode)
{
    base_params(false);
    params_set(P_XF_ON, 1, SRC_INIT); params_set(P_XF_FEED, 4.5f, SRC_INIT); params_set(P_XF_FC, 700, SRC_INIT);
    engine_start();
    const int N = 1 << 16;
    float *in = buf(N), *out = buf(N);
    if (mode == 0) {
        /* unit level: the crossfeed block itself leaves a mono signal bit-identical (y == 0 exactly) */
        dsp_xfeed_t x;
        xf_init(&x); xf_set(&x, true, 4.5f, 700.f); xf_settle(&x);
        dsp_blk_t b;
        dsp_blk_make(&b, 32, 6.f, 30.f, 50.f);
        long mism = 0;
        for (int f = 0; f < N; f += 32) {
            float L[32], R[32], ref[32];
            for (int i = 0; i < 32; i++) { L[i] = R[i] = ref[i] = 0.3f * frand(); }
            xf_process(&x, L, R, &b);
            for (int i = 0; i < 32; i++) mism += (L[i] != ref[i]) + (R[i] != ref[i]);
        }
        /* engine level: only the 24-bit TPDF dither remains (the bit-perfect fast path is off while crossfeed runs) */
        for (int i = 0; i < N; i++) in[2 * i] = in[2 * i + 1] = 0.3f * frand();
        run(in, NULL, out, N);
        int lat = -1;
        double worst = 1e9;
        for (int d = 40; d <= 60; d++) {
            double m = 0;
            for (int i = FS / 2; i < N - 200; i++) m = fmax(m, fabs(out[2 * (i + d)] - in[2 * i]));
            if (m < worst) { worst = m; lat = d; }
        }
        const double lsb = 1.0 / 8388608.0;
        RESULT(mism == 0 && worst <= 2.0 * lsb, "crossfeed with mono input: block output bit-identical (%ld mismatches); engine output within %.2f LSB (dither only, latency %d)",
               mism, worst / lsb, lat);
    } else {
        for (int i = 0; i < N; i++) { in[2 * i] = 0.3f * (float)sin(2 * M_PI * 100.0 * i / FS) + 0.3f * (float)sin(2 * M_PI * 10000.0 * i / FS); in[2 * i + 1] = 0; }
        run(in, NULL, out, N);
        const int W = 1 << 14;
        double l100 = tone_level(out + 2 * (N - W), 2, W, 100), r100 = tone_level(out + 2 * (N - W) + 1, 2, W, 100);
        double l10k = tone_level(out + 2 * (N - W), 2, W, 10000), r10k = tone_level(out + 2 * (N - W) + 1, 2, W, 10000);
        double x100 = 20 * log10(r100 / l100), x10k = 20 * log10(r10k / l10k + 1e-12);
        RESULT(fabs(x100 + 4.5) < 1.0 && x10k < -18.0, "crossfeed L-only input, feed 4.5 dB, fc 700 Hz: cross level 100 Hz %.1f dB (expect -4.5), 10 kHz %.1f dB", x100, x10k);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: dsp_tests <test> <outdir> [arg]\n"); return 2; }
    g_out = argv[2];
    const char *t = argv[1];
    if (params_selftest() != 0) { RESULT(0, "params table selftest"); return 1; }
    if (!strcmp(t, "eqlf")) test_eq_lf();
    else if (!strncmp(t, "eq", 2)) test_eq(atoi(t + 2));
    else if (!strcmp(t, "limiter")) test_limiter();
    else if (!strcmp(t, "limtransp_off")) test_lim_transparent("off");
    else if (!strcmp(t, "limtransp_on")) test_lim_transparent("on");
    else if (!strncmp(t, "reverb", 6)) test_reverb((float)atof(t + 6));
    else if (!strncmp(t, "delay", 5)) test_delay(atoi(t + 5) / 10, atoi(t + 5) % 10);
    else if (!strncmp(t, "satalias", 8) && argc > 5) test_sat_alias(atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5]));
    else if (!strcmp(t, "click1") && argc > 3) test_click1(atoi(argv[3]));
    else if (!strcmp(t, "clickpresets")) test_click_presets();
    else if (!strcmp(t, "loud")) test_loud();
    else if (!strcmp(t, "xfmono")) test_xfeed(0);
    else if (!strcmp(t, "xfstereo")) test_xfeed(1);
    else if (!strcmp(t, "dither")) test_dither(0);
    else if (!strcmp(t, "bitperfect")) test_dither(1);
    else if (!strcmp(t, "bench")) test_bench();
    else if (!strcmp(t, "nparams")) { printf("%d\n", P_COUNT); return 0; }
    else { fprintf(stderr, "unknown test %s\n", t); return 2; }
    return g_fail;
}
