/* gfx.c: 1-bit primitives on an SSD1306 page-layout framebuffer. See gfx.h. */
#include "gfx.h"
#include <string.h>

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

void gfx_init(gfx_t *g, uint8_t fb[GFX_PAGES][GFX_W])
{
    g->fb = fb;
    gfx_reset(g);
}

void gfx_reset(gfx_t *g)
{
    g->cx0 = 0; g->cy0 = 0; g->cx1 = GFX_W; g->cy1 = GFX_H;
    g->ox = 0; g->oy = 0;
}

gfx_state_t gfx_save(const gfx_t *g)
{
    gfx_state_t s = { g->cx0, g->cy0, g->cx1, g->cy1, g->ox, g->oy };
    return s;
}

void gfx_restore(gfx_t *g, gfx_state_t s)
{
    g->cx0 = s.cx0; g->cy0 = s.cy0; g->cx1 = s.cx1; g->cy1 = s.cy1;
    g->ox = s.ox; g->oy = s.oy;
}

void gfx_clip(gfx_t *g, int x, int y, int w, int h)
{
    int x0 = x + g->ox, y0 = y + g->oy;
    g->cx0 = (int16_t)imax(g->cx0, x0);
    g->cy0 = (int16_t)imax(g->cy0, y0);
    g->cx1 = (int16_t)imin(g->cx1, x0 + w);
    g->cy1 = (int16_t)imin(g->cy1, y0 + h);
    if (g->cx1 < g->cx0) g->cx1 = g->cx0;
    if (g->cy1 < g->cy0) g->cy1 = g->cy0;
}

void gfx_translate(gfx_t *g, int dx, int dy)
{
    g->ox = (int16_t)(g->ox + dx);
    g->oy = (int16_t)(g->oy + dy);
}

void gfx_clear(gfx_t *g) { memset(g->fb, 0, GFX_PAGES * GFX_W); }

/* ---- core: absolute-coordinate helpers ---- */

static inline uint64_t vmask(const gfx_t *g)
{
    uint64_t hi = g->cy1 >= 64 ? ~0ull : ((1ull << g->cy1) - 1);
    uint64_t lo = g->cy0 <= 0 ? 0 : ((1ull << g->cy0) - 1);
    return hi & ~lo;
}

static inline void put_byte(uint8_t *p, uint8_t b, gfx_color_t c)
{
    if (c == GFX_ON) *p |= b;
    else if (c == GFX_OFF) *p &= (uint8_t)~b;
    else *p ^= b;
}

/* m: absolute row mask for absolute column ax (already vertically clipped) */
static inline void put_col(gfx_t *g, int ax, uint64_t m, gfx_color_t c)
{
    if (!m || ax < g->cx0 || ax >= g->cx1) return;
    for (int p = 0; p < GFX_PAGES; p++) {
        uint8_t b = (uint8_t)(m >> (p * 8));
        if (b) put_byte(&g->fb[p][ax], b, c);
    }
}

void gfx_column(gfx_t *g, int x, int y, uint32_t bits, gfx_color_t c)
{
    int ax = x + g->ox, ay = y + g->oy;
    if (!bits || ay >= GFX_H || ay <= -32) return;
    uint64_t m = ay >= 0 ? ((uint64_t)bits << ay) : ((uint64_t)bits >> -ay);
    put_col(g, ax, m & vmask(g), c);
}

static void abs_span(gfx_t *g, int ax0, int ax1, int ay0, int ay1, gfx_color_t c)
{
    /* filled rect in absolute coords [ax0,ax1) x [ay0,ay1), clipped */
    ax0 = imax(ax0, g->cx0); ax1 = imin(ax1, g->cx1);
    ay0 = imax(ay0, g->cy0); ay1 = imin(ay1, g->cy1);
    if (ax0 >= ax1 || ay0 >= ay1) return;
    for (int p = ay0 >> 3; p <= (ay1 - 1) >> 3; p++) {
        int r0 = imax(ay0, p * 8) - p * 8, r1 = imin(ay1, p * 8 + 8) - p * 8;
        uint8_t b = (uint8_t)(((1u << r1) - 1) & ~((1u << r0) - 1));
        uint8_t *row = g->fb[p];
        if (c == GFX_ON) for (int x = ax0; x < ax1; x++) row[x] |= b;
        else if (c == GFX_OFF) { uint8_t nb = (uint8_t)~b; for (int x = ax0; x < ax1; x++) row[x] &= nb; }
        else for (int x = ax0; x < ax1; x++) row[x] ^= b;
    }
}

void gfx_fill(gfx_t *g, gfx_color_t c) { abs_span(g, g->cx0, g->cx1, g->cy0, g->cy1, c); }

void gfx_pixel(gfx_t *g, int x, int y, gfx_color_t c)
{
    int ax = x + g->ox, ay = y + g->oy;
    if (ax < g->cx0 || ax >= g->cx1 || ay < g->cy0 || ay >= g->cy1) return;
    put_byte(&g->fb[ay >> 3][ax], (uint8_t)(1u << (ay & 7)), c);
}

bool gfx_get(const gfx_t *g, int x, int y)
{
    int ax = x + g->ox, ay = y + g->oy;
    if (ax < 0 || ax >= GFX_W || ay < 0 || ay >= GFX_H) return false;
    return (g->fb[ay >> 3][ax] >> (ay & 7)) & 1;
}

void gfx_hline(gfx_t *g, int x, int y, int w, gfx_color_t c)
{
    if (w <= 0) return;
    int ax = x + g->ox, ay = y + g->oy;
    abs_span(g, ax, ax + w, ay, ay + 1, c);
}

void gfx_vline(gfx_t *g, int x, int y, int h, gfx_color_t c)
{
    if (h <= 0) return;
    int ax = x + g->ox, ay = y + g->oy;
    abs_span(g, ax, ax + 1, ay, ay + h, c);
}

void gfx_hline_pat(gfx_t *g, int x, int y, int w, uint8_t pat, gfx_color_t c)
{
    int ay = y + g->oy;
    if (ay < g->cy0 || ay >= g->cy1) return;
    int ax0 = imax(x + g->ox, g->cx0), ax1 = imin(x + g->ox + w, g->cx1);
    uint8_t b = (uint8_t)(1u << (ay & 7));
    uint8_t *row = g->fb[ay >> 3];
    for (int ax = ax0; ax < ax1; ax++)
        if ((pat >> (ax & 7)) & 1) put_byte(&row[ax], b, c);
}

void gfx_vline_pat(gfx_t *g, int x, int y, int h, uint8_t pat, gfx_color_t c)
{
    int ax = x + g->ox;
    if (ax < g->cx0 || ax >= g->cx1) return;
    int ay0 = imax(y + g->oy, g->cy0), ay1 = imin(y + g->oy + h, g->cy1);
    for (int ay = ay0; ay < ay1; ay++)
        if ((pat >> (ay & 7)) & 1) put_byte(&g->fb[ay >> 3][ax], (uint8_t)(1u << (ay & 7)), c);
}

void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, gfx_color_t c)
{
    if (y0 == y1) { gfx_hline(g, imin(x0, x1), y0, (x0 > x1 ? x0 - x1 : x1 - x0) + 1, c); return; }
    if (x0 == x1) { gfx_vline(g, x0, imin(y0, y1), (y0 > y1 ? y0 - y1 : y1 - y0) + 1, c); return; }
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(gfx_t *g, int x, int y, int w, int h, gfx_color_t c)
{
    if (w <= 0 || h <= 0) return;
    int ax = x + g->ox, ay = y + g->oy;
    abs_span(g, ax, ax + w, ay, ay + h, c);
}

void gfx_frame(gfx_t *g, int x, int y, int w, int h, gfx_color_t c)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(g, x, y, w, c);
    if (h > 1) gfx_hline(g, x, y + h - 1, w, c);
    if (h > 2) {
        gfx_vline(g, x, y + 1, h - 2, c);
        if (w > 1) gfx_vline(g, x + w - 1, y + 1, h - 2, c);
    }
}

/* Corner insets per row for pixel-art rounded corners (hand-tuned for small radii). */
static int corner_inset(int r, int i)
{
    static const uint8_t t1[] = { 1 }, t2[] = { 2, 1 }, t3[] = { 3, 1, 1 }, t4[] = { 4, 2, 1, 1 };
    static const uint8_t *const tabs[] = { NULL, t1, t2, t3, t4 };
    if (i >= r) return 0;
    if (r <= 4) return tabs[r][i];
    int dy = r - i, rr = r * r - dy * dy, s = 0;
    while ((s + 1) * (s + 1) <= rr) s++;
    if ((s + 1) * (s + 1) - rr < rr - s * s) s++;
    return r - s;
}

void gfx_rrect(gfx_t *g, int x, int y, int w, int h, int r, gfx_color_t c)
{
    if (w <= 0 || h <= 0) return;
    r = imin(r, imin(w, h) / 2);
    if (r <= 0) { gfx_rect(g, x, y, w, h, c); return; }
    for (int i = 0; i < r; i++) {
        int in = corner_inset(r, i);
        gfx_hline(g, x + in, y + i, w - 2 * in, c);
        gfx_hline(g, x + in, y + h - 1 - i, w - 2 * in, c);
    }
    gfx_rect(g, x, y + r, w, h - 2 * r, c);
}

void gfx_rframe(gfx_t *g, int x, int y, int w, int h, int r, gfx_color_t c)
{
    if (w <= 0 || h <= 0) return;
    r = imin(r, imin(w, h) / 2);
    if (r <= 0) { gfx_frame(g, x, y, w, h, c); return; }
    for (int i = 0; i < r; i++) {
        int in = corner_inset(r, i);
        if (i == 0) {
            gfx_hline(g, x + in, y, w - 2 * in, c);
            gfx_hline(g, x + in, y + h - 1, w - 2 * in, c);
            continue;
        }
        int end = imax(in, corner_inset(r, i - 1) - 1);
        int len = end - in + 1;
        gfx_hline(g, x + in, y + i, len, c);
        gfx_hline(g, x + w - 1 - end, y + i, len, c);
        gfx_hline(g, x + in, y + h - 1 - i, len, c);
        gfx_hline(g, x + w - 1 - end, y + h - 1 - i, len, c);
    }
    if (h - 2 * r > 0) {
        gfx_vline(g, x, y + r, h - 2 * r, c);
        gfx_vline(g, x + w - 1, y + r, h - 2 * r, c);
    }
}

void gfx_circle(gfx_t *g, int cx, int cy, int r, gfx_color_t c)
{
    if (r <= 0) { gfx_pixel(g, cx, cy, c); return; }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        /* avoid double-plotting octant joins in XOR mode */
        gfx_pixel(g, cx + x, cy + y, c); gfx_pixel(g, cx - x, cy + y, c);
        if (y) { gfx_pixel(g, cx + x, cy - y, c); gfx_pixel(g, cx - x, cy - y, c); }
        if (x != y) {
            gfx_pixel(g, cx + y, cy + x, c); gfx_pixel(g, cx + y, cy - x, c);
            if (y) { gfx_pixel(g, cx - y, cy + x, c); gfx_pixel(g, cx - y, cy - x, c); }
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_disc(gfx_t *g, int cx, int cy, int r, gfx_color_t c)
{
    if (r <= 0) { gfx_pixel(g, cx, cy, c); return; }
    /* row half-widths from the same midpoint walk -> matches gfx_circle outline */
    int8_t hw[64];
    if (r > 63) r = 63;
    for (int i = 0; i <= r; i++) hw[i] = 0;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (hw[y] < x) hw[y] = (int8_t)x;
        if (hw[x] < y) hw[x] = (int8_t)y;
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
    gfx_hline(g, cx - hw[0], cy, 2 * hw[0] + 1, c);
    for (int i = 1; i <= r; i++) {
        gfx_hline(g, cx - hw[i], cy - i, 2 * hw[i] + 1, c);
        gfx_hline(g, cx - hw[i], cy + i, 2 * hw[i] + 1, c);
    }
}

/* sin in Q14 for 0..90 degrees */
static const int16_t k_sin90[91] = {
    0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126, 3406, 3686, 3964, 4240, 4516, 4790,
    5063, 5334, 5604, 5872, 6138, 6402, 6664, 6924, 7182, 7438, 7692, 7943, 8192, 8438, 8682, 8923, 9162,
    9397, 9630, 9860, 10087, 10311, 10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365,
    12551, 12733, 12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598,
    14726, 14849, 14968, 15082, 15191, 15296, 15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964,
    16026, 16083, 16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382, 16384 };

static int isin_deg(int a)
{
    a %= 360; if (a < 0) a += 360;
    if (a <= 90) return k_sin90[a];
    if (a <= 180) return k_sin90[180 - a];
    if (a <= 270) return -k_sin90[a - 180];
    return -k_sin90[360 - a];
}

void gfx_arc(gfx_t *g, int cx, int cy, int r, int a0, int a1, gfx_color_t c)
{
    if (a1 < a0) { int t = a0; a0 = a1; a1 = t; }
    if (r <= 0) return;
    /* step in 1/8 degree so that adjacent samples are <= ~1 px apart */
    int step8 = imax(1, (int)(8 * 57 / (r + 1)));
    int lx = 1 << 30, ly = 0;
    for (int a8 = a0 * 8; a8 <= a1 * 8; a8 += step8) {
        int a = a8 >> 3, fr = a8 & 7;
        int s = isin_deg(a) + ((isin_deg(a + 1) - isin_deg(a)) * fr >> 3);
        int co = isin_deg(a + 90) + ((isin_deg(a + 91) - isin_deg(a + 90)) * fr >> 3);
        int px = cx + ((co * r + (co >= 0 ? 8192 : -8192)) >> 14);
        int py = cy + ((s * r + (s >= 0 ? 8192 : -8192)) >> 14);
        if (px != lx || py != ly) gfx_pixel(g, px, py, c);
        lx = px; ly = py;
    }
}

void gfx_dither(gfx_t *g, int x, int y, int w, int h, int level, gfx_color_t c)
{
    if (level <= 0 || w <= 0 || h <= 0) return;
    if (level >= 16) { gfx_rect(g, x, y, w, h, c); return; }
    int ax0 = imax(x + g->ox, g->cx0), ax1 = imin(x + g->ox + w, g->cx1);
    int ay0 = imax(y + g->oy, g->cy0), ay1 = imin(y + g->oy + h, g->cy1);
    for (int ax = ax0; ax < ax1; ax++) {
        /* build the column's Bayer mask for 8 rows once (pattern period 4) */
        uint8_t colpat = 0;
        for (int r = 0; r < 8; r++) if (gfx_bayer(ax, r, level)) colpat |= (uint8_t)(1u << r);
        for (int p = ay0 >> 3; ay0 < ay1 && p <= (ay1 - 1) >> 3; p++) {
            int r0 = imax(ay0, p * 8) - p * 8, r1 = imin(ay1, p * 8 + 8) - p * 8;
            uint8_t b = (uint8_t)(((1u << r1) - 1) & ~((1u << r0) - 1)) & colpat;
            if (b) put_byte(&g->fb[p][ax], b, c);
        }
    }
}

void gfx_fade(gfx_t *g, int x, int y, int w, int h, int level)
{
    if (level >= 16 || w <= 0 || h <= 0) return;
    int ax0 = imax(x + g->ox, g->cx0), ax1 = imin(x + g->ox + w, g->cx1);
    int ay0 = imax(y + g->oy, g->cy0), ay1 = imin(y + g->oy + h, g->cy1);
    for (int ax = ax0; ax < ax1; ax++) {
        uint8_t keep = 0;
        for (int r = 0; r < 8; r++) if (gfx_bayer(ax, r, level)) keep |= (uint8_t)(1u << r);
        for (int p = ay0 >> 3; ay0 < ay1 && p <= (ay1 - 1) >> 3; p++) {
            int r0 = imax(ay0, p * 8) - p * 8, r1 = imin(ay1, p * 8 + 8) - p * 8;
            uint8_t b = (uint8_t)(((1u << r1) - 1) & ~((1u << r0) - 1));
            g->fb[p][ax] &= (uint8_t)(~b | keep);
        }
    }
}

void gfx_invert(gfx_t *g, int x, int y, int w, int h) { gfx_rect(g, x, y, w, h, GFX_XOR); }

void gfx_bitmap(gfx_t *g, int x, int y, const gfx_bitmap_t *bm, gfx_color_t c)
{
    int stride = (bm->w + 7) / 8;
    for (int cx = 0; cx < bm->w; cx++) {
        int ax = x + cx + g->ox;
        if (ax < g->cx0 || ax >= g->cx1) continue;
        /* gather the column in chunks of 32 rows */
        for (int r0 = 0; r0 < bm->h; r0 += 32) {
            uint32_t col = 0;
            int rn = imin(32, bm->h - r0);
            for (int r = 0; r < rn; r++)
                if (bm->bits[(r0 + r) * stride + (cx >> 3)] & (0x80u >> (cx & 7))) col |= 1u << r;
            gfx_column(g, x + cx, y + r0, col, c);
        }
    }
}

/* ---------------- text ---------------- */

static const gfx_glyph_t *find_glyph(const gfx_font_t *f, uint8_t *ch)
{
    uint8_t c = *ch;
    if (c >= f->first && c <= f->last && f->glyphs[c - f->first].w) return &f->glyphs[c - f->first];
    if ((f->flags & GFX_FONT_UPPER_FALLBACK) && c >= 'a' && c <= 'z') {
        c = (uint8_t)(c - 32);
        if (c >= f->first && c <= f->last && f->glyphs[c - f->first].w) { *ch = c; return &f->glyphs[c - f->first]; }
    }
    return NULL;
}

static int kern_of(const gfx_font_t *f, uint8_t a, uint8_t b)
{
    for (int i = 0; i < f->nkern; i++) {
        if (f->kern[i].a == a && f->kern[i].b == b) return f->kern[i].k;
        if (f->kern[i].a > a) break;
    }
    return 0;
}

int gfx_glyph(gfx_t *g, const gfx_font_t *f, int x, int y, uint8_t ch, gfx_color_t c)
{
    const gfx_glyph_t *gl = find_glyph(f, &ch);
    if (!gl) return f->space;
    const uint8_t *d = f->bits + gl->off;
    for (int cx = 0; cx < gl->w; cx++, d += f->bpc) {
        uint32_t col = d[0];
        if (f->bpc > 1) col |= (uint32_t)d[1] << 8;
        if (f->bpc > 2) col |= (uint32_t)d[2] << 16;
        if (f->bpc > 3) col |= (uint32_t)d[3] << 24;
        if (col) gfx_column(g, x + cx, y, col, c);
    }
    return gl->adv;
}

/* Walk a string: returns ink width; draws when g != NULL. */
static int walk(gfx_t *g, const gfx_font_t *f, int x, int y, const char *s, int tracking, gfx_color_t c)
{
    int pen = x, ink_end = x;
    uint8_t prev = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        uint8_t ch = *p;
        const gfx_glyph_t *gl = find_glyph(f, &ch);
        if (prev && f->nkern) pen += kern_of(f, prev, ch);
        if (!gl) { pen += f->space + tracking; prev = 0; continue; }
        if (g) gfx_glyph(g, f, pen, y, ch, c);
        ink_end = pen + gl->w;
        pen += gl->adv + tracking;
        prev = ch;
    }
    return ink_end - x;
}

int gfx_text_ex(gfx_t *g, const gfx_font_t *f, int x, int y, const char *s, int tracking, gfx_color_t c)
{
    return x + walk(g, f, x, y, s, tracking, c);
}

int gfx_text(gfx_t *g, const gfx_font_t *f, int x, int y, const char *s, gfx_color_t c)
{
    return gfx_text_ex(g, f, x, y, s, 0, c);
}

int gfx_text_width_ex(const gfx_font_t *f, const char *s, int tracking) { return walk(NULL, f, 0, 0, s, tracking, GFX_ON); }
int gfx_text_width(const gfx_font_t *f, const char *s) { return gfx_text_width_ex(f, s, 0); }

int gfx_text_align(gfx_t *g, const gfx_font_t *f, int x, int y, const char *s, int tracking,
                   gfx_align_t a, gfx_color_t c)
{
    if (a != GFX_LEFT) {
        int w = gfx_text_width_ex(f, s, tracking);
        x -= (a == GFX_CENTER) ? w / 2 : w;
    }
    walk(g, f, x, y, s, tracking, c);
    return x;
}
