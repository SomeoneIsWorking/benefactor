/* Internal declarations shared by the game-loop and native-override modules. */
#pragma once
#include "common/game_state.h" /* single g_state instance + legacy-name macros */
#include "common/log.h"
#include "engine/hw.h"
#include "port/config.h"
#include "port/port.h"
#include "runtime/guest_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Native owners call retail-image guest addresses through the image-qualified
 * runtime adapter. */

#ifdef HARNESS_BUILD
#include "harness/trace.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Logging ─────────────────────────────────────────────────────────────────── */
#define PC_LOG(...) benefactor_log_write(BENEFACTOR_LOG_DEBUG, "port", __VA_ARGS__)

#define TRACE_CHIP_MEMSET(offset, value, length)                                                   \
    do {                                                                                           \
        benefactor_log_write(BENEFACTOR_LOG_TRACE, "memory",                                       \
                             "[memset] %s addr=$%06X len=$%04X val=$%02X", __func__,               \
                             (unsigned)(offset), (unsigned)(length), (unsigned)(value));           \
        memset(g_chip + (offset), (value), (length));                                              \
    } while (0)

/* ── Chip RAM pointer ─────────────────────────────────────────────────────────── */
extern uint8_t *g_chip;

/* ── Game loop (game_loop.c) ───────────────────────────────────────────────────── */
/* Discard the game thread and spawn a fresh one that re-enters the steady
 * gameplay cycle at $577114. Used by the savestate load (savestate.c), which
 * restores g_state + g_mem under a thread parked on the pre-load memory. */
void pc_resume_gameplay_thread(void);

/* ── Override registration ─────────────────────────────────────────────────────── */
void pc_register_overrides(void);

/* Give every custom-register busy-wait in a freshly decrunched image a native
 * owner (overrides/wait_idioms.c, src/port/wait_idiom.h). Call it once per
 * image, with that image's mask, after the bytes are in g_mem. */
void pc_register_wait_idioms(uint32_t image_mask, uint32_t low, uint32_t high);

/* ── Native override function declarations (`src/port/overrides/`) ── */
/* overrides/hw.c */
void native_hw_wait(M68KCtx *ctx);
void native_blitter_wait_clear(M68KCtx *ctx);
void native_poster_vblank_poll(M68KCtx *ctx);
/* overrides/boot.c */
void native_boot_anim_iterator(M68KCtx *ctx);
void native_overlay_loader(M68KCtx *ctx);
void native_overlay_loader_reloc(M68KCtx *ctx);
void native_overlay_load(void);
void native_gp_disk_read(M68KCtx *ctx);
void native_overlay_load_d0(void);
/* overrides/audio.c — native gameplay audio engine (staged) */
void native_sfx_trigger(M68KCtx *ctx);
/* overrides/pickup.c — native object-pickup mechanic (widened range) */
void pickup_register(void);
void pickup_register_scan(void);
/* overrides/copper.c */
void native_sprite_blitter_setup(M68KCtx *ctx);
/* overrides/render.c */
void native_post_blit_handler(M68KCtx *ctx);
/* overrides/gameplay.c */
void native_end_of_level(M68KCtx *ctx);
void native_level_load(M68KCtx *ctx);
void native_level_setup(M68KCtx *ctx);
void native_objwalk(M68KCtx *ctx);         /* $57D79A — per-frame object-list walker */
void native_objstep(M68KCtx *ctx);         /* $57D7BC — per-object loop step (wide cull) */
void native_objstep_b(M68KCtx *ctx);       /* $57D8B4 — animated-object loop step (wide cull) */
void native_objdraw_capture(M68KCtx *ctx); /* $57D8D0 — capture object for widescreen */
void native_player_capture(M68KCtx *ctx);  /* $57A666 — capture player for widescreen */
void native_char_capture(M68KCtx *ctx);    /* $57D3F4 — capture cookie-cut characters */
/* Widescreen object capture, read by native_renderer.c (last complete frame). */
int native_wsobj_count(void);
int native_wsobj_get(int i, int *x, int *y, int *w, int *h, uint32_t *src, uint32_t *mod);
/* Captured player draw params (cookie-cut 16x16, 5-plane data + 1-plane mask).
 * black=1 on a damage-blink black-silhouette frame (fill the mask with colour 0). */
int native_wsplayer_get(int *x, int *y, uint32_t *dbase, uint32_t *mbase, int *black);
/* Captured cookie-cut characters (walkers/enemies) drawn via $57D3F4/$57D6C4.
 * 5-plane DATA (plane stride h*rowstride) + 1-plane MASK, both row stride rowstride. */
int native_wschar_count(void);
int native_wschar_get(int i, int *x, int *y, int *w, int *h, uint32_t *data, uint32_t *mask,
                      int *rowstride);

/* overrides/boot.c — intro/menu screens */
void native_menu_glyph_blit(M68KCtx *ctx);
void native_menu_setup(M68KCtx *ctx);
void native_main_menu_fire_dispatch(M68KCtx *ctx);
void native_menu_cursor_up(M68KCtx *ctx);
void native_menu_cursor_down(M68KCtx *ctx);
void native_menu_diff_left(M68KCtx *ctx);
void native_menu_diff_right(M68KCtx *ctx);
void native_menu_art_unpack(M68KCtx *ctx);
void native_menu_pwfield_draw(M68KCtx *ctx);
void native_password_build(M68KCtx *ctx);
void native_gameover_menu(M68KCtx *ctx);
/* overrides/level_load.c */
void native_level_decrunch(M68KCtx *ctx);
/* overrides/gameplay.c — capture + widescreen world state */
void native_gameplay_input(M68KCtx *ctx);
void native_anim_patch(M68KCtx *ctx);
void native_banner_capture(M68KCtx *ctx);
void native_telanim_capture(M68KCtx *ctx);
void native_getready_capture(M68KCtx *ctx);
void native_gameover_text_capture(M68KCtx *ctx);
void native_levelcomplete_text_capture(M68KCtx *ctx);
void native_obj_anim_59AC38(M68KCtx *ctx);
void native_build_red(M68KCtx *ctx);
void native_build_blind(M68KCtx *ctx);
void native_build_clear(M68KCtx *ctx);
void native_wsrope_build(M68KCtx *ctx);
void native_wsrope_seg(M68KCtx *ctx);
/* Widescreen world state, published for the renderer after a complete frame. */
void native_ws_promote(void);
void native_wsobj_commit_reset(void);
int native_wsrope_count(void);
void native_wsrope_get(int i, int *x0, int *y0, int *x1, int *y1);
int native_wswater_count(void);
int native_wswater_get(int i, int *worldX, int *row, int *col, uint32_t *src);
/* overrides/platformer.c — the optional modern movement model */
int pc_platformer_on(void);
void native_pf_hop(M68KCtx *ctx);
void native_pf_longjump(M68KCtx *ctx);
void native_pf_arc(M68KCtx *ctx);
void native_pf_lj(M68KCtx *ctx);
void native_pf_diag(M68KCtx *ctx);
void native_pf_fall(M68KCtx *ctx);
void native_pf_collision(M68KCtx *ctx);
void native_pf_landing_impact(M68KCtx *ctx);
/* overrides/pickup.c — the widened object-interaction range */
void interact_register(void);
int interact_extend_px(void);
void native_mm_pickup_gate(M68KCtx *ctx);
void native_place_probe(M68KCtx *ctx);
/* port/guest_trace.c — traps executing the exception vector table. */
void pc_trap_vector_execution(M68KCtx *ctx);
#ifdef __cplusplus
} /* extern "C" */
#endif
