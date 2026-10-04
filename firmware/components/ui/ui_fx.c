/*
 * ui_fx.c: panel screens = header + live visualisation (y 9..44) + gliding parameter strip.
 *   GAIN      hero input gain, stereo input meter, monitor crossfader
 *   SATURATE  transfer curve (drive / warmth asymmetry / mix) + live input dot, sine through the curve
 *   CHORUS    voice LFO traces over a 2 s window, animated from the engine's LFO phases
 *   DELAY     tap train decaying by feedback, ping-pong alternates L (up) / R (down), time bracket
 *   REVERB    pre-delay gap (zoomed) + decay envelope; damping shades the tail, MOD ripples it
 *   COMPRESS  static curve (threshold / knee / ratio / makeup), live GR bar and GR history
 */
#include "ui_priv.h"

void ui_mark_anim(void);

typedef void (*viz_fn)(gfx_t *g, int x, int y, int w, int h, bool on);

typedef struct {
    const char *title;
    int16_t on;                 /* -1: no on/off */
    const int16_t *ids;
    const char *const *labels;
    int n;
    viz_fn viz;
} panel_def_t;

#define VX 0
#define VY UI_VIZ_Y
#define VW GFX_W
#define VH UI_VIZ_H

/* ------------------------------------------------------------------ GAIN */
static void viz_gain(gfx_t *g, int x, int y, int w, int h, bool on)
{
    (void)x; (void)w; (void)h; (void)on;
    char v[12], u[6];
    ui_fmt_value(P_IN_GAIN, params_get(P_IN_GAIN), v, sizeof v, u, sizeof u);
    const int rx = 100;
    int vw = gfx_text_width(F_BIG, v);
    gfx_text(g, F_BIG, rx - vw, y + 2, v, C_ON);
    gfx_text_ex(g, F_SMALL, rx + 4, y + 2, "IN", 1, C_ON);
    gfx_text(g, F_SMALL, rx + 4, y + 15, "dB", C_ON);
    /* monitor crossfader, left column */
    float mix = params_get(P_MON_MIX) / 100.f;
    const int fx0 = UI_MX, fw = 36, fy = y + 9;
    gfx_text_ex(g, F_TINY, fx0, y + 2, "MONITOR", 0, C_ON);
    gfx_hline(g, fx0, fy + 2, fw, C_ON);
    gfx_vline(g, fx0, fy, 5, C_ON);
    gfx_vline(g, fx0 + fw - 1, fy, 5, C_ON);
    int kx = fx0 + iroundf(mix * (float)(fw - 1));
    gfx_rrect(g, kx - 2, fy - 1, 5, 7, 1, C_ON);
    gfx_vline(g, kx, fy, 5, C_OFF);
    gfx_text(g, F_TINY, fx0, fy + 8, "USB", C_ON);
    gfx_text_align(g, F_TINY, fx0 + fw, fy + 8, "IN", 0, GFX_RIGHT, C_ON);
    /* input meters */
    const int my = y + 25, mx = 12, mw = 104;
    gfx_text(g, F_TINY, UI_MX, my, "L", C_ON);
    gfx_text(g, F_TINY, UI_MX, my + 6, "R", C_ON);
    for (int c = 0; c < 2; c++) ui_meter_bar(g, mx, my + c * 6 + 1, mw, 3, U.in_db[c], U.in_pk[c], false, true);
    bool clip = (int32_t)(U.in_clip_until[0] - U.now) > 0 || (int32_t)(U.in_clip_until[1] - U.now) > 0;
    if (clip) gfx_rect(g, mx + mw + 3, my + 1, 3, 9, C_ON);
    else gfx_vline_pat(g, mx + mw + 4, my + 1, 9, 0x55, C_ON);
}

/* ------------------------------------------------------------------ SATURATE */
static float sat_curve(float xin, float drive_db, float warmth, float mix)
{
    float gd = powf(10.f, drive_db / 20.f);
    float b = warmth * 0.35f;                      /* asymmetry -> even harmonics */
    float norm = tanhf(gd + b) - tanhf(b);
    float y = (tanhf(gd * xin + b) - tanhf(b)) / (norm > 0.2f ? norm : 0.2f);
    if (y > 1.f) y = 1.f;
    if (y < -1.25f) y = -1.25f;
    return mix * y + (1.f - mix) * xin;
}

static void viz_sat(gfx_t *g, int x, int y, int w, int h, bool on)
{
    (void)x; (void)w; (void)on;
    float dr = params_get(P_SAT_DRIVE), wa = params_get(P_SAT_WARMTH) / 100.f, mx = params_get(P_SAT_MIX) / 100.f;
    const int bx = UI_MX, by = y + 1, bs = h - 2, c = bs / 2;
    /* frame corners + axes */
    gfx_hline_pat(g, bx, by + c, bs, 0x55, C_ON);
    gfx_vline_pat(g, bx + c, by, bs, 0x55, C_ON);
    for (int i = 0; i < bs; i += 3) gfx_pixel(g, bx + i, by + bs - 1 - i, C_ON);     /* unity */
    gfx_frame(g, bx - 1, by - 1, bs + 2, bs + 2, C_ON);
    gfx_rect(g, bx - 1, by - 1, 2, 1, C_OFF);
    int py = 0;
    for (int i = 0; i < bs; i++) {
        float xi = (float)(2 * i - (bs - 1)) / (float)(bs - 1);
        float yo = sat_curve(xi, dr, wa, mx);
        int yy = by + c - iroundf(yo * (float)c);
        yy = clampi_(yy, by, by + bs - 1);
        if (i) gfx_line(g, bx + i - 1, py, bx + i, yy, C_ON);
        py = yy;
    }
    /* live input dot rides the curve */
    float lv = clampf_(U.sat_in, 0.f, 1.f);
    if (lv > 0.003f) {
        int dx = bx + c + iroundf(lv * (float)c);
        int dy = by + c - iroundf(sat_curve(lv, dr, wa, mx) * (float)c);
        gfx_disc(g, dx, clampi_(dy, by, by + bs - 1), 2, C_ON);
    }
    /* right: a sine through the curve (input dotted, output solid) */
    const int wx = bx + bs + 8, ww = GFX_W - UI_MX - wx, wc = y + h / 2, amp = h / 2 - 2;
    float ph = U.t * 0.6f;
    int pyo = 0;
    for (int i = 0; i < ww; i++) {
        float s = sinf(6.2831853f * ((float)i / (float)ww * 1.5f + ph)) * 0.95f;
        int yi = wc - iroundf(s * (float)amp);
        if ((i & 1) == 0) gfx_pixel(g, wx + i, yi, C_ON);
        int yo = wc - iroundf(clampf_(sat_curve(s, dr, wa, mx), -1.2f, 1.f) * (float)amp);
        if (i) gfx_line(g, wx + i - 1, pyo, wx + i, yo, C_ON);
        pyo = yo;
    }
    ui_mark_anim();
}

/* ------------------------------------------------------------------ CHORUS */
static void viz_chorus(gfx_t *g, int x, int y, int w, int h, bool on)
{
    (void)x; (void)w; (void)on;
    int mode = params_get_int(P_CHO_MODE);
    int voices = mode == 2 ? 1 : (mode == 1 ? 3 : 2);
    float rate = params_get(P_CHO_RATE), depth = params_get(P_CHO_DEPTH) / 100.f;
    const int x0 = UI_MX, x1 = GFX_W - UI_MX - 4, cy = y + h / 2;
    const int amp = iroundf(depth * (float)(h / 2 - 3));
    const float window = 2.0f;           /* seconds shown */
    static const uint8_t pat[3] = { 0xFF, 0x55, 0x77 };
    gfx_hline_pat(g, x0, cy, x1 - x0 + 1, 0x11, C_ON);
    for (int v = 0; v < voices; v++) {
        float phase = U.viz.chorus_lfo_phase[v];
        int py = 0;
        for (int i = x0; i <= x1; i++) {
            float age = (float)(x1 - i) / (float)(x1 - x0) * window;   /* seconds into the past */
            float s = sinf(6.2831853f * (phase - age * rate));
            int yy = cy - iroundf(s * (float)amp);
            if (i > x0 && ((pat[v] >> (i & 7)) & 1)) gfx_line(g, i - 1, py, i, yy, C_ON);
            py = yy;
        }
        gfx_disc(g, x1 + 2, py, 2, C_ON);
    }
    char b[24];
    snprintf(b, sizeof b, "%d VOICE%s", voices, voices > 1 ? "S" : "");
    gfx_text(g, F_TINY, x0, y + 1, b, C_ON);
    ui_mark_anim();
}

/* ------------------------------------------------------------------ DELAY */
static void viz_delay(gfx_t *g, int x, int y, int w, int h, bool on)
{
    (void)x; (void)w; (void)on;
    const int x0 = UI_MX, x1 = GFX_W - UI_MX, cy = y + h / 2 + 3, hh = h / 2 - 5;
    const float window = 2000.f;         /* ms */
    float T = U.viz.delay_time_ms > 1.f ? U.viz.delay_time_ms : params_get(P_DLY_TIME);
    float fb = params_get(P_DLY_FB) / 100.f;
    bool pp = params_get_int(P_DLY_MODE) == 1;
    float hc = params_get(P_DLY_HICUT);
    float soft = clampf_(log2f(20000.f / hc) / 4.3f, 0.f, 1.f);    /* 0 bright .. 1 dark */
    gfx_hline_pat(g, x0, cy, x1 - x0, 0x11, C_ON);
    /* dry impulse */
    gfx_vline(g, x0, cy - hh, 2 * hh + 1, C_ON);
    float a = 1.f;
    for (int k = 1; k < 64; k++) {
        int tx = x0 + iroundf((float)k * T / window * (float)(x1 - x0));
        if (tx > x1) break;
        float db = 20.f * log10f(a + 1e-9f);
        int th = iroundf((1.f + db / 42.f) * (float)hh);
        if (th < 1) break;
        int lvl = clampi_(16 - iroundf((float)k * soft * 3.f), 4, 16);
        bool up = !pp || (k & 1);
        bool dn = !pp || !(k & 1);
        if (up) { gfx_dither(g, tx, cy - th, 2, th, lvl, C_ON); gfx_hline(g, tx, cy - th, 2, C_ON); }
        if (dn) { gfx_dither(g, tx, cy + 1, 2, th, lvl, C_ON); gfx_hline(g, tx, cy + th, 2, C_ON); }
        a *= fb;
    }
    /* time bracket between the dry hit and the first repeat */
    int t1 = x0 + iroundf(T / window * (float)(x1 - x0));
    char tb[16], u[6];
    int sync = params_get_int(P_DLY_SYNC);
    if (sync) params_format(P_DLY_SYNC, (float)sync, tb, sizeof tb);
    else { ui_fmt_value(P_DLY_TIME, T, tb, sizeof tb, u, sizeof u); size_t n = strlen(tb); snprintf(tb + n, sizeof tb - n, "%s", u); }
    int by = y + 1;
    int tw = gfx_text_width_ex(F_TINY, tb, 0);
    if (t1 - x0 >= tw + 6) {
        gfx_hline_pat(g, x0, by + 2, t1 - x0, 0x55, C_ON);
        gfx_vline(g, t1, by, 5, C_ON);
        int lx = x0 + (t1 - x0 - tw) / 2;
        gfx_rect(g, lx - 2, by, tw + 4, 5, C_OFF);
        gfx_text(g, F_TINY, lx, by, tb, C_ON);
    } else {
        gfx_vline(g, t1, by, 5, C_ON);
        gfx_text(g, F_TINY, t1 + 3, by, tb, C_ON);
    }
    if (pp) {
        gfx_text(g, F_TINY, x1 - 3, cy - hh - 1, "L", C_ON);
        gfx_text(g, F_TINY, x1 - 3, cy + hh - 3, "R", C_ON);
    }
    /* travelling echo: a cursor runs along the timeline in real time */
    float ph = fmodf(U.t * 1000.f, window) / window;
    int cx = x0 + iroundf(ph * (float)(x1 - x0));
    gfx_vline_pat(g, cx, cy - 2, 5, 0xFF, C_XOR);
    ui_mark_anim();
}

/* ------------------------------------------------------------------ REVERB */
static tw_t s_rev_win;

static void viz_reverb(gfx_t *g, int x, int y, int w, int h, bool on)
{
    (void)x; (void)w; (void)on;
    float pd = params_get(P_REV_PREDLY), rt = params_get(P_REV_DECAY);
    float damp = params_get(P_REV_DAMP) / 100.f, mod = params_get(P_REV_MOD) / 100.f;
    const int top = y + 8, bot = y + h - 1, eh = bot - top;
    /* zone A: pre-delay, 0..150 ms over 22 px */
    const int ax0 = UI_MX, ax1 = UI_MX + 22;
    int onset = ax0 + iroundf(pd / 150.f * (float)(ax1 - ax0));
    gfx_vline_pat(g, ax0, top, eh + 1, 0x55, C_ON);                 /* dry hit */
    gfx_hline(g, ax0, bot, onset - ax0, C_ON);
    gfx_vline(g, onset, top, eh + 1, C_ON);
    gfx_hline(g, onset, top, ax1 - onset + 1, C_ON);
    gfx_dither(g, onset + 1, top + 1, ax1 - onset, eh, 8, C_ON);
    char b[12];
    snprintf(b, sizeof b, "%.0fMS", (double)pd);
    gfx_text(g, F_TINY, ax0, y + 1, b, C_ON);
    /* axis break */
    gfx_line(g, ax1 + 2, bot + 1, ax1 + 4, bot - 3, C_ON);
    gfx_line(g, ax1 + 4, bot + 1, ax1 + 6, bot - 3, C_ON);
    /* zone B: decay, linear time, window quantised to octaves and gliding when it changes */
    const int bx0 = ax1 + 8, bx1 = GFX_W - UI_MX;
    float win = 1.f;
    while (win < rt * 1.15f && win < 16.f) win *= 2.f;
    tw_to(&s_rev_win, win);
    if (s_rev_win.v < 0.5f) tw_snap(&s_rev_win, win);
    if (tw_step(&s_rev_win, U.dt, 0.08f)) ui_mark_anim();
    win = s_rev_win.v;
    float k = 6.9078f / rt;               /* ln(1000): amplitude falls 60 dB at t = rt */
    float kd = k * (1.f + 3.f * damp);    /* high frequencies die faster with damping */
    int py = top;
    for (int i = bx0; i <= bx1; i++) {
        float t = (float)(i - bx0) / (float)(bx1 - bx0) * win;
        float a = expf(-k * t);
        float ripple = mod * 0.9f * sinf((float)i * 0.55f + U.t * 2.5f) * a;
        int yy = bot - iroundf((a * (float)eh) + ripple);
        yy = clampi_(yy, top, bot);
        float hf = expf(-kd * t);
        int lvl = clampi_(iroundf(hf * 9.f), 0, 9);
        if (lvl > 0 && yy < bot) gfx_dither(g, i, yy + 1, 1, bot - yy, lvl, C_ON);
        if (i > bx0) gfx_line(g, i - 1, py, i, yy, C_ON);
        py = yy;
    }
    gfx_hline(g, bx0, bot, bx1 - bx0 + 1, C_ON);
    /* RT60 marker */
    int rx = bx0 + iroundf(rt / win * (float)(bx1 - bx0));
    if (rx <= bx1) {
        gfx_vline_pat(g, rx, top, eh, 0x55, C_ON);
        char u[6];
        ui_fmt_value(P_REV_DECAY, rt, b, sizeof b, u, sizeof u);
        size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, "S");
        int tw = gfx_text_width(F_TINY, b);
        int lx = rx + 3 + tw <= bx1 ? rx + 3 : rx - 2 - tw;
        gfx_text(g, F_TINY, lx, y + 1, b, C_ON);
    }
    if (mod > 0.01f) ui_mark_anim();
}

/* ------------------------------------------------------------------ COMPRESS */
static uint8_t s_gr_hist[80];
static int s_gr_head;
static float s_gr_acc;

static float comp_out(float in, float T, float R, float K, float M)
{
    float o;
    if (2.f * (in - T) < -K) o = in;
    else if (2.f * fabsf(in - T) <= K && K > 0.f) { float d = in - T + K / 2.f; o = in + (1.f / R - 1.f) * d * d / (2.f * K); }
    else o = T + (in - T) / R;
    return o + M;
}

static void viz_comp(gfx_t *g, int x, int y, int w, int h, bool on)
{
    (void)x; (void)w; (void)on;
    float T = params_get(P_CMP_THRESH), R = params_get(P_CMP_RATIO), K = params_get(P_CMP_KNEE), M = params_get(P_CMP_MAKEUP);
    const int bx = UI_MX, by = y + 1, bs = h - 2;
    const float lo = -48.f;
    /* transfer box: -48..0 dB both axes */
    gfx_frame(g, bx - 1, by - 1, bs + 2, bs + 2, C_ON);
    for (int i = 0; i < bs; i += 3) gfx_pixel(g, bx + i, by + bs - 1 - i, C_ON);
    int tx = bx + iroundf((T - lo) / -lo * (float)(bs - 1));
    gfx_vline_pat(g, tx, by, bs, 0x11, C_ON);
    int py = 0;
    for (int i = 0; i < bs; i++) {
        float in = lo + (float)i / (float)(bs - 1) * -lo;
        float o = comp_out(in, T, R, K, M);
        int yy = by + bs - 1 - iroundf((o - lo) / -lo * (float)(bs - 1));
        yy = clampi_(yy, by, by + bs - 1);
        if (i) gfx_line(g, bx + i - 1, py, bx + i, yy, C_ON);
        py = yy;
    }
    /* live operating point from the current gain reduction (inverse of the static curve) */
    float gr = U.comp_gr_db;
    if (gr > 0.1f && R > 1.01f) {
        float in = T + gr / (1.f - 1.f / R);
        if (in < 0.f) {
            float o = comp_out(in, T, R, K, M);
            int dx = bx + iroundf((in - lo) / -lo * (float)(bs - 1));
            int dy = by + bs - 1 - iroundf((o - lo) / -lo * (float)(bs - 1));
            gfx_disc(g, dx, clampi_(dy, by, by + bs - 1), 2, C_ON);
        }
    }
    /* GR bar (grows downward) + history */
    const int gx = bx + bs + 5, gh = bs;
    int gpx = iroundf(clampf_(gr / 12.f, 0.f, 1.f) * (float)gh);
    gfx_vline_pat(g, gx + 1, by, gh, 0x11, C_ON);
    gfx_rect(g, gx, by, 3, gpx, C_ON);
    const int hx0 = gx + 8, hw = GFX_W - UI_MX - hx0;
    for (int i = 0; i < hw; i++) {
        int idx = (s_gr_head - hw + i + 160) % 80;
        int v = s_gr_hist[idx];
        if (v) {
            gfx_dither(g, hx0 + i, by, 1, v, 5, C_ON);
            gfx_pixel(g, hx0 + i, by + v, C_ON);
        } else if ((i & 3) == 0) {
            gfx_pixel(g, hx0 + i, by, C_ON);
        }
    }
    char b[12];
    snprintf(b, sizeof b, "GR %.1f", (double)gr);
    gfx_rect(g, GFX_W - UI_MX - gfx_text_width(F_TINY, b) - 2, by + bs - 6, gfx_text_width(F_TINY, b) + 2, 6, C_OFF);
    gfx_text_align(g, F_TINY, GFX_W - UI_MX, by + bs - 5, b, 0, GFX_RIGHT, C_ON);
}

static void comp_hist_tick(float dt)
{
    s_gr_acc += dt;
    while (s_gr_acc >= 1.f / 25.f) {
        s_gr_acc -= 1.f / 25.f;
        s_gr_hist[s_gr_head] = (uint8_t)clampi_(iroundf(U.comp_gr_db / 12.f * (float)(UI_VIZ_H - 3)), 0, UI_VIZ_H - 3);
        s_gr_head = (s_gr_head + 1) % 80;
    }
    ui_mark_anim();
}

/* ------------------------------------------------------------------ definitions */
static const int16_t k_gain_ids[] = { P_IN_GAIN, P_IN_MODE, P_MON_MIX, P_MON_PATH };
static const char *const k_gain_lab[] = { "GAIN", "INPUT", "MONITOR", "PATH" };
static const int16_t k_sat_ids[] = { P_SAT_ON, P_SAT_DRIVE, P_SAT_WARMTH, P_SAT_TONE, P_SAT_MIX };
static const int16_t k_cho_ids[] = { P_CHO_ON, P_CHO_MODE, P_CHO_RATE, P_CHO_DEPTH, P_CHO_MIX };
static const int16_t k_dly_ids[] = { P_DLY_ON, P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MODE, P_DLY_HICUT, P_DLY_LOCUT, P_DLY_MIX };
static const int16_t k_rev_ids[] = { P_REV_ON, P_REV_DECAY, P_REV_PREDLY, P_REV_DAMP, P_REV_MOD, P_REV_LOCUT, P_REV_WIDTH, P_REV_MIX };
static const int16_t k_cmp_ids[] = { P_CMP_ON, P_CMP_THRESH, P_CMP_RATIO, P_CMP_ATTACK, P_CMP_RELEASE, P_CMP_KNEE, P_CMP_MAKEUP, P_CMP_MIX };
static const char *const k_fx_lab0[] = { "EFFECT" };

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const panel_def_t k_panels[6] = {
    { "GAIN", -1, k_gain_ids, k_gain_lab, N(k_gain_ids), viz_gain },
    { "SATURATE", P_SAT_ON, k_sat_ids, k_fx_lab0, N(k_sat_ids), viz_sat },
    { "CHORUS", P_CHO_ON, k_cho_ids, k_fx_lab0, N(k_cho_ids), viz_chorus },
    { "DELAY", P_DLY_ON, k_dly_ids, k_fx_lab0, N(k_dly_ids), viz_delay },
    { "REVERB", P_REV_ON, k_rev_ids, k_fx_lab0, N(k_rev_ids), viz_reverb },
    { "COMPRESS", P_CMP_ON, k_cmp_ids, k_fx_lab0, N(k_cmp_ids), viz_comp },
};

/* labels: only the first cell is overridden for effects; build full arrays lazily */
static const char *s_labels[6][8];
static strip_t s_strip[6];

static void panel_enter(int i)
{
    const panel_def_t *d = &k_panels[i];
    for (int k = 0; k < d->n; k++)
        s_labels[i][k] = (d->labels && (i == 0 || k == 0)) ? d->labels[k] : ui_short_label((param_id_t)d->ids[k]);
    s_strip[i].cur = 0;
    strip_init(&s_strip[i], d->ids, s_labels[i], d->n, UI_CELL_W);
}

static void panel_input(int i, ui_act_t a, int raw)
{
    strip_t *s = &s_strip[i];
    /* DELAY: turning TIME while tempo-synced switches to FREE at the current effective time */
    if (i == 3 && s->edit && a == A_ROT && s->params[s->cur] == P_DLY_TIME && params_get_int(P_DLY_SYNC) != 0) {
        float eff = U.viz.delay_time_ms > 1.f ? U.viz.delay_time_ms : params_get(P_DLY_TIME);
        params_set(P_DLY_SYNC, 0.f, SRC_UI);
        params_set(P_DLY_TIME, eff, SRC_UI);
    }
    if (!strip_input(s, a, raw) && a == A_BACK) ui_pop();
}

static void panel_tick(int i, float dt)
{
    strip_tick(&s_strip[i], dt);
    if (i == 5) comp_hist_tick(dt);
}

static void panel_draw(int i, gfx_t *g)
{
    const panel_def_t *d = &k_panels[i];
    bool on = d->on < 0 || params_get_bool((param_id_t)d->on);
    gfx_text_ex(g, F_SMALL, UI_MX, 1, d->title, 1, C_ON);
    if (d->on >= 0) ui_onoff_pill(g, on);
    else {
        char m[12];
        params_format(P_IN_MODE, params_get(P_IN_MODE), m, sizeof m);
        gfx_text_align(g, F_SMALL, GFX_W - UI_MX, 1, m, 1, GFX_RIGHT, C_ON);
    }
    gfx_state_t st = gfx_save(g);
    gfx_clip(g, VX, VY, VW, VH);
    d->viz(g, VX, VY, VW, VH, on);
    gfx_restore(g, st);
    if (!on) gfx_fade(g, 0, VY, GFX_W, VH, 5);
    strip_draw(g, &s_strip[i], UI_STRIP_Y);
    /* DELAY: TIME is inactive while synced */
    if (i == 3 && params_get_int(P_DLY_SYNC) != 0) {
        strip_t *s = &s_strip[i];
        int total = s->n * s->cell_w, x0 = total <= GFX_W ? (GFX_W - total) / 2 : 1;
        int cx = x0 + 2 * s->cell_w - iroundf(s->scroll.v);
        if (!(s->edit && s->cur == 2)) gfx_fade(g, cx + 2, UI_STRIP_Y + 8, s->cell_w - 4, GFX_H - UI_STRIP_Y - 9, 7);
    }
}

#define PANEL(idx, name)                                                              \
    static void name##_enter(void) { panel_enter(idx); }                            \
    static void name##_input(ui_act_t a, int raw) { panel_input(idx, a, raw); }     \
    static void name##_tick(float dt) { panel_tick(idx, dt); }                      \
    static void name##_draw(gfx_t *g) { panel_draw(idx, g); }                       \
    const screen_t g_scr_##name = { name##_enter, name##_input, name##_tick, name##_draw, NULL };

PANEL(0, gain)
PANEL(1, sat)
PANEL(2, cho)
PANEL(3, dly)
PANEL(4, rev)
PANEL(5, cmp)
