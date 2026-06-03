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

/* Shared: widen the pickup radius for one collectible handler, delegating the
 * actual collect to the recompiled body. `addr` = the handler being overridden. */
static void pickup_wide(M68KCtx *ctx, uint32_t addr)
{
    static int rx = -1, ry = -1;
    if (rx < 0) { const char *e = getenv("PICKUP_RX"); rx = e ? atoi(e) : 18; }
    if (ry < 0) { const char *e = getenv("PICKUP_RY"); ry = e ? atoi(e) : 12; }

    int objY = (int16_t)MR16(ctx->A[0] + 0u);
    int objX = (int16_t)MR16(ctx->A[0] + 2u);
    int py   = (int16_t)MR16(ctx->A[5] + A5_F96);
    int px   = (int16_t)MR16(ctx->A[5] + A5_F94);
    int dy = py - objY, dx = px - objX;

    /* Spoof only when the player is pressing fire (alone) and is inside the WIDER
     * centered zone — then the item's own (narrow) test will see player==item and
     * collect. Otherwise leave everything vanilla. */
    int spoof = (MR16(ctx->A[5] + A5_F80) == 0x20)
             && dy >= -ry && dy <= ry && dx >= -rx && dx <= rx;

    uint16_t sy = 0, sx = 0;
    if (spoof) {
        g_native_pickup_hits++;
        if (getenv("PICKUP_LOG"))
            fprintf(stderr, "[pickup] $%06X dx=%d dy=%d -> widen-collect\n", addr, dx, dy);
        sy = MR16(ctx->A[5] + A5_F96);
        sx = MR16(ctx->A[5] + A5_F94);
        MW16(ctx->A[5] + A5_F96, (uint16_t)objY);   /* present player AT the item */
        MW16(ctx->A[5] + A5_F94, (uint16_t)objX);
    }
    rt_call_generated(ctx, addr);                   /* the item's own full logic   */
    if (spoof) {
        MW16(ctx->A[5] + A5_F96, sy);               /* restore before the tail runs */
        MW16(ctx->A[5] + A5_F94, sx);
    }
}

/* Thin per-handler thunks (the override dispatch can't pass the address). */
#define PK(hex) void native_pickup_##hex(M68KCtx *ctx) { pickup_wide(ctx, 0x##hex##u); }
PK(586B1C) PK(586B2A) PK(586C10) PK(586C1E) PK(586D14)
PK(586E6C) PK(586ECC) PK(586F9C) PK(587006) PK(5870CE)
PK(5871F4) PK(587272) PK(58733A) PK(58743C) PK(5874FE)
PK(587554) PK(587616) PK(58766E) PK(5876C4)

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
