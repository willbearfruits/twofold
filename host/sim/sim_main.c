/*
 * sim_main.c: TWOFOLD UI simulator. Drives the real ui/gfx/params code with a virtual clock and
 * scripted encoder input, and dumps frames (PBM, 1:1) plus a manifest that tools/post.py turns into
 * PNGs, OLED-look renders, transition filmstrips, the tour GIF and the contact sheet.
 *
 *   twofold_sim <outdir>            run selftest + tour + catalog
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "gfx.h"
#include "params.h"
#include "pbm.h"
#include "rbj.h"
#include "sim.h"
#include "ui.h"
#include "ui_priv.h"

static uint8_t s_fb[GFX_PAGES][GFX_W];
static gfx_t G;
static double s_now_f;          /* exact 60 Hz clock */
static const char *s_out;
static FILE *s_man;

static bool s_gif;
static uint32_t s_gif_next;
static int s_gif_idx;
static bool s_seq;
static char s_seq_name[48];
static int s_seq_idx;
static int s_shots;

/* timing */
typedef struct { double sum, max; long n; } tstat_t;
static tstat_t s_t_render[SCR_COUNT], s_t_update, s_t_all;

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void mkdirs(const char *p)
{
    char b[512];
    snprintf(b, sizeof b, "%s", p);
    for (char *q = b + 1; *q; q++) {
        if (*q == '/') { *q = 0; mkdir(b, 0755); *q = '/'; }
    }
    mkdir(b, 0755);
}

static void render(void)
{
    scr_id_t cur = ui_current();
    double t0 = now_ns();
    ui_render(&G);
    double dt = now_ns() - t0;
    tstat_t *s = &s_t_render[cur];
    s->sum += dt; s->n++; if (dt > s->max) s->max = dt;
    s_t_all.sum += dt; s_t_all.n++; if (dt > s_t_all.max) s_t_all.max = dt;
}

static void save(const char *kind, const char *name)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s.pbm", s_out, name);
    char dir[600];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdirs(dir); }
    if (pbm_write(path, s_fb) != 0) { fprintf(stderr, "write %s: %s\n", path, strerror(errno)); exit(1); }
    fprintf(s_man, "%s %s %u %u\n", kind, name, SIM.disp_level, SIM.now);
}

static void frame(void)
{
    s_now_f += 1000.0 / 60.0;
    uint32_t now = (uint32_t)llround(s_now_f);
    SIM.now = now;
    double t0 = now_ns();
    ui_update(now);
    double dt = now_ns() - t0;
    s_t_update.sum += dt; s_t_update.n++; if (dt > s_t_update.max) s_t_update.max = dt;
    render();
    char nm[96];
    if (s_gif && now >= s_gif_next) {
        snprintf(nm, sizeof nm, "tour/f%05d", s_gif_idx++);
        save("gif", nm);
        s_gif_next += 40;
    }
    if (s_seq) {
        snprintf(nm, sizeof nm, "seq/%s/f%03d", s_seq_name, s_seq_idx++);
        save("seq", nm);
    }
}

static void wait_ms(uint32_t ms)
{
    double end = s_now_f + ms;
    while (s_now_f + 1000.0 / 60.0 <= end + 0.01) frame();
}

static void ev(ui_ev_type_t t, int d)
{
    ui_event_t e = { t, (int16_t)d, SIM.now };
    ui_event(&e);
}

static void rot(int n, uint32_t interval)
{
    int dir = n > 0 ? 1 : -1;
    for (int i = 0; i < abs(n); i++) { ev(UI_EV_ROTATE, dir); wait_ms(interval); }
}
static void press(void) { ev(UI_EV_BTN_DOWN, 0); wait_ms(85); ev(UI_EV_BTN_UP, 0); wait_ms(34); }
static void press_home(void) { press(); wait_ms(300); }        /* HOME waits out the double-click window */
static void longpress(void) { ev(UI_EV_BTN_DOWN, 0); wait_ms(620); ev(UI_EV_BTN_UP, 0); wait_ms(34); }
static void dbl(void) { ev(UI_EV_BTN_DOWN, 0); wait_ms(70); ev(UI_EV_BTN_UP, 0); wait_ms(120); ev(UI_EV_BTN_DOWN, 0); wait_ms(70); ev(UI_EV_BTN_UP, 0); wait_ms(34); }

static void shot(const char *name)
{
    render();
    save("shot", name);
    s_shots++;
}

static void seq_start(const char *name) { snprintf(s_seq_name, sizeof s_seq_name, "%s", name); s_seq_idx = 0; s_seq = true; }
static void seq_stop(void) { s_seq = false; fprintf(s_man, "seqend %s %d 0 0\n", s_seq_name, s_seq_idx); }

static void boot_fresh(void)
{
    params_init();
    sim_reset();
    s_now_f = 0.0;
    SIM.now = 0;
    gfx_init(&G, s_fb);
    ui_init(0);
}

/* ------------------------------------------------------------------ selftest */
float eq_debug_column_db(int x);

static int selftest(void)
{
    int err = params_selftest();
    if (err) fprintf(stderr, "params selftest: %d errors\n", err);
    /* EQ curve (float phi form from actual coefficients) vs double reference */
    params_init();
    params_set(P_EQ_B1_GAIN, 6.f, SRC_INIT); params_set(P_EQ_B1_FREQ, 60.f, SRC_INIT);
    params_set(P_EQ_B2_GAIN, -9.f, SRC_INIT); params_set(P_EQ_B2_FREQ, 180.f, SRC_INIT); params_set(P_EQ_B2_Q, 4.f, SRC_INIT);
    params_set(P_EQ_B3_GAIN, 4.5f, SRC_INIT); params_set(P_EQ_B3_FREQ, 2500.f, SRC_INIT); params_set(P_EQ_B3_Q, 0.5f, SRC_INIT);
    params_set(P_EQ_B4_GAIN, -12.f, SRC_INIT); params_set(P_EQ_B4_FREQ, 7000.f, SRC_INIT); params_set(P_EQ_B4_Q, 9.f, SRC_INIT);
    params_set(P_EQ_B5_GAIN, 18.f, SRC_INIT); params_set(P_EQ_B5_FREQ, 14000.f, SRC_INIT);
    eq_model_update();
    static const param_id_t bp[5][3] = {
        { P_EQ_B1_FREQ, P_EQ_B1_GAIN, P_EQ_B1_Q }, { P_EQ_B2_FREQ, P_EQ_B2_GAIN, P_EQ_B2_Q },
        { P_EQ_B3_FREQ, P_EQ_B3_GAIN, P_EQ_B3_Q }, { P_EQ_B4_FREQ, P_EQ_B4_GAIN, P_EQ_B4_Q },
        { P_EQ_B5_FREQ, P_EQ_B5_GAIN, P_EQ_B5_Q } };
    biquad_coef_t c[5];
    for (int b = 0; b < 5; b++) rbj_design(&c[b], eq_band_type(b), 48000.f, params_get(bp[b][0]), params_get(bp[b][1]), params_get(bp[b][2]));
    double worst = 0;
    for (int x = 0; x < 128; x++) {
        double f = 20.0 * pow(1000.0, x / 127.0), ref = 0;
        for (int b = 0; b < 5; b++) ref += biquad_mag_db(&c[b], f, 48000.0);
        double e = fabs(ref - eq_debug_column_db(x));
        if (e > worst) worst = e;
    }
    printf("selftest: EQ display curve vs biquad_mag_db (double): max error %.4f dB\n", worst);
    if (worst > 0.05) { fprintf(stderr, "EQ curve error too large\n"); err++; }
    return err;
}

/* ------------------------------------------------------------------ tour (GIF) */
static void tour(void)
{
    boot_fresh();
    params_set(P_UI_DIM, 0.f, SRC_INIT);
    params_set(P_UI_SAVER, 1.f, SRC_INIT);          /* 1 min */
    s_gif = true; s_gif_next = 0; s_gif_idx = 0;
    wait_ms(2600);
    /* volume: a brisk spin up, a slow settle down */
    rot(16, 40); wait_ms(500);
    rot(-6, 140); wait_ms(900);
    press_home(); wait_ms(600);                     /* MENU */
    press(); wait_ms(1000);                         /* EQ */
    rot(1, 300); rot(1, 300); wait_ms(300);         /* PEAK 2 */
    press(); wait_ms(250);                          /* FREQ */
    rot(9, 70); wait_ms(250);
    press(); rot(10, 70); wait_ms(300);             /* GAIN */
    press(); rot(-7, 90); wait_ms(400);             /* Q */
    press(); press(); wait_ms(500);                 /* BAND -> done */
    rot(2, 250); wait_ms(500);                      /* HIGH SHELF */
    press(); rot(-6, 60); press(); rot(8, 60); wait_ms(300); press(); press(); press();
    wait_ms(700);
    longpress(); wait_ms(500);                      /* MENU */
    rot(2, 220); press(); wait_ms(700);             /* EFFECTS */
    for (int fx = 0; fx < 5; fx++) {
        press(); wait_ms(500);                      /* open effect */
        press(); wait_ms(500);                      /* EFFECT ON */
        rot(1, 200); press(); rot(fx == 3 ? -5 : 7, 90); press(); wait_ms(300);
        rot(1, 200); press(); rot(6, 90); press();
        wait_ms(1300);
        longpress(); wait_ms(400);
        if (fx < 4) rot(1, 250);
    }
    longpress(); wait_ms(400);                      /* MENU */
    rot(3, 220); press(); wait_ms(700);             /* PRESETS */
    rot(2, 250); press(); wait_ms(400); press();    /* load */
    wait_ms(1800);
    longpress(); wait_ms(400);
    rot(1, 220); press(); wait_ms(700);             /* SETTINGS */
    rot(3, 200); wait_ms(300); rot(11, 110); wait_ms(400);
    press(); wait_ms(900);                          /* ABOUT */
    ev(UI_EV_BTN_DOWN, 0); wait_ms(3300); ev(UI_EV_BTN_UP, 0);
    wait_ms(1500);
    rot(1, 100); wait_ms(1700);
    longpress(); wait_ms(250); longpress(); wait_ms(250); longpress(); wait_ms(250); longpress(); wait_ms(1500);
    /* fast-forward to the screensaver */
    s_gif = false;
    wait_ms(57000);
    s_gif = true; s_gif_next = SIM.now;
    wait_ms(4500);
    rot(1, 100); wait_ms(1500);
    s_gif = false;
    fprintf(s_man, "gifend tour %d 0 0\n", s_gif_idx);
}

/* ------------------------------------------------------------------ catalog */
/* deterministic list navigation: lists clamp at the top, so "rewind then step" */
static void go(int k) { rot(-16, 20); wait_ms(60); if (k) rot(k, 70); wait_ms(250); }

static void catalog(void)
{
    boot_fresh();
    params_set(P_UI_DIM, 0.f, SRC_INIT);
    params_set(P_UI_SAVER, 0.f, SRC_INIT);
    seq_start("boot");
    wait_ms(120); shot("boot_1_line");
    wait_ms(330); shot("boot_2_unfold");
    wait_ms(500); shot("boot_3_wordmark");
    wait_ms(900);
    seq_stop();
    wait_ms(1500);
    shot("home");
    seq_start("home_volume_spin");
    rot(12, 35);
    shot("home_volume_ruler");
    wait_ms(400);
    seq_stop();
    rot(-14, 35);
    wait_ms(1800);
    dbl(); wait_ms(1600); shot("home_muted");
    dbl(); wait_ms(1600);
    SIM.clip_in = true; SIM.clip_out = true; wait_ms(120); shot("home_clip");
    wait_ms(2500);
    params_set(P_VOLUME, -80.f, SRC_INIT); wait_ms(400); shot("home_vol_min");
    params_set(P_VOLUME, -20.f, SRC_INIT); wait_ms(400);
    SIM.st.play_active = false; SIM.st.rec_active = false; SIM.music = 0.f; SIM.input = 0.f;
    wait_ms(3000); shot("home_idle");
    SIM.st.play_active = true; SIM.st.rec_active = true; SIM.music = 1.f; SIM.input = 0.6f;
    wait_ms(800);

    seq_start("push_home_menu");
    press_home(); wait_ms(250);
    seq_stop();
    shot("menu");
    go(5); wait_ms(300); shot("menu_scrolled");

    /* EQ */
    go(0);
    seq_start("push_menu_eq");
    press(); wait_ms(260);
    seq_stop();
    wait_ms(600); shot("eq");
    rot(2, 200); wait_ms(400); shot("eq_band_select");
    press(); rot(8, 70); wait_ms(400); shot("eq_edit_freq");
    press(); rot(12, 70); wait_ms(400); shot("eq_edit_gain");
    press(); rot(-8, 70); wait_ms(400); shot("eq_edit_q");
    press(); wait_ms(300); shot("eq_edit_band");
    press(); wait_ms(300);
    rot(-2, 150); press(); rot(8, 60); press(); rot(-6, 60); press(); press(); press(); wait_ms(400);
    rot(4, 150); press(); rot(-10, 60); press(); rot(-12, 60); press(); press(); press(); wait_ms(600);
    shot("eq_shaped");
    rot(1, 150); wait_ms(500); shot("eq_global");
    press(); rot(1, 150); press(); rot(-6, 60); wait_ms(400); shot("eq_global_edit");
    press(); longpress(); wait_ms(200);
    params_set(P_UI_SPECTRUM, 0.f, SRC_INIT); rot(-3, 120); wait_ms(500); shot("eq_no_spectrum");
    params_set(P_UI_SPECTRUM, 1.f, SRC_INIT);
    params_set(P_EQ_ON, 0.f, SRC_UI); wait_ms(500); shot("eq_bypassed");
    params_set(P_EQ_ON, 1.f, SRC_UI); wait_ms(300);
    seq_start("pop_eq_menu");
    longpress(); wait_ms(260);
    seq_stop();

    /* GAIN */
    go(1); press(); wait_ms(700); shot("gain");
    press(); rot(10, 60); wait_ms(500); shot("gain_edit");
    press(); rot(2, 150); press(); rot(40, 25); press(); wait_ms(500); shot("gain_monitor");
    SIM.clip_in = true; wait_ms(100); shot("gain_clip");
    longpress(); wait_ms(400);

    /* EFFECTS */
    go(2);
    seq_start("push_menu_effects");
    press(); wait_ms(260);
    seq_stop();
    wait_ms(400); shot("effects");
    static const char *const names[7] = { "fx_saturate", "fx_chorus", "fx_delay", "fx_reverb", "fx_compress", "fx_loudness", "fx_crossfeed" };
    for (int i = 0; i < 7; i++) {
        go(i);
        press(); wait_ms(600);
        char nm[64];
        snprintf(nm, sizeof nm, "%s_off", names[i]); shot(nm);
        press(); wait_ms(400);                                  /* EFFECT ON */
        if (i == 0) { rot(1, 120); press(); rot(14, 50); press(); }
        if (i == 1) { rot(2, 120); press(); rot(10, 50); press(); }
        if (i == 2) { rot(1, 120); press(); rot(-2, 150); press(); }
        if (i == 3) { rot(1, 120); press(); rot(5, 60); press(); }
        if (i == 4) { rot(1, 120); press(); rot(-12, 50); press(); rot(1, 120); press(); rot(10, 50); press(); }
        if (i == 5) { params_set(P_VOLUME, -40.f, SRC_UI); wait_ms(300); }   /* volume -40 dB: visible compensation */
        wait_ms(900); shot(names[i]);
        if (i == 0) { rot(1, 120); press(); wait_ms(300); shot("fx_saturate_edit"); press(); }
        if (i == 1) { rot(-1, 120); press(); rot(1, 150); press(); wait_ms(800); shot("fx_chorus_ensemble"); }
        if (i == 2) {
            rot(3, 150); press(); rot(-1, 150); press(); wait_ms(800); shot("fx_delay_stereo");
            rot(-3, 150); press(); rot(-2, 150); press(); wait_ms(600); shot("fx_delay_free");
        }
        if (i == 3) { rot(2, 120); press(); rot(40, 25); press(); wait_ms(800); shot("fx_reverb_damped"); }
        longpress(); wait_ms(400);
    }
    wait_ms(300); shot("effects_on");
    longpress(); wait_ms(400);

    /* NETWORK */
    go(3); press(); wait_ms(600); shot("network_off");
    go(0); press(); wait_ms(400); shot("network_starting");
    wait_ms(2000); shot("network_connected");
    longpress(); wait_ms(300); longpress(); wait_ms(1200); shot("home_radios");
    press_home(); wait_ms(300);
    press(); wait_ms(400);                                      /* NETWORK (cursor remembered) */
    go(1); press(); wait_ms(300); shot("wifi_setup_starting");
    wait_ms(1300); shot("wifi_setup");
    longpress(); wait_ms(300);
    go(2); press(); wait_ms(400); shot("network_forget_confirm");
    rot(1, 150); wait_ms(300); shot("network_forget_confirm_yes");
    press(); wait_ms(250); shot("toast_forgotten");
    wait_ms(1500);
    go(0); press(); wait_ms(300);                               /* wifi off */
    longpress(); wait_ms(400);

    /* BLUETOOTH */
    go(4); press(); wait_ms(600); shot("bluetooth_off");
    press(); wait_ms(600); shot("bluetooth_pairing");
    wait_ms(2600); shot("bluetooth_connected");
    rot(4, 150); wait_ms(500); shot("bluetooth_help");
    press(); wait_ms(300);                                      /* BLE off again */
    longpress(); wait_ms(400);

    /* PRESETS */
    go(5); press(); wait_ms(600); shot("presets");
    rot(2, 150); press(); wait_ms(400); shot("presets_choose");
    rot(1, 150); wait_ms(300); shot("presets_choose_save");
    press(); wait_ms(400); shot("presets_save_confirm");
    rot(1, 120); press(); wait_ms(220); shot("toast_saved");
    wait_ms(1600);
    rot(1, 150); press(); wait_ms(200); press(); wait_ms(220); shot("toast_loaded");
    wait_ms(1600);
    params_set(P_EQ_B3_GAIN, 3.f, SRC_UI); wait_ms(300); shot("presets_edited");
    longpress(); wait_ms(400);

    /* SETTINGS */
    go(6); press(); wait_ms(600); shot("settings");
    go(0); press(); rot(-4, 120); wait_ms(400); shot("settings_edit_brightness");
    press(); rot(6, 150); wait_ms(500); shot("settings_scrolled");
    rot(10, 150); wait_ms(500); shot("settings_bottom");
    rot(-1, 150); press(); wait_ms(400); shot("factory_confirm");
    press(); wait_ms(300);
    rot(1, 150);
    seq_start("push_settings_about");
    press(); wait_ms(260);
    seq_stop();
    wait_ms(400); shot("about");
    ev(UI_EV_BTN_DOWN, 0); wait_ms(1600); shot("about_hold");
    wait_ms(1500); ev(UI_EV_BTN_UP, 0); wait_ms(600); shot("diag_cpu");
    rot(1, 100); wait_ms(600); shot("diag_system");
    longpress(); wait_ms(300); longpress(); wait_ms(300); longpress(); wait_ms(300);
    seq_start("pop_menu_home");
    longpress(); wait_ms(260);
    seq_stop();
    wait_ms(600);

    /* knob in MIDI CC mode */
    params_set(P_MIDI_ENC, 1.f, SRC_UI);
    rot(6, 60); wait_ms(200); shot("home_midi_cc");
    wait_ms(2000);
    params_set(P_MIDI_ENC, 0.f, SRC_UI);

    SIM.st.usb_mounted = false; SIM.st.play_active = SIM.st.rec_active = false;
    wait_ms(500); shot("home_no_usb");
    SIM.st.usb_mounted = true; SIM.st.play_active = SIM.st.rec_active = true;

    /* dim + screensaver */
    params_set(P_UI_DIM, 1.f, SRC_INIT);
    params_set(P_UI_SAVER, 1.f, SRC_INIT);
    wait_ms(16500); shot("home_dimmed");
    wait_ms(60000 - 16500 - 400);
    seq_start("saver_enter");
    wait_ms(1300);
    seq_stop();
    shot("saver");
    wait_ms(30000); shot("saver_later");
    rot(1, 100); wait_ms(600); shot("home_woken");
}

static void bench(void)
{
    /* steady-state cost of the heaviest screens */
    printf("\nrender time per frame (host, ns): avg / max\n");
    static const char *const nm[SCR_COUNT] = { "BOOT", "HOME", "MENU", "EQ", "GAIN", "FXLIST", "SAT", "CHORUS", "DELAY",
        "REVERB", "COMP", "NET", "NETSETUP", "BT", "PRESETS", "SETTINGS", "ABOUT", "DIAG", "SAVER" };
    for (int i = 0; i < SCR_COUNT; i++)
        if (s_t_render[i].n) printf("  %-9s %8.0f / %8.0f  (%ld frames)\n", nm[i], s_t_render[i].sum / s_t_render[i].n, s_t_render[i].max, s_t_render[i].n);
    printf("  all       %8.0f / %8.0f\n", s_t_all.sum / s_t_all.n, s_t_all.max);
    printf("  ui_update %8.0f / %8.0f\n", s_t_update.sum / s_t_update.n, s_t_update.max);
    FILE *f;
    char p[600];
    snprintf(p, sizeof p, "%s/timing_%s.txt", s_out, s_shots ? "catalog" : "tour");
    f = fopen(p, "w");
    if (f) {
        for (int i = 0; i < SCR_COUNT; i++)
            if (s_t_render[i].n) fprintf(f, "%s %.0f %.0f %ld\n", nm[i], s_t_render[i].sum / s_t_render[i].n, s_t_render[i].max, s_t_render[i].n);
        fprintf(f, "ALL %.0f %.0f %ld\n", s_t_all.sum / s_t_all.n, s_t_all.max, s_t_all.n);
        fprintf(f, "UPDATE %.0f %.0f %ld\n", s_t_update.sum / s_t_update.n, s_t_update.max, s_t_update.n);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    s_out = argc > 1 ? argv[1] : "out";
    mkdirs(s_out);
    char p[600];
    snprintf(p, sizeof p, "%s/manifest_%s.txt", s_out, argc > 2 ? argv[2] : "all");
    s_man = fopen(p, "w");
    if (!s_man) { perror(p); return 1; }
    const char *mode = argc > 2 ? argv[2] : "all";
    int err = 0;
    if (!strcmp(mode, "all") || !strcmp(mode, "tour")) { err += selftest(); tour(); }
    if (!strcmp(mode, "all") || !strcmp(mode, "catalog")) catalog();
    fclose(s_man);
    bench();
    printf("\n%d shots, %d tour frames\n", s_shots, s_gif_idx);
    return err ? 1 : 0;
}
