/* factory_presets.c: the 8 presets shipped in the preset slots on first boot / factory reset.
 * Each preset is "defaults for every PF_PRESET param" plus the overrides listed here. */
#include "params.h"

#define KV(id, v) { id, v }
#define N(a) (int)(sizeof(a) / sizeof(a[0]))

static const preset_kv_t p_init[] = { KV(P_EQ_ON, 1) };

static const preset_kv_t p_vocal[] = {
    KV(P_EQ_B1_FREQ, 120), KV(P_EQ_B1_GAIN, -2.0f),
    KV(P_EQ_B3_FREQ, 3000), KV(P_EQ_B3_GAIN, 2.0f), KV(P_EQ_B3_Q, 1.2f),
    KV(P_EQ_B5_FREQ, 10000), KV(P_EQ_B5_GAIN, 2.5f),
    KV(P_CMP_ON, 1), KV(P_CMP_THRESH, -18), KV(P_CMP_RATIO, 3), KV(P_CMP_ATTACK, 5),
    KV(P_CMP_RELEASE, 120), KV(P_CMP_MAKEUP, 4),
    KV(P_REV_ON, 1), KV(P_REV_DECAY, 1.8f), KV(P_REV_PREDLY, 30), KV(P_REV_DAMP, 40), KV(P_REV_MIX, 18),
};

static const preset_kv_t p_tape[] = {
    KV(P_SAT_ON, 1), KV(P_SAT_DRIVE, 8), KV(P_SAT_WARMTH, 50), KV(P_SAT_TONE, -30),
    KV(P_DLY_ON, 1), KV(P_DLY_SYNC, 3 /* 1/4 D */), KV(P_DLY_FB, 45), KV(P_DLY_MODE, 0),
    KV(P_DLY_HICUT, 3500), KV(P_DLY_LOCUT, 200), KV(P_DLY_MIX, 28),
};

static const preset_kv_t p_ensemble[] = {
    KV(P_CHO_ON, 1), KV(P_CHO_MODE, 1), KV(P_CHO_RATE, 0.8f), KV(P_CHO_DEPTH, 60), KV(P_CHO_MIX, 45),
    KV(P_REV_ON, 1), KV(P_REV_DECAY, 2.5f), KV(P_REV_MIX, 15),
};

static const preset_kv_t p_glue[] = {
    KV(P_CMP_ON, 1), KV(P_CMP_THRESH, -16), KV(P_CMP_RATIO, 2), KV(P_CMP_ATTACK, 30),
    KV(P_CMP_RELEASE, 300), KV(P_CMP_KNEE, 6), KV(P_CMP_MAKEUP, 3),
    KV(P_EQ_B1_FREQ, 60), KV(P_EQ_B1_GAIN, 1.0f), KV(P_EQ_B5_FREQ, 12000), KV(P_EQ_B5_GAIN, 1.5f),
};

static const preset_kv_t p_warm[] = {
    KV(P_SAT_ON, 1), KV(P_SAT_DRIVE, 10), KV(P_SAT_WARMTH, 70), KV(P_SAT_TONE, -20), KV(P_SAT_MIX, 60),
    KV(P_EQ_B5_GAIN, -1.5f),
};

static const preset_kv_t p_space[] = {
    KV(P_REV_ON, 1), KV(P_REV_DECAY, 7.0f), KV(P_REV_PREDLY, 60), KV(P_REV_DAMP, 30), KV(P_REV_MOD, 60),
    KV(P_REV_LOCUT, 200), KV(P_REV_MIX, 35),
    KV(P_DLY_ON, 1), KV(P_DLY_SYNC, 6 /* 1/8 D */), KV(P_DLY_FB, 30), KV(P_DLY_MIX, 15), KV(P_DLY_MODE, 1),
};

static const preset_kv_t p_phones[] = {
    KV(P_EQ_B1_FREQ, 90), KV(P_EQ_B1_GAIN, 2.0f), KV(P_EQ_B1_Q, 0.7f),
    KV(P_EQ_B3_FREQ, 3000), KV(P_EQ_B3_GAIN, -1.5f), KV(P_EQ_B3_Q, 1.5f),
    KV(P_EQ_B5_FREQ, 9000), KV(P_EQ_B5_GAIN, -1.0f),
};

const factory_preset_t g_factory_presets[PRESET_SLOTS] = {
    { "INIT",        p_init,     N(p_init) },
    { "VOCAL PLATE", p_vocal,    N(p_vocal) },
    { "TAPE ECHO",   p_tape,     N(p_tape) },
    { "ENSEMBLE",    p_ensemble, N(p_ensemble) },
    { "MIX GLUE",    p_glue,     N(p_glue) },
    { "WARMTH",      p_warm,     N(p_warm) },
    { "DEEP SPACE",  p_space,    N(p_space) },
    { "HEADPHONES",  p_phones,   N(p_phones) },
};
