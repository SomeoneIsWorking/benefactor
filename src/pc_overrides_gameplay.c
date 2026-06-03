/* pc_overrides_gameplay.c — Native maps of high-level gameplay behaviors.
 *
 * These overrides exist so WE own the game-flow decision points in readable C,
 * rather than leaving them buried in recompiled M68K. They capture the
 * high-level behavior (the "what" and "when") and delegate the heavy
 * hardware/teardown work to the existing handlers for now.
 */
#include "pc_internal.h"
#include "generated/game_gpl.h"   /* raw gfn_gpl_* (to delegate without re-dispatch) */

/* gameplay work-area globals (a5 = $0057EE12) */
#define GP_MODE_001E   0x001Eu    /* screen/mode word: 2 = in a level, 8 = game-over menu */
#define GP_FLAGS_10AC  0x10ACu    /* a5-relative end-of-level flags word (bit15 = level ended) */

static int s_dbg_endlevel = -1;   /* BENEFACTOR_DBG_ENDLEVEL=1 logs the win/lose trigger state */

/* Master switch for the opt-in modern control scheme (X = interact, X+Down = drop,
 * Hop as its own action). Set once from "modern_controls" in pc_register_overrides;
 * when 0 (default) the modern overrides aren't registered and controls are vanilla. */
int g_modern_controls = 0;

extern uint32_t hw_get_cop1lc(void);   /* for GO_TRACE diagnostics */

/* ── $578C3E — end-of-level handler ──────────────────────────────────────────
 * Reached once a level ends. A level ends two ways, both of which set bit15 of
 * $10AC(a5) (tested by $5771C0/$579532) and route here:
 *
 *   LOSE  (death, e.g. boulder)               — lose graphic set upstream
 *   WIN   (all merry men teleported away, then
 *          the player re-enters the teleporter) — win graphic set upstream
 *
 * CORRECTED RE (2026-06-03, verified with BENEFACTOR_GO_TRACE + screenshots — the
 * OLD comment here had the two branches BACKWARDS):
 *   $1E != 8 → $578C74 — sets up selectable handlers ($585ac6/$578afc/$578a4a) and
 *              IS the CONTINUE / GAME OVER menu path (renders at gp-bank cop1lc
 *              $003914). On a normal death (lives left) this same path reloads the
 *              level (card); on game-over (lives 0) it shows the menu.
 *   $1E == 8 → $578C46 → jmp $57731C — and $57731C currently HANGS in our port (HTTP
 *              goes unresponsive; not a clean reload). Do NOT route game-over here.
 * pc_debug_game_over (O key) enters $578C3E with $1E ALREADY = 2, so the old
 * "if $1E==8 set 2" did nothing → the menu still appeared (the user's bug).
 *
 * TODO (game-over → level card): make the $578C74 path take its lives-left RELOAD
 * branch on game-over. Find the lives/continue check inside $578C74's subtree
 * (jsr -$6576/-$627e, reads $5859cd) and force the reload, OR find the level-reload
 * entry and drive it (e.g. set g_enter_gameplay + g_gameplay_entry=$577000 for the
 * current $20.w level). $57731C is a dead end — fix or avoid it. For now: passthrough. */
void native_end_of_level(M68KCtx *ctx)
{
    if (s_dbg_endlevel < 0) s_dbg_endlevel = getenv("BENEFACTOR_DBG_ENDLEVEL") ? 1 : 0;
    if (s_dbg_endlevel) {
        uint32_t a5 = ctx->A[5];
        fprintf(stderr, "[end-of-level] $578C3E: $1E=%u $10AC=%04X "
                "(%s)\n", r16(GP_MODE_001E), r16(a5 + GP_FLAGS_10AC),
                (r16(GP_MODE_001E) == 8) ? "-> game-over menu" : "-> banner+load");
        fflush(stderr);
    }
    /* Bypass the CONTINUE / GAME OVER screen: when lives run out the engine sets
     * $1E=8 and routes to the game-over menu ($57731C). We instead force the normal
     * death path ($1E!=8 → lose banner + reload of the CURRENT level), so the player
     * just sees the death banner and returns to the level card to retry — no
     * game-over screen. (The lose banner graphic is already set upstream.) */
    /* NOTE: the game-over bypass is NOT solved (see the RE block above the function).
     * The CONTINUE/GAME OVER menu is the $578C74 ($1E!=8) path; forcing $1E=8 hits
     * $57731C which HANGS. The likely real fix is to make $578C74 take its reload
     * (level-card) branch on game-over — TBD. For now: clean passthrough (vanilla). */
    if (getenv("BENEFACTOR_GO_TRACE"))
        fprintf(stderr, "[GO] $578C3E enter $1E=%u cop1lc=%06X $10AC=%04X\n",
                r16(GP_MODE_001E), hw_get_cop1lc(), r16(ctx->A[5] + GP_FLAGS_10AC));
    gfn_gpl_578C3E(ctx);   /* let the banner play; the menu is intercepted at $585AC6 */
}

/* ── $59C5B0 — card/menu screen renderer: port the game-over transition ───────
 * $59C5B0 renders BOTH the in-game level card AND the CONTINUE/GAME OVER screen
 * (same cop1lc $003914), keyed by bit6 of $1093. On death the engine runs the
 * game-over screen ($578C74: skull banner → CONTINUE/GAME OVER menu) which, once
 * a choice is made, transitions by jumping back into the gameplay engine to
 * reload the level (CONTINUE → $5770D0 / GAME OVER → $57731C). Those raw jumps
 * reset a7 and re-run gameplay setup from DEEP in the call stack — which hangs in
 * our threaded model (rt_jump is a same-frame trampoline, it can't unwind the C
 * stack the way a real m68k jmp does).
 *
 * Port the transition to the engine-faithful destination — "reload the current
 * level" — via the clean PC re-entry we already own: respawn the game thread at
 * $577000 for the current $20.w level (pc_request_level_restart, the same call
 * the pause-menu Retry uses → level card → cavern). We take it the moment the
 * game-over screen begins, so the player goes straight to the level card to retry
 * instead of the CONTINUE/GAME OVER menu.
 *
 * Discriminator: bit6 of $1093 — set ($60) for the whole game-over screen, clear
 * ($80) during gameplay and the in-game level card. Verified the ONLY writer is
 * `bset #6,$1093` at $578C84 (the game-over setup), so this never fires on a win
 * or normal play. Read it ABSOLUTE at $57FEA5, NOT a5-relative: by the time
 * $59C5B0 runs, $59BA7A has done `movea.l a6,a5` so a5 = $DFF000. */
void native_gameover_menu(M68KCtx *ctx)
{
    uint8_t f = MR8(0x57FEA5u);        /* $1093: bit6 game-over, bit5 menu-phase */
    if (getenv("BENEFACTOR_GO_TRACE"))
        fprintf(stderr, "[GO] $59C5B0 $57FEA5=%02X cop1lc=%06X\n", f, hw_get_cop1lc());
    /* Banner phase = bit6 set, bit5 clear ($40): leave it alone so the skull
     * GAME OVER animation + fade play in full. Menu phase = bit6+bit5 ($60): the
     * banner has faded and the engine is about to show the CONTINUE/GAME OVER
     * menu. Redirect THERE to the level card (reload current level) via the clean
     * thread re-entry we already own. */
    if ((f & 0x60u) == 0x60u) {
        extern void pc_request_level_restart(void);
        MW8(0x57FEA5u, f & ~0x60u);    /* drop the markers for a clean restarted state */
        pc_request_level_restart();    /* respawn at $577000 (current level) → level card */
        if (getenv("BENEFACTOR_GO_TRACE"))
            fprintf(stderr, "[GO] menu phase → reload current level (level card)\n");
        /* Set-flags-and-return (the proven $150 hand-off pattern): we skip the
         * menu render; the flow parks at its next vblank wait and pc_step_threaded
         * tears this thread down and respawns at $577000. */
        return;
    }
    gfn_gpl_59C5B0(ctx);               /* banner / in-game level card → render as usual */
}

/* ── $57DEAC — gameplay input read: re-gate item DROP onto the interact key ────
 * Drop = the only carried-item action (throw unused): Fire+Down while carrying ($1094).
 * The drop is selected by the engine's decoded-input state machine, so we steer it at
 * the INPUT SOURCE (before $57DEAC decodes), scoped to carrying:
 *   - interact key held → present FIRE at $bfe001 (bit7, active-low) so the engine's own
 *     decode produces Fire (+ whatever direction is held → Down = drop);
 *   - real Fire while carrying without interact → strip the fire bit from $f80 after the
 *     decode so Fire alone can no longer drop.
 * Jump=Up and long-jump=Fire+Left/Right are untouched outside the carry case. */
/* DROP bindings — the engine performs a drop on Fire+Down while carrying, and Fire
 * (held) is what stops Down from going prone. Rather than hard-wire one key combo, we
 * resolve a logical DROP from any of several bindings and translate it uniformly:
 *
 *   DROP requested  := (interact + Down)  OR  (dedicated Drop button)   [extensible]
 *
 *   - DROP requested → present Fire (+ inject Down for a button-only press) at the hw
 *     source BEFORE $57DEAC decodes → the engine drops AND, because Fire is held, never
 *     goes prone (matching vanilla Fire+Down) — even after the item has left your hands.
 *   - Down without any DROP binding → strip Fire so plain Down/Fire+Down just go prone
 *     and never drop.
 *   - Everything else (Up=hop, Fire+Left/Right=long-jump, Interact alone) is untouched.
 *
 * Adding a controller button later is just OR-ing another source into `drop`; the
 * translation + prone handling below are binding-agnostic. $bfe001 bit7 reads as
 * (s_fire_pressed || s_mouse_lmb), so both are driven. State is restored after decode. */
void native_gameplay_input(M68KCtx *ctx)
{
    extern int hw_get_interact(void), hw_get_fire(void), hw_get_mouse_lmb(void);
    extern int hw_joy_down(void), hw_get_drop(void), hw_joy_up(void), hw_get_hop(void);
    extern void hw_set_fire(int), hw_set_mouse_lmb(int), hw_set_joy_down(int), hw_set_joy_up(int);

    int down = hw_joy_down();
    int carrying = MR16(ctx->A[5] + 0x1094u) != 0;
    int drop_intent = (hw_get_interact() && down) || hw_get_drop();

    /* HOP: a dedicated Hop binding ORs into the up/jump input. The Up *direction* is left
     * fully vanilla — it hops, enters doors, climbs ladders and drives menus, exactly as
     * the engine intends. (An earlier grounded-gate that suppressed up-hop while standing
     * was wrong: it broke up-to-enter-door.) */
    int up_dir   = hw_joy_up();
    int want_up  = up_dir || hw_get_hop();
    int up_restore = (want_up != up_dir);
    if (up_restore) hw_set_joy_up(want_up);

    /* DROP via the engine's real place (it dispatches the held-item action by player pose
     * through the $5834de table — all those targets, incl. the prone pose's, are now
     * recompiled; see the $57EB16 seed):
     *   - drop intent + carrying → present Fire+Down so the engine drops in whatever pose
     *     the player is in (correct placement + SFX); the place action runs instead of
     *     prone, so it never prones.
     *   - drop intent + empty-handed → strip Down so it doesn't prone and doesn't enter the
     *     held-item flow (nothing to drop).
     *   - plain Down (no drop intent) → strip Fire so a real Fire+Down can't drop. */
    int restore = 0, sf = 0, sl = 0, down_add = 0, down_strip = 0;
    if (drop_intent && carrying) {
        sf = hw_get_fire(); sl = hw_get_mouse_lmb();
        hw_set_fire(1); hw_set_mouse_lmb(1);
        if (!down) { hw_set_joy_down(1); down_add = 1; }   /* button-only press supplies Down */
        restore = 1;
    } else if (drop_intent) {
        if (down) { hw_set_joy_down(0); down_strip = 1; }  /* empty-handed: no prone, no flow */
    } else if (down) {
        sf = hw_get_fire(); sl = hw_get_mouse_lmb();
        hw_set_fire(0); hw_set_mouse_lmb(0);               /* plain Down → prone, never drop */
        restore = 1;
    }

    gfn_gpl_57DEAC(ctx);

    if (restore)     { hw_set_fire(sf); hw_set_mouse_lmb(sl); }
    if (down_add)    hw_set_joy_down(0);
    if (down_strip)  hw_set_joy_down(1);
    if (up_restore)  hw_set_joy_up(up_dir);

    if (getenv("BENEFACTOR_DBG_DROP") && (drop_intent || down))
        fprintf(stderr, "[drop-input] f80=%04X int=%d down=%d carry=%d intent=%d\n",
                MR16(ctx->A[5] + 0xf80u), hw_get_interact(), down, carrying, drop_intent);
}

/* ── $57EB20 — "place carried item at tile" PROBE (BENEFACTOR_DBG_DROP=1) ──────
 * Logs the caller PC + input state each time the place routine runs, to pin the
 * fire+down gate that selects the drop (so we can port it to interact+down). */
void native_place_probe(M68KCtx *ctx)
{
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("BENEFACTOR_DBG_DROP") ? 1 : 0;
    if (dbg) {
        extern uint32_t rt_get_last_insn(void);
        uint32_t a5 = ctx->A[5];
        fprintf(stderr, "[place] $57EB20 from $%06X  $f80=%04X d4=%08X $1094=%04X $109c=%08X\n",
                rt_get_last_insn(), MR16(a5 + 0xf80u),
                ctx->D[4], MR16(a5 + 0x1094u), MR32(a5 + 0x109Cu));
        fflush(stderr);
    }
    gfn_gpl_57EB20(ctx);
}

/* ── $5782B4 — level setup (runs on every level entry, incl. the win's next
 * level — where it FREEZES). Log how it's dispatched (the jumping instruction
 * via rt_get_last_insn) and the level-selecting registers, so we learn the
 * dispatch mechanism from the level-1 entry we can reach headless. */
void native_level_setup(M68KCtx *ctx)
{
    if (s_dbg_endlevel < 0) s_dbg_endlevel = getenv("BENEFACTOR_DBG_ENDLEVEL") ? 1 : 0;
    if (s_dbg_endlevel) {
        extern uint32_t rt_get_last_insn(void);
        extern int rt_insn_ring_snapshot(uint32_t *out, int max);
        fprintf(stderr, "[level-setup] $5782B4 entered from insn $%06X  "
                "d0=%08X d1=%08X d4=%08X d5=%08X d6=%08X d7=%08X a3=%08X\n",
                rt_get_last_insn(), ctx->D[0], ctx->D[1], ctx->D[4], ctx->D[5],
                ctx->D[6], ctx->D[7], ctx->A[3]);
        uint32_t ins[64]; int in = rt_insn_ring_snapshot(ins, 64);
        fprintf(stderr, "[level-setup]   lead-up insns:");
        for (int i = 0; i < in; i++) fprintf(stderr, " %06X", ins[i]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    gfn_gpl_5782B4(ctx);
}

/* ── $59DC02 — level loader ───────────────────────────────────────────────────
 * Called from the end-of-level banner (twice) to build/load a level. The
 * next-level load on a WIN currently freezes; log its inputs so we can see how
 * the win-path load differs from the (working) game-over load. Pure passthrough
 * otherwise. */
void native_level_load(M68KCtx *ctx)
{
    if (s_dbg_endlevel < 0) s_dbg_endlevel = getenv("BENEFACTOR_DBG_ENDLEVEL") ? 1 : 0;
    if (s_dbg_endlevel) {
        fprintf(stderr, "[level-load] $59DC02: d0=%08X d1=%08X a0=%08X a1=%08X a2=%08X a3=%08X\n",
                ctx->D[0], ctx->D[1], ctx->A[0], ctx->A[1], ctx->A[2], ctx->A[3]);
        fflush(stderr);
    }
    gfn_gpl_59DC02(ctx);
}
