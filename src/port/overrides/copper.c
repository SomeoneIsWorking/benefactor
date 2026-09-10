/* src/port/overrides/copper.c — Copper list rebuild helpers and frame-level overrides */
#include "port/frame_accounting.h"
#include "port/port_internal.h"

/* $0041A4 — per-frame blitter setup for sprite rendering. Counted: this is the
 * intro/title screen's redraw, and comparing its rate to the presented frame
 * rate says whether the screen is being stepped faster than it is shown. */
void native_sprite_blitter_setup(M68KCtx *ctx) {
    g_pc_title_draws++;
    rt_call_original(ctx, ctx->image, 0x0041A4u);
}

/* $003488 — per-frame game logic entry (car-demo screen) */
