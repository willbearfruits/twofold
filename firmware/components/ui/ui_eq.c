/*
 * ui_eq.c: EQ screen. The curve is the product of the 5 bands' magnitude responses evaluated from
 * the actual float coefficients that rbj_design() produces for the engine (same function, same fs).
 *
 * Cost control: |H|^2 uses the phi form (phi = sin^2(w/2)), which is numerically safe in float at
 * 20 Hz where the direct cos/sin form cancels. Per band we convert the coefficients ONCE (in double)
 * into 6 float polynomial terms; per column it is then 2 Horner steps per band and one log10f per
 * column, only when an EQ parameter changes. The column phi table is computed at init.
 */
#include "ui_priv.h"

void ui_mark_anim(void);

#define EQ_W     128
#define EQ_GY0   9          /* graph top */
#define EQ_GH    37         /* 9..45 */
#define EQ_ZERO  27         /* 0 dB row */
#define EQ_FS    48000.f

typedef struct { float n0, n1, n2, d0, d1, d2; } phi_poly_t;

static float s_phi[EQ_W];             /* sin^2(pi f / fs) per column */
static phi_poly_t s_poly[EQ_BANDS];
static float s_target[EQ_W];          /* combined response, dB */
static float s_disp[EQ_W];            /* displayed (morphs toward target) */
static float s_band_db[EQ_W];         /* selected band alone, dB */
static int s_band_cached = -1;
static bool s_inited;

static const param_id_t k_band_p[EQ_BANDS][4] = {
    { P_EQ_B1_FREQ, P_EQ_B1_GAIN, P_EQ_B1_Q, P_EQ_B1_ON },
    { P_EQ_B2_FREQ, P_EQ_B2_GAIN, P_EQ_B2_Q, P_EQ_B2_ON },
    { P_EQ_B3_FREQ, P_EQ_B3_GAIN, P_EQ_B3_Q, P_EQ_B3_ON },
    { P_EQ_B4_FREQ, P_EQ_B4_GAIN, P_EQ_B4_Q, P_EQ_B4_ON },
    { P_EQ_B5_FREQ, P_EQ_B5_GAIN, P_EQ_B5_Q, P_EQ_B5_ON },
};

float eq_x_to_hz(int x, int w) { return 20.f * powf(1000.f, (float)x / (float)(w - 1)); }
int eq_hz_to_x(float hz, int w) { return iroundf(log10f(hz / 20.f) / 3.f * (float)(w - 1)); }

static void band_poly(int b, phi_poly_t *p)
{
    biquad_coef_t c;
    bool on = params_get_bool(k_band_p[b][3]);
    if (!on) { *p = (phi_poly_t){ 1.f, 0.f, 0.f, 1.f, 0.f, 0.f }; return; }
    rbj_design(&c, eq_band_type(b), EQ_FS, params_get(k_band_p[b][0]), params_get(k_band_p[b][1]), params_get(k_band_p[b][2]));
    const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
    double sb = b0 + b1 + b2, sa = 1.0 + a1 + a2;
    p->n0 = (float)(sb * sb);
    p->n1 = (float)(-4.0 * (b0 * b1 + 4.0 * b0 * b2 + b1 * b2));
    p->n2 = (float)(16.0 * b0 * b2);
    p->d0 = (float)(sa * sa);
    p->d1 = (float)(-4.0 * (a1 + 4.0 * a2 + a1 * a2));
    p->d2 = (float)(16.0 * a2);
}

static inline float poly_mag2(const phi_poly_t *p, float ph)
{
    float n = p->n0 + ph * (p->n1 + ph * p->n2);
    float d = p->d0 + ph * (p->d1 + ph * p->d2);
    return n / (d > 1e-30f ? d : 1e-30f);
}

void eq_model_update(void)
{
    if (!s_inited) {
        for (int x = 0; x < EQ_W; x++) {
            float s = sinf(3.14159265f * eq_x_to_hz(x, EQ_W) / EQ_FS);
            s_phi[x] = s * s;
        }
    }
    for (int b = 0; b < EQ_BANDS; b++) band_poly(b, &s_poly[b]);
    for (int x = 0; x < EQ_W; x++) {
        float m = 1.f;
        for (int b = 0; b < EQ_BANDS; b++) m *= poly_mag2(&s_poly[b], s_phi[x]);
        s_target[x] = 10.f * log10f(m > 1e-12f ? m : 1e-12f);
        if (!s_inited) s_disp[x] = s_target[x];
    }
    s_inited = true;
    s_band_cached = -1;
}

/* exported for the host self-test: combined response at column x (dB) */
float eq_debug_column_db(int x) { return s_target[x]; }

/* ------------------------------------------------------------------ screen state */
typedef enum { M_BAND = 0, M_FIELD, M_GLOBAL } eq_mode_t;
static struct {
    int band;              /* 0..4 bands, 5 = global page */
    eq_mode_t mode;
    strip_t strip;
    int16_t ids[4];
    tw_t nodex, nodey;     /* selection ring glides between nodes */
    tw_t page;             /* 0 = band strip, 1 = global strip (crossfade) */
} E;

static const char *const k_band_names[EQ_BANDS] = { "LOW SHELF", "PEAK 1", "PEAK 2", "PEAK 3", "HIGH SHELF" };
static const char *const k_field_labels[4] = { "FREQ", "GAIN", "Q", "BAND" };
static const char *const k_shelf_labels[4] = { "FREQ", "GAIN", "SLOPE", "BAND" };
static const int16_t k_global_ids[3] = { P_EQ_ON, P_EQ_IN, P_EQ_OUT };
static const char *const k_global_labels[3] = { "EQ", "IN TRIM", "OUT TRIM" };

static int node_x(int b) { return eq_hz_to_x(params_get(k_band_p[b][0]), EQ_W); }
static float node_db(int b)
{
    float g = params_get(k_band_p[b][1]);
    return eq_band_type(b) == RBJ_PEAK ? g : g * 0.5f;
}
static int db_to_y(float db) { return EQ_ZERO - iroundf(clampf_(db, -18.f, 18.f)); }

static void load_strip(void)
{
    if (E.band < EQ_BANDS) {
        for (int i = 0; i < 4; i++) E.ids[i] = (int16_t)k_band_p[E.band][i];
        int keep = E.strip.cur;
        strip_init(&E.strip, E.ids, eq_band_type(E.band) == RBJ_PEAK ? k_field_labels : k_shelf_labels, 4, 32);
        E.strip.cur = keep;
    } else {
        strip_init(&E.strip, k_global_ids, k_global_labels, 3, 42);
    }
}

static void eq_enter(void)
{
    if (E.band < 0 || E.band > EQ_BANDS) E.band = 0;
    E.mode = M_BAND;
    E.strip.cur = 0;
    load_strip();
    int b = E.band < EQ_BANDS ? E.band : 0;
    tw_snap(&E.nodex, (float)node_x(b));
    tw_snap(&E.nodey, (float)db_to_y(node_db(b)));
    tw_snap(&E.page, E.band == EQ_BANDS ? 1.f : 0.f);
}

static void eq_input(ui_act_t a, int raw)
{
    switch (E.mode) {
    case M_BAND:
        if (a == A_ROT) {
            int nb = clampi_(E.band + raw, 0, EQ_BANDS);
            if (nb != E.band) {
                E.band = nb;
                E.strip.cur = 0;
                load_strip();
                tw_to(&E.page, E.band == EQ_BANDS ? 1.f : 0.f);
            }
        } else if (a == A_PRESS) {
            E.strip.cur = 0;
            E.mode = E.band < EQ_BANDS ? M_FIELD : M_GLOBAL;
            if (E.mode == M_FIELD) E.strip.edit = true;
            strip_init(&E.strip, E.strip.params, E.strip.labels, E.strip.n, E.strip.cell_w);
            if (E.mode == M_FIELD) E.strip.edit = true;
        } else if (a == A_BACK) {
            ui_pop();
        }
        break;
    case M_FIELD:
        if (a == A_ROT) {
            param_id_t id = (param_id_t)E.ids[E.strip.cur];
            if (params_desc(id)->type == PT_BOOL) { if (raw) ui_param_toggle(id); }
            else ui_param_rotate(id, raw, &E.strip.carry);
        } else if (a == A_PRESS) {
            if (E.strip.cur < 3) { E.strip.cur++; E.strip.carry = 0.f; tw_to(&E.strip.hl, (float)(E.strip.cur * E.strip.cell_w)); }
            else { E.mode = M_BAND; E.strip.edit = false; }
        } else if (a == A_BACK) {
            E.mode = M_BAND;
            E.strip.edit = false;
        }
        break;
    case M_GLOBAL:
        if (!strip_input(&E.strip, a, raw) && a == A_BACK) E.mode = M_BAND;
        break;
    }
}

static void eq_tick(float dt)
{
    bool moving = false;
    float a = dt / (0.06f + dt);
    for (int x = 0; x < EQ_W; x++) {
        float d = s_target[x] - s_disp[x];
        if (fabsf(d) > 0.05f) { s_disp[x] += d * a; moving = true; }
        else s_disp[x] = s_target[x];
    }
    int b = E.band < EQ_BANDS ? E.band : -1;
    if (b >= 0) {
        tw_to(&E.nodex, (float)node_x(b));
        tw_to(&E.nodey, (float)db_to_y(node_db(b)));
    }
    moving |= tw_step(&E.nodex, dt, 0.04f);
    moving |= tw_step(&E.nodey, dt, 0.04f);
    moving |= tw_step(&E.page, dt, 0.05f);
    moving |= strip_tick(&E.strip, dt);
    if (params_get_bool(P_UI_SPECTRUM) && params_get_bool(P_EQ_ON)) { spec_update(dt); moving = true; }
    if (moving) ui_mark_anim();
}

static void band_curve(int b)
{
    if (s_band_cached == b) return;
    for (int x = 0; x < EQ_W; x++) {
        float m = poly_mag2(&s_poly[b], s_phi[x]);
        s_band_db[x] = 10.f * log10f(m > 1e-12f ? m : 1e-12f);
    }
    s_band_cached = b;
}

static void draw_grid(gfx_t *g)
{
    static const float fl[] = { 100.f, 1000.f, 10000.f };
    static const char *const fn[] = { "100", "1K", "10K" };
    for (int i = 0; i < 3; i++) {
        int x = eq_hz_to_x(fl[i], EQ_W);
        gfx_vline_pat(g, x, EQ_GY0, EQ_GH, 0x11, C_ON);
        gfx_text(g, F_TINY, x + 2, EQ_GY0 + EQ_GH - 5, fn[i], C_ON);
    }
    gfx_hline_pat(g, 0, EQ_ZERO, EQ_W, 0x11, C_ON);
    /* +-12 dB ticks on both edges */
    for (int s = -1; s <= 1; s += 2) {
        gfx_hline(g, 0, EQ_ZERO + s * 12, 2, C_ON);
        gfx_hline(g, EQ_W - 2, EQ_ZERO + s * 12, 2, C_ON);
    }
}

static void draw_spectrum(gfx_t *g)
{
    const uint8_t *col = spec_columns();
    const int base = EQ_GY0 + EQ_GH;     /* bottom */
    for (int x = 0; x < EQ_W; x++) {
        int h = col[x] >> 2;
        if (h <= 0) continue;
        if (h > EQ_GH) h = EQ_GH;
        /* a faint texture under the curve: sparse dither, top edge only on even columns */
        gfx_dither(g, x, base - h, 1, h, 1, C_ON);
        if (!(x & 1)) gfx_pixel(g, x, base - h, C_ON);
    }
}

static void draw_curve(gfx_t *g, bool on)
{
    int py = db_to_y(s_disp[0]);
    for (int x = 0; x < EQ_W; x++) {
        int y = db_to_y(s_disp[x]);
        int y0 = y, y1 = y;
        if (x > 0) {
            /* connect to the previous column through the midpoint */
            if (py < y) y0 = (py + y + 1) / 2; else if (py > y) y1 = (py + y) / 2;
        }
        if (on) gfx_vline(g, x, y0, y1 - y0 + 1, C_ON);
        else if (x % 2 == 0) gfx_pixel(g, x, y, C_ON);
        if (x > 0 && on) {
            /* the previous column's half of the step */
            if (py < y) gfx_vline(g, x - 1, py, (py + y + 1) / 2 - py, C_ON);
            else if (py > y) gfx_vline(g, x - 1, (py + y) / 2 + 1, py - (py + y) / 2 - 1, C_ON);
        }
        py = y;
    }
}

static void draw_band_fill(gfx_t *g, int b)
{
    band_curve(b);
    for (int x = 0; x < EQ_W; x++) {
        int y = db_to_y(s_band_db[x]);
        if (y < EQ_ZERO) gfx_dither(g, x, y, 1, EQ_ZERO - y, 5, C_ON);
        else if (y > EQ_ZERO) gfx_dither(g, x, EQ_ZERO + 1, 1, y - EQ_ZERO, 5, C_ON);
    }
}

static void draw_nodes(gfx_t *g)
{
    for (int b = 0; b < EQ_BANDS; b++) {
        if (b == E.band) continue;
        int x = node_x(b), y = db_to_y(node_db(b));
        gfx_disc(g, x, y, 3, C_OFF);
        if (params_get_bool(k_band_p[b][3])) gfx_circle(g, x, y, 2, C_ON);
        else { gfx_pixel(g, x - 2, y, C_ON); gfx_pixel(g, x + 2, y, C_ON); gfx_pixel(g, x, y - 2, C_ON); gfx_pixel(g, x, y + 2, C_ON); }
    }
    if (E.band < EQ_BANDS) {
        int x = iroundf(E.nodex.v), y = iroundf(E.nodey.v);
        gfx_disc(g, x, y, 5, C_OFF);
        gfx_disc(g, x, y, 3, C_ON);
        if (E.mode == M_FIELD) gfx_circle(g, x, y, 5, C_ON);
        if (!params_get_bool(k_band_p[E.band][3])) gfx_disc(g, x, y, 1, C_OFF);
    }
}

static void eq_draw(gfx_t *g)
{
    bool on = params_get_bool(P_EQ_ON);
    const char *right = E.band < EQ_BANDS ? k_band_names[E.band] : "GLOBAL";
    gfx_text_ex(g, F_SMALL, UI_MX, 1, "EQ", 1, C_ON);
    if (!on) ui_pill(g, UI_MX + 14, 0, "BYPASS", false, GFX_LEFT);
    gfx_text_align(g, F_SMALL, GFX_W - UI_MX, 1, right, 1, GFX_RIGHT, C_ON);

    gfx_state_t st = gfx_save(g);
    gfx_clip(g, 0, EQ_GY0, EQ_W, EQ_GH);
    draw_grid(g);
    if (on && params_get_bool(P_UI_SPECTRUM)) draw_spectrum(g);
    if (on && E.band < EQ_BANDS) draw_band_fill(g, E.band);
    draw_curve(g, on);
    draw_nodes(g);
    gfx_restore(g, st);

    /* strip: band fields or global page; in band-select mode nothing is highlighted */
    if (E.mode == M_BAND) {
        /* draw without highlight by drawing then erasing the frame region */
        strip_t tmp = E.strip;
        tmp.hl.v = -1000.f;
        strip_draw(g, &tmp, UI_STRIP_Y);
    } else {
        strip_draw(g, &E.strip, UI_STRIP_Y);
    }
}

const screen_t g_scr_eq = { eq_enter, eq_input, eq_tick, eq_draw, NULL };
