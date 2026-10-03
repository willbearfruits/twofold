/*
 * ui.h: portable UI entry points (firmware ui task and host simulator).
 * Rendering target is a gfx_t 128x64 1-bit framebuffer (see gfx.h).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gfx;

typedef enum {
    UI_EV_ROTATE = 0,   /* delta = detents (+cw), raw (no acceleration; the UI accelerates) */
    UI_EV_BTN_DOWN,
    UI_EV_BTN_UP,
} ui_ev_type_t;

typedef struct {
    ui_ev_type_t type;
    int16_t  delta;
    uint32_t t_ms;      /* timestamp of the event */
} ui_event_t;

void ui_init(uint32_t now_ms);
void ui_event(const ui_event_t *ev);
void ui_update(uint32_t now_ms);          /* timers, gestures, tweens, screensaver */
void ui_render(struct gfx *g);            /* draw current frame */
bool ui_is_animating(void);               /* hint: true while something moves (frame pacing) */

#ifdef __cplusplus
}
#endif
