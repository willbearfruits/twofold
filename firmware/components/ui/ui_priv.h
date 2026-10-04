/*
 * ui_priv.h: internals shared by the UI screens (not part of the public contract).
 *
 * Layout grid (see docs/ui_design.md):
 *   - 4 px outer text margin (x = 4 .. 123), 2 px for full-bleed highlights.
 *   - header band y 0..6 (labels on rows 1..5), hairline at y 7, content from y 9.
 *   - list rows 11 px (5 visible rows: y 9, 20, 31, 42, 53).
 *   - panel screens: header, visualisation y 9..45, parameter strip y 46..63.
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "params.h"
#include "rbj.h"
#include "ui.h"
#include "ui_sys.h"

#define F_TINY  (&gfx_font_tiny)
#define F_SMALL (&gfx_font_small)
#define F_MED   (&gfx_font_med)
#define F_BIG   (&gfx_font_big)
#define F_ICON  (&gfx_font_icons)

#define C_ON  GFX_ON
#define C_OFF GFX_OFF
#define C_XOR GFX_XOR

/* grid */
#define UI_MX        4      /* outer text margin */
#define UI_HDR_LINE  7
#define UI_TOP       9      /* content start */
#define UI_ROW       11
#define UI_ROWS_VIS  5
#define UI_VIZ_Y     9
#define UI_VIZ_H     36     /* 9..44 */
#define UI_STRIP_Y   46
#define UI_CELL_W    42

/* timing (ms) */
#define UI_LONG_MS      500
#define UI_DOUBLE_MS    250
#define UI_DIAG_HOLD_MS 3000
#define UI_SLIDE_MS     190
#define UI_FADE_MS      260
#define UI_TOAST_MS     1400

/* ---------- tweens ---------- */
typedef struct { float v, t; } tw_t;
static inline void tw_snap(tw_t *w, float v) { w->v = w->t = v; }
static inline void tw_to(tw_t *w, float t) { w->t = t; }
/* exponential approach with time constant tau (s); frame-rate independent. Returns true while moving. */
bool tw_step(tw_t *w, float dt, float tau);
static inline float ease_out_cubic(float p) { p = 1.f - p; return 1.f - p * p * p; }
static inline float ease_in_out(float p) { return p < 0.5f ? 4.f * p * p * p : 1.f - powf(-2.f * p + 2.f, 3.f) * 0.5f; }
static inline float clampf_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int clampi_(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int iroundf(float v) { return (int)lrintf(v); }

/* ---------- screens ---------- */
typedef enum {
    SCR_BOOT = 0, SCR_HOME, SCR_MENU, SCR_EQ, SCR_GAIN, SCR_FXLIST,
    SCR_FX_SAT, SCR_FX_CHO, SCR_FX_DLY, SCR_FX_REV, SCR_FX_CMP,
    SCR_NET, SCR_NET_SETUP, SCR_BT, SCR_PRESETS, SCR_SETTINGS, SCR_ABOUT, SCR_DIAG,
    SCR_SAVER, SCR_COUNT
} scr_id_t;

typedef enum { A_ROT = 0, A_PRESS, A_BACK, A_DOUBLE } ui_act_t;

typedef struct {
    void (*enter)(void);                       /* fresh entry (push) */
    void (*input)(ui_act_t a, int raw);        /* raw = detents for A_ROT */
    void (*tick)(float dt);                    /* every ui_update while visible */
    void (*draw)(gfx_t *g);                    /* local 128x64 */
    /* long-hold ownership: called while the button is held past UI_LONG_MS.
     * Return true to suppress the default "back" action on release. */
    bool (*hold)(uint32_t held_ms, bool released);
} screen_t;

extern const screen_t *const g_screens[SCR_COUNT];

void ui_push(scr_id_t s);
void ui_pop(void);
void ui_go_home(void);
scr_id_t ui_current(void);

/* overlays */
void ui_toast(const char *msg);
typedef void (*ui_confirm_cb)(bool yes, int arg);
void ui_confirm(const char *title, const char *detail, const char *yes_label, ui_confirm_cb cb, int arg);
bool ui_modal_active(void);

/* ---------- shared live context (refreshed in ui_update) ---------- */
typedef struct {
    uint32_t now;            /* ms */
    float    t;              /* seconds since init */
    float    dt;             /* last frame delta (s) */
    sys_status_t st;
    dsp_viz_t viz;
    float comp_gr_db, lim_gr_db, sat_in;       /* latest taken */
    /* meters (dBFS, ballistic) */
    float in_db[2], out_db[2];
    float in_pk[2], out_pk[2];
    float in_hold[2], out_hold[2];             /* seconds left */
    uint32_t in_clip_until[2], out_clip_until[2];
    uint32_t last_in_clip[2], last_out_over[2];
    bool  signal;                              /* any visible meter activity */
} ui_ctx_t;
extern ui_ctx_t U;

/* ---------- input helpers ---------- */
/* Accelerated steps for a raw detent delta (velocity based, fractional carry kept per call site). */
int  ui_accel_steps(int raw, float *carry);
float ui_accel_mult(void);
/* Nudge a parameter from the encoder: floats accelerate, enums/ints/bools step by detent. */
bool ui_param_rotate(param_id_t id, int raw, float *carry);
/* Side effects of UI-originated parameter changes (radios, OLED type, ...). */
void ui_param_changed(param_id_t id);
void ui_param_toggle(param_id_t id);

/* ---------- formatting ---------- */
void ui_fmt_value(param_id_t id, float v, char *val, size_t vn, char *unit, size_t un);
const char *ui_short_label(param_id_t id);

/* ---------- drawing helpers ---------- */
void ui_header(gfx_t *g, const char *title, const char *right);
int  ui_pill(gfx_t *g, int x, int y, const char *txt, bool filled, gfx_align_t a);   /* returns width */
void ui_onoff_pill(gfx_t *g, bool on);                                             /* header right */
/* value + small unit, unit baseline-aligned to value. Returns total width. */
int  ui_value_unit(gfx_t *g, int x, int y, const char *val, const char *unit, const gfx_font_t *fv,
                   gfx_align_t a, gfx_color_t c);
int  ui_value_unit_width(const char *val, const char *unit, const gfx_font_t *fv);
float ui_meter_pos(float db);                 /* IEC 60268-18 style deflection 0..1 */
void ui_meter_bar(gfx_t *g, int x, int y, int w, int h, float db, float pk, bool clip, bool seg);
void ui_scrollbar(gfx_t *g, int x, int y, int h, float pos, float vis, float total);
void ui_draw_hold_ring(gfx_t *g, int cx, int cy, int r, float p);

/* ---------- list widget ---------- */
typedef enum { LI_SUB = 0, LI_PARAM, LI_ACTION, LI_INFO, LI_TEXT } li_kind_t;
struct list;
typedef struct list_item {
    uint8_t kind;
    uint8_t icon;               /* GFX_I_* or 0 */
    int16_t param;              /* LI_PARAM */
    int16_t arg;                /* LI_SUB: screen id, LI_ACTION: action id */
    const char *label;
    /* optional dynamic value (LI_SUB/LI_ACTION/LI_INFO); return false to hide the row value */
    bool (*value)(const struct list_item *it, char *buf, size_t n);
} list_item_t;

typedef struct list {
    const list_item_t *items;
    int n;
    int cur;
    bool edit;
    tw_t hl, scroll;
    float carry;
    void (*on_action)(int arg);
    bool (*enabled)(int idx);   /* optional: greys out + skips items */
} list_t;

void list_init(list_t *l, const list_item_t *items, int n, void (*on_action)(int));
void list_input(list_t *l, ui_act_t a, int raw);
bool list_tick(list_t *l, float dt);
void list_draw(gfx_t *g, list_t *l, int y0);

/* ---------- parameter strip (panel screens) ---------- */
typedef struct {
    const int16_t *params;      /* param ids */
    const char *const *labels;  /* optional override labels (NULL = ui_short_label) */
    int n;
    int cur;
    bool edit;
    tw_t scroll, hl;
    float carry;
    int cell_w;
} strip_t;

void strip_init(strip_t *s, const int16_t *params, const char *const *labels, int n, int cell_w);
/* returns true when the action was consumed (false: A_BACK at top level -> caller pops) */
bool strip_input(strip_t *s, ui_act_t a, int raw);
bool strip_tick(strip_t *s, float dt);
void strip_draw(gfx_t *g, strip_t *s, int y);

/* ---------- EQ curve helpers (shared by EQ screen and HOME mini view) ---------- */
void eq_model_update(void);                     /* recompute when EQ params changed */
float eq_x_to_hz(int x, int w);
int   eq_hz_to_x(float hz, int w);

/* ---------- spectrum ---------- */
void spec_init(void);
void spec_update(float dt);                     /* pull samples, FFT, smooth */
const uint8_t *spec_columns(void);              /* 128 heights in 1/4 px units (0..255) */

/* ---------- misc ---------- */
void ui_mark_anim(void);                        /* something moved: keep frame rate up */
void ui_brightness_apply(bool force);
extern uint32_t g_last_input_ms;
const char *ui_wifi_state_name(wifi_state_t w);
const char *ui_ble_state_name(ble_state_t b);
