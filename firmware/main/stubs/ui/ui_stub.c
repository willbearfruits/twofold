/* Stub UI (TWOFOLD_STUBS builds only): draws nothing. */
#include <string.h>
#include "ui.h"
#include "gfx.h"
void ui_init(uint32_t now_ms) { (void)now_ms; }
void ui_event(const ui_event_t *ev) { (void)ev; }
void ui_update(uint32_t now_ms) { (void)now_ms; }
void ui_render(struct gfx *g) { memset(g->fb, 0, GFX_PAGES * GFX_W); }
bool ui_is_animating(void) { return false; }
