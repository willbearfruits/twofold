/*
 * ui_spectrum.c: real-time spectrum for the EQ screen.
 *
 * 1024-point real FFT done as a 512-point complex radix-2 FFT plus the standard real-split
 * post-processing (the interleaved sample buffer IS the packed complex input, so no copy).
 * Twiddles and the Hann window both come from one quarter-wave sine table (257 floats).
 * Runs at ~30 Hz from the UI task; on the S3 this is ~40 k cycles per frame (FPU single precision).
 * The firmware may swap fft_cplx() for esp-dsp's dsps_fft2r_fc32 later; the layout is identical.
 *
 * RAM: 4 KB samples/FFT + 2 KB bin powers + 1 KB table + 0.8 KB column state.
 */
#include "ui_priv.h"

#define SN   1024             /* real samples */
#define SM   (SN / 2)         /* complex points */
#define SCOL 128

static float s_buf[SN];                  /* interleaved re/im */
static float s_pw[SM + 1];               /* |X[k]|^2 */
static float s_qs[SN / 4 + 1];           /* sin(2 pi j / SN), j = 0..SN/4 */
static float s_cbin[SCOL];               /* fractional centre bin of each column */
static int8_t s_tilt[SCOL];              /* +3 dB/oct display tilt around 1 kHz */
static uint8_t s_col[SCOL];              /* heights, 1/4 px */
static float s_acc;

static inline float tsin(int k)   /* sin(2 pi k / SN), k in 0..SN/2 */
{
    return k <= SN / 4 ? s_qs[k] : s_qs[SN / 2 - k];
}
static inline float tcos(int k)   /* cos(2 pi k / SN), k in 0..SN/2 */
{
    return k <= SN / 4 ? s_qs[SN / 4 - k] : -s_qs[k - SN / 4];
}

void spec_init(void)
{
    for (int j = 0; j <= SN / 4; j++) s_qs[j] = sinf(6.28318531f * (float)j / (float)SN);
    const float df = 48000.f / (float)SN;
    for (int x = 0; x < SCOL; x++) {
        float f = eq_x_to_hz(x, SCOL);
        s_cbin[x] = f / df;
        s_tilt[x] = (int8_t)iroundf(3.f * log2f(f / 1000.f));
        s_col[x] = 0;
    }
    s_acc = 0.f;
}

static void fft_cplx(float *d, int n)
{
    /* bit reversal */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = d[2 * i], ti = d[2 * i + 1];
            d[2 * i] = d[2 * j]; d[2 * i + 1] = d[2 * j + 1];
            d[2 * j] = tr; d[2 * j + 1] = ti;
        }
    }
    /* butterflies; twiddle e^{-2 pi i k / len} = table index k * (SN / len) */
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, stride = SN / len;
        for (int k = 0; k < half; k++) {
            float wr = tcos(k * stride), wi = -tsin(k * stride);
            for (int i = k; i < n; i += len) {
                int j = i + half;
                float xr = d[2 * j] * wr - d[2 * j + 1] * wi;
                float xi = d[2 * j] * wi + d[2 * j + 1] * wr;
                d[2 * j] = d[2 * i] - xr; d[2 * j + 1] = d[2 * i + 1] - xi;
                d[2 * i] += xr; d[2 * i + 1] += xi;
            }
        }
    }
}

static void analyse(void)
{
    size_t got = sys_get_spectrum_samples(s_buf, SN);
    for (size_t i = got; i < SN; i++) s_buf[i] = 0.f;
    /* Hann window: 0.5 - 0.5 cos(2 pi n / N), cos via the table and symmetry */
    for (int i = 0; i < SN; i++) {
        int k = i <= SN / 2 ? i : SN - i;
        s_buf[i] *= 0.5f - 0.5f * tcos(k);
    }
    fft_cplx(s_buf, SM);
    /* real split: X[k] = (Z[k] + Z*[M-k])/2 - i/2 W^k (Z[k] - Z*[M-k]) */
    for (int k = 0; k <= SM; k++) {
        int a = k % SM, b = (SM - k) % SM;
        float zr = s_buf[2 * a], zi = s_buf[2 * a + 1];
        float cr = s_buf[2 * b], ci = -s_buf[2 * b + 1];
        float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);
        float orr = 0.5f * (zr - cr), oi = 0.5f * (zi - ci);
        float wr = tcos(k), wi = -tsin(k);
        /* -i * W * O */
        float tr = wr * orr - wi * oi, ti = wr * oi + wi * orr;
        float xr = er + ti, xi = ei - tr;
        s_pw[k] = xr * xr + xi * xi;
    }
}

void spec_update(float dt)
{
    s_acc += dt;
    if (s_acc >= 1.f / 30.f) {
        s_acc = 0.f;
        analyse();
        const float ref = 1.f / ((float)SN * (float)SN / 16.f);   /* full-scale sine -> 0 dB */
        for (int x = 0; x < SCOL; x++) {
            float c = s_cbin[x];
            float lo = x > 0 ? 0.5f * (s_cbin[x - 1] + c) : c;
            float hi = x < SCOL - 1 ? 0.5f * (s_cbin[x + 1] + c) : c;
            int i0 = (int)ceilf(lo), i1 = (int)floorf(hi);
            float p = 0.f;
            if (i1 >= i0) {
                for (int i = i0; i <= i1 && i <= SM; i++) p = fmaxf(p, s_pw[i]);
            } else {
                int i = (int)c;
                float t = c - (float)i;
                p = s_pw[i] * (1.f - t) + s_pw[i + 1 <= SM ? i + 1 : SM] * t;
            }
            float db = 10.f * log10f(p * ref + 1e-12f) + (float)s_tilt[x];
            /* -78 .. -6 dB -> 0 .. 36 px, in quarter pixels */
            float h = (db + 78.f) * (36.f * 4.f / 72.f);
            int hi4 = clampi_(iroundf(h), 0, 255);
            if (hi4 > s_col[x]) s_col[x] = (uint8_t)hi4;
        }
    }
    /* release: ~36 dB/s */
    int fall = iroundf(dt * 36.f * (36.f * 4.f / 72.f));
    if (fall < 1) fall = 1;
    for (int x = 0; x < SCOL; x++) s_col[x] = (uint8_t)(s_col[x] > fall ? s_col[x] - fall : 0);
}

const uint8_t *spec_columns(void) { return s_col; }
