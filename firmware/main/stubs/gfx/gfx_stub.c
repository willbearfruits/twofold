/* Stub gfx (TWOFOLD_STUBS builds only). */
#include <string.h>
#include "gfx.h"
void gfx_init(gfx_t *g, uint8_t fb[GFX_PAGES][GFX_W]) { memset(g, 0, sizeof *g); g->fb = fb; g->cx1 = GFX_W; g->cy1 = GFX_H; }
