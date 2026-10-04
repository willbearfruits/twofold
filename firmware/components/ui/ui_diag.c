/* ui_diag.c: hidden DIAGNOSTICS (SETTINGS > ABOUT, hold 3 s). Page 1 CPU per DSP stage, page 2 system. */
#include "ui_priv.h"

void ui_mark_anim(void);

static struct { int page; tw_t px; sys_diag_t d; uint32_t last; } D;

static void diag_enter(void)
{
    D.page = 0;
    tw_snap(&D.px, 0.f);
    sys_get_diag(&D.d);
    D.last = U.now;
}

static void diag_input(ui_act_t a, int raw)
{
    if (a == A_ROT) { D.page = clampi_(D.page + (raw > 0 ? 1 : -1), 0, 1); tw_to(&D.px, (float)(D.page * GFX_W)); }
    else if (a == A_BACK) ui_pop();
    else if (a == A_PRESS) { D.page ^= 1; tw_to(&D.px, (float)(D.page * GFX_W)); }
}

static void diag_tick(float dt)
{
    if (U.now - D.last >= 250) { sys_get_diag(&D.d); D.last = U.now; }
    if (tw_step(&D.px, dt, 0.05f)) ui_mark_anim();
}

static void page_cpu(gfx_t *g, int ox)
{
    const dsp_prof_t *p = &D.d.prof;
    const int lx = ox + UI_MX, bx = ox + 32, bw = 70, vx = ox + GFX_W - UI_MX;
    const float full = 12.f;               /* % of one core at full bar */
    char b[16];
    for (int i = 0; i < DSP_ST_COUNT; i++) {
        int y = UI_TOP + 1 + i * 6;
        gfx_text(g, F_TINY, lx, y, dsp_stage_names[i], C_ON);
        gfx_hline_pat(g, bx, y + 2, bw, 0x11, C_ON);
        int aw = iroundf(clampf_(p->avg_pct[i] / full, 0.f, 1.f) * (float)bw);
        int mw = iroundf(clampf_(p->max_pct[i] / full, 0.f, 1.f) * (float)(bw - 1));
        gfx_rect(g, bx, y + 1, aw, 3, C_ON);
        gfx_vline(g, bx + mw, y, 5, C_ON);
        snprintf(b, sizeof b, "%.1f", (double)p->avg_pct[i]);
        gfx_text_align(g, F_TINY, vx, y, b, 0, GFX_RIGHT, C_ON);
    }
}

static void kv(gfx_t *g, int x, int y, const char *k, const char *v)
{
    gfx_text(g, F_TINY, x, y, k, C_ON);
    gfx_text_align(g, F_TINY, x + 58, y, v, 0, GFX_RIGHT, C_ON);
}

static void fmt_bytes(char *b, size_t n, uint32_t v)
{
    if (v >= 1048576u) snprintf(b, n, "%.2fM", (double)v / 1048576.0);
    else snprintf(b, n, "%luK", (unsigned long)(v / 1024u));
}

static void page_sys(gfx_t *g, int ox)
{
    const sys_diag_t *d = &D.d;
    char b[24];
    int xl = ox + UI_MX, xr = ox + 66, y = UI_TOP + 1;
    snprintf(b, sizeof b, "%lu", (unsigned long)d->xrun_play); kv(g, xl, y, "XRUN PLAY", b);
    snprintf(b, sizeof b, "%lu", (unsigned long)d->xrun_rec); kv(g, xr, y, "XRUN REC", b);
    y += 7;
    snprintf(b, sizeof b, "%ld", (long)d->usb_out_fifo); kv(g, xl, y, "FIFO OUT", b);
    snprintf(b, sizeof b, "%ld", (long)d->usb_in_fifo); kv(g, xr, y, "FIFO IN", b);
    y += 7;
    snprintf(b, sizeof b, "%.4f", (double)d->usb_feedback); kv(g, xl, y, "FEEDBACK", b);
    snprintf(b, sizeof b, "%lu", (unsigned long)d->i2s_errors); kv(g, xr, y, "I2S ERR", b);
    y += 7;
    fmt_bytes(b, sizeof b, d->heap_int_free); kv(g, xl, y, "HEAP INT", b);
    fmt_bytes(b, sizeof b, d->heap_int_min); kv(g, xr, y, "HEAP MIN", b);
    y += 7;
    fmt_bytes(b, sizeof b, d->heap_ext_free); kv(g, xl, y, "PSRAM", b);
    uint32_t u = d->uptime_s;
    snprintf(b, sizeof b, "%lu:%02lu:%02lu", (unsigned long)(u / 3600), (unsigned long)(u / 60 % 60), (unsigned long)(u % 60));
    kv(g, xr, y, "UPTIME", b);
    y += 7;
    snprintf(b, sizeof b, "%.2f", (double)d->measured_fs); kv(g, xl, y, "FS", b);
    kv(g, xr, y, "CLOCK", U.st.clock_ext ? "EXT OSC" : "PLL");
    y += 7;
    snprintf(b, sizeof b, "%lu", (unsigned long)d->prof.blocks); kv(g, xl, y, "BLOCKS", b);
    snprintf(b, sizeof b, "%.1f%%", (double)d->prof.total_max_pct); kv(g, xr, y, "CPU MAX", b);
}

static void diag_draw(gfx_t *g)
{
    char r[24];
    if (D.page == 0) snprintf(r, sizeof r, "CPU %.1f%%", (double)D.d.prof.total_avg_pct);
    else snprintf(r, sizeof r, "SYSTEM");
    gfx_text_ex(g, F_SMALL, UI_MX, 1, "DIAG", 1, C_ON);
    gfx_text_align(g, F_SMALL, GFX_W - UI_MX, 1, r, 1, GFX_RIGHT, C_ON);
    /* page dots */
    for (int i = 0; i < 2; i++) {
        if (i == D.page) gfx_rect(g, 56 + i * 8, 2, 4, 3, C_ON);
        else gfx_frame(g, 56 + i * 8, 2, 4, 3, C_ON);
    }
    gfx_hline_pat(g, 0, UI_HDR_LINE, GFX_W, 0x55, C_ON);
    gfx_state_t st = gfx_save(g);
    gfx_clip(g, 0, UI_TOP, GFX_W, GFX_H - UI_TOP);
    int ox = -iroundf(D.px.v);
    page_cpu(g, ox);
    page_sys(g, ox + GFX_W);
    gfx_restore(g, st);
}

const screen_t g_scr_diag = { diag_enter, diag_input, diag_tick, diag_draw, NULL };
