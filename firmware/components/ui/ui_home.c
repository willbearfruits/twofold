/*
 * ui_home.c: HOME. Hero master volume (right-aligned so the decimal point never moves), stereo
 * IN / OUT meters with peak hold and clip lamps, studio / radio state, USB stream state, preset.
 * Rotate = volume (accelerated) or MIDI CC (KNOB MODE), press = menu, double press = mute.
 */
#include "ui_priv.h"

void ui_mark_anim(void);

static struct {
    tw_t vol;               /* displayed volume (glides) */
    float carry;
    uint32_t touch_ms;      /* last knob interaction (volume ruler visibility) */
    tw_t ruler;             /* 0..1 visibility */
    int cc_val;             /* MIDI knob mode value */
    uint32_t cc_ms;
    tw_t cc_vis;
} H;

static void home_enter(void)
{
    tw_snap(&H.vol, params_get(P_VOLUME));
    tw_snap(&H.ruler, 0.f);
    tw_snap(&H.cc_vis, 0.f);
    if (H.cc_val == 0) H.cc_val = 64;
}

static void home_input(ui_act_t a, int raw)
{
    switch (a) {
    case A_ROT:
        if (params_get_int(P_MIDI_ENC) == 1) {
            int steps = ui_accel_steps(raw, &H.carry);
            int nv = clampi_(H.cc_val + steps, 0, 127);
            if (nv != H.cc_val) {
                H.cc_val = nv;
                sys_midi_send_cc((uint8_t)params_get_int(P_MIDI_ENC_CC), (uint8_t)nv);
            }
            H.cc_ms = U.now;
            tw_to(&H.cc_vis, 1.f);
        } else {
            ui_param_rotate(P_VOLUME, raw, &H.carry);
            H.touch_ms = U.now;
            tw_to(&H.ruler, 1.f);
        }
        break;
    case A_PRESS: ui_push(SCR_MENU); break;
    case A_DOUBLE:
        ui_param_toggle(P_MUTE);
        ui_toast(params_get_bool(P_MUTE) ? "MUTED" : "UNMUTED");
        break;
    default: break;
    }
}

static void home_tick(float dt)
{
    tw_to(&H.vol, params_get(P_VOLUME));
    if (tw_step(&H.vol, dt, 0.045f)) ui_mark_anim();
    if (H.ruler.t > 0.f && U.now - H.touch_ms > 1400) tw_to(&H.ruler, 0.f);
    if (H.cc_vis.t > 0.f && U.now - H.cc_ms > 1400) tw_to(&H.cc_vis, 0.f);
    if (tw_step(&H.ruler, dt, 0.08f)) ui_mark_anim();
    if (tw_step(&H.cc_vis, dt, 0.06f)) ui_mark_anim();
    if (U.st.wifi == WIFI_STARTING || U.st.wifi == WIFI_STA_CONNECTING || U.st.ble == BLE_ADVERTISING) ui_mark_anim();
}

/* ---- status row ---- */
static int wifi_icon(void)
{
    if (U.st.wifi != WIFI_STA_CONNECTED) return GFX_I_WIFI;
    if (U.st.wifi_rssi > -60) return GFX_I_WIFI;
    if (U.st.wifi_rssi > -75) return GFX_I_WIFI2;
    return GFX_I_WIFI1;
}

static void draw_status(gfx_t *g)
{
    const sys_status_t *s = &U.st;
    bool blink = ((U.now / 500) & 1) != 0;
    int x = 2;
    if (s->wifi == WIFI_OFF && s->ble == BLE_OFF) {
        ui_pill(g, x, 0, "STUDIO", true, GFX_LEFT);
    } else {
        if (s->wifi != WIFI_OFF) {
            bool pending = s->wifi == WIFI_STARTING || s->wifi == WIFI_STA_CONNECTING;
            if (!pending || blink) gfx_glyph(g, F_ICON, x, 0, (uint8_t)wifi_icon(), C_ON);
            x += 9;
            const char *t = s->wifi == WIFI_AP_SETUP ? "SETUP" : (s->wifi == WIFI_ERROR ? "ERR" : NULL);
            if (t) x = gfx_text_ex(g, F_SMALL, x, 1, t, 1, C_ON) + 4;
            else x += 2;
        }
        if (s->ble != BLE_OFF) {
            bool pending = s->ble == BLE_ADVERTISING;
            if (!pending || blink) gfx_glyph(g, F_ICON, x, 0, GFX_I_BLE, C_ON);
            x += 7;
            if (s->ble == BLE_CONNECTED) gfx_text_ex(g, F_SMALL, x, 1, "MIDI", 1, C_ON);
        }
    }
    /* right: stream state + USB-C */
    int rx = GFX_W - 2 - 9;
    gfx_glyph(g, F_ICON, rx, 1, GFX_I_USB, C_ON);
    if (!s->usb_mounted) gfx_rect(g, rx + 2, 3, 5, 1, C_OFF);       /* empty port when not enumerated */
    rx -= 3;
    if (s->rec_active) { rx -= 5; gfx_glyph(g, F_SMALL, rx, 1, 0x8f, C_ON); rx -= 3; }
    if (s->play_active) { rx -= 3; gfx_glyph(g, F_SMALL, rx, 1, 0x8e, C_ON); rx -= 3; }
    if (params_get_int(P_MIDI_ENC) == 1) { rx -= 7; gfx_glyph(g, F_ICON, rx, 0, GFX_I_MIDI, C_ON); }
}

/* ---- hero volume ---- */
#define HERO_Y   13
#define HERO_RX  100      /* right edge of the numerals */

static void draw_volume(gfx_t *g)
{
    float v = H.vol.v;
    float q = roundf(v * 2.f) * 0.5f;
    char buf[12];
    if (q <= params_desc(P_VOLUME)->min + 0.01f) snprintf(buf, sizeof buf, "-" GFX_S_INF);
    else if (fabsf(q) < 0.05f) snprintf(buf, sizeof buf, "0.0");
    else snprintf(buf, sizeof buf, "%.1f", (double)q);
    bool muted = params_get_bool(P_MUTE);
    int w = gfx_text_width(F_BIG, buf);
    gfx_text(g, F_BIG, HERO_RX - w, HERO_Y, buf, C_ON);
    /* unit column */
    gfx_text(g, F_SMALL, HERO_RX + 4, HERO_Y + 13, "dB", C_ON);
    if (muted) {
        gfx_fade(g, HERO_RX - w - 1, HERO_Y, w + 2, 18, 5);
        ui_pill(g, GFX_W - 2, HERO_Y, "MUTE", true, GFX_RIGHT);
    } else {
        gfx_text_ex(g, F_SMALL, HERO_RX + 4, HERO_Y, "VOL", 1, C_ON);
    }
    /* left column: preset */
    const sys_status_t *s = &U.st;
    if (s->preset_active >= 0 && s->preset_active < 8) {
        char p[6];
        snprintf(p, sizeof p, "P%d", s->preset_active + 1);
        int xe = gfx_text_ex(g, F_SMALL, UI_MX, HERO_Y, p, 1, C_ON);
        if (s->preset_dirty) gfx_rect(g, xe + 2, HERO_Y + 2, 2, 2, C_ON);
        char nm[9];
        snprintf(nm, sizeof nm, "%.7s", s->preset_names[s->preset_active]);
        (void)nm;
    }
}

static void draw_ruler(gfx_t *g)
{
    float vis = H.ruler.v;
    if (vis < 0.02f) return;
    const int x = UI_MX, w = GFX_W - 2 * UI_MX, y = 33;
    const param_desc_t *d = params_desc(P_VOLUME);
    float n = (H.vol.v - d->min) / (d->max - d->min);
    int fx = iroundf(n * (float)(w - 1));
    gfx_hline_pat(g, x, y + 1, w, 0x55, C_ON);
    gfx_rect(g, x, y, fx + 1, 3, C_ON);
    gfx_vline(g, x + fx, y - 2, 7, C_ON);
    gfx_fade(g, 0, y - 2, GFX_W, 7, iroundf(vis * 16.f));
}

/* ---- meters ---- */
#define MX0 20
#define MW  98

static void draw_scale(gfx_t *g, int y)
{
    static const int8_t ticks[] = { -40, -30, -20, -12, -6, -3, 0 };
    static const char *const labs[] = { "40", "30", "20", "12", "6", "3", "0" };
    for (unsigned i = 0; i < sizeof ticks; i++) {
        int tx = MX0 + iroundf(ui_meter_pos((float)ticks[i]) * (float)(MW - 1));
        gfx_pixel(g, tx, y, C_ON);
        gfx_text_align(g, F_TINY, tx + 1, y + 2, labs[i], 0, GFX_CENTER, C_ON);
    }
}

static void clip_lamp(gfx_t *g, int x, int y, int h, bool on)
{
    if (on) gfx_rect(g, x, y, 3, h, C_ON);
    else gfx_vline_pat(g, x + 1, y, h, 0x55, C_ON);
}

static void draw_meters(gfx_t *g)
{
    const int yi = 38, yo = 55;
    gfx_text_ex(g, F_SMALL, UI_MX, yi + 1, "IN", 1, C_ON);
    gfx_text_ex(g, F_SMALL, UI_MX, yo + 1, "OUT", 0, C_ON);
    for (int c = 0; c < 2; c++) {
        ui_meter_bar(g, MX0, yi + c * 4, MW, 3, U.in_db[c], U.in_pk[c], false, true);
        ui_meter_bar(g, MX0, yo + c * 4, MW, 3, U.out_db[c], U.out_pk[c], false, true);
    }
    bool ic = (int32_t)(U.in_clip_until[0] - U.now) > 0 || (int32_t)(U.in_clip_until[1] - U.now) > 0;
    bool oc = (int32_t)(U.out_clip_until[0] - U.now) > 0 || (int32_t)(U.out_clip_until[1] - U.now) > 0;
    clip_lamp(g, MX0 + MW + 3, yi, 7, ic);
    clip_lamp(g, MX0 + MW + 3, yo, 7, oc);
    draw_scale(g, yi + 8);
}

static void draw_cc(gfx_t *g)
{
    float vis = H.cc_vis.v;
    if (vis < 0.02f) return;
    int y = GFX_H - iroundf(vis * 22.f);
    gfx_rect(g, 0, y - 1, GFX_W, 24, C_OFF);
    gfx_hline(g, 0, y, GFX_W, C_ON);
    char b[16];
    snprintf(b, sizeof b, "CC %d", params_get_int(P_MIDI_ENC_CC));
    gfx_text_ex(g, F_SMALL, UI_MX, y + 4, "MIDI", 1, C_ON);
    gfx_text_ex(g, F_SMALL, UI_MX, y + 12, b, 1, C_ON);
    snprintf(b, sizeof b, "%d", H.cc_val);
    gfx_text_align(g, F_MED, GFX_W - UI_MX, y + 4, b, 0, GFX_RIGHT, C_ON);
    int bx = 36, bw = 64;
    gfx_rframe(g, bx, y + 13, bw, 5, 2, C_ON);
    gfx_rect(g, bx + 1, y + 14, iroundf((float)(bw - 2) * (float)H.cc_val / 127.f), 3, C_ON);
}

static void home_draw(gfx_t *g)
{
    draw_status(g);
    draw_volume(g);
    draw_meters(g);
    draw_ruler(g);
    draw_cc(g);
}

const screen_t g_scr_home = { home_enter, home_input, home_tick, home_draw, NULL };
