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
#include "pc_internal.h"

#define A5_F80   3968u    /* current input bits ($20 = fire)  */
#define A5_F94   3988u    /* player screen X                  */
#define A5_F96   3990u    /* player screen Y                  */

unsigned long g_native_pickup_hits = 0;

/* Pickup window params — runtime-tunable (env defaults, live via HTTP /pickup).
 * cx/cy bias the window onto the sprite centre ($f94 is a player edge, so there's
 * an intrinsic ~player-width bias); rx/ry = half-window; off = spoof hotspot. */
int g_pickup_rx = -1, g_pickup_ry = -1, g_pickup_cx = -999, g_pickup_cy = -999, g_pickup_off = -1;

/* Interactable (lever/switch/door) window — a SEPARATE, more generous default than
 * the collectible window. Two reasons: the user wants interactables widened too, and
 * some native interactable windows are already wider than collectibles (e.g. $597892
 * is X≤24) — the override REPLACES the native trigger (it clears fire when out of the
 * wide zone), so the wide window must be ≥ the widest native window or it would NARROW
 * that handler's reach. */
int g_interact_rx = -1, g_interact_ry = -1;

/* One-trigger-per-press latch (interactables only). Levers are one-shot toggles
 * (you rewind to reset them), so holding the interact key must NOT re-fire across
 * frames. Collectibles don't use this (they're removed on collect; their verified
 * "hold to grab" behaviour is preserved). Frame-scoped via the shared key state:
 * consumed on the first in-range trigger of a press, rearmed when the key releases. */
static int s_interact_consumed = 0;

/* Shared widening core. latch=1 → one-shot per key press (levers); latch=0 →
 * fire every in-range frame (collectibles). */
static void interact_wide(M68KCtx *ctx, uint32_t addr,
                          int rx, int ry, int cx, int cy, int off, int latch)
{
    int objY = (int16_t)MR16(ctx->A[0] + 0u);
    int objX = (int16_t)MR16(ctx->A[0] + 2u);
    int py   = (int16_t)MR16(ctx->A[5] + A5_F96);
    int px   = (int16_t)MR16(ctx->A[5] + A5_F94);
    int dy = (py - objY) - cy, dx = (px - objX) - cx;   /* centred on the sprite */

    if (latch && getenv("INTERACT_SCAN")) {
        /* Log the first dispatch of each interactable handler in a level + its
         * world position, so we can confirm the address is live and locate it. */
        static uint32_t seen[32]; static int nseen = 0;
        int known = 0; for (int i = 0; i < nseen; i++) if (seen[i] == addr) known = 1;
        if (!known && nseen < 32) {
            seen[nseen++] = addr;
            fprintf(stderr, "[interact-scan] $%06X a0=%06X objX=%d objY=%d st4=%04X\n",
                    addr, ctx->A[0], objX, objY, MR16(ctx->A[0] + 4u));
            fflush(stderr);
        }
    }

    extern int hw_get_interact(void);
    int interact = hw_get_interact();
    if (!interact) s_interact_consumed = 0;            /* key released → rearm latch */
    int in_range = dy >= -ry && dy <= ry && dx >= -rx && dx <= rx;
    int edge     = latch ? (interact && !s_interact_consumed) : interact;
    int collect  = edge && in_range;

    if (getenv("PICKUP_MEASURE") && interact
        && dx > -60 && dx < 60 && dy > -60 && dy < 60)
        fprintf(stderr, "[pk-measure] $%06X dx=%d dy=%d (range rx=%d ry=%d -> %s)\n",
                addr, dx, dy, rx, ry, collect ? "IN" : "out");

    /* Decouple INTERACT from FIRE. The vanilla handler collects when $f80==$20
     * (fire alone) AND the player is in its narrow window — which is why holding
     * fire to charge a long-jump grabs the item. We instead:
     *  - on the INTERACT key (in our wider window): present the player at the item
     *    hotspot + set fire-alone so the handler's gate passes -> collect;
     *  - otherwise: CLEAR the fire bit for this handler so fire never picks up,
     *    leaving fire free for long-jump. $f80 is restored before any other code. */
    uint16_t s_f80 = MR16(ctx->A[5] + A5_F80), sy = 0, sx = 0;
    if (collect) {
        if (latch) s_interact_consumed = 1;            /* one trigger per key press */
        sy = MR16(ctx->A[5] + A5_F96);
        sx = MR16(ctx->A[5] + A5_F94);
        MW16(ctx->A[5] + A5_F96, (uint16_t)(objY + off));
        MW16(ctx->A[5] + A5_F94, (uint16_t)(objX + off));
        MW16(ctx->A[5] + A5_F80, 0x20);             /* fire-alone -> gate passes   */
    } else {
        MW16(ctx->A[5] + A5_F80, (uint16_t)(s_f80 & ~0x20));  /* free fire for jump */
    }
    uint16_t pre_obj = MR16(ctx->A[0]);
    rt_call_generated(ctx, addr);                   /* the item's own full logic   */
    MW16(ctx->A[5] + A5_F80, s_f80);                /* restore input              */
    if (collect) {
        if (getenv("PICKUP_LOG"))
            fprintf(stderr, "[pickup] $%06X dx=%d dy=%d collected=%d\n",
                    addr, dx, dy, MR16(ctx->A[0]) != pre_obj);
        MW16(ctx->A[5] + A5_F96, sy);
        MW16(ctx->A[5] + A5_F94, sx);
        if (MR16(ctx->A[0]) != pre_obj) g_native_pickup_hits++;
    }
}

/* Collectible widening — narrow centred window, no latch (vanilla "hold to grab"
 * preserved); the object is removed on collect so it can't re-fire. */
static void pickup_wide(M68KCtx *ctx, uint32_t addr)
{
    extern int pc_config_int(const char *, int);
    /* env override > benefactor.json > default. -1/-999 = "not yet resolved". */
    if (g_pickup_rx  < 0)    { const char *e = getenv("PICKUP_RX");  g_pickup_rx  = e ? atoi(e) : pc_config_int("pickup_rx", 14); }
    if (g_pickup_ry  < 0)    { const char *e = getenv("PICKUP_RY");  g_pickup_ry  = e ? atoi(e) : pc_config_int("pickup_ry", 12); }
    if (g_pickup_cx  == -999){ const char *e = getenv("PICKUP_CX");  g_pickup_cx  = e ? atoi(e) : pc_config_int("pickup_cx", -4); }
    if (g_pickup_cy  == -999){ const char *e = getenv("PICKUP_CY");  g_pickup_cy  = e ? atoi(e) : pc_config_int("pickup_cy", 0); }
    if (g_pickup_off < 0)    { const char *e = getenv("PICKUP_SPOOF_OFF"); g_pickup_off = e ? atoi(e) : pc_config_int("pickup_off", 4); }
    interact_wide(ctx, addr, g_pickup_rx, g_pickup_ry, g_pickup_cx, g_pickup_cy, g_pickup_off, 0);
}

/* Interactable (lever/switch/door) widening — wider window, one-shot latch. Reuses
 * the collectible cx/cy bias + spoof-offset (same player-edge physics). */
static void lever_wide(M68KCtx *ctx, uint32_t addr)
{
    extern int pc_config_int(const char *, int);
    if (g_interact_rx < 0) { const char *e = getenv("INTERACT_RX"); g_interact_rx = e ? atoi(e) : pc_config_int("interact_rx", 24); }
    if (g_interact_ry < 0) { const char *e = getenv("INTERACT_RY"); g_interact_ry = e ? atoi(e) : pc_config_int("interact_ry", 16); }
    if (g_pickup_cx  == -999){ const char *e = getenv("PICKUP_CX");  g_pickup_cx  = e ? atoi(e) : pc_config_int("pickup_cx", -4); }
    if (g_pickup_cy  == -999){ const char *e = getenv("PICKUP_CY");  g_pickup_cy  = e ? atoi(e) : pc_config_int("pickup_cy", 0); }
    if (g_pickup_off < 0)    { const char *e = getenv("PICKUP_SPOOF_OFF"); g_pickup_off = e ? atoi(e) : pc_config_int("pickup_off", 4); }
    interact_wide(ctx, addr, g_interact_rx, g_interact_ry, g_pickup_cx, g_pickup_cy, g_pickup_off, 1);
}

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
