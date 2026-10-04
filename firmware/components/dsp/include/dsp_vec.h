/*
 * dsp_vec.h: the few whole-block vector operations of the engine, with esp-dsp on the target and an
 * identical-result portable fallback (one IEEE multiply per element either way, so host tests are exact).
 *
 * Where esp-dsp is used and why:
 *   dsps_mulc_f32 (constant gain over a block: settled master volume). Its ae32 kernel is a zero-overhead
 *   loop with lsip / mul.s / ssip, i.e. ~1 element per cycle with no loop-carried dependency, the same as
 *   what GCC emits for the fallback at -O2, so the gain is marginal; it is used because it is the canonical
 *   tuned kernel for this pattern on the S3.
 * Where it is deliberately NOT used:
 *   - biquads: dsps_biquad_f32 is DF-II (state gain ~1/|A|, +100 dB for 20 Hz shelves => float noise at LF);
 *     we keep our own stereo TDF-II (see dsp_biquad.h).
 *   - FIR kernels: the oversampler is a polyphase IIR (7+4 multiplies per sample, see dsp_halfband.h), a
 *     FIR alternative would need ~4x the multiplies even with dsps_fir_f32.
 *   - gain ramps / mixes: they are fused into the loops that already touch the data (one pass instead of
 *     dsps_mul + dsps_add passes over memory).
 *   - S3 PIE SIMD is integer-only; all float work is scalar FPU anyway.
 */
#pragma once

#if defined(ESP_PLATFORM) && !defined(DSP_NO_ESP_DSP)
#include "dsps_mulc.h"
static inline void dsp_vec_mulc(float *x, float c, int n) { dsps_mulc_f32(x, x, n, c, 1, 1); }
#else
static inline void dsp_vec_mulc(float *x, float c, int n) { for (int i = 0; i < n; i++) x[i] *= c; }
#endif

static inline void dsp_vec_mulc2(float *L, float *R, float c, int n) { dsp_vec_mulc(L, c, n); dsp_vec_mulc(R, c, n); }
