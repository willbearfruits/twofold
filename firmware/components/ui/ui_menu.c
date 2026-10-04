/* ui_menu.c: MENU and the list screens (EFFECTS, NETWORK, WIFI SETUP, BLUETOOTH, PRESETS, SETTINGS, ABOUT). */
#include "ui_priv.h"

void ui_mark_anim(void);

static char s_hdr_right[24];

/* ================================================================== MENU */
static bool v_eq(const list_item_t *it, char *b, size_t n) { (void)it; snprintf(b, n, "%s", params_get_bool(P_EQ_ON) ? "ON" : "OFF"); return true; }
static bool v_gain(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    char u[8];
    ui_fmt_value(P_IN_GAIN, params_get(P_IN_GAIN), b, n, u, sizeof u);
    size_t k = strlen(b);
    snprintf(b + k, n - k, "dB");
    return true;
}
static bool v_fx(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    int c = params_get_bool(P_SAT_ON) + params_get_bool(P_CHO_ON) + params_get_bool(P_DLY_ON) +
            params_get_bool(P_REV_ON) + params_get_bool(P_CMP_ON);
    if (c) snprintf(b, n, "%d ON", c); else snprintf(b, n, "OFF");
    return true;
}
static bool v_net(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    const char *t = "OFF";
    switch (U.st.wifi) {
    case WIFI_STARTING: case WIFI_STA_CONNECTING: t = "..."; break;
    case WIFI_AP_SETUP: t = "SETUP"; break;
    case WIFI_STA_CONNECTED: t = "ON"; break;
    case WIFI_ERROR: t = "ERROR"; break;
    default: break;
    }
    snprintf(b, n, "%s", t);
    return true;
}
static bool v_bt(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", U.st.ble == BLE_OFF ? "OFF" : (U.st.ble == BLE_CONNECTED ? "LINKED" : "ON"));
    return true;
}
static bool v_preset(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    if (U.st.preset_active < 0) return false;
    snprintf(b, n, "P%d%s", U.st.preset_active + 1, U.st.preset_dirty ? "*" : "");
    return true;
}

static const list_item_t k_menu[] = {
    { LI_SUB, GFX_I_EQ, 0, SCR_EQ, "EQ", v_eq },
    { LI_SUB, GFX_I_GAIN, 0, SCR_GAIN, "GAIN", v_gain },
    { LI_SUB, GFX_I_FX, 0, SCR_FXLIST, "EFFECTS", v_fx },
    { LI_SUB, GFX_I_NET, 0, SCR_NET, "NETWORK", v_net },
    { LI_SUB, GFX_I_BT, 0, SCR_BT, "BLUETOOTH", v_bt },
    { LI_SUB, GFX_I_PRESET, 0, SCR_PRESETS, "PRESETS", v_preset },
    { LI_SUB, GFX_I_SETTINGS, 0, SCR_SETTINGS, "SETTINGS", NULL },
};
static list_t s_menu;

static void menu_enter(void) { list_init(&s_menu, k_menu, (int)(sizeof k_menu / sizeof k_menu[0]), NULL); }
static void menu_input(ui_act_t a, int raw) { list_input(&s_menu, a, raw); }
static void menu_tick(float dt) { list_tick(&s_menu, dt); }
static void menu_draw(gfx_t *g)
{
    snprintf(s_hdr_right, sizeof s_hdr_right, "%d/%d", s_menu.cur + 1, s_menu.n);
    ui_header(g, "MENU", s_hdr_right);
    list_draw(g, &s_menu, UI_TOP);
}
const screen_t g_scr_menu = { menu_enter, menu_input, menu_tick, menu_draw, NULL };

/* ================================================================== EFFECTS list */
static bool v_onoff(const list_item_t *it, char *b, size_t n)
{
    snprintf(b, n, "%s", params_get_bool((param_id_t)it->param) ? "ON" : "OFF");
    return true;
}
static const list_item_t k_fx[] = {
    { LI_SUB, 0, P_SAT_ON, SCR_FX_SAT, "SATURATE", v_onoff },
    { LI_SUB, 0, P_CHO_ON, SCR_FX_CHO, "CHORUS", v_onoff },
    { LI_SUB, 0, P_DLY_ON, SCR_FX_DLY, "DELAY", v_onoff },
    { LI_SUB, 0, P_REV_ON, SCR_FX_REV, "REVERB", v_onoff },
    { LI_SUB, 0, P_CMP_ON, SCR_FX_CMP, "COMPRESS", v_onoff },
};
static list_t s_fx;
static void fx_enter(void) { list_init(&s_fx, k_fx, 5, NULL); }
static void fx_input(ui_act_t a, int raw)
{
    /* double press on a row toggles the effect without entering it */
    if (a == A_DOUBLE) { ui_param_toggle((param_id_t)k_fx[s_fx.cur].param); return; }
    list_input(&s_fx, a, raw);
}
static void fx_tick(float dt) { list_tick(&s_fx, dt); }
static void fx_draw(gfx_t *g)
{
    ui_header(g, "EFFECTS", "CHAIN");
    list_draw(g, &s_fx, UI_TOP);
}
const screen_t g_scr_fxlist = { fx_enter, fx_input, fx_tick, fx_draw, NULL };

/* ================================================================== NETWORK */
enum { ACT_FORGET = 1, ACT_FACTORY, ACT_REBOOT };

static bool v_wstate(const list_item_t *it, char *b, size_t n) { (void)it; snprintf(b, n, "%s", ui_wifi_state_name(U.st.wifi)); return true; }
static bool v_ssid(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    if (U.st.wifi != WIFI_STA_CONNECTED || !U.st.wifi_ssid[0]) { snprintf(b, n, "-"); return true; }
    snprintf(b, n, "%.18s", U.st.wifi_ssid);
    return true;
}
static bool v_ip(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    if (U.st.wifi == WIFI_STA_CONNECTED || U.st.wifi == WIFI_AP_SETUP) snprintf(b, n, "%s", U.st.ip);
    else snprintf(b, n, "-");
    return true;
}
static bool v_rssi(const list_item_t *it, char *b, size_t n)
{
    (void)it;
    if (U.st.wifi != WIFI_STA_CONNECTED) { snprintf(b, n, "-"); return true; }
    snprintf(b, n, "%d dBm", U.st.wifi_rssi);
    return true;
}

static const list_item_t k_net[] = {
    { LI_PARAM, 0, P_NET_WIFI, 0, "WIFI", NULL },
    { LI_INFO, 0, 0, 0, "STATUS", v_wstate },
    { LI_SUB, 0, 0, SCR_NET_SETUP, "JOIN A NETWORK", NULL },
    { LI_INFO, 0, 0, 0, "SSID", v_ssid },
    { LI_INFO, 0, 0, 0, "ADDRESS", v_ip },
    { LI_INFO, 0, 0, 0, "SIGNAL", v_rssi },
    { LI_ACTION, 0, 0, ACT_FORGET, "FORGET NETWORK", NULL },
};
static list_t s_net;

static void forget_cb(bool yes, int arg)
{
    (void)arg;
    if (yes) { sys_cmd(SYS_CMD_WIFI_FORGET, 0); ui_toast("NETWORK FORGOTTEN"); }
}
static void net_action(int a)
{
    if (a == ACT_FORGET) ui_confirm("FORGET NETWORK?", "STORED WIFI LOGIN IS ERASED", "FORGET", forget_cb, 0);
}
static void net_enter(void) { list_init(&s_net, k_net, (int)(sizeof k_net / sizeof k_net[0]), net_action); }
static void net_input(ui_act_t a, int raw) { list_input(&s_net, a, raw); }
static void net_tick(float dt) { list_tick(&s_net, dt); }
static void net_draw(gfx_t *g)
{
    ui_header(g, "NETWORK", NULL);
    list_draw(g, &s_net, UI_TOP);
}
const screen_t g_scr_net = { net_enter, net_input, net_tick, net_draw, NULL };

/* ================================================================== WIFI SETUP */
static void setup_enter(void)
{
    if (!params_get_bool(P_NET_WIFI)) params_set(P_NET_WIFI, 1.f, SRC_UI);
    if (U.st.wifi != WIFI_AP_SETUP) sys_cmd(SYS_CMD_WIFI_SETUP, 0);
}
static void setup_input(ui_act_t a, int raw)
{
    (void)raw;
    if (a == A_BACK || a == A_PRESS) ui_pop();
}
static void setup_tick(float dt) { (void)dt; if (U.st.wifi != WIFI_AP_SETUP) ui_mark_anim(); }

static void setup_row(gfx_t *g, int y, int step, const char *lab, const char *val)
{
    char n[2] = { (char)('0' + step), 0 };
    gfx_circle(g, UI_MX + 3, y + 3, 4, C_ON);
    gfx_text_align(g, F_TINY, UI_MX + 4, y + 1, n, 0, GFX_CENTER, C_ON);
    gfx_text_ex(g, F_SMALL, UI_MX + 12, y + 1, lab, 1, C_ON);
    gfx_text(g, F_MED, 44, y, val, C_ON);
}

static void setup_draw(gfx_t *g)
{
    const sys_status_t *s = &U.st;
    ui_header(g, "JOIN A NETWORK", NULL);
    if (s->wifi == WIFI_AP_SETUP) {
        char url[24];
        snprintf(url, sizeof url, "%s", s->ip[0] ? s->ip : "192.168.4.1");
        setup_row(g, 13, 1, "JOIN", s->wifi_ssid);
        setup_row(g, 27, 2, "PASS", s->wifi_pass);
        setup_row(g, 41, 3, "OPEN", url);
        gfx_text_align(g, F_TINY, GFX_W / 2, 57, "PICK YOUR WIFI IN THE PAGE", 0, GFX_CENTER, C_ON);
    } else if (s->wifi == WIFI_STA_CONNECTED) {
        gfx_text_align(g, F_MED, GFX_W / 2, 20, "Connected", 0, GFX_CENTER, C_ON);
        gfx_text_align(g, F_MED, GFX_W / 2, 34, s->wifi_ssid, 0, GFX_CENTER, C_ON);
        gfx_text_align(g, F_SMALL, GFX_W / 2, 48, s->ip, 1, GFX_CENTER, C_ON);
    } else {
        /* starting: three dots breathing */
        gfx_text_align(g, F_SMALL, GFX_W / 2, 24, "STARTING ACCESS POINT", 1, GFX_CENTER, C_ON);
        for (int i = 0; i < 3; i++) {
            float ph = U.t * 3.f - (float)i * 0.6f;
            int r = (sinf(ph) > 0.3f) ? 2 : 1;
            gfx_disc(g, GFX_W / 2 - 8 + i * 8, 40, r, C_ON);
        }
    }
}
const screen_t g_scr_net_setup = { setup_enter, setup_input, setup_tick, setup_draw, NULL };

/* ================================================================== BLUETOOTH */
static bool v_bstate(const list_item_t *it, char *b, size_t n) { (void)it; snprintf(b, n, "%s", ui_ble_state_name(U.st.ble)); return true; }
static bool v_bname(const list_item_t *it, char *b, size_t n) { (void)it; snprintf(b, n, "%s", U.st.ble_name[0] ? U.st.ble_name : "TWOFOLD"); return true; }

static const list_item_t k_bt[] = {
    { LI_PARAM, 0, P_NET_BLE, 0, "BLE MIDI", NULL },
    { LI_INFO, 0, 0, 0, "STATUS", v_bstate },
    { LI_INFO, 0, 0, 0, "NAME", v_bname },
    { LI_TEXT, 0, 0, 0, "BLUETOOTH IS FOR", NULL },
    { LI_TEXT, 0, 0, 0, "MIDI CONTROL ONLY.", NULL },
    { LI_TEXT, 0, 0, 0, "NO AUDIO OVER BT:", NULL },
    { LI_TEXT, 0, 0, 0, "THE ESP32-S3 HAS", NULL },
    { LI_TEXT, 0, 0, 0, "NO BT CLASSIC, SO", NULL },
    { LI_TEXT, 0, 0, 0, "NO A2DP STREAMING.", NULL },
    { LI_TEXT, 0, 0, 0, "AUDIO GOES OVER USB.", NULL },
};
static list_t s_bt;
static void bt_enter(void) { list_init(&s_bt, k_bt, (int)(sizeof k_bt / sizeof k_bt[0]), NULL); }
static void bt_input(ui_act_t a, int raw)
{
    if (a == A_ROT && s_bt.cur == 0 && !s_bt.edit) {
        /* only one selectable row: rotating scrolls the help text */
        float maxs = 0.f;
        int total = 0;
        for (int i = 0; i < s_bt.n; i++) total += k_bt[i].kind == LI_TEXT ? 8 : UI_ROW;
        if (total > GFX_H - UI_TOP) maxs = (float)(total - (GFX_H - UI_TOP));
        tw_to(&s_bt.scroll, clampf_(s_bt.scroll.t + (float)(raw * 8), 0.f, maxs));
        return;
    }
    list_input(&s_bt, a, raw);
}
static void bt_tick(float dt) { list_tick(&s_bt, dt); if (U.st.ble == BLE_ADVERTISING) ui_mark_anim(); }
static void bt_draw(gfx_t *g)
{
    ui_header(g, "BLUETOOTH", NULL);
    list_draw(g, &s_bt, UI_TOP);
}
const screen_t g_scr_bt = { bt_enter, bt_input, bt_tick, bt_draw, NULL };

/* ================================================================== PRESETS */
static struct {
    int cur;
    bool choose;          /* inline LOAD / SAVE choice */
    int act;              /* 0 load, 1 save */
    tw_t hl, scroll, actx;
} P;

static void pr_retarget(void)
{
    tw_to(&P.hl, (float)(P.cur * UI_ROW));
    int view = GFX_H - UI_TOP, total = PRESET_SLOTS * UI_ROW, maxs = total - view;
    float s = P.scroll.t;
    int top = P.cur * UI_ROW;
    if (top - UI_ROW < s) s = (float)(top - UI_ROW);
    if (top + 2 * UI_ROW > s + view) s = (float)(top + 2 * UI_ROW - view);
    tw_to(&P.scroll, clampf_(s, 0.f, (float)maxs));
}

static void pr_enter(void)
{
    P.cur = U.st.preset_active >= 0 ? U.st.preset_active : 0;
    P.choose = false;
    pr_retarget();
    tw_snap(&P.hl, P.hl.t);
    tw_snap(&P.scroll, P.scroll.t);
}

static void save_cb(bool yes, int slot)
{
    if (!yes) return;
    sys_cmd(SYS_CMD_PRESET_SAVE, slot);
    char b[24];
    snprintf(b, sizeof b, "SAVED TO P%d", slot + 1);
    ui_toast(b);
}

static void pr_input(ui_act_t a, int raw)
{
    if (P.choose) {
        if (a == A_ROT) { P.act = raw > 0 ? 1 : 0; tw_to(&P.actx, (float)P.act); }
        else if (a == A_BACK) P.choose = false;
        else if (a == A_PRESS) {
            P.choose = false;
            char b[28];
            if (P.act == 0) {
                sys_cmd(SYS_CMD_PRESET_LOAD, P.cur);
                snprintf(b, sizeof b, "PRESET %d LOADED", P.cur + 1);
                ui_toast(b);
            } else {
                char d[32];
                snprintf(d, sizeof d, "REPLACES %.14s", U.st.preset_names[P.cur]);
                snprintf(b, sizeof b, "SAVE TO P%d?", P.cur + 1);
                ui_confirm(b, d, "SAVE", save_cb, P.cur);
            }
        }
        return;
    }
    if (a == A_ROT) { P.cur = clampi_(P.cur + raw, 0, PRESET_SLOTS - 1); pr_retarget(); }
    else if (a == A_PRESS) { P.choose = true; P.act = 0; tw_snap(&P.actx, 0.f); }
    else if (a == A_BACK) ui_pop();
}

static void pr_tick(float dt)
{
    bool m = tw_step(&P.hl, dt, 0.035f);
    m |= tw_step(&P.scroll, dt, 0.05f);
    m |= tw_step(&P.actx, dt, 0.035f);
    if (m) ui_mark_anim();
}

static void pr_draw(gfx_t *g)
{
    const sys_status_t *s = &U.st;
    char hr[24];
    if (s->preset_active >= 0) snprintf(hr, sizeof hr, "P%d%s", s->preset_active + 1, s->preset_dirty ? " EDITED" : "");
    else hr[0] = 0;
    ui_header(g, "PRESETS", hr);
    gfx_state_t st = gfx_save(g);
    gfx_clip(g, 0, UI_TOP, GFX_W, GFX_H - UI_TOP);
    int sc = iroundf(P.scroll.v);
    int hy = UI_TOP + iroundf(P.hl.v) - sc;
    if (P.choose) gfx_rframe(g, 1, hy, GFX_W - 4, UI_ROW, 2, C_ON);
    else gfx_rrect(g, 1, hy, GFX_W - 4, UI_ROW, 2, C_ON);
    for (int i = 0; i < PRESET_SLOTS; i++) {
        int y = UI_TOP + i * UI_ROW - sc;
        if (y >= GFX_H || y + UI_ROW <= UI_TOP) continue;
        char n[3] = { (char)('1' + i), 0, 0 };
        gfx_text(g, F_TINY, UI_MX + 2, y + 3, n, C_XOR);
        gfx_text(g, F_MED, UI_MX + 10, y + 2, s->preset_names[i][0] ? s->preset_names[i] : "EMPTY", C_XOR);
        bool active = i == s->preset_active;
        if (P.choose && i == P.cur) {
            const int bw = 25, bx1 = GFX_W - 4 - UI_MX - bw + 2, bx0 = bx1 - bw - 2;
            int hx = bx0 + iroundf(P.actx.v * (float)(bx1 - bx0));
            gfx_rrect(g, hx, y + 2, bw, 7, 2, C_ON);
            gfx_text_align(g, F_SMALL, bx0 + bw / 2, y + 3, "LOAD", 1, GFX_CENTER, C_XOR);
            gfx_text_align(g, F_SMALL, bx1 + bw / 2, y + 3, "SAVE", 1, GFX_CENTER, C_XOR);
        } else if (active) {
            int rx = GFX_W - 4 - UI_MX;
            if (s->preset_dirty) {
                gfx_text_align(g, F_SMALL, rx - 6, y + 3, "EDITED", 1, GFX_RIGHT, C_XOR);
                gfx_circle(g, rx - 1, y + 5, 2, C_XOR);
            } else {
                gfx_disc(g, rx - 1, y + 5, 2, C_XOR);
            }
        }
    }
    gfx_restore(g, st);
    ui_scrollbar(g, GFX_W - 1, UI_TOP, GFX_H - UI_TOP, P.scroll.v, (float)(GFX_H - UI_TOP), (float)(PRESET_SLOTS * UI_ROW));
}
const screen_t g_scr_presets = { pr_enter, pr_input, pr_tick, pr_draw, NULL };

/* ================================================================== SETTINGS */
static const list_item_t k_set[] = {
    { LI_PARAM, 0, P_UI_BRIGHT, 0, "BRIGHTNESS", NULL },
    { LI_PARAM, 0, P_UI_DIM, 0, "DIM AFTER", NULL },
    { LI_PARAM, 0, P_UI_SAVER, 0, "SCREENSAVER", NULL },
    { LI_PARAM, 0, P_UI_SPECTRUM, 0, "EQ SPECTRUM", NULL },
    { LI_PARAM, 0, P_UI_OLED, 0, "DISPLAY", NULL },
    { LI_PARAM, 0, P_UI_LED, 0, "STATUS LED", NULL },
    { LI_PARAM, 0, P_LIM_ON, 0, "LIMITER", NULL },
    { LI_PARAM, 0, P_LIM_CEIL, 0, "CEILING", NULL },
    { LI_PARAM, 0, P_MIDI_CH, 0, "MIDI CHANNEL", NULL },
    { LI_PARAM, 0, P_MIDI_ENC, 0, "KNOB MODE", NULL },
    { LI_PARAM, 0, P_MIDI_ENC_CC, 0, "KNOB CC", NULL },
    { LI_PARAM, 0, P_TEMPO, 0, "TEMPO", NULL },
    { LI_PARAM, 0, P_NET_BOOT, 0, "RADIOS AT BOOT", NULL },
    { LI_ACTION, 0, 0, ACT_FACTORY, "FACTORY RESET", NULL },
    { LI_SUB, 0, 0, SCR_ABOUT, "ABOUT", NULL },
};
static list_t s_set;

static bool set_enabled(int i)
{
    if (k_set[i].param == P_MIDI_ENC_CC) return params_get_int(P_MIDI_ENC) == 1;
    if (k_set[i].param == P_LIM_CEIL) return params_get_bool(P_LIM_ON);
    return true;
}
static void factory_cb(bool yes, int arg)
{
    (void)arg;
    if (yes) { sys_cmd(SYS_CMD_FACTORY_RESET, 0); ui_toast("FACTORY RESET"); }
}
static void set_action(int a)
{
    if (a == ACT_FACTORY) ui_confirm("FACTORY RESET?", "ALL SETTINGS AND PRESETS", "RESET", factory_cb, 0);
}
static void set_enter(void)
{
    s_set.enabled = set_enabled;
    list_init(&s_set, k_set, (int)(sizeof k_set / sizeof k_set[0]), set_action);
}
static void set_input(ui_act_t a, int raw) { list_input(&s_set, a, raw); }
static void set_tick(float dt) { list_tick(&s_set, dt); }
static void set_draw(gfx_t *g)
{
    ui_header(g, "SETTINGS", NULL);
    list_draw(g, &s_set, UI_TOP);
}
const screen_t g_scr_settings = { set_enter, set_input, set_tick, set_draw, NULL };

/* ================================================================== ABOUT (hold 3 s -> DIAGNOSTICS) */
static struct { float hold; bool armed; uint32_t last; } A;

static void about_enter(void) { A.hold = 0.f; A.armed = false; }
static void about_input(ui_act_t a, int raw)
{
    (void)raw;
    if (a == A_BACK || a == A_PRESS) ui_pop();
}
static bool about_hold(uint32_t held, bool released)
{
    if (held < A.last) A.armed = false;      /* a new hold (e.g. after returning from DIAGNOSTICS) */
    A.last = released ? 0 : held;
    if (released) {
        A.hold = 0.f;
        if (!A.armed) ui_pop();              /* released early: behaves like a normal long press */
        A.armed = false;
        return true;
    }
    if (A.armed) return true;
    A.hold = clampf_((float)(held - UI_LONG_MS) / (float)(UI_DIAG_HOLD_MS - UI_LONG_MS), 0.f, 1.f);
    if (held >= UI_DIAG_HOLD_MS) {
        A.armed = true;
        A.hold = 0.f;
        ui_push(SCR_DIAG);
    }
    return true;
}
static void about_draw(gfx_t *g)
{
    const sys_status_t *s = &U.st;
    gfx_bitmap(g, (GFX_W - gfx_wordmark.w) / 2, 6, &gfx_wordmark, C_ON);
    gfx_text_align(g, F_SMALL, GFX_W / 2, 23, "USB AUDIO INTERFACE", 1, GFX_CENTER, C_ON);
    char b[40];
    snprintf(b, sizeof b, "FIRMWARE %s", s->fw_version ? s->fw_version : "-");
    gfx_text_align(g, F_TINY, GFX_W / 2, 37, b, 1, GFX_CENTER, C_ON);
    snprintf(b, sizeof b, "48 KHZ " GFX_S_BULLET " 24 BIT " GFX_S_BULLET " 2 IN 2 OUT");
    gfx_text_align(g, F_TINY, GFX_W / 2, 44, b, 1, GFX_CENTER, C_ON);
    snprintf(b, sizeof b, "CLOCK %s", s->clock_ext ? "EXT OSCILLATOR" : "INTERNAL PLL");
    gfx_text_align(g, F_TINY, GFX_W / 2, 51, b, 1, GFX_CENTER, C_ON);
    if (A.hold > 0.f) {
        int w = iroundf(A.hold * 120.f);
        gfx_hline(g, (GFX_W - w) / 2, 61, w, C_ON);
        ui_mark_anim();
    }
}
const screen_t g_scr_about = { about_enter, about_input, NULL, about_draw, about_hold };
