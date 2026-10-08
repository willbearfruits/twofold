/*
 * ui_boot.c: boot animation (1.25 s + 0.2 s dissolve into HOME) and the screensaver.
 *
 * Boot, "the fold": a hairline grows from the centre, then opens like a folded sheet, revealing the
 * wordmark from its centre row outwards while the two halves of the line ride the edges; the tagline
 * resolves in through the dither ramp. A button press skips it.
 */
#include "ui_priv.h"

void ui_mark_anim(void);

#define BOOT_MS 1250

static uint32_t s_t0;

static void boot_enter(void) { s_t0 = U.now; }
static void boot_input(ui_act_t a, int raw) { (void)raw; if (a == A_PRESS || a == A_DOUBLE) ui_go_home(); }
static void boot_tick(float dt)
{
    (void)dt;
    ui_mark_anim();
    if (U.now - s_t0 >= BOOT_MS && ui_current() == SCR_BOOT) ui_go_home();
}

static float seg(uint32_t t, uint32_t a, uint32_t b)
{
    if (t <= a) return 0.f;
    if (t >= b) return 1.f;
    return (float)(t - a) / (float)(b - a);
}

static void boot_draw(gfx_t *g)
{
    uint32_t t = U.now - s_t0;
    const int ww = gfx_wordmark.w, wh = gfx_wordmark.h;
    const int wx = (GFX_W - ww) / 2, wy = 22, cy = wy + wh / 2;
    /* 1: hairline grows from the centre */
    float p1 = ease_out_cubic(seg(t, 0, 340));
    /* 2: unfold */
    float p2 = ease_in_out(seg(t, 240, 760));
    int half = iroundf(p2 * (float)(wh / 2 + 1));
    if (half > 0) {
        gfx_state_t st = gfx_save(g);
        gfx_clip(g, 0, cy - half, GFX_W, 2 * half);
        gfx_bitmap(g, wx, wy, &gfx_wordmark, C_ON);
        gfx_restore(g, st);
    }
    int lw = iroundf(p1 * (float)(ww + 8));
    float p_out = seg(t, 760, 1000);
    if (p_out < 1.f && lw > 0) {
        int lx = GFX_W / 2 - lw / 2;
        int lvl = 16 - iroundf(p_out * 16.f);
        int ya = cy - half - (half ? 2 : 0), yb = cy + half + (half ? 1 : 0);
        gfx_dither(g, lx, ya, lw, 1, lvl, C_ON);
        if (half) gfx_dither(g, lx, yb, lw, 1, lvl, C_ON);
    }
    /* 3: tagline resolves through the dither ramp */
    float p3 = seg(t, 700, 1050);
    if (p3 > 0.f) {
        const char *tag = "USB AUDIO INTERFACE";
        gfx_text_align(g, F_TINY, GFX_W / 2, wy + wh + 9, tag, 2, GFX_CENTER, C_ON);
        gfx_fade(g, 0, wy + wh + 8, GFX_W, 7, iroundf(p3 * 16.f));
    }
}

const screen_t g_scr_boot = { boot_enter, boot_input, boot_tick, boot_draw, NULL };

/* ------------------------------------------------------------------ screensaver */
/* A tiny volume readout drifting on slow, incommensurate sines (period minutes), so no pixel stays
 * lit for long. Drawn at the panel's floor brightness; any input wakes and is swallowed. */
static void saver_draw(gfx_t *g)
{
    char b[16];
    float v = params_get(P_VOLUME);
    if (params_get_bool(P_MUTE)) snprintf(b, sizeof b, "MUTE");
    else if (v <= params_desc(P_VOLUME)->min + 0.01f) snprintf(b, sizeof b, "-" GFX_S_INF);
    else snprintf(b, sizeof b, "%.1f", (double)(roundf(v * 2.f) * 0.5f));
    int tw = gfx_text_width(F_MED, b) + 2 + gfx_text_width(F_SMALL, "dB");
    float t = U.t;
    float fx = 0.5f + 0.5f * sinf(t * 0.0371f) * cosf(t * 0.0113f + 0.7f);
    float fy = 0.5f + 0.5f * sinf(t * 0.0297f + 1.3f);
    int x = iroundf(fx * (float)(GFX_W - tw - 2)) + 1;
    int y = iroundf(fy * (float)(GFX_H - 9)) + 1;
    int xe = gfx_text(g, F_MED, x, y, b, C_ON);
    if (strcmp(b, "MUTE")) gfx_text(g, F_SMALL, xe + 2, y + 2, "dB", C_ON);
}

const screen_t g_scr_saver = { NULL, NULL, NULL, saver_draw, NULL };

/* ------------------------------------------------------------------ screen table */
extern const screen_t g_scr_home, g_scr_menu, g_scr_eq, g_scr_gain, g_scr_fxlist, g_scr_sat, g_scr_cho,
    g_scr_dly, g_scr_rev, g_scr_cmp, g_scr_loud, g_scr_xf, g_scr_net, g_scr_net_setup, g_scr_bt, g_scr_presets, g_scr_settings,
    g_scr_about, g_scr_diag;

const screen_t *const g_screens[SCR_COUNT] = {
    [SCR_BOOT] = &g_scr_boot, [SCR_HOME] = &g_scr_home, [SCR_MENU] = &g_scr_menu, [SCR_EQ] = &g_scr_eq,
    [SCR_GAIN] = &g_scr_gain, [SCR_FXLIST] = &g_scr_fxlist, [SCR_FX_SAT] = &g_scr_sat,
    [SCR_FX_CHO] = &g_scr_cho, [SCR_FX_DLY] = &g_scr_dly, [SCR_FX_REV] = &g_scr_rev,
    [SCR_FX_CMP] = &g_scr_cmp, [SCR_FX_LOUD] = &g_scr_loud, [SCR_FX_XF] = &g_scr_xf, [SCR_NET] = &g_scr_net, [SCR_NET_SETUP] = &g_scr_net_setup,
    [SCR_BT] = &g_scr_bt, [SCR_PRESETS] = &g_scr_presets, [SCR_SETTINGS] = &g_scr_settings,
    [SCR_ABOUT] = &g_scr_about, [SCR_DIAG] = &g_scr_diag, [SCR_SAVER] = &g_scr_saver,
};
