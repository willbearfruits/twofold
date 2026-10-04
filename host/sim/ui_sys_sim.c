/*
 * ui_sys_sim.c: host implementation of ui_sys.h with plausible, deterministic, animated data:
 * music-like meters (kick / bass / transient envelopes), a synthetic mix for the spectrum,
 * occasional clips on request, CPU numbers, WiFi / BLE state machines driven by sys_cmd().
 */
#include "sim.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

sim_t SIM;

const char *const dsp_stage_names[DSP_ST_COUNT] = {
    "INPUT", "EQ", "SAT", "CHORUS", "DELAY", "REVERB", "COMP", "OUTPUT", "DITHER",
};

static uint32_t s_rng = 0x1234567u;
static float frand(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xFFFFFF) / 16777216.f;
}

static float s_seq_at_load;
static uint32_t s_cho_last;

void sim_reset(void)
{
    memset(&SIM, 0, sizeof SIM);
    SIM.music = 1.f;
    SIM.input = 0.6f;
    SIM.st.usb_mounted = true;
    SIM.st.play_active = true;
    SIM.st.rec_active = true;
    SIM.st.sample_rate = 48000;
    SIM.st.clock_ext = false;
    SIM.st.preset_active = 1;
    SIM.st.fw_version = "1.0.0";
    snprintf(SIM.st.ble_name, sizeof SIM.st.ble_name, "TWOFOLD-4F2A");
    for (int i = 0; i < PRESET_SLOTS; i++) snprintf(SIM.st.preset_names[i], 16, "%s", g_factory_presets[i].name);
    SIM.has_creds = true;
    s_rng = 0x1234567u;
    s_seq_at_load = 0;
    s_cho_last = 0;
}

/* ---------------------------------------------------------------- state machines */
static void sm_tick(void)
{
    uint32_t t = SIM.now;
    if (SIM.wifi_target_t && t >= SIM.wifi_target_t) {
        SIM.st.wifi = SIM.wifi_target;
        SIM.wifi_target_t = 0;
        if (SIM.st.wifi == WIFI_STA_CONNECTED) {
            snprintf(SIM.st.wifi_ssid, sizeof SIM.st.wifi_ssid, "Studio-5G");
            snprintf(SIM.st.ip, sizeof SIM.st.ip, "192.168.1.42");
            SIM.st.wifi_rssi = -54;
        } else if (SIM.st.wifi == WIFI_AP_SETUP) {
            snprintf(SIM.st.wifi_ssid, sizeof SIM.st.wifi_ssid, "TWOFOLD-4F2A");
            snprintf(SIM.st.wifi_pass, sizeof SIM.st.wifi_pass, "fold4f2a");
            snprintf(SIM.st.ip, sizeof SIM.st.ip, "192.168.4.1");
        }
    }
    if (SIM.ble_target_t && t >= SIM.ble_target_t) { SIM.st.ble = SIM.ble_target; SIM.ble_target_t = 0; }
    /* preset dirty: any preset parameter changed since the last load/save */
    uint32_t mask[PARAMS_MASK_WORDS];
    if (params_take_dirty(PL_BLE, mask) && SIM.st.preset_active >= 0) {
        for (int i = 0; i < P_COUNT; i++)
            if (params_mask_test(mask, (param_id_t)i) && (g_param_desc[i].flags & PF_PRESET)) SIM.st.preset_dirty = true;
    }
}

void sys_get_status(sys_status_t *s)
{
    sm_tick();
    *s = SIM.st;
}

void sys_cmd(sys_cmd_t cmd, int arg)
{
    uint32_t mask[PARAMS_MASK_WORDS];
    switch (cmd) {
    case SYS_CMD_WIFI_ENABLE:
        if (arg) {
            SIM.st.wifi = WIFI_STARTING;
            SIM.wifi_target = SIM.has_creds ? WIFI_STA_CONNECTED : WIFI_AP_SETUP;
            SIM.wifi_target_t = SIM.now + 1600;
        } else {
            SIM.st.wifi = WIFI_OFF; SIM.wifi_target_t = 0;
        }
        break;
    case SYS_CMD_WIFI_SETUP:
        SIM.st.wifi = WIFI_STARTING;
        SIM.wifi_target = WIFI_AP_SETUP;
        SIM.wifi_target_t = SIM.now + 900;
        break;
    case SYS_CMD_WIFI_FORGET: SIM.has_creds = false; break;
    case SYS_CMD_BLE_ENABLE:
        if (arg) { SIM.st.ble = BLE_ADVERTISING; SIM.ble_target = BLE_CONNECTED; SIM.ble_target_t = SIM.now + 2500; }
        else { SIM.st.ble = BLE_OFF; SIM.ble_target_t = 0; }
        break;
    case SYS_CMD_PRESET_LOAD: {
        uint8_t blob[1024];
        size_t n = params_factory_blob(arg, blob, sizeof blob);
        params_preset_apply(blob, n, NULL);
        SIM.st.preset_active = arg;
        params_take_dirty(PL_BLE, mask);
        SIM.st.preset_dirty = false;
        break;
    }
    case SYS_CMD_PRESET_SAVE:
        SIM.st.preset_active = arg;
        SIM.st.preset_dirty = false;
        params_take_dirty(PL_BLE, mask);
        break;
    case SYS_CMD_FACTORY_RESET: params_init(); break;
    default: break;
    }
    SIM.cmd_count++;
    (void)s_seq_at_load;
}

/* ---------------------------------------------------------------- audio-ish signals */
static float env_kick(float t, float bpm)
{
    float beat = 60.f / bpm;
    float ph = fmodf(t, beat);
    return expf(-ph * 9.f);
}

static float env_snare(float t, float bpm)
{
    float beat = 60.f / bpm;
    float ph = fmodf(t + beat, 2.f * beat);
    return expf(-ph * 14.f);
}

static float s_last_t;

void sys_get_meters(dsp_meters_t *m)
{
    memset(m, 0, sizeof *m);
    float t = (float)SIM.now * 0.001f;
    float bpm = 118.f;
    float k = env_kick(t, bpm), s = env_snare(t, bpm);
    float mus = SIM.music;
    for (int c = 0; c < 2; c++) {
        float jitter = 0.85f + 0.15f * frand();
        float body = 0.22f + 0.12f * sinf(t * 0.7f + (float)c * 1.3f);
        float pk = mus * jitter * (body + 0.42f * k + 0.25f * s * (c ? 1.1f : 0.9f));
        float vol = powf(10.f, params_get(P_VOLUME) / 20.f) * (params_get_bool(P_MUTE) ? 0.f : 1.f);
        m->play_peak[c] = pk;
        m->out_peak[c] = fminf(pk * vol * 1.9f, 0.966f);
        float g = powf(10.f, params_get(P_IN_GAIN) / 20.f);
        float in = SIM.input * g * (0.25f + 0.2f * sinf(t * 2.1f + (float)c) * sinf(t * 0.33f) + 0.35f * env_kick(t + 0.11f, bpm * 0.5f)) * (0.9f + 0.1f * frand());
        m->in_peak[c] = fminf(in, 1.2f);
    }
    if (SIM.clip_in) { m->in_peak[0] = 1.12f; SIM.in_clips++; SIM.clip_in = false; }
    if (SIM.clip_out) { m->out_peak[1] = 1.0f; SIM.out_overs++; SIM.clip_out = false; }
    m->in_clip[0] = m->in_clip[1] = SIM.in_clips;
    m->out_over[0] = m->out_over[1] = SIM.out_overs;
    if (params_get_bool(P_CMP_ON)) {
        float lvl = 20.f * log10f(m->play_peak[0] + 1e-6f);
        float over = lvl - params_get(P_CMP_THRESH) + 6.f;
        float r = params_get(P_CMP_RATIO);
        m->comp_gr_db = over > 0.f ? over * (1.f - 1.f / r) : 0.f;
    }
    m->lim_gr_db = 0.f;
    m->sat_in_level = m->play_peak[0] * 1.2f;
    s_last_t = t;
}

size_t sys_get_spectrum_samples(float *dst, size_t n)
{
    /* a little "mix": bass line + kick thump + chords + pink-ish hiss, deterministic in time */
    double t0 = (double)SIM.now * 0.001;
    float k = env_kick((float)t0, 118.f);
    static float b0, b1, b2;
    for (size_t i = 0; i < n; i++) {
        double t = t0 + (double)i / 48000.0;
        float w = frand() * 2.f - 1.f;
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        float pink = (b0 + b1 + b2 + w * 0.1848f) * 0.05f;
        float x = 0.30f * (float)sin(2 * M_PI * 55.0 * t) + 0.18f * (float)sin(2 * M_PI * 110.0 * t)
                + 0.35f * k * (float)sin(2 * M_PI * (48.0 + 40.0 * k) * t)
                + 0.06f * (float)sin(2 * M_PI * 261.6 * t) + 0.05f * (float)sin(2 * M_PI * 329.6 * t)
                + 0.04f * (float)sin(2 * M_PI * 392.0 * t) + 0.02f * (float)sin(2 * M_PI * 1568.0 * t)
                + pink;
        dst[i] = x * SIM.music * 0.8f;
    }
    return n;
}

void sys_get_viz(dsp_viz_t *v)
{
    uint32_t dtm = SIM.now - s_cho_last;
    s_cho_last = SIM.now;
    float rate = params_get(P_CHO_RATE);
    for (int i = 0; i < 3; i++) {
        SIM.cho_phase[i] += rate * (1.f + 0.07f * (float)i) * (float)dtm * 0.001f;
        SIM.cho_phase[i] -= floorf(SIM.cho_phase[i]);
    }
    for (int i = 0; i < 3; i++) v->chorus_lfo_phase[i] = fmodf(SIM.cho_phase[i] + (float)i / 3.f, 1.f);
    static const float mult[8] = { 0, 2.f, 1.f, 1.5f, 2.f / 3.f, 0.5f, 0.75f, 0.25f };
    int sync = params_get_int(P_DLY_SYNC);
    v->delay_time_ms = sync ? 60000.f / params_get(P_TEMPO) * mult[sync] : params_get(P_DLY_TIME);
    v->reverb_energy = 0.1f;
}

void sys_get_diag(sys_diag_t *d)
{
    memset(d, 0, sizeof *d);
    static const float base[DSP_ST_COUNT] = { 1.1f, 2.6f, 5.4f, 2.7f, 5.1f, 7.6f, 2.2f, 2.9f, 0.8f };
    static const int16_t on_p[DSP_ST_COUNT] = { -1, P_EQ_ON, P_SAT_ON, P_CHO_ON, P_DLY_ON, P_REV_ON, P_CMP_ON, -1, -1 };
    float tot = 0.f, tmax = 0.f;
    for (int i = 0; i < DSP_ST_COUNT; i++) {
        bool on = on_p[i] < 0 || params_get_bool((param_id_t)on_p[i]);
        float a = on ? base[i] * (0.97f + 0.06f * frand()) : 0.15f;
        d->prof.avg_pct[i] = a;
        d->prof.max_pct[i] = a * (1.12f + 0.1f * frand());
        tot += a;
        tmax += d->prof.max_pct[i];
    }
    d->prof.total_avg_pct = tot;
    d->prof.total_max_pct = tmax * 0.92f;
    d->prof.blocks = SIM.now * 3 / 2;
    d->prof.window_ms = 1000;
    d->xrun_play = 0;
    d->xrun_rec = 0;
    d->i2s_errors = 0;
    d->usb_out_fifo = 1152 + (int32_t)(frand() * 48.f) - 24;
    d->usb_in_fifo = 1104 + (int32_t)(frand() * 48.f) - 24;
    d->usb_feedback = 48.0013f;
    d->heap_int_free = 101 * 1024 + 312;
    d->heap_int_min = 88 * 1024;
    d->heap_ext_free = 1290000;
    d->uptime_s = 3 * 3600 + 25 * 60 + SIM.now / 1000;
    d->measured_fs = 48000.02f;
}

void sys_display_set(uint8_t level, bool on)
{
    SIM.disp_level = level;
    SIM.disp_on = on;
    SIM.disp_calls++;
}

void sys_midi_send_cc(uint8_t cc, uint8_t value)
{
    SIM.last_cc = cc;
    SIM.last_cc_val = value;
    SIM.cc_count++;
}
