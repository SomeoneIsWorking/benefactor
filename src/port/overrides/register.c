/* pc_overrides.c — Override registration hub
 *
 * Implementations live in domain-specific files:
 *   pc_overrides_hw.c     — hardware-wait eliminators, blitter init
 *   pc_overrides_boot.c   — boot animation
 *   pc_overrides_copper.c — copper list rebuild helpers, frame-level overrides
 *   pc_overrides_render.c — render pipeline hook wrappers
 */
#include "port/port_internal.h"

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
     *   $003C6E / $003C9A handle LEFT/RIGHT — the DIFFICULTY selector on the
     *   PLAY GAME row (rotate the bit in $1E.w, clamped by $1F bits 0/2).
     *   They were stubbed to gfn_gp_003872 for a long time, which is why
     *   left/right appeared dead — now direct C ports (native_menu_diff_*).
     *
     * Recompiler crashes on these (IndexError in emitter._pre_rd) so we
     * implement them natively here. */
    { extern void native_menu_cursor_down(M68KCtx *ctx);
      extern void native_menu_cursor_up(M68KCtx *ctx);
      extern void native_menu_diff_left(M68KCtx *ctx);
      extern void native_menu_diff_right(M68KCtx *ctx);
      rt_register_override(0x00003C5Au, native_menu_cursor_down);
      rt_register_override(0x00003C88u, native_menu_cursor_up);
      rt_register_override(0x00003C6Eu, native_menu_diff_left);
      rt_register_override(0x00003C9Au, native_menu_diff_right); }
    /* $003700 — menu-art unpacker post-hook: re-draws on-page extras (the
     * DISK.4 indicator) after each page-1 unpack. */
    { extern void native_menu_art_unpack(M68KCtx *ctx);
      rt_register_override(0x00003700u, native_menu_art_unpack); }
    /* $003DAA — the password-field text renderer (cell-wise, from the live
     * password buffer). We OWN it and draw nothing: the field is replaced by
     * LEVEL SELECT in this port. See the RE block in boot.c. (The art itself
     * ships a clean field area — verified.) */
    { extern void native_menu_pwfield_draw(M68KCtx *ctx);
      rt_register_override(0x00003DAAu, native_menu_pwfield_draw); }
    /* Gameplay overlay's disk reader ($577B8C) — services the "ACCESSING!"
     * level load natively (gp-only: doesn't affect the title/intro). */
    rt_register_override_gp(0x00577B8Cu, native_gp_disk_read);
    /* In-game level-segment decruncher ($577E96): adds IMP! (fan Disk.4)
     * support next to the recompiled ATN! path. */
    { extern void native_level_decrunch(M68KCtx *ctx);
      rt_register_override_gp(0x00577E96u, native_level_decrunch); }

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
    /* Per-object loop step ($57D7BC): re-implements one walker iteration with a WIDENED
     * camera cull so $06xxxx objects (torches/teleporter/enemies) in the wide margins
     * still dispatch and reach the $57D8D0 capture (issue #4). margin==0 (default 352)
     * keeps the original $30/$170 window → vanilla unchanged. */
    rt_register_override_gp(0x0057D7BCu, native_objstep);
    /* Animated-object cull ($57D8B4): the walker routes objects with a non-zero anim
     * nibble to a SEPARATE camera window ($30/$1b0) — this is where $06xxxx torches/
     * teleporter/enemies were culled (static $05xxxx decorations use the $57D7BC path).
     * Widen it the same way. */
    rt_register_override_gp(0x0057D8B4u, native_objstep_b);
    rt_register_override_gp(0x0057D8D0u, native_objdraw_capture);
    /* The player is drawn by its own routine ($57A666), not the object loop. */
    rt_register_override_gp(0x0057A666u, native_player_capture);
    /* Cookie-cut characters (walkers/enemies) build blit descriptors at $57D3F4
     * (executor $57D6C4) — a separate path from the $57D8D0 object loop. Capture
     * each at entry (D0/D1/D5/A1) before its camera-clip; super-call the body. */
    rt_register_override_gp(0x0057D3F4u, native_char_capture);
    /* Object anim+draw dispatcher ($59AC38): own its init-integrity gate so a corrupt
     * low-RAM state ($1890 != $0200, e.g. a stale savestate) yields a clear diagnostic
     * in our code instead of the engine's bail wild-jumping into object data (the W6L2
     * "no function at $58081A" crash). Good state delegates to the recompiled body. */
    { extern void native_obj_anim_59AC38(M68KCtx *ctx);
      rt_register_override_gp(0x0059AC38u, native_obj_anim_59AC38); }
    /* Marry-Man build-entry capture: $57B07C (compositor frame start → clear), $57B19E
     * (RED build) and $57B856 (BLIND build) — fire per-record before the cull, so EVERY
     * marry man (in view + culled) is captured with the engine's own frame/facing/variant,
     * resolved natively from the $4a72 table. See pc_overrides_gameplay.c. */
    { extern void native_build_red(M68KCtx *ctx), native_build_blind(M68KCtx *ctx),
                  native_build_clear(M68KCtx *ctx);
      rt_register_override_gp(0x0057B07Cu, native_build_clear);
      rt_register_override_gp(0x0057B19Eu, native_build_red);
      rt_register_override_gp(0x0057B856u, native_build_blind); }
    /* Static-placement OBJECTS (caged Marry Men + level sprites) are drawn by walking
     * the object-only queue $5A39EC in native_wsstatic_compose (native_renderer.c) — no
     * override needed (the $57B0EE builder hook was double-emitted/unreliable, removed). */
    /* ROPES (blitter LINE-mode, $57DD42): capture every rope segment's UNCLIPPED world
     * endpoints at the SHARED clip/emit entry $57DCD4 (= a5-$113e — every rope creator
     * jmps there via rt_jump, BEFORE the vanilla-window clip/cull), reset at the build
     * driver $57DCAE. native_wsrope_compose (native_renderer.c) draws from the capture, so
     * ropes show across the full wide view (incl. creators culled off the vanilla view).
     * Both super-call the recomp body → vanilla unaffected. */
    { extern void native_wsrope_build(M68KCtx *ctx), native_wsrope_seg(M68KCtx *ctx);
      rt_register_override_gp(0x0057DCAEu, native_wsrope_build);
      rt_register_override_gp(0x0057DCD4u, native_wsrope_seg); }
    /* ANIMATED PAGE PATCHES (water surface line): the walker's multi-tile path $57D81C
     * CPU-writes 16x2 5-plane patches straight into the page (no blit) and culls to the
     * vanilla window — BenRen missed them entirely. Capture each record PRE-cull; the
     * recomp body still runs (engine behaviour identical). native_wswater_compose draws. */
    { extern void native_anim_patch(M68KCtx *ctx);
      rt_register_override_gp(0x0057D81Cu, native_anim_patch); }
    /* GET READY / GAME OVER banner: the native wide renderer ignores the engine page,
     * so the banner (drawn there) was invisible. Capture each of its three elements so
     * the renderer can composite them as a centered top UI overlay: the box ($578974),
     * the teleport animation ($578B94), and the text ($578860 GET READY / $57889C GAME
     * OVER, rendered by the $57892E font). */
    { extern void native_banner_capture(M68KCtx *ctx);
      extern void native_telanim_capture(M68KCtx *ctx);
      extern void native_getready_capture(M68KCtx *ctx);
      extern void native_gameover_text_capture(M68KCtx *ctx);
      rt_register_override_gp(0x00578974u, native_banner_capture);
      rt_register_override_gp(0x00578B94u, native_telanim_capture);
      rt_register_override_gp(0x00578860u, native_getready_capture);
      rt_register_override_gp(0x0057889Cu, native_gameover_text_capture);
      /* LEVEL COMPLETE banner: capture its text like the siblings + replace the
       * vanilla "PASSWORD: ..." with "LEVEL COMPLETE" after each build. */
      { extern void native_levelcomplete_text_capture(M68KCtx *ctx);
        extern void native_password_build(M68KCtx *ctx);
        rt_register_override_gp(0x005788DEu, native_levelcomplete_text_capture);
        rt_register_override_gp(0x0057901Eu, native_password_build); } }

    /* BENMOTION platformer physics (opt-in "platformer_physics", pc_overrides_
     * platformer.c): native flight owns rise ($579D84) + fall ($579F3A), the
     * vanilla UP-hop/long-jump commits are suppressed, and the JUMP trigger
     * rides the per-frame terrain pass. Knob off = verified passthrough. */
    { extern void native_pf_hop(M68KCtx *ctx), native_pf_longjump(M68KCtx *ctx),
                  native_pf_fall(M68KCtx *ctx), native_pf_collision(M68KCtx *ctx);
      extern void native_pf_arc(M68KCtx *ctx);
      rt_register_override_gp(0x00579D84u, native_pf_hop);
      rt_register_override_gp(0x00579D52u, native_pf_arc);   /* abort-arc variants */
      { extern void native_pf_lj(M68KCtx *ctx);
        rt_register_override_gp(0x00579A62u, native_pf_lj); } /* the LONG JUMP */
      rt_register_override_gp(0x00579DDCu, native_pf_longjump);
      rt_register_override_gp(0x00579F3Au, native_pf_fall);
      { extern void native_pf_diag(M68KCtx *ctx);
        rt_register_override_gp(0x00579E02u, native_pf_diag); } /* UP+dir diagonal hop */
      rt_register_override_gp(0x0057A934u, native_pf_collision); }

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
     * Two INDEPENDENT user knobs (resolved via the unified config, ENV > REPL > JSON):
     *   - "modern_controls" (bool): X=interact trigger, X+Down drop, Hop; gates ONLY the
     *     input-remap override below.
     *   - "interact_extend" (px): extra horizontal pickup/interact reach; works in BOTH
     *     schemes and is DECOUPLED from modern_controls. The pickup/interact wideners are
     *     registered unconditionally (with extend==0 they are a verified pure passthrough)
     *     and extend is resolved LIVE per frame, so a runtime `cfg interact_extend N`
     *     widens the reach with no restart. Debug-only env: PICKUP_SCAN (identify item
     *     handlers), BENEFACTOR_RECOMP_PICKUP (keep recompiled pickup for A/B). */
    { extern int pc_modern_kb(void), pc_modern_pad(void);
      extern void native_gameplay_input(M68KCtx *ctx);
      extern void interact_register(void);
      extern int interact_extend_px(void);

      fprintf(stderr, "[controls] modern_keyboard=%d modern_controller=%d interact_extend=%d\n",
              pc_modern_kb(), pc_modern_pad(), interact_extend_px());

      if (getenv("PICKUP_SCAN")) {
          pickup_register_scan();                 /* diagnostic — identify item handlers */
      } else {
          /* Registered unconditionally: the per-device modern flags are resolved
           * LIVE inside the overrides (options-menu toggles need no restart) and
           * with both flags off every one of these is a verified passthrough. */
          rt_register_override_gp(0x0057DEACu, native_gameplay_input);
          if (!getenv("BENEFACTOR_RECOMP_PICKUP")) {   /* wideners always on (live extend) */
              pickup_register();
              interact_register();
          }
      }
    }
}

