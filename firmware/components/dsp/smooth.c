/* smooth.c: block context helpers (see dsp_smooth.h). */
#include "dsp_smooth.h"
#include "dsp_port.h"

float dsp_onepole_k(float tau_ms, int n)
{
    if (tau_ms <= 0.f) return 1.f;
    /* exp(-x) = 2^(-x * log2(e)) */
    const float x = (float)n / (tau_ms * 1e-3f * (float)DSP_FS);
    return 1.f - dsp_exp2(-x * 1.44269504f);
}

void dsp_blk_make(dsp_blk_t *b, int n, float tau_ms, float fade_ms, float xfade_ms)
{
    b->n = n;
    b->inv_n = 1.f / (float)n;
    b->k = dsp_onepole_k(tau_ms, n);
    b->fade_step = 1000.f / (fade_ms * (float)DSP_FS);
    b->xfade_step = 1000.f / (xfade_ms * (float)DSP_FS);
}
