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
 * Zone size: PICKUP_RX / PICKUP_RY env (default 18 / 12 px, half-window each side).
 */
#include "port/port_internal.h"

#define A5_F80   3968u    /* current input bits ($20 = fire)  */
#define A5_F94   3988u    /* player screen X                  */
#define A5_F96   3990u    /* player screen Y                  */

unsigned long g_native_pickup_hits = 0;

/* Extra HORIZONTAL (X) pickup/interaction reach, in pixels, ON TOP of each handler's
 * own vanilla window. Single knob ("interact_extend" in benefactor.json, default 5).
 * Vertical reach is left exactly as the original game. -1 = unresolved. */
int g_interact_extend = -1;

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
    extern int hw_get_interact(void);
    extern int g_modern_controls;

    /* VANILLA controls: keep FIRE as the trigger and the handler's own semantics —
     * only EXTEND the horizontal reach by `extend` px (nudge the player toward the
     * object while fire is held so the handler's narrow window triggers sooner). No
     * fire-clearing, no latch; with extend==0 this is a pure passthrough. This is why
     * interact_extend works whether or not modern controls are on. */
    if (!g_modern_controls) {
        uint16_t s_f80 = MR16(ctx->A[5] + A5_F80);
        uint16_t s_f94 = MR16(ctx->A[5] + A5_F94);
        if (extend > 0 && (s_f80 & 0x20)) {            /* fire held → reach further    */
            int objX = (int16_t)MR16(ctx->A[0] + 2u);
            int px   = (int16_t)s_f94;
            int d    = objX - px;
            if (d >  extend) d =  extend;
            if (d < -extend) d = -extend;
            MW16(ctx->A[5] + A5_F94, (uint16_t)(px + d));
        }
        rt_call_generated(ctx, addr);
        MW16(ctx->A[5] + A5_F94, s_f94);               /* restore X (f80 untouched)    */
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
    uint16_t s_f94 = MR16(ctx->A[5] + A5_F94);
    if (active) {
        int objX = (int16_t)MR16(ctx->A[0] + 2u);
        int px   = (int16_t)s_f94;
        int d    = objX - px;                          /* nudge toward object (X only) */
        if (d >  extend) d =  extend;
        if (d < -extend) d = -extend;
        MW16(ctx->A[5] + A5_F94, (uint16_t)(px + d));
        MW16(ctx->A[5] + A5_F80, 0x20);                /* present interact as fire     */
    } else {
        MW16(ctx->A[5] + A5_F80, (uint16_t)(s_f80 & ~0x20));  /* FIRE never interacts  */
    }

    uint32_t pre0 = MR32(ctx->A[0]);                   /* obj +0/+2 (coords / active)  */
    uint32_t pre4 = MR32(ctx->A[0] + 4u);              /* obj +4/+6 (state)            */
    rt_call_generated(ctx, addr);
    int triggered = active && (MR32(ctx->A[0]) != pre0 || MR32(ctx->A[0] + 4u) != pre4);

    MW16(ctx->A[5] + A5_F80, s_f80);                   /* restore input                */
    MW16(ctx->A[5] + A5_F94, s_f94);
    if (triggered) {
        if (latch) s_interact_consumed = 1;            /* one toggle per key press     */
        g_native_pickup_hits++;
        if (getenv("PICKUP_LOG"))
            fprintf(stderr, "[interact] $%06X triggered (extend=%d)\n", addr, extend);
    }
}

/* Resolve the single horizontal-extend knob (env override > benefactor.json > 5). */
int interact_extend_px(void)
{
    extern int pc_config_int(const char *, int);
    if (g_interact_extend < 0) {
        const char *e = getenv("INTERACT_EXTEND");
        g_interact_extend = e ? atoi(e) : pc_config_int("interact_extend", 0);
        if (g_interact_extend < 0) g_interact_extend = 0;
    }
    return g_interact_extend;
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
IK(595FF4) IK(596064) IK(59610A) IK(597548) IK(597892) IK(59B0B0)

void pickup_register(void)
{
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
