/* ui_widgets.c: shared drawing primitives and the two interaction widgets (list, parameter strip). */
#include "ui_priv.h"

void ui_mark_anim(void);

/* ------------------------------------------------------------------ text helpers */
const char *ui_wifi_state_name(wifi_state_t w)
{
    switch (w) {
    case WIFI_OFF: return "OFF";
    case WIFI_STARTING: return "STARTING";
    case WIFI_AP_SETUP: return "SETUP MODE";
    case WIFI_STA_CONNECTING: return "CONNECTING";
    case WIFI_STA_CONNECTED: return "CONNECTED";
    default: return "ERROR";
    }
}

const char *ui_ble_state_name(ble_state_t b)
{
    switch (b) {
    case BLE_OFF: return "OFF";
    case BLE_ADVERTISING: return "PAIRING";
    case BLE_CONNECTED: return "CONNECTED";
    default: return "ERROR";
    }
}

void ui_fmt_value(param_id_t id, float v, char *val, size_t vn, char *unit, size_t un)
{
    params_format(id, v, val, vn);
    const char *u = params_format_unit(id, v);
    if (!strcmp(val, "-inf")) snprintf(val, vn, "-" GFX_S_INF);
    if (!strcmp(u, ":1")) u = ":1";
    snprintf(unit, un, "%s", u);
}

const char *ui_short_label(param_id_t id)
{
    switch (id) {
    case P_DLY_HICUT: case P_REV_LOCUT: case P_DLY_LOCUT: break;
    case P_UI_DIM: return "DIM";
    case P_MIDI_ENC: return "KNOB";
    default: break;
    }
    return params_desc(id)->name;
}

/* ------------------------------------------------------------------ chrome */
void ui_header(gfx_t *g, const char *title, const char *right)
{
    gfx_text_ex(g, F_SMALL, UI_MX, 1, title, 1, C_ON);
    if (right && right[0]) gfx_text_align(g, F_SMALL, GFX_W - UI_MX, 1, right, 1, GFX_RIGHT, C_ON);
    gfx_hline_pat(g, 0, UI_HDR_LINE, GFX_W, 0x55, C_ON);
}

int ui_pill(gfx_t *g, int x, int y, const char *txt, bool filled, gfx_align_t a)
{
    int w = gfx_text_width_ex(F_SMALL, txt, 1) + 7;
    if (a == GFX_RIGHT) x -= w;
    else if (a == GFX_CENTER) x -= w / 2;
    if (filled) {
        gfx_rrect(g, x, y, w, 7, 2, C_ON);
        gfx_text_ex(g, F_SMALL, x + 4, y + 1, txt, 1, C_OFF);
    } else {
        gfx_rframe(g, x, y, w, 7, 2, C_ON);
        gfx_text_ex(g, F_SMALL, x + 4, y + 1, txt, 1, C_ON);
    }
    return w;
}

void ui_onoff_pill(gfx_t *g, bool on)
{
    ui_pill(g, GFX_W - 2, 0, on ? "ON" : "OFF", on, GFX_RIGHT);
}

int ui_value_unit_width(const char *val, const char *unit, const gfx_font_t *fv)
{
    int w = gfx_text_width(fv, val);
    if (unit && unit[0]) w += 2 + gfx_text_width(F_SMALL, unit);
    return w;
}

int ui_value_unit(gfx_t *g, int x, int y, const char *val, const char *unit, const gfx_font_t *fv,
                  gfx_align_t a, gfx_color_t c)
{
    int w = ui_value_unit_width(val, unit, fv);
    if (a == GFX_CENTER) x -= w / 2;
    else if (a == GFX_RIGHT) x -= w;
    int xe = gfx_text(g, fv, x, y, val, c);
    if (unit && unit[0]) gfx_text(g, F_SMALL, xe + 2, y + fv->cap - 5, unit, c);
    return w;
}

/* IEC 60268-18 style deflection: more resolution near 0 dBFS. */
float ui_meter_pos(float db)
{
    float d;
    if (db < -70.f) d = 0.f;
    else if (db < -60.f) d = (db + 70.f) * 0.25f;
    else if (db < -50.f) d = (db + 60.f) * 0.5f + 2.5f;
    else if (db < -40.f) d = (db + 50.f) * 0.75f + 7.5f;
    else if (db < -30.f) d = (db + 40.f) * 1.5f + 15.f;
    else if (db < -20.f) d = (db + 30.f) * 2.0f + 30.f;
    else d = (db + 20.f) * 2.5f + 50.f;
    return clampf_(d * 0.01f, 0.f, 1.f);
}

void ui_meter_bar(gfx_t *g, int x, int y, int w, int h, float db, float pk, bool clip, bool seg)
{
    int fill = iroundf(ui_meter_pos(db) * (float)w);
    if (seg) {
        for (int i = 0; i < fill; i++)
            if (i % 3 != 2) gfx_vline(g, x + i, y, h, C_ON);
    } else {
        gfx_rect(g, x, y, fill, h, C_ON);
    }
    if (pk > -69.f) {
        int px = iroundf(ui_meter_pos(pk) * (float)(w - 1));
        gfx_vline(g, x + px, y, h, C_ON);
    }
    (void)clip;
}

void ui_scrollbar(gfx_t *g, int x, int y, int h, float pos, float vis, float total)
{
    if (total <= vis + 0.5f) return;
    int th = clampi_(iroundf((float)h * vis / total), 4, h);
    int ty = y + iroundf((float)(h - th) * clampf_(pos / (total - vis), 0.f, 1.f));
    gfx_vline_pat(g, x, y, h, 0x11, C_ON);
    gfx_vline(g, x, ty, th, C_ON);
}

void ui_draw_hold_ring(gfx_t *g, int cx, int cy, int r, float p)
{
    gfx_circle(g, cx, cy, r, C_OFF);
    gfx_arc(g, cx, cy, r, -90, -90 + iroundf(360.f * clampf_(p, 0.f, 1.f)), C_ON);
}

/* ------------------------------------------------------------------ list */
static bool li_selectable(const list_t *l, int i)
{
    const list_item_t *it = &l->items[i];
    if (it->kind == LI_TEXT || it->kind == LI_INFO) return false;
    if (l->enabled && !l->enabled(i)) return false;
    return true;
}

static int item_h(const list_item_t *it) { return it->kind == LI_TEXT ? 8 : UI_ROW; }
static int item_y(const list_t *l, int i)
{
    int y = 0;
    for (int k = 0; k < i; k++) y += item_h(&l->items[k]);
    return y;
}

static void list_retarget(list_t *l)
{
    tw_to(&l->hl, (float)item_y(l, l->cur));
    int view = GFX_H - UI_TOP;
    int total = item_y(l, l->n);
    int maxs = total > view ? total - view : 0;
    float s = l->scroll.t;
    int top = item_y(l, l->cur), bot = top + UI_ROW;
    int last_sel = l->n - 1;
    while (last_sel > 0 && !li_selectable(l, last_sel)) last_sel--;
    if (l->cur == last_sel && last_sel < l->n - 1) s = (float)maxs;   /* reveal trailing text rows */
    else {
        int margin = UI_ROW;                                          /* keep one row of context */
        if (top - margin < s) s = (float)(top - margin);
        if (bot + margin > s + view) s = (float)(bot + margin - view);
    }
    tw_to(&l->scroll, clampf_(s, 0.f, (float)maxs));
}

void list_init(list_t *l, const list_item_t *items, int n, void (*on_action)(int))
{
    l->items = items; l->n = n; l->on_action = on_action;
    l->edit = false; l->carry = 0.f;
    if (l->cur >= n || l->cur < 0) l->cur = 0;
    while (l->cur < n - 1 && !li_selectable(l, l->cur)) l->cur++;
    list_retarget(l);
    tw_snap(&l->hl, l->hl.t);
    tw_snap(&l->scroll, l->scroll.t);
}

void list_input(list_t *l, ui_act_t a, int raw)
{
    const list_item_t *it = &l->items[l->cur];
    if (a == A_ROT) {
        if (l->edit && it->kind == LI_PARAM) {
            ui_param_rotate((param_id_t)it->param, raw, &l->carry);
            return;
        }
        int dir = raw > 0 ? 1 : -1, n = raw > 0 ? raw : -raw;
        for (int k = 0; k < n; k++) {
            int j = l->cur + dir;
            while (j >= 0 && j < l->n && !li_selectable(l, j)) j += dir;
            if (j < 0 || j >= l->n) break;
            l->cur = j;
        }
        list_retarget(l);
    } else if (a == A_PRESS) {
        switch (it->kind) {
        case LI_SUB: ui_push((scr_id_t)it->arg); break;
        case LI_ACTION: if (l->on_action) l->on_action(it->arg); break;
        case LI_PARAM:
            if (params_desc((param_id_t)it->param)->type == PT_BOOL) ui_param_toggle((param_id_t)it->param);
            else { l->edit = !l->edit; l->carry = 0.f; }
            break;
        default: break;
        }
    } else if (a == A_BACK) {
        if (l->edit) l->edit = false;
        else ui_pop();
    }
}

bool list_tick(list_t *l, float dt)
{
    bool m = tw_step(&l->hl, dt, 0.035f);
    m |= tw_step(&l->scroll, dt, 0.05f);
    if (m) ui_mark_anim();
    return m;
}

static void item_value(const list_item_t *it, char *v, size_t vn, char *u, size_t un)
{
    v[0] = u[0] = 0;
    if (it->value) {
        if (!it->value(it, v, vn)) v[0] = 0;
        return;
    }
    if (it->kind == LI_PARAM) ui_fmt_value((param_id_t)it->param, params_get((param_id_t)it->param), v, vn, u, un);
}

void list_draw(gfx_t *g, list_t *l, int y0)
{
    gfx_state_t st = gfx_save(g);
    gfx_clip(g, 0, y0, GFX_W, GFX_H - y0);
    int sc = iroundf(l->scroll.v);
    int hy = y0 + iroundf(l->hl.v) - sc;
    const int hx = 1, hw = GFX_W - 4;
    if (!l->edit) gfx_rrect(g, hx, hy, hw, UI_ROW, 2, C_ON);
    else gfx_rframe(g, hx, hy, hw, UI_ROW, 2, C_ON);

    char v[28], u[8];
    int yy = y0 - sc;
    for (int i = 0; i < l->n; i++) {
        const list_item_t *it = &l->items[i];
        int y = yy;
        yy += item_h(it);
        if (y >= GFX_H || y + UI_ROW <= y0) continue;
        if (it->kind == LI_TEXT) {
            gfx_text_ex(g, F_SMALL, UI_MX + 2, y + 1, it->label, 1, C_ON);
            continue;
        }
        bool dis = l->enabled && !l->enabled(i);
        int tx = UI_MX + 2;
        if (it->icon) {
            gfx_glyph(g, F_ICON, tx, y + 1, it->icon, C_XOR);
            tx += 14;
        }
        gfx_text(g, F_MED, tx, y + 2, it->label, C_XOR);
        item_value(it, v, sizeof v, u, sizeof u);
        const int rx = GFX_W - 4 - UI_MX;
        if (l->edit && i == l->cur) {
            int w = ui_value_unit_width(v, u, F_SMALL) + 8;
            int px = rx - w + 3;
            gfx_rrect(g, px, y + 2, w, 7, 2, C_ON);
            ui_value_unit(g, px + 4, y + 3, v, u, F_SMALL, GFX_LEFT, C_OFF);
            const param_desc_t *d = params_desc((param_id_t)it->param);
            float pv = params_get((param_id_t)it->param);
            if (pv > d->min) gfx_glyph(g, F_SMALL, px - 5, y + 3, 0x82, C_ON);
            if (pv < d->max) gfx_glyph(g, F_SMALL, rx + 5, y + 3, 0x81, C_ON);
        } else if (v[0] && it->kind == LI_INFO) {
            gfx_text_align(g, F_MED, rx, y + 2, v, 0, GFX_RIGHT, C_XOR);   /* data: case preserved */
        } else if (v[0]) {
            ui_value_unit(g, rx, y + 3, v, u, F_SMALL, GFX_RIGHT, C_XOR);
        } else if (it->kind == LI_SUB) {
            gfx_glyph(g, F_SMALL, rx - 2, y + 3, 0x81, C_XOR);
        }
        if (dis) gfx_fade(g, 0, y, GFX_W - 2, UI_ROW, 6);
    }
    gfx_restore(g, st);
    ui_scrollbar(g, GFX_W - 1, y0, GFX_H - y0, l->scroll.v, (float)(GFX_H - y0), (float)item_y(l, l->n));
}

/* ------------------------------------------------------------------ parameter strip */
static int strip_vis_w(void) { return GFX_W; }

static void strip_retarget(strip_t *s)
{
    int total = s->n * s->cell_w;
    float sc = 0.f;
    if (total > strip_vis_w()) {
        sc = (float)(s->cur * s->cell_w - (strip_vis_w() - s->cell_w) / 2);
        sc = clampf_(sc, 0.f, (float)(total - strip_vis_w() + 2));
    }
    tw_to(&s->scroll, sc);
    tw_to(&s->hl, (float)(s->cur * s->cell_w));
}

void strip_init(strip_t *s, const int16_t *params, const char *const *labels, int n, int cell_w)
{
    s->params = params; s->labels = labels; s->n = n; s->cell_w = cell_w;
    if (s->cur >= n || s->cur < 0) s->cur = 0;
    s->edit = false; s->carry = 0.f;
    strip_retarget(s);
    tw_snap(&s->scroll, s->scroll.t);
    tw_snap(&s->hl, s->hl.t);
}

bool strip_input(strip_t *s, ui_act_t a, int raw)
{
    param_id_t id = (param_id_t)s->params[s->cur];
    const param_desc_t *d = params_desc(id);
    if (a == A_ROT) {
        if (s->edit) { ui_param_rotate(id, raw, &s->carry); return true; }
        s->cur = clampi_(s->cur + raw, 0, s->n - 1);
        strip_retarget(s);
        return true;
    }
    if (a == A_PRESS) {
        if (d->type == PT_BOOL) ui_param_toggle(id);
        else { s->edit = !s->edit; s->carry = 0.f; }
        return true;
    }
    if (a == A_BACK && s->edit) { s->edit = false; return true; }
    return false;
}

bool strip_tick(strip_t *s, float dt)
{
    bool m = tw_step(&s->scroll, dt, 0.045f);
    m |= tw_step(&s->hl, dt, 0.035f);
    if (m) ui_mark_anim();
    return m;
}

void strip_draw(gfx_t *g, strip_t *s, int y)
{
    const int h = GFX_H - y;
    int total = s->n * s->cell_w;
    int x0 = total <= GFX_W ? (GFX_W - total) / 2 : 1;
    int sc = iroundf(s->scroll.v);
    char v[20], u[8];
    /* highlight (glides) */
    int hx = x0 + iroundf(s->hl.v) - sc;
    if (s->edit) gfx_rrect(g, hx + 1, y, s->cell_w - 2, h, 3, C_ON);
    else gfx_rframe(g, hx + 1, y, s->cell_w - 2, h, 3, C_ON);
    for (int i = 0; i < s->n; i++) {
        int cx = x0 + i * s->cell_w - sc;
        if (cx >= GFX_W || cx + s->cell_w <= 0) continue;
        param_id_t id = (param_id_t)s->params[i];
        const char *lab = (s->labels && s->labels[i]) ? s->labels[i] : ui_short_label(id);
        int mid = cx + s->cell_w / 2;
        gfx_color_t c = (s->edit && i == s->cur) ? C_OFF : C_ON;
        gfx_text_align(g, F_SMALL, mid, y + 3, lab, 1, GFX_CENTER, c);
        ui_fmt_value(id, params_get(id), v, sizeof v, u, sizeof u);
        const gfx_font_t *fv = F_MED;
        if (ui_value_unit_width(v, u, F_MED) > s->cell_w - 5) {
            fv = F_SMALL;
            if (ui_value_unit_width(v, u, F_SMALL) > s->cell_w - 4) {
                /* too wide for a unit: keep the magnitude visible ("1.94k", not a bare "1.94") */
                if (!strcmp(u, "kHz")) snprintf(u, sizeof u, "k");
                else u[0] = 0;
            }
        }
        int vy = fv == F_MED ? y + 10 : y + 11;
        ui_value_unit(g, mid, vy, v, u, fv, GFX_CENTER, c);
    }
    /* overflow hints */
    if (sc > 2) gfx_glyph(g, F_SMALL, 0, y + 6, 0x82, C_ON);
    if (total - sc > GFX_W + 2) gfx_glyph(g, F_SMALL, GFX_W - 3, y + 6, 0x81, C_ON);
}
