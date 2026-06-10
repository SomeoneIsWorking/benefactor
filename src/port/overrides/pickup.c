/* pc_overrides_pickup.c — native port of the object-PICKUP mechanic (widened).
 *
 * Vanilla: each collectible's per-frame handler collects it only when the player
 * presses FIRE alone ($f80(a5)==$20) AND the player's screen pos is within a tiny,
 * ONE-SIDED window of the item — e.g. X in [objX, objX+$a] (≤10px, one side). That
 * ~10px one-sided horizontal window is why "even when you're touching the key you
 * can't pick it up unless you're right on top of it."
 *
 * There are ~19 collectible handlers (all play the $6c24 "item collected" sound),
 * and they differ a lot past the proximity test: simple ones just clr the object,
 * others animate (a spinning key), others write an inventory list ($109c), others
 * are carry/throw. Re-implementing all 19 byte-for-byte just to enlarge two range
 * constants would be a lot of porting-error surface.
 *
 * So we OWN the part that's actually wrong — the pickup RADIUS — and reuse each
 * item's own (correct, verified) collect routine: the override computes a WIDER,
 * CENTERED pickup zone; when the player is inside it and pressing fire, it invokes
 * the item's original handler with the player presented AT the item, so the item's
 * own narrow proximity test passes and it runs its full normal collect (inventory,
 * animation, sound, tail). Outside the wide zone, the original runs untouched, so
 * vanilla behavior is exactly preserved everywhere else. The player position is
 * restored immediately after. Set BENEFACTOR_RECOMP_PICKUP=1 to disable.
 *
 * Reach: `interact_extend` cfg knob (px, horizontal only). Logging: REPL `pklog`.
 */
#include "port/port_internal.h"
#include "engine/generated/game_gpl.h"   /* gfn_gpl_57EA76 (MM pickup gate super-call) */

#define A5_F80   3968u    /* current input bits ($20 = fire)  */
#define A5_F94   3988u    /* player Y (vertical)              */
#define A5_F96   3990u    /* player X (horizontal)            */
/* AXIS NOTE (verified live 2026-06-10, savestate gear repro): the object struct is
 * (a0)+0 = obj X, (a0)+2 = obj Y, and the player pair is $f94 = Y, $f96 = X — the
 * OPPOSITE of what the old comments (and gameplay-engine-map.md) claimed. The first
 * version of this override nudged $f94 toward (a0)+2, i.e. extended reach
 * VERTICALLY, which let the player grab a gear through a platform while hanging
 * under it. The horizontal pair is $f96 ↔ (a0)+0. */

unsigned long g_native_pickup_hits = 0;
int g_pickup_log = 0;      /* REPL `pklog` — log interact-wide decisions */

/* Per-handler X-window constants, RE-extracted from the generated handlers
 * (2026-06-10). Every one of the 30 handlers gates on the SAME pattern:
 *     move.w $f96(a5),d4 ; sub.w d0,d4 ; cmpi.w #range,d4 ; bhi fail
 * with d0 = objX + bias (bias = the addq/subq applied to d0 after its last load).
 * So the handler passes iff  playerX in [objX+bias, objX+bias+range].
 * A NUDGE TO objX IS OUTSIDE THE WINDOW whenever bias > 0 — that was the
 * "window moves instead of widening" bug: pulling the player onto objX
 * overshot past the lower edge, so only players approaching from ≥extend px
 * right got clamped INTO the window. The nudge must target the NEAREST EDGE
 * of the real window, and never move a player who is already inside it.
 * objX source: most read MR16(a0); $595FF4 receives objX in d0 from its
 * caller; $59B0B0 reads it from $84(a0).
 * Biases are PATH-verified (a linear scan over the listing double-counts addq's
 * from mutually exclusive paths — $58A828 has one addq #8 per path, true bias 8;
 * $589572/$5876C4's subq's sit on paths that jump away before the compare, true
 * bias 0). $58BCF2 does `add.w (a3),d0` on every path (dynamic base) — its window
 * can't be tabled, so it has NO entry here and is never nudged (vanilla reach).
 * $595FE4 is the DISPATCH ENTRY of the lever family whose interior label $595FF4
 * was wrapped alone at first (the walker calls $595FE4, so the override/extension
 * never fired for those levers). */
typedef enum { OBJX_A0 = 0, OBJX_D0, OBJX_A0_84 } ObjXSrc;
static const struct { uint32_t addr; int bias, range; ObjXSrc src; } s_xwin[] = {
    { 0x586B1Cu,  4,  8, OBJX_A0 }, { 0x586B2Au,  4,  8, OBJX_A0 },
    { 0x586C10u,  4,  8, OBJX_A0 }, { 0x586C1Eu,  4,  8, OBJX_A0 },
    { 0x586D14u, -4, 12, OBJX_A0 }, { 0x586E6Cu,  0, 16, OBJX_A0 },
    { 0x586ECCu,  0, 16, OBJX_A0 }, { 0x586F9Cu,  0,  9, OBJX_A0 },
    { 0x587006u,  4,  8, OBJX_A0 }, { 0x5870CEu,  0, 16, OBJX_A0 },
    { 0x5871F4u,  0, 16, OBJX_A0 }, { 0x587272u,  3,  8, OBJX_A0 },
    { 0x58733Au,  0, 12, OBJX_A0 }, { 0x58743Cu,  0, 16, OBJX_A0 },
    { 0x5874FEu,  0, 16, OBJX_A0 }, { 0x587554u,  0, 16, OBJX_A0 },
    { 0x587616u,  0, 16, OBJX_A0 }, { 0x58766Eu,  0, 16, OBJX_A0 },
    { 0x5876C4u,  0, 16, OBJX_A0 }, { 0x589572u,  0, 16, OBJX_A0 },
    { 0x589642u, -8, 16, OBJX_A0 }, { 0x58A828u,  8, 16, OBJX_A0 },
    { 0x58BF24u,-32, 48, OBJX_A0 },
    { 0x595FE4u,  4, 10, OBJX_A0 },
    { 0x595FF4u,  4, 10, OBJX_D0 }, { 0x596064u,  4, 10, OBJX_A0 },
    { 0x59610Au,  4, 10, OBJX_A0 }, { 0x597548u,  0, 16, OBJX_A0 },
    { 0x597892u,  4,  8, OBJX_A0 }, { 0x59B0B0u, -8, 20, OBJX_A0_84 },
};

/* Compute the nudged player X for `addr`'s handler. WE own the interaction
 * zone (the handler's window stays only the delivery mechanism): the player
 * can interact when within `extend` px of the OBJECT'S 16px TILE
 * ([objX, objX+16] — every interactable/collectible is one tile wide), not of
 * the handler's window. The vanilla windows are arbitrary offset slices of
 * the tile (the lever's is [objX+4, objX+12] — standing 2px left of the
 * sprite is 6px out of the window), so measuring reach from the window made
 * the feel uneven per object. In zone → return a point inside the handler's
 * window (clamp) so its own check passes; outside → px unchanged (vanilla;
 * note extend==0 never reaches here, so vanilla stays bit-exact). */
static int nudged_px(M68KCtx *ctx, uint32_t addr, int px, int extend)
{
    for (unsigned i = 0; i < sizeof s_xwin / sizeof s_xwin[0]; i++) {
        if (s_xwin[i].addr != addr) continue;
        int objX;
        switch (s_xwin[i].src) {
        case OBJX_D0:    objX = (int16_t)ctx->D[0];              break;
        case OBJX_A0_84: objX = (int16_t)MR16(ctx->A[0] + 0x84u); break;
        default:         objX = (int16_t)MR16(ctx->A[0]);         break;
        }
        if (px < objX - extend || px > objX + 16 + extend)
            return px;                   /* out of reach: vanilla          */
        int lo = objX + s_xwin[i].bias, hi = lo + s_xwin[i].range;
        if (px < lo) return lo;
        if (px > hi) return hi;
        return px;                       /* already inside the window      */
    }
    return px;                           /* unknown handler: never touch   */
}

/* Extra HORIZONTAL (X) pickup/interaction reach, in pixels, ON TOP of each handler's
 * own vanilla window. Single knob "interact_extend", resolved live each frame through
 * the unified config (ENV > REPL `cfg` > benefactor.json > 0). Vertical reach is left
 * exactly as the original game. */

/* One-trigger-per-press latch (interactables only). Levers are one-shot toggles, so
 * holding the interact key must NOT re-fire across frames. Consumed only when a trigger
 * actually changes the object's state; rearmed when the key releases. Collectibles
 * (latch=0) keep firing each frame (they vanish on collect, so there's no flip-flop). */
static int s_interact_consumed = 0;

/* Native interaction-reach extension around the engine's object-driven handlers.
 *
 * Each collectible/interactable handler reads the player ($f94/$f96) + its own object
 * coords, gates on FIRE ($f80==$20) within a narrow proximity window, then runs its
 * collect/toggle. We keep those handlers (re-implementing ~30 of them byte-for-byte to
 * change two constants would be pure porting risk) but OWN the interaction decision:
 *   - drive it from the INTERACT key, never FIRE — so FIRE stays free for the long-jump
 *     (when the key is up, the fire bit is cleared for the handler, so FIRE can never
 *     pick up or toggle); and
 *   - extend the player's HORIZONTAL reach by `extend` px: nudge the player's apparent X
 *     toward the object by up to `extend` before the handler runs, so the handler's OWN
 *     (unchanged) window triggers `extend` px sooner, symmetrically on both sides. Y is
 *     never touched, so vertical reach is exactly the original.
 * $f80/$f94 are restored immediately after the handler runs. latch=1 (levers) fires once
 * per key press (consumed on a real state change); latch=0 (collectibles) every frame. */
static void interact_wide(M68KCtx *ctx, uint32_t addr, int extend, int latch)
{
    extern int hw_get_interact(void), hw_get_fire_vanilla(void);
    extern int pc_modern_any(void);

    /* VANILLA controls: keep FIRE as the trigger and the handler's own semantics —
     * only EXTEND the horizontal reach by `extend` px (nudge the player toward the
     * object while fire is held so the handler's narrow window triggers sooner). No
     * fire-clearing, no latch; with extend==0 this is a pure passthrough. This is why
     * interact_extend works whether or not modern controls are on. */
    if (!pc_modern_any()) {
        uint16_t s_f80 = MR16(ctx->A[5] + A5_F80);
        uint16_t s_f96 = MR16(ctx->A[5] + A5_F96);
        if (extend > 0 && (s_f80 & 0x20)) {            /* fire held → reach further    */
            int px  = (int16_t)s_f96;
            int npx = nudged_px(ctx, addr, px, extend);
            MW16(ctx->A[5] + A5_F96, (uint16_t)npx);
            if (g_pickup_log)
                fprintf(stderr, "[interact-v] $%06X a0=%06X objX=%d objY=%d "
                        "px=%d py=%d npx=%d\n", addr, ctx->A[0],
                        (int)(int16_t)MR16(ctx->A[0]),
                        (int)(int16_t)MR16(ctx->A[0] + 2u), px,
                        (int)(int16_t)MR16(ctx->A[5] + A5_F94), npx);
        }
        rt_call_generated(ctx, addr);
        MW16(ctx->A[5] + A5_F96, s_f96);               /* restore X (f80 untouched)    */
        return;
    }

    int interact = hw_get_interact();
    if (!interact) s_interact_consumed = 0;            /* key released → rearm latch */

    if (latch && getenv("INTERACT_SCAN")) {
        static uint32_t seen[32]; static int nseen = 0;
        int known = 0; for (int i = 0; i < nseen; i++) if (seen[i] == addr) known = 1;
        if (!known && nseen < 32) {
            seen[nseen++] = addr;
            fprintf(stderr, "[interact-scan] $%06X a0=%06X objX=%d objY=%d st4=%04X\n",
                    addr, ctx->A[0], (int)(int16_t)MR16(ctx->A[0]+2u),
                    (int)(int16_t)MR16(ctx->A[0]+0u), MR16(ctx->A[0] + 4u));
            fflush(stderr);
        }
    }

    int active = interact && !(latch && s_interact_consumed);

    uint16_t s_f80 = MR16(ctx->A[5] + A5_F80);
    uint16_t s_f96 = MR16(ctx->A[5] + A5_F96);
    if (active) {
        int px = (int16_t)s_f96;
        MW16(ctx->A[5] + A5_F96, (uint16_t)nudged_px(ctx, addr, px, extend));
        MW16(ctx->A[5] + A5_F80, 0x20);                /* present interact as fire     */
    } else if (hw_get_fire_vanilla() && (s_f80 & 0x20)) {
        /* Fire held on a VANILLA-scheme device (mixed setups: e.g. modern pad +
         * vanilla keyboard, or the harness's forced fire): keep the original
         * fire-interacts semantics, with the same reach extension. */
        if (extend > 0)
            MW16(ctx->A[5] + A5_F96,
                 (uint16_t)nudged_px(ctx, addr, (int16_t)s_f96, extend));
    } else {
        MW16(ctx->A[5] + A5_F80, (uint16_t)(s_f80 & ~0x20));  /* modern FIRE never interacts */
    }

    uint32_t pre0 = MR32(ctx->A[0]);                   /* obj +0/+2 (coords / active)  */
    uint32_t pre4 = MR32(ctx->A[0] + 4u);              /* obj +4/+6 (state)            */
    rt_call_generated(ctx, addr);
    int triggered = active && (MR32(ctx->A[0]) != pre0 || MR32(ctx->A[0] + 4u) != pre4);

    MW16(ctx->A[5] + A5_F80, s_f80);                   /* restore input                */
    MW16(ctx->A[5] + A5_F96, s_f96);
    if (triggered) {
        if (latch) s_interact_consumed = 1;            /* one toggle per key press     */
        g_native_pickup_hits++;
        if (g_pickup_log)
            fprintf(stderr, "[interact] $%06X triggered (extend=%d)\n", addr, extend);
    }
}

/* Resolve the horizontal-extend knob LIVE (ENV > REPL > JSON > 0), so a runtime
 * `cfg interact_extend N` takes effect on the next frame with no restart. */
int interact_extend_px(void)
{
    extern int pc_cfg_int(const char *, int);
    int e = pc_cfg_int("interact_extend", 0);
    return e < 0 ? 0 : e;
}

static void pickup_wide(M68KCtx *ctx, uint32_t addr) { interact_wide(ctx, addr, interact_extend_px(), 0); }
static void lever_wide (M68KCtx *ctx, uint32_t addr) { interact_wide(ctx, addr, interact_extend_px(), 1); }

/* Thin per-handler thunks (the override dispatch can't pass the address). */
#define PK(hex) void native_pickup_##hex(M68KCtx *ctx) { pickup_wide(ctx, 0x##hex##u); }
PK(586B1C) PK(586B2A) PK(586C10) PK(586C1E) PK(586D14)
PK(586E6C) PK(586ECC) PK(586F9C) PK(587006) PK(5870CE)
PK(5871F4) PK(587272) PK(58733A) PK(58743C) PK(5874FE)
PK(587554) PK(587616) PK(58766E) PK(5876C4)

/* Interactable (lever/switch/door) handlers — each reads its object coords from
 * (a0) and gates on fire+proximity like a collectible, but toggles/triggers instead
 * of being collected (so: latched, one trigger per interact-key press).
 *
 * NOTE: $579690 is deliberately NOT here — despite gating on $f80==$20 it is the
 * PLAYER input/movement handler (lea $f70(a5),a0; reads $f90/$fa4), so wrapping it
 * would clear the player's fire bit every frame and break jump/throw. Only handlers
 * that read a real world object via `movem.w (a0)` belong here. */
#define IK(hex) void native_interact_##hex(M68KCtx *ctx) { lever_wide(ctx, 0x##hex##u); }
IK(589572) IK(589642) IK(58A828) IK(58BCF2) IK(58BF24)
IK(595FE4) IK(595FF4) IK(596064) IK(59610A) IK(597548) IK(597892) IK(59B0B0)

/* ── $57EA76 — MERRY-MAN pickup pose-action ──────────────────────────────────
 * Reached from the player handler $579690's FIRE pose dispatch (verified live:
 * `pcwatch $57FD82` at the pickup attributes the $f70 lift-action install
 * ($57A018) to this function). Gates on d4 bit14 — the "merry man here"
 * collision flag the player handler computes. The carry STATE is $109c(a5)
 * (see native_hands_full below); the held MM record handler becomes $57C4E2
 * and tracks the player. NOTE: $1094(a5) is the ITEM carry only — a held
 * merry man does NOT set it; and $fa2 bit15 latches on first pickup but never
 * clears on drop (NOT a carry flag).
 *
 * CAVEAT (verified live): the real lift path is the INLINE double-emitted copy
 * of this code inside the player handler ($579750 chain) — this entry override
 * never fires for it, so it CANNOT gate bare modern fire by itself. The actual
 * modern-fire block is the $108e pickup-cooldown arm in native_gameplay_input
 * (gameplay.c), which also hosts the one-shot interact→fire edge bridge that
 * makes the interact key trigger the pickup. This override stays registered as
 * defense-in-depth for any path that DOES rt_call $57EA76: same policy (strip
 * d4 bit14 for empty-handed bare modern fire). */
/* Carrying a merry man: bit14 of $10AC(a5) (mirror at $10B6). Verified live
 * (2026-06-10, savestate on top of an idle MM): $10AC = $0002 free → $4002
 * across the lift → $0002 after the fire-drop, and again 0 after walking away.
 *
 * $109c(a5) was FALSIFIED as a carry flag (the previous version of this
 * function): it is the LIFT-CANDIDATE descriptor — set ($580118) by the idle
 * MM handler ($57C13A) while a liftable MM overlaps the player, and 0 WHILE
 * the man is actually carried. Reading it as "hands full" inverted both
 * modern-control gates (interact pickup blocked / bare fire allowed exactly
 * when a fresh MM was underfoot — the "pickup key flips after a drop" bug).
 * The engine also consumes $109c as the lift linkage: poking it to 0 before
 * a lift produced a frozen, detached MM record. Do not write it.
 * ($1094 covers only items; $fa2 bit15 latches on first lift, never clears.) */
int native_hands_full(M68KCtx *ctx)
{
    return (MR16(ctx->A[5] + 0x10ACu) & 0x4000u) != 0;
}

void native_mm_pickup_gate(M68KCtx *ctx)
{
    extern int pc_modern_any(void);
    extern int hw_get_interact(void), hw_get_fire_vanilla(void);
    int carrying = MR16(ctx->A[5] + 4244u) != 0 ||          /* item ($1094)   */
                   native_hands_full(ctx);                  /* item or MM     */
    if (g_pickup_log && (ctx->D[4] & 0x4000u))
        fprintf(stderr, "[mm] $57EA76 d4=%08X carry=%d int=%d vfire=%d\n",
                ctx->D[4], carrying, hw_get_interact(), hw_get_fire_vanilla());
    if (pc_modern_any() && !carrying &&
        !(hw_get_interact() || hw_get_fire_vanilla()))
        ctx->D[4] &= ~0x4000u;            /* modern FIRE alone: no MM pickup */
    gfn_gpl_57EA76(ctx);
}

void pickup_register(void)
{
    rt_register_override_gp(0x57EA76u, native_mm_pickup_gate);
    rt_register_override_gp(0x586B1Cu, native_pickup_586B1C);
    rt_register_override_gp(0x586B2Au, native_pickup_586B2A);
    rt_register_override_gp(0x586C10u, native_pickup_586C10);
    rt_register_override_gp(0x586C1Eu, native_pickup_586C1E);
    rt_register_override_gp(0x586D14u, native_pickup_586D14);
    rt_register_override_gp(0x586E6Cu, native_pickup_586E6C);
    rt_register_override_gp(0x586ECCu, native_pickup_586ECC);
    rt_register_override_gp(0x586F9Cu, native_pickup_586F9C);
    rt_register_override_gp(0x587006u, native_pickup_587006);
    rt_register_override_gp(0x5870CEu, native_pickup_5870CE);
    rt_register_override_gp(0x5871F4u, native_pickup_5871F4);
    rt_register_override_gp(0x587272u, native_pickup_587272);
    rt_register_override_gp(0x58733Au, native_pickup_58733A);
    rt_register_override_gp(0x58743Cu, native_pickup_58743C);
    rt_register_override_gp(0x5874FEu, native_pickup_5874FE);
    rt_register_override_gp(0x587554u, native_pickup_587554);
    rt_register_override_gp(0x587616u, native_pickup_587616);
    rt_register_override_gp(0x58766Eu, native_pickup_58766E);
    rt_register_override_gp(0x5876C4u, native_pickup_5876C4);
}

/* Interactable (lever/switch/door) handlers — same widening, latched (one-shot). */
void interact_register(void)
{
    rt_register_override_gp(0x589572u, native_interact_589572);
    rt_register_override_gp(0x589642u, native_interact_589642);
    rt_register_override_gp(0x58A828u, native_interact_58A828);
    rt_register_override_gp(0x58BCF2u, native_interact_58BCF2);
    rt_register_override_gp(0x58BF24u, native_interact_58BF24);
    rt_register_override_gp(0x595FE4u, native_interact_595FE4);
    rt_register_override_gp(0x595FF4u, native_interact_595FF4);
    rt_register_override_gp(0x596064u, native_interact_596064);
    rt_register_override_gp(0x59610Au, native_interact_59610A);
    rt_register_override_gp(0x597548u, native_interact_597548);
    rt_register_override_gp(0x597892u, native_interact_597892);
    rt_register_override_gp(0x59B0B0u, native_interact_59B0B0);
}

/* ── Identification scan (PICKUP_SCAN=1) ───────────────────────────────────────
 * Log the first dispatch of each collectible handler in a level (the object list
 * dispatches the FULL set every frame), then delegate — to see which handler each
 * item is. Used to confirm e.g. $586C10 is the universal key/item. */
#define PKSCAN(hex) \
    static void pkscan_##hex(M68KCtx *ctx) { static int seen = 0; \
        if (!seen) { seen = 1; fprintf(stderr, "[pkscan] $" #hex \
            " objX=%d objY=%d\n", (int)(int16_t)MR16(ctx->A[0]+2u), \
            (int)(int16_t)MR16(ctx->A[0])); } \
        rt_call_generated(ctx, 0x##hex##u); }
PKSCAN(586B1C) PKSCAN(586B2A) PKSCAN(586C10) PKSCAN(586C1E) PKSCAN(586D14)
PKSCAN(586E6C) PKSCAN(586ECC) PKSCAN(586F9C) PKSCAN(587006) PKSCAN(5870CE)
PKSCAN(5871F4) PKSCAN(587272) PKSCAN(58733A) PKSCAN(58743C) PKSCAN(5874FE)
PKSCAN(587554) PKSCAN(587616) PKSCAN(58766E) PKSCAN(5876C4)

void pickup_register_scan(void)
{
    rt_register_override_gp(0x586B1Cu, pkscan_586B1C);
    rt_register_override_gp(0x586B2Au, pkscan_586B2A);
    rt_register_override_gp(0x586C10u, pkscan_586C10);
    rt_register_override_gp(0x586C1Eu, pkscan_586C1E);
    rt_register_override_gp(0x586D14u, pkscan_586D14);
    rt_register_override_gp(0x586E6Cu, pkscan_586E6C);
    rt_register_override_gp(0x586ECCu, pkscan_586ECC);
    rt_register_override_gp(0x586F9Cu, pkscan_586F9C);
    rt_register_override_gp(0x587006u, pkscan_587006);
    rt_register_override_gp(0x5870CEu, pkscan_5870CE);
    rt_register_override_gp(0x5871F4u, pkscan_5871F4);
    rt_register_override_gp(0x587272u, pkscan_587272);
    rt_register_override_gp(0x58733Au, pkscan_58733A);
    rt_register_override_gp(0x58743Cu, pkscan_58743C);
    rt_register_override_gp(0x5874FEu, pkscan_5874FE);
    rt_register_override_gp(0x587554u, pkscan_587554);
    rt_register_override_gp(0x587616u, pkscan_587616);
    rt_register_override_gp(0x58766Eu, pkscan_58766E);
    rt_register_override_gp(0x5876C4u, pkscan_5876C4);
}
