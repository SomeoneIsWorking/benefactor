/* pc_overrides_copper.c — Copper list rebuild helpers and frame-level overrides */
#include "port/port_internal.h"

/* $0041A4 — per-frame blitter setup for sprite rendering */
void native_sprite_blitter_setup(M68KCtx *ctx)
{
    rt_call_generated(ctx, 0x0041A4u);
}

/* $003488 — per-frame game logic entry (car-demo screen) */
void native_game_frame(M68KCtx *ctx)
{
    rt_call_generated(ctx, 0x003488u);
}
