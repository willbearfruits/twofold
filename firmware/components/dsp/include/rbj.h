/*
 * rbj.h: RBJ "Audio EQ Cookbook" biquad design and analytic magnitude response.
 * Shared by the audio engine (filter coefficients) and the UI (EQ curve drawn from the
 * actual coefficients) and the host tests (analytic target).
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RBJ_LOWSHELF, RBJ_PEAK, RBJ_HIGHSHELF, RBJ_LOWPASS, RBJ_HIGHPASS } rbj_type_t;

/* Normalised coefficients: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2  (a0 == 1). */
typedef struct { float b0, b1, b2, a1, a2; } biquad_coef_t;

void  rbj_design(biquad_coef_t *c, rbj_type_t type, float fs, float f0, float gain_db, float q);
/* |H(e^jw)|^2 evaluated in double precision for display / tests. */
double biquad_mag2(const biquad_coef_t *c, double f, double fs);
double biquad_mag_db(const biquad_coef_t *c, double f, double fs);

/* Fixed band layout of the TWOFOLD EQ. */
#define EQ_BANDS 5
static inline rbj_type_t eq_band_type(int b) { return b == 0 ? RBJ_LOWSHELF : (b == 4 ? RBJ_HIGHSHELF : RBJ_PEAK); }

#ifdef __cplusplus
}
#endif
