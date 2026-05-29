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
    gfn_gpl_578C3E(ctx);
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
