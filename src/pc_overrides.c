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
    { extern void pc_config_load(void); pc_config_load(); }   /* benefactor.json */

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

    /* We OWN the title menu's option setup: native_menu_setup ($003872) rewrites
     * item 1 to "LEVEL SELECT" before the engine builds/draws the menu, and the
     * fire-dispatch ($0039D0) routes cursor 1 to the native level picker (then
     * $150) instead of the password screen. cursor 0 (PLAY GAME) and the per-
     * frame drawing/animation still run through the engine's gfn_gp_003872. The
     * leaf helpers (glyph blit $0049B6, cursor $3C5A/$3C88) stay overridden above. */
    { extern void native_menu_setup(M68KCtx *ctx);
      extern void native_main_menu_fire_dispatch(M68KCtx *ctx);
      rt_register_override(0x00003872u, native_menu_setup);
      rt_register_override(0x000039D0u, native_main_menu_fire_dispatch); }

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
    /* $59C5B0 = card/menu screen renderer. Native game-over transition: when the
     * CONTINUE/GAME OVER screen begins (bit6 of $1093), reload the current level
     * (→ level card) instead of showing the menu, via the clean thread re-entry.
     * See native_gameover_menu. */
    { extern void native_gameover_menu(M68KCtx *ctx);
      rt_register_override_gp(0x0059C5B0u, native_gameover_menu); }
    rt_register_override_gp(0x0059DC02u, native_level_load);
    rt_register_override_gp(0x005782B4u, native_level_setup);
    { extern void native_place_probe(M68KCtx *ctx);   /* BENEFACTOR_DBG_DROP probe */
      rt_register_override_gp(0x0057EB20u, native_place_probe); }

    /* Widescreen object capture (pc_overrides_gameplay.c): record each object's
     * UNCLIPPED draw params at the engine's per-object draw choke point ($57D8D0,
     * before its 352px camera-clip) so the native renderer can draw objects across
     * the full wide view. Both super-call the recomp body → vanilla unaffected. */
    rt_register_override_gp(0x0057D79Au, native_objwalk);
    rt_register_override_gp(0x0057D8D0u, native_objdraw_capture);

    /* Audio engine — native port, staged (pc_overrides_audio.c).
     * Stage 1: SFX trigger. Set BENEFACTOR_RECOMP_AUDIO=1 to keep the recompiled
     * bodies (A/B comparison while porting). */
    if (!getenv("BENEFACTOR_RECOMP_AUDIO")) {
        rt_register_override_gp(0x0058656Eu, native_sfx_trigger);
    }

    /* ── Modern controls (OPT-IN: "modern_controls", default false) ──────────────
     * The modern scheme separates INTERACT from FIRE: X (interact) collects items,
     * toggles levers, and — with Down — drops the carried item; Hop is its own
     * bindable action; FIRE no longer interacts. It also widens the pickup/interact
     * reach (vanilla's window is tiny and one-sided). All of it is implemented by
     * three overrides — the input re-gate ($57DEAC, pc_overrides_gameplay.c) and the
     * pickup/lever wideners (pc_overrides_pickup.c).
     *
     * When modern_controls is OFF (default), NONE of these are registered, so the
     * engine's ORIGINAL handlers run: Fire does pickup/interact/drop-with-Down and
     * Up is jump — i.e. exactly the authentic Amiga control scheme.
     *
     * KNOWN GAP (TODO): a few interactions are NOT yet on the interact key in modern
     * mode and still respond to FIRE — notably PICKING UP / CARRYING MERRY MEN (you
     * lift and toss them to spots they can't reach on their own, and move the gray
     * "blind"/unconverted ones away from hazards). Their pick-up handlers aren't in
     * the pickup/interact override lists (pc_overrides_pickup.c) yet; find them
     * (PICKUP_SCAN / object-table walk) and add them so modern mode covers them.
     *
     * The only user config is "modern_controls" (on/off) and "interact_extend" (the
     * reach in px). interact_extend applies in BOTH schemes — in vanilla it just
     * extends Fire's reach; in modern it also moves the trigger to X. Debug-only env:
     * PICKUP_SCAN (identify collectible handlers), BENEFACTOR_RECOMP_PICKUP (keep the
     * recompiled pickup for A/B). */
    { extern int pc_config_bool(const char *, int);
      extern void native_gameplay_input(M68KCtx *ctx);
      extern void interact_register(void);
      extern int interact_extend_px(void);
      extern int g_modern_controls;

      int modern = getenv("BENEFACTOR_MODERN_CONTROLS")
                     ? atoi(getenv("BENEFACTOR_MODERN_CONTROLS"))
                     : pc_config_bool("modern_controls", 0);
      g_modern_controls = modern;
      int extend = interact_extend_px();
      fprintf(stderr, "[controls] modern_controls=%d interact_extend=%d\n", modern, extend);

      if (getenv("PICKUP_SCAN")) {
          pickup_register_scan();                 /* diagnostic — identify item handlers */
      } else {
          if (modern)                             /* X+Down drop, hop — modern only */
              rt_register_override_gp(0x0057DEACu, native_gameplay_input);
          /* Pickup/interact wideners: needed in modern (X trigger) OR whenever the
           * reach is extended in vanilla (Fire trigger + interact_extend>0). */
          if ((modern || extend > 0) && !getenv("BENEFACTOR_RECOMP_PICKUP")) {
              pickup_register();
              interact_register();
          }
      }
      /* else (vanilla, extend==0): no overrides; the engine's own Fire handlers run. */
    }
}

