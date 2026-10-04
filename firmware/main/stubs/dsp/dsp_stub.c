/* Stub dsp_engine (TWOFOLD_STUBS builds only): passthrough, no effects. */
#include <string.h>
#include "dsp_engine.h"
const char *const dsp_stage_names[DSP_ST_COUNT] = { "INPUT", "EQ", "SAT", "CHORUS", "DELAY", "REVERB", "COMP", "OUTPUT", "DITHER" };
static volatile int s_mute;
int  dsp_engine_init(const dsp_alloc_t *a) { (void)a; return 0; }
void dsp_engine_reset(void) {}
void dsp_engine_process(const int32_t *play, const int32_t *adc, int32_t *dac, int32_t *rec, int n)
{
    if (play && !s_mute) memcpy(dac, play, (size_t)n * 8); else memset(dac, 0, (size_t)n * 8);
    if (rec) { if (adc && !s_mute) memcpy(rec, adc, (size_t)n * 8); else memset(rec, 0, (size_t)n * 8); }
}
void dsp_engine_get_meters(dsp_meters_t *m, bool take) { (void)take; memset(m, 0, sizeof *m); }
void dsp_engine_get_prof(dsp_prof_t *p) { memset(p, 0, sizeof *p); }
size_t dsp_engine_spectrum_read(float *dst, size_t n) { memset(dst, 0, n * sizeof *dst); return n; }
void dsp_engine_get_viz(dsp_viz_t *v) { memset(v, 0, sizeof *v); }
void dsp_engine_soft_mute(bool on) { s_mute += on ? 1 : -1; }
bool dsp_engine_is_silent(void) { return s_mute > 0; }
bool dsp_engine_output_quiet(void) { return true; }
void dsp_engine_startup_ramp(uint32_t ms) { (void)ms; }
