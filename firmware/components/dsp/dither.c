/* dither.c: TPDF dither + 24-bit conversion (see dsp_dither.h). */
#include "dsp_dither.h"
#include "dsp_port.h"

void dither_init(dsp_dither_t *d, uint32_t seed)
{
    uint32_t s = seed ? seed : 0x12345678u;
    for (int c = 0; c < 2; c++) {
        s = s * 1664525u + 1013904223u;
        d->s[c] = s ? s : 1u;
        for (int k = 0; k < 8; k++) (void)dsp_xorshift32(&d->s[c]);   /* decorrelate the seeds */
    }
}

DSP_HOT void dither_convert(dsp_dither_t *d, const float *L, const float *R, int32_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        out[2 * i]     = dsp_f2i24(L[i] * DSP_I24_SCALE + dither_tpdf(d, 0));
        out[2 * i + 1] = dsp_f2i24(R[i] * DSP_I24_SCALE + dither_tpdf(d, 1));
    }
}

DSP_HOT void quantize_convert(const float *L, const float *R, int32_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        out[2 * i]     = dsp_f2i24(L[i] * DSP_I24_SCALE);
        out[2 * i + 1] = dsp_f2i24(R[i] * DSP_I24_SCALE);
    }
}
