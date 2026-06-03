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

/* ── $578C3E — end-of-level handler ──────────────────────────────────────────
 * Reached once a level ends. A level ends two ways, both of which set bit15 of
 * $10AC(a5) (tested by $5771C0/$579532) and route here:
 *
 *   LOSE  (death, e.g. boulder)               — lose graphic set upstream
 *   WIN   (all merry men teleported away, then
 *          the player re-enters the teleporter) — win graphic set upstream
 *
 * $1E then selects: != 8 → end-of-level BANNER ($578C74), then load level via
 * $59DC02 (next level on a win — this is where the post-level FREEZE is);
 *         == 8 → CONTINUE / GAME OVER menu ($57731C).
 *
 * We own the decision here; bodies are still the original handlers (calling the
 * raw gfn_gpl_578C3E avoids re-entering this override). */
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
    if (r16(GP_MODE_001E) == 8) {
        MW16(GP_MODE_001E, 2);     /* 2 = "in a level" → banner+reload branch */
        if (s_dbg_endlevel) fprintf(stderr, "[end-of-level] game-over bypassed -> level card\n");
    }
    gfn_gpl_578C3E(ctx);
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
