/* pc_internal.h – Internal shared declarations for pc.c and pc_overrides.c */
#pragma once
#include "pc.h"
#include "common_log.h"
#include "recomp/hw.h"
#include "recomp/rt.h"
#include "game_state.h"   /* single g_state instance + legacy-name macros */
/* NOTE: generated/game.h is intentionally NOT included here.
 * pc_overrides.c and boot/engine code must not call gfn_* functions by name.
 * Use rt_call(ctx, addr) for sub-functions without an override, or
 * rt_call_generated(ctx, addr) from within an override to invoke the
 * original recompiled logic without re-entering the override. */

#ifdef HARNESS_BUILD
#include "harness/trace.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Logging ─────────────────────────────────────────────────────────────────── */
#ifdef HARNESS_BUILD
# define PC_VERBOSE 0
#else
# define PC_VERBOSE 1
#endif
#define PC_LOG(...) GLOBAL_LOG_IF(PC_VERBOSE, __VA_ARGS__)

#define TRACE_CHIP_MEMSET(offset, value, length) do { \
    GLOBAL_LOG("[memset] %s addr=$%06X len=$%04X val=$%02X\n", __func__, \
               (unsigned)(offset), (unsigned)(length), (unsigned)(value)); \
    memset(g_chip + (offset), (value), (length)); \
} while (0)

/* ── Chip RAM pointer ─────────────────────────────────────────────────────────── */
extern uint8_t *g_chip;

/* ── Override registration ─────────────────────────────────────────────────────── */
void pc_register_overrides(void);

/* ── Native override function declarations (implementations in pc_overrides_*.c) ── */
/* pc_overrides_hw.c */
void native_hw_wait(M68KCtx *ctx);
void native_blitter_wait_clear(M68KCtx *ctx);
/* pc_overrides_boot.c */
void native_boot_anim_iterator(M68KCtx *ctx);
void native_overlay_loader(M68KCtx *ctx);
void native_overlay_loader_reloc(M68KCtx *ctx);
void native_overlay_load(void);
void native_gp_disk_read(M68KCtx *ctx);
void native_overlay_load_d0(void);
/* pc_overrides_audio.c — native gameplay audio engine (staged) */
void native_sfx_trigger(M68KCtx *ctx);
/* pc_overrides_pickup.c — native object-pickup mechanic (widened range) */
void pickup_register(void);
void pickup_register_scan(void);
/* pc_overrides_copper.c */
void native_sprite_blitter_setup(M68KCtx *ctx);
void native_game_frame(M68KCtx *ctx);
/* pc_overrides_render.c */
void native_text_sprite_render(M68KCtx *ctx);
void native_dispatch_table    (M68KCtx *ctx);
void native_item_dispatch_1   (M68KCtx *ctx);
void native_item_dispatch_2   (M68KCtx *ctx);
void native_item_dispatch_3   (M68KCtx *ctx);
void native_item_decrement    (M68KCtx *ctx);
void native_item_scroll       (M68KCtx *ctx);
void native_item_position     (M68KCtx *ctx);
void native_item_blitter      (M68KCtx *ctx);
void native_blit_row_callback (M68KCtx *ctx);
void native_post_blit_handler (M68KCtx *ctx);
void native_timer_interrupt   (M68KCtx *ctx);
/* pc_overrides_gameplay.c */
void native_end_of_level      (M68KCtx *ctx);
void native_level_load        (M68KCtx *ctx);
void native_level_setup       (M68KCtx *ctx);
