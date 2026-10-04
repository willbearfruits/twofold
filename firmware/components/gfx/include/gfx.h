/*
 * gfx.h: portable 1-bit graphics for a 128x64 monochrome OLED.
 *
 * Framebuffer layout is SSD1306 page layout: fb[page][x], 8 pages of 128 bytes, bit 0 = top row
 * of the page. The display driver can send a page row directly (SH1106: same bytes, +2 column offset).
 *
 * All drawing takes LOCAL coordinates: the context has a translation (origin) and an absolute clip
 * rectangle. Screen transitions draw two screens into one buffer with different origins and clips,
 * so no extra framebuffer is needed. Everything is integer math; no allocation.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_W     128
#define GFX_H     64
#define GFX_PAGES (GFX_H / 8)

typedef enum { GFX_OFF = 0, GFX_ON = 1, GFX_XOR = 2 } gfx_color_t;
typedef enum { GFX_LEFT = 0, GFX_CENTER = 1, GFX_RIGHT = 2 } gfx_align_t;

typedef struct gfx {
    uint8_t (*fb)[GFX_W];          /* [GFX_PAGES][GFX_W] */
    int16_t cx0, cy0, cx1, cy1;    /* clip, absolute pixels, [x0,x1) x [y0,y1) */
    int16_t ox, oy;                /* origin: local (0,0) maps to absolute (ox,oy) */
} gfx_t;

/* Saved drawing state (clip + origin) for nested drawing. */
typedef struct { int16_t cx0, cy0, cx1, cy1, ox, oy; } gfx_state_t;

void        gfx_init(gfx_t *g, uint8_t fb[GFX_PAGES][GFX_W]);
gfx_state_t gfx_save(const gfx_t *g);
void        gfx_restore(gfx_t *g, gfx_state_t s);
void        gfx_reset(gfx_t *g);                                  /* full clip, origin 0,0 */
void        gfx_clip(gfx_t *g, int x, int y, int w, int h);       /* intersect clip with local rect */
void        gfx_translate(gfx_t *g, int dx, int dy);              /* move origin */

void gfx_clear(gfx_t *g);                                         /* whole buffer, ignores clip */
void gfx_fill(gfx_t *g, gfx_color_t c);                           /* within clip */

void gfx_pixel(gfx_t *g, int x, int y, gfx_color_t c);
bool gfx_get(const gfx_t *g, int x, int y);
void gfx_hline(gfx_t *g, int x, int y, int w, gfx_color_t c);
void gfx_vline(gfx_t *g, int x, int y, int h, gfx_color_t c);
/* Patterned lines: pixel drawn when bit ((abs_coord) & 7) of pat is set (pattern anchored to the
 * panel, so dotted lines do not crawl when content moves by whole pattern periods).
 * e.g. 0x55 = every other pixel, 0x11 = every 4th, 0x33 = 2 on 2 off. */
void gfx_hline_pat(gfx_t *g, int x, int y, int w, uint8_t pat, gfx_color_t c);
void gfx_vline_pat(gfx_t *g, int x, int y, int h, uint8_t pat, gfx_color_t c);
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, gfx_color_t c);

void gfx_rect(gfx_t *g, int x, int y, int w, int h, gfx_color_t c);          /* filled */
void gfx_frame(gfx_t *g, int x, int y, int w, int h, gfx_color_t c);         /* 1px outline */
void gfx_rrect(gfx_t *g, int x, int y, int w, int h, int r, gfx_color_t c);  /* filled, rounded */
void gfx_rframe(gfx_t *g, int x, int y, int w, int h, int r, gfx_color_t c); /* outline, rounded */
void gfx_circle(gfx_t *g, int cx, int cy, int r, gfx_color_t c);
void gfx_disc(gfx_t *g, int cx, int cy, int r, gfx_color_t c);
/* Arc from angle a0 to a1 (degrees, 0 = 3 o'clock, clockwise because y grows downwards). */
void gfx_arc(gfx_t *g, int cx, int cy, int r, int a0, int a1, gfx_color_t c);

/* Ordered dither ("grey"): level 0..16 = fraction of pixels set (4x4 Bayer, panel-anchored). */
void gfx_dither(gfx_t *g, int x, int y, int w, int h, int level, gfx_color_t c);
/* Fade existing content: keep only pixels where Bayer < level (16 = unchanged, 0 = cleared). */
void gfx_fade(gfx_t *g, int x, int y, int w, int h, int level);
void gfx_invert(gfx_t *g, int x, int y, int w, int h);
static inline bool gfx_bayer(int ax, int ay, int level)
{
    static const uint8_t m[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    return m[ay & 3][ax & 3] < level;
}

/* 1-bit bitmap, row-major, MSB = leftmost pixel, rows padded to whole bytes. Set bits drawn in c. */
typedef struct { uint16_t w, h; const uint8_t *bits; } gfx_bitmap_t;
void gfx_bitmap(gfx_t *g, int x, int y, const gfx_bitmap_t *bm, gfx_color_t c);

/* Raw column blit: mask bit n = local row (y + n). Used by fonts; exported for custom renderers. */
void gfx_column(gfx_t *g, int x, int y, uint32_t bits, gfx_color_t c);

/* ---------------- text ---------------- */
typedef struct { uint16_t off; uint8_t w; uint8_t adv; } gfx_glyph_t;     /* adv = w + spacing */
typedef struct { uint8_t a, b; int8_t k; } gfx_kern_t;

#define GFX_FONT_UPPER_FALLBACK 0x01   /* lowercase falls back to uppercase */

typedef struct gfx_font {
    uint8_t h;               /* rows per glyph (<= 32) */
    uint8_t bpc;             /* bytes per column = (h + 7) / 8 */
    uint8_t first, last;     /* code range */
    uint8_t cap;             /* cap height (rows from top) */
    uint8_t space;           /* space advance */
    uint8_t flags;
    uint8_t nkern;
    const gfx_glyph_t *glyphs;   /* last - first + 1 entries (w = 0: missing) */
    const uint8_t *bits;         /* column-major, LSB = top row of each byte */
    const gfx_kern_t *kern;      /* sorted by (a, b) */
} gfx_font_t;

/* Draw string with top-left at (x,y); tracking adds px between glyphs. Returns x after the text. */
int  gfx_text(gfx_t *g, const gfx_font_t *f, int x, int y, const char *s, gfx_color_t c);
int  gfx_text_ex(gfx_t *g, const gfx_font_t *f, int x, int y, const char *s, int tracking, gfx_color_t c);
int  gfx_text_width(const gfx_font_t *f, const char *s);
int  gfx_text_width_ex(const gfx_font_t *f, const char *s, int tracking);
/* Aligned: x is the left edge, center or right edge depending on align. Returns left edge used. */
int  gfx_text_align(gfx_t *g, const gfx_font_t *f, int x, int y, const char *s, int tracking,
                    gfx_align_t a, gfx_color_t c);
int  gfx_glyph(gfx_t *g, const gfx_font_t *f, int x, int y, uint8_t ch, gfx_color_t c);  /* returns advance */

/* Fonts (generated from fonts/<name>.txt by tools/fontconv.py). */
extern const gfx_font_t gfx_font_tiny;    /* 3x5 condensed caps + digits (axis labels) */
extern const gfx_font_t gfx_font_small;   /* 5 px caps, proportional (labels) */
extern const gfx_font_t gfx_font_med;     /* 7 px caps + lowercase with descenders (items, values) */
extern const gfx_font_t gfx_font_big;     /* 18 px proportional numerals (hero values) */
extern const gfx_font_t gfx_font_icons;   /* 9x9 menu icons + 7 px status icons */
extern const gfx_bitmap_t gfx_wordmark;   /* "TWOFOLD" wordmark */

/* Symbol codes present in the text fonts (use as string literals: "VOL " GFX_S_INF). */
#define GFX_S_INF    "\x80"   /* infinity */
#define GFX_S_RIGHT  "\x81"   /* small right triangle */
#define GFX_S_LEFT   "\x82"
#define GFX_S_UP     "\x83"
#define GFX_S_DOWN   "\x84"
#define GFX_S_BULLET "\x85"
#define GFX_S_DEG    "\x86"
#define GFX_S_TIMES  "\x87"
#define GFX_S_PM     "\x88"
#define GFX_S_ARROW  "\x89"   /* right arrow */
#define GFX_S_PLAY   "\x8e"
#define GFX_S_REC    "\x8f"

/* Icon codes in gfx_font_icons. */
enum {
    GFX_I_EQ = 'A', GFX_I_GAIN, GFX_I_FX, GFX_I_NET, GFX_I_BT, GFX_I_PRESET, GFX_I_SETTINGS,
    GFX_I_WIFI = 'a', GFX_I_WIFI1, GFX_I_WIFI2, GFX_I_BLE, GFX_I_USB, GFX_I_PLAY, GFX_I_REC,
    GFX_I_MIDI, GFX_I_LINK, GFX_I_MUTE,
};

#ifdef __cplusplus
}
#endif
