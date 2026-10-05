/*
 * ui_core.c: UI runtime: event -> gesture recognition, encoder acceleration, screen stack and
 * transitions, overlays (toast, confirm), meters ballistics, dim / screensaver / brightness.
 */
#include "ui_priv.h"

ui_ctx_t U;
uint32_t g_last_input_ms;

/* ------------------------------------------------------------------ tweens */
bool tw_step(tw_t *w, float dt, float tau)
{
    float d = w->t - w->v;
    if (fabsf(d) < 0.02f) { w->v = w->t; return false; }
    float a = dt / (tau + dt);          /* cheap 1-exp(-dt/tau) */
    w->v += d * a;
    return true;
}

/* ------------------------------------------------------------------ state */
#define STACK_MAX 6
static scr_id_t s_stack[STACK_MAX];
static int s_sp;
static bool s_anim;
static uint32_t s_last_update;
static bool s_started;

typedef enum { TR_NONE = 0, TR_PUSH, TR_POP, TR_FADE } tr_kind_t;
static struct {
    tr_kind_t kind;
    scr_id_t from, to;
    uint32_t t0;
    uint16_t dur;
} s_tr;

static struct {
    bool down, long_fired, used, pending_single, swallow, hold_claimed;
    uint32_t t_down, t_up;
} B;

static struct {
    float speed;
    uint32_t last_t;
    int last_dir;
    bool fine;
} s_acc;

static struct { bool active; char msg[28]; uint32_t t0; } s_toast;

static struct {
    bool active, closing;
    char title[28], detail[28], yes[12];
    ui_confirm_cb cb;
    int arg, sel;
    tw_t hl;
    uint32_t t0;
} s_cf;

/* screensaver / brightness */
static struct {
    bool dimmed, saver;
    uint32_t saver_t0;
    float level;                  /* current applied level (smoothed) */
    int sent_level;
    bool sent_on;
} s_disp;

void ui_mark_anim(void) { s_anim = true; }

scr_id_t ui_current(void) { return s_stack[s_sp - 1]; }

static void start_tr(tr_kind_t k, scr_id_t from, scr_id_t to, uint16_t dur)
{
    s_tr.kind = k; s_tr.from = from; s_tr.to = to; s_tr.t0 = U.now; s_tr.dur = dur;
}

void ui_push(scr_id_t s)
{
    if (s_sp >= STACK_MAX) return;
    scr_id_t from = ui_current();
    s_stack[s_sp++] = s;
    if (g_screens[s]->enter) g_screens[s]->enter();
    start_tr(TR_PUSH, from, s, UI_SLIDE_MS);
}

void ui_pop(void)
{
    if (s_sp <= 1) return;
    scr_id_t from = ui_current();
    s_sp--;
    start_tr(TR_POP, from, ui_current(), UI_SLIDE_MS);
}

void ui_go_home(void)
{
    static bool s_noted;
    if (!s_noted) {               /* first arrival on HOME after boot: report an abnormal last reset */
        s_noted = true;
        sys_status_t st;
        sys_get_status(&st);
        if (st.boot_note) ui_toast(st.boot_note);
    }
    scr_id_t from = ui_current();
    s_stack[0] = SCR_HOME;
    s_sp = 1;
    if (from == SCR_BOOT) {
        if (g_screens[SCR_HOME]->enter) g_screens[SCR_HOME]->enter();
        start_tr(TR_FADE, from, SCR_HOME, 420);
    } else if (from != SCR_HOME) {
        start_tr(TR_POP, from, SCR_HOME, UI_SLIDE_MS);
    }
}

/* ------------------------------------------------------------------ overlays */
void ui_toast(const char *msg)
{
    snprintf(s_toast.msg, sizeof s_toast.msg, "%s", msg);
    /* if one is already fully visible, restart without re-sliding */
    uint32_t age = U.now - s_toast.t0;
    s_toast.t0 = (s_toast.active && age > 180 && age < UI_TOAST_MS - 180) ? U.now - 180 : U.now;
    s_toast.active = true;
}

void ui_confirm(const char *title, const char *detail, const char *yes_label, ui_confirm_cb cb, int arg)
{
    snprintf(s_cf.title, sizeof s_cf.title, "%s", title);
    snprintf(s_cf.detail, sizeof s_cf.detail, "%s", detail ? detail : "");
    snprintf(s_cf.yes, sizeof s_cf.yes, "%s", yes_label ? yes_label : "OK");
    s_cf.cb = cb; s_cf.arg = arg; s_cf.sel = 0;
    tw_snap(&s_cf.hl, 0);
    s_cf.active = true; s_cf.closing = false; s_cf.t0 = U.now;
}

bool ui_modal_active(void) { return s_cf.active && !s_cf.closing; }

static void confirm_close(bool yes)
{
    s_cf.closing = true;
    s_cf.t0 = U.now;
    if (s_cf.cb) s_cf.cb(yes, s_cf.arg);
}

static void confirm_input(ui_act_t a, int raw)
{
    if (a == A_ROT) { s_cf.sel = raw > 0 ? 1 : 0; tw_to(&s_cf.hl, (float)s_cf.sel); }
    else if (a == A_PRESS) confirm_close(s_cf.sel == 1);
    else if (a == A_BACK) confirm_close(false);
}

/* ------------------------------------------------------------------ acceleration */
static void accel_feed(int raw, uint32_t t)
{
    int dir = raw > 0 ? 1 : -1;
    uint32_t dt = t - s_acc.last_t;
    if (dt > 220 || dir != s_acc.last_dir) {
        s_acc.speed = 0.f;
    } else {
        float inst = (float)(raw < 0 ? -raw : raw) * 1000.f / (float)(dt < 4 ? 4 : dt);
        s_acc.speed = s_acc.speed * 0.55f + inst * 0.45f;
    }
    s_acc.last_t = t;
    s_acc.last_dir = dir;
}

float ui_accel_mult(void)
{
    if (s_acc.fine) return 1.f;
    float v = s_acc.speed - 4.f;          /* detents per second above the "fine" zone */
    if (v <= 0.f) return 1.f;
    float m = 1.f + 0.028f * v * v;
    return m > 10.f ? 10.f : m;
}

int ui_accel_steps(int raw, float *carry)
{
    if ((*carry > 0.f && raw < 0) || (*carry < 0.f && raw > 0)) *carry = 0.f;
    float f = (float)raw * ui_accel_mult() + *carry;
    int steps = (int)f;
    *carry = f - (float)steps;
    if (steps == 0 && raw != 0 && fabsf(*carry) >= 0.999f) { steps = raw > 0 ? 1 : -1; *carry = 0.f; }
    return steps;
}

bool ui_param_rotate(param_id_t id, int raw, float *carry)
{
    const param_desc_t *d = params_desc(id);
    int steps = raw;
    if (d->type == PT_FLOAT || (d->type == PT_INT && d->max - d->min > 24.f)) steps = ui_accel_steps(raw, carry);
    if (!steps) return false;
    bool ch = params_nudge(id, steps, SRC_UI);
    if (ch) ui_param_changed(id);
    return ch;
}

void ui_param_toggle(param_id_t id)
{
    if (params_toggle(id, SRC_UI)) ui_param_changed(id);
}

void ui_param_changed(param_id_t id)
{
    switch (id) {
    case P_NET_WIFI: sys_cmd(SYS_CMD_WIFI_ENABLE, params_get_bool(P_NET_WIFI)); break;
    case P_NET_BLE:  sys_cmd(SYS_CMD_BLE_ENABLE, params_get_bool(P_NET_BLE)); break;
    case P_UI_OLED:  sys_cmd(SYS_CMD_OLED_TYPE, params_get_int(P_UI_OLED)); break;
    case P_UI_BRIGHT: ui_brightness_apply(false); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ brightness */
/* Perceptual curve: level = 4 + 251 * (pct/100)^2.2. Never 0 at 0 % (the panel stays readable);
 * the display driver maps 0..255 onto contrast (+ precharge at the low end). */
static float bright_level(float pct)
{
    float p = clampf_(pct, 0.f, 100.f) / 100.f;
    return 4.f + 251.f * powf(p, 2.2f);
}

static float target_level(void)
{
    float base = bright_level(params_get(P_UI_BRIGHT));
    if (s_disp.saver) return 2.f;
    if (s_disp.dimmed) return fmaxf(4.f, base * 0.22f);
    return base;
}

void ui_brightness_apply(bool force)
{
    float tgt = target_level();
    if (force) s_disp.level = tgt;
    int lv = iroundf(s_disp.level);
    lv = clampi_(lv, 1, 255);
    if (force || lv != s_disp.sent_level || !s_disp.sent_on) {
        sys_display_set((uint8_t)lv, true);
        s_disp.sent_level = lv;
        s_disp.sent_on = true;
    }
}

static void brightness_tick(float dt)
{
    float tgt = target_level();
    float d = tgt - s_disp.level;
    if (fabsf(d) > 0.5f) {
        /* ~350 ms perceptual glide (faster when waking up) */
        float tau = d > 0 ? 0.06f : 0.35f;
        s_disp.level += d * (dt / (tau + dt));
        s_anim = true;
    } else {
        s_disp.level = tgt;
    }
    ui_brightness_apply(false);
}

static const uint32_t k_dim_ms[] = { 0, 15000, 30000, 60000, 300000 };
static const uint32_t k_saver_ms[] = { 0, 60000, 300000, 600000, 1800000 };

static void idle_tick(void)
{
    if (ui_current() == SCR_BOOT) return;
    uint32_t idle = U.now - g_last_input_ms;
    int di = clampi_(params_get_int(P_UI_DIM), 0, 4), si = clampi_(params_get_int(P_UI_SAVER), 0, 4);
    bool dim = k_dim_ms[di] && idle >= k_dim_ms[di];
    bool saver = k_saver_ms[si] && idle >= k_saver_ms[si];
    if (s_cf.active || s_toast.active || s_tr.kind != TR_NONE) saver = false;
    if (saver && !s_disp.saver) s_disp.saver_t0 = U.now;
    s_disp.dimmed = dim || saver;
    s_disp.saver = saver;
}

/* returns true when the input only woke the panel */
static bool wake(uint32_t t)
{
    g_last_input_ms = t;
    bool was_saver = s_disp.saver;
    if (s_disp.dimmed || s_disp.saver) {
        s_disp.dimmed = false;
        s_disp.saver = false;
    }
    return was_saver;
}

/* ------------------------------------------------------------------ dispatch */
static void deliver(ui_act_t a, int raw)
{
    if (s_cf.active) {
        if (!s_cf.closing) confirm_input(a, raw);
        return;
    }
    const screen_t *s = g_screens[ui_current()];
    if (s->input) s->input(a, raw);
}

static void flush_pending_single(void)
{
    if (B.pending_single) {
        B.pending_single = false;
        deliver(A_PRESS, 0);
    }
}

void ui_event(const ui_event_t *ev)
{
    if (!s_started) return;
    switch (ev->type) {
    case UI_EV_ROTATE:
        if (!ev->delta) return;
        if (wake(ev->t_ms)) return;                 /* swallowed */
        accel_feed(ev->delta, ev->t_ms);
        s_acc.fine = B.down;                        /* push-and-turn = fine adjust, no acceleration */
        if (B.down) B.used = true;
        flush_pending_single();
        deliver(A_ROT, ev->delta);
        break;
    case UI_EV_BTN_DOWN:
        if (wake(ev->t_ms)) { B.swallow = true; B.down = false; return; }
        B.down = true; B.t_down = ev->t_ms;
        B.long_fired = B.used = B.hold_claimed = false;
        if (B.pending_single && ev->t_ms - B.t_up <= UI_DOUBLE_MS) {
            B.pending_single = false;
            B.used = true;
            deliver(A_DOUBLE, 0);
        }
        break;
    case UI_EV_BTN_UP:
        g_last_input_ms = ev->t_ms;
        if (B.swallow) { B.swallow = false; return; }
        if (!B.down) return;
        B.down = false;
        s_acc.fine = false;
        if (B.hold_claimed) {
            const screen_t *s = g_screens[ui_current()];
            if (s->hold) s->hold(ev->t_ms - B.t_down, true);
            return;
        }
        if (B.used || B.long_fired) return;
        if (ui_current() == SCR_HOME && !s_cf.active && s_tr.kind != TR_FADE) {
            B.pending_single = true;                /* wait for a possible double press */
            B.t_up = ev->t_ms;
        } else {
            deliver(A_PRESS, 0);
        }
        break;
    }
}

static void gesture_tick(void)
{
    if (B.down && !B.used) {
        uint32_t held = U.now - B.t_down;
        if (!B.long_fired && held >= UI_LONG_MS) {
            B.long_fired = true;
            const screen_t *s = g_screens[ui_current()];
            if (!s_cf.active && s->hold && s->hold(held, false)) B.hold_claimed = true;
            else deliver(A_BACK, 0);
        } else if (B.hold_claimed) {
            const screen_t *s = g_screens[ui_current()];
            if (s->hold) s->hold(held, false);
            s_anim = true;
        }
    }
    if (B.pending_single && U.now - B.t_up > UI_DOUBLE_MS) flush_pending_single();
}

/* ------------------------------------------------------------------ meters */
static float lin_to_db(float v) { return v > 1e-6f ? 20.f * log10f(v) : -120.f; }

static void meter_chan(float lvl, float *disp, float *pk, float *hold, float dt)
{
    float db = lin_to_db(lvl);
    if (db >= *disp) *disp = db;
    else *disp = fmaxf(db, *disp - 20.f * dt);          /* release 20 dB/s */
    if (db >= *pk) { *pk = db; *hold = 1.5f; }
    else if (*hold > 0.f) *hold -= dt;
    else *pk = fmaxf(*disp, *pk - 24.f * dt);           /* peak marker falls after hold */
    if (*disp < -90.f) *disp = -90.f;
    if (*pk < -90.f) *pk = -90.f;
}

static void meters_tick(float dt)
{
    dsp_meters_t m;
    sys_get_meters(&m);
    bool sig = false;
    for (int c = 0; c < 2; c++) {
        meter_chan(m.in_peak[c], &U.in_db[c], &U.in_pk[c], &U.in_hold[c], dt);
        meter_chan(m.out_peak[c], &U.out_db[c], &U.out_pk[c], &U.out_hold[c], dt);
        if (m.in_clip[c] != U.last_in_clip[c]) { U.in_clip_until[c] = U.now + 2000; U.last_in_clip[c] = m.in_clip[c]; }
        if (m.out_over[c] != U.last_out_over[c]) { U.out_clip_until[c] = U.now + 2000; U.last_out_over[c] = m.out_over[c]; }
        sig |= U.in_db[c] > -70.f || U.out_db[c] > -70.f || U.in_pk[c] > -70.f || U.out_pk[c] > -70.f;
    }
    /* compressor GR: fast attack, 30 dB/s release for a readable bar */
    U.comp_gr_db = fmaxf(m.comp_gr_db, U.comp_gr_db - 30.f * dt);
    U.lim_gr_db = fmaxf(m.lim_gr_db, U.lim_gr_db - 30.f * dt);
    U.sat_in = fmaxf(m.sat_in_level, U.sat_in * (1.f - 4.f * dt));
    U.signal = sig;
}

/* ------------------------------------------------------------------ public API */
void ui_init(uint32_t now_ms)
{
    memset(&U, 0, sizeof U);
    memset(&B, 0, sizeof B);
    memset(&s_tr, 0, sizeof s_tr);
    memset(&s_toast, 0, sizeof s_toast);
    memset(&s_cf, 0, sizeof s_cf);
    memset(&s_acc, 0, sizeof s_acc);
    memset(&s_disp, 0, sizeof s_disp);
    for (int c = 0; c < 2; c++) U.in_db[c] = U.out_db[c] = U.in_pk[c] = U.out_pk[c] = -90.f;
    U.now = now_ms;
    s_last_update = now_ms;
    g_last_input_ms = now_ms;
    s_sp = 1;
    s_stack[0] = SCR_BOOT;
    s_disp.sent_level = -1;
    s_started = true;
    spec_init();
    eq_model_update();
    sys_get_status(&U.st);
    {
        dsp_meters_t m;
        sys_get_meters(&m);
        for (int c = 0; c < 2; c++) { U.last_in_clip[c] = m.in_clip[c]; U.last_out_over[c] = m.out_over[c]; }
    }
    if (g_screens[SCR_BOOT]->enter) g_screens[SCR_BOOT]->enter();
    ui_brightness_apply(true);
}

void ui_update(uint32_t now_ms)
{
    if (!s_started) return;
    uint32_t el = now_ms - s_last_update;
    s_last_update = now_ms;
    float dt = clampf_((float)el * 0.001f, 0.f, 0.1f);
    U.now = now_ms;
    U.dt = dt;
    U.t += dt;
    s_anim = false;

    sys_get_status(&U.st);
    sys_get_viz(&U.viz);
    meters_tick(dt);
    if (U.signal) s_anim = true;

    {   /* EQ model follows parameter changes from any source */
        uint32_t mask[PARAMS_MASK_WORDS];
        if (params_take_dirty(PL_UI, mask)) {
            for (int i = P_EQ_ON; i <= P_EQ_B5_Q; i++)
                if (params_mask_test(mask, (param_id_t)i)) { eq_model_update(); break; }
            if (params_mask_test(mask, P_UI_BRIGHT) || params_mask_test(mask, P_UI_DIM)) s_anim = true;
        }
    }

    gesture_tick();
    idle_tick();
    brightness_tick(dt);

    if (s_tr.kind != TR_NONE) {
        s_anim = true;
        if (now_ms - s_tr.t0 >= s_tr.dur) s_tr.kind = TR_NONE;
    }
    if (s_toast.active) {
        s_anim = true;
        if (now_ms - s_toast.t0 > UI_TOAST_MS) s_toast.active = false;
    }
    if (s_cf.active) {
        s_anim = true;
        if (tw_step(&s_cf.hl, dt, 0.035f)) s_anim = true;
        if (s_cf.closing && now_ms - s_cf.t0 > 140) s_cf.active = false;
    }

    /* tick visible screens (both during a transition) */
    scr_id_t cur = ui_current();
    if (s_tr.kind != TR_NONE && s_tr.from != cur && g_screens[s_tr.from]->tick) g_screens[s_tr.from]->tick(dt);
    if (g_screens[cur]->tick) g_screens[cur]->tick(dt);
    if (s_disp.saver) {
        if (g_screens[SCR_SAVER]->tick) g_screens[SCR_SAVER]->tick(dt);
        /* saver drifts slowly: request ~2 fps only */
        s_anim = (now_ms - s_disp.saver_t0) < 600;
    }
}

bool ui_is_animating(void) { return s_anim; }

/* ------------------------------------------------------------------ rendering */
static void draw_screen(gfx_t *g, scr_id_t id, int x, int clip_x0, int clip_x1)
{
    gfx_state_t st = gfx_save(g);
    gfx_clip(g, clip_x0, 0, clip_x1 - clip_x0, GFX_H);
    gfx_translate(g, x, 0);
    g_screens[id]->draw(g);
    gfx_restore(g, st);
}

static void draw_toast(gfx_t *g)
{
    uint32_t age = U.now - s_toast.t0;
    float p;
    if (age < 180) p = ease_out_cubic((float)age / 180.f);
    else if (age > UI_TOAST_MS - 160) p = 1.f - ease_out_cubic((float)(age - (UI_TOAST_MS - 160)) / 160.f);
    else p = 1.f;
    int tw = gfx_text_width_ex(F_SMALL, s_toast.msg, 1);
    int w = tw + 14, h = 11;
    int x = (GFX_W - w) / 2;
    int y = -h - 1 + iroundf(p * (float)(h + 3));
    gfx_rrect(g, x - 1, y - 1, w + 2, h + 2, 3, C_OFF);     /* black keep-out ring */
    gfx_rrect(g, x, y, w, h, 3, C_ON);
    gfx_text_ex(g, F_SMALL, x + 7, y + 3, s_toast.msg, 1, C_OFF);
}

static void draw_confirm(gfx_t *g)
{
    uint32_t age = U.now - s_cf.t0;
    float p = s_cf.closing ? 1.f - clampf_((float)age / 140.f, 0.f, 1.f) : ease_out_cubic(clampf_((float)age / 170.f, 0.f, 1.f));
    gfx_fade(g, 0, 0, GFX_W, GFX_H, 16 - iroundf(p * 13.f));
    const int w = 112, h = 44;
    int x = (GFX_W - w) / 2;
    int y = (GFX_H - h) / 2 + iroundf((1.f - p) * 10.f);
    gfx_rrect(g, x - 1, y - 1, w + 2, h + 2, 4, C_OFF);
    gfx_rframe(g, x, y, w, h, 3, C_ON);
    gfx_text_align(g, F_MED, GFX_W / 2, y + 6, s_cf.title, 0, GFX_CENTER, C_ON);
    gfx_text_align(g, F_SMALL, GFX_W / 2, y + 17, s_cf.detail, 1, GFX_CENTER, C_ON);
    /* two buttons, highlight glides between them */
    const int bw = 46, bh = 11, by = y + h - bh - 4;
    int bx0 = x + 6, bx1 = x + w - 6 - bw;
    int hx = bx0 + iroundf(s_cf.hl.v * (float)(bx1 - bx0));
    gfx_rframe(g, bx0, by, bw, bh, 2, C_ON);
    gfx_rframe(g, bx1, by, bw, bh, 2, C_ON);
    gfx_rrect(g, hx, by, bw, bh, 2, C_ON);
    gfx_text_align(g, F_SMALL, bx0 + bw / 2, by + 3, "CANCEL", 1, GFX_CENTER, C_XOR);
    gfx_text_align(g, F_SMALL, bx1 + bw / 2, by + 3, s_cf.yes, 1, GFX_CENTER, C_XOR);
}

void ui_render(gfx_t *g)
{
    gfx_reset(g);
    gfx_clear(g);
    if (!s_started) return;

    if (s_disp.saver && s_tr.kind == TR_NONE) {
        uint32_t age = U.now - s_disp.saver_t0;
        if (age < 500) {                      /* dissolve the current screen away */
            g_screens[ui_current()]->draw(g);
            gfx_fade(g, 0, 0, GFX_W, GFX_H, 16 - (int)(age * 16 / 500));
        } else {
            g_screens[SCR_SAVER]->draw(g);
        }
        return;
    }

    if (s_tr.kind == TR_NONE) {
        g_screens[ui_current()]->draw(g);
    } else {
        float p = clampf_((float)(U.now - s_tr.t0) / (float)s_tr.dur, 0.f, 1.f);
        if (s_tr.kind == TR_FADE) {
            if (p < 0.45f) {
                draw_screen(g, s_tr.from, 0, 0, GFX_W);
                gfx_fade(g, 0, 0, GFX_W, GFX_H, 16 - iroundf(p / 0.45f * 16.f));
            } else if (p > 0.55f) {
                draw_screen(g, s_tr.to, 0, 0, GFX_W);
                gfx_fade(g, 0, 0, GFX_W, GFX_H, iroundf((p - 0.55f) / 0.45f * 16.f));
            }
        } else {
            /* parallax push: the new screen slides fully over the old one, which drifts 40 %
             * and fades into the background. Pop is the exact reverse. */
            float e = ease_out_cubic(p);
            bool push = s_tr.kind == TR_PUSH;
            scr_id_t under = push ? s_tr.from : s_tr.to, over = push ? s_tr.to : s_tr.from;
            float q = push ? e : 1.f - e;                  /* 0 = under fully visible */
            int over_x = iroundf((float)GFX_W * (1.f - q));
            int under_x = -iroundf(0.4f * (float)GFX_W * q);
            draw_screen(g, under, under_x, 0, over_x);
            gfx_fade(g, 0, 0, over_x, GFX_H, 16 - iroundf(q * 12.f));
            if (over_x < GFX_W) {
                draw_screen(g, over, over_x, over_x, GFX_W);
                if (over_x > 0) gfx_vline_pat(g, over_x - 1, 0, GFX_H, 0x55, C_ON);
                if (over_x > 1) gfx_vline(g, over_x - 2, 0, GFX_H, C_OFF);
            }
        }
    }
    if (s_cf.active) draw_confirm(g);
    if (s_toast.active) draw_toast(g);
}
