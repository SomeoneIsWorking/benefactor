/* pc_overrides.c — Override registration hub
 *
 * Implementations live in domain-specific files:
 *   pc_overrides_hw.c     — hardware-wait eliminators, blitter init
 *   pc_overrides_boot.c   — boot animation
 *   pc_overrides_copper.c — copper list rebuild helpers, frame-level overrides
 *   pc_overrides_render.c — render pipeline hook wrappers
 */
#include "pc_internal.h"

/* ── Registration ────────────────────────────────────────────────────────────
 * Called once from pc_init_state().  Maps M68K addresses to native C
 * implementations.  Address order matches the game call sequence. */
void pc_register_overrides(void)
{
    /* Hardware waits (pc_overrides_hw.c) */
    /* NOTE: $0030C2 is NOT a hardware-wait loop — it is the state-machine
     * dispatch RE-ENTRY (movem.l (a7)+,a4-a6; bra $3092). Overriding it with a
     * no-op swallowed every screen transition, so the disk-boot flow unwound
     * after the intro instead of advancing. Let the recompiled
     * gfn_dispatch_reentry run. */
    rt_register_override(0x0031A0u, native_blitter_wait_clear);

    /* Boot animation (pc_overrides_boot.c) */
    rt_register_override(0x0074AAu, native_boot_anim_iterator);

    /* Runtime disk-overlay loader → loads the gameplay bank and enters it.
     * $6D714 is the original entry; the game relocates the body to $150 and the
     * title IRQ calls it there to start the game (the path that actually fires). */
    rt_register_override(0x006D714u, native_overlay_loader);
    rt_register_override(0x00000150u, native_overlay_loader_reloc);
    /* Native port of the title-bank glyph blitter ($0049B6) — the engine's
     * menu-text drawer. Lets us render "LEVEL SELECT" when asked to draw
     * "ENTER PASSWORD" without touching the chip-RAM strings. See
     * native_menu_glyph_blit() for the full disassembly translation. */
    { extern void native_menu_glyph_blit(M68KCtx *ctx);
      rt_register_override(0x000049B6u, native_menu_glyph_blit); }

    /* Main-menu fire-dispatch at $0039D0 — intercepts the cursor-based
     * dispatch (PLAY GAME / ENTER PASSWORD / LOAD EXTRA LEVELS) so cursor=1
     * routes to LEVEL SELECT (gameplay overlay with the F2/F3-picked $20.w)
     * instead of the password screen. cursor=0 still delegates to the
     * original (PLAY GAME). cursor=2 returns to the menu top until Disk.4
     * is supported. The fire detection itself stays in the engine — this
     * fn is only entered when the engine already decided fire was pressed
     * (see comments in pc_overrides_boot.c). */
    /* Title main menu ($003872) runs host-driven off the coroutine (pc_menu.c).
     * Its fire dispatch ($0039D0) is handled host-side via pc_menu_dispatch_decide,
     * so $0039D0 needs no override (the recompiled menu loop never runs). */
    { extern void pc_native_main_menu(M68KCtx *ctx);
      rt_register_override(0x00003872u, pc_native_main_menu); }

    /* $003C5A / $003C6E / $003C88 / $003C9A — four arrow-direction handlers
     * in the title menu. Each is a short rts-terminated mini-routine
     * starting with `move.w #$384,$2BE2(a5)` (audio re-trig?) and updating
     * the menu cursor at $E742(a5). Reconstructed from the chip-dump bytes:
     *
     *   $003C5A (DOWN): move.w #$384,$2BE2(a5)
     *                   cmpi.w #$2,$E742(a5)
     *                   beq.s  end   ; if cursor already 2, do nothing
     *                   addq.w #1,$E742(a5)
     *
     *   $003C88 (UP):   move.w #$384,$2BE2(a5)
     *                   tst.w  $E742(a5)
     *                   beq.s  end   ; if cursor 0, do nothing
     *                   subq.w #1,$E742(a5)
     *
     *   $003C6E / $003C9A handle LEFT/RIGHT — more complex (test a bit at
     *   low-mem $1F, shifts). The menu has 3 vertical options so LEFT/RIGHT
     *   are no-ops in normal play; we delegate them to gfn_gp_003872 (which
     *   handles a no-op pass safely).
     *
     * Recompiler crashes on these (IndexError in emitter._pre_rd) so we
     * implement them natively here. */
    { extern void native_menu_cursor_down(M68KCtx *ctx);
      extern void native_menu_cursor_up(M68KCtx *ctx);
      extern void gfn_gp_003872(M68KCtx *ctx);
      rt_register_override(0x00003C5Au, native_menu_cursor_down);
      rt_register_override(0x00003C88u, native_menu_cursor_up);
      rt_register_override(0x00003C6Eu, gfn_gp_003872);
      rt_register_override(0x00003C9Au, gfn_gp_003872); }
    /* Gameplay overlay's disk reader ($577B8C) — services the "ACCESSING!"
     * level load natively (gp-only: doesn't affect the title/intro). */
    rt_register_override_gp(0x00577B8Cu, native_gp_disk_read);

    /* Copper / frame (pc_overrides_copper.c) */
    rt_register_override(0x0041A4u, native_sprite_blitter_setup);
    rt_register_override(0x003488u, native_game_frame);

    /* Render pipeline (pc_overrides_render.c) */
    rt_register_override(0x00405Cu, native_text_sprite_render);
    rt_register_override(0x0040B6u, native_dispatch_table);
    rt_register_override(0x0040B8u, native_item_dispatch_1);
    rt_register_override(0x0040BAu, native_item_dispatch_2);
    rt_register_override(0x0040BCu, native_item_dispatch_3);
    rt_register_override(0x0040BEu, native_item_decrement);
    rt_register_override(0x0040CCu, native_item_scroll);
    rt_register_override(0x004102u, native_item_position);
    rt_register_override(0x00412Eu, native_item_blitter);
    rt_register_override(0x004236u, native_blit_row_callback);
    rt_register_override(0x0052A4u, native_post_blit_handler);
    rt_register_override(0x0055A0u, native_timer_interrupt);

    /* Gameplay flow (pc_overrides_gameplay.c) — native maps of game-flow
     * decisions. $578C3E is the end-of-level trigger (game-over vs next level). */
    rt_register_override_gp(0x00578C3Eu, native_end_of_level);
    rt_register_override_gp(0x0059DC02u, native_level_load);
    rt_register_override_gp(0x005782B4u, native_level_setup);
}

