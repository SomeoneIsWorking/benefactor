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
    /* Title menu — full-native replacement (pc_overrides_title.c). */
    { extern void native_title_menu(M68KCtx *ctx);
      rt_register_override(0x00003872u, native_title_menu); }
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

