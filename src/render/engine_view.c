/* native_engine_view.c — capture engine state for the wide renderer.
 *
 * See native_engine_view.h for the firewall contract. This file is the ONLY
 * place the wide-render pipeline touches raw g_mem: it reads the named engine
 * variables once, validates them, and hands the renderer a typed snapshot. The
 * composers downstream see only `const EngineView*` and cannot reach around it. */
#include "engine/hw_private.h"          /* g_mem, RT_MEM_SIZE */
#include "render/engine_view.h"

/* Big-endian 16-bit read of an engine variable, sign-extended. Bounds-checked:
 * an out-of-range address is a hard failure (caught by the caller), never a 0
 * fudge — the firewall forbids silent defaults. */
static int ev_r16s(uint32_t addr, int *ok)
{
    if (addr + 1u >= RT_MEM_SIZE) { *ok = 0; return 0; }
    return (int16_t)((g_mem[addr] << 8) | g_mem[addr + 1]);
}

int engine_view_capture(EngineView *ev)
{
    int ok = 1;
    ev->valid = 0;
    if (!g_mem) return 0;

    /* Camera: screen-left world X (a5+$0FA8), already engine-clamped to the
     * level edges. Signed (can be slightly negative at the left edge). */
    ev->camera = ev_r16s(EV_CAMERA_ADDR, &ok);

    /* Level edges -> engine camera-clamp range. The biases are the engine's own
     * clamp constants (clamp routine $57C79E), not display fudge. */
    ev->level_lo = ev_r16s(EV_LEVEL_LO_ADDR, &ok) - EV_CLAMP_LO_BIAS;
    ev->level_hi = ev_r16s(EV_LEVEL_HI_ADDR, &ok) - EV_CLAMP_HI_BIAS;

    /* Per-level tilemap row stride: phase-table entry[1].d2adj (+4). Stored as a
     * signed phase delta; the stride is its magnitude. No L9 fallback — if the
     * table is unreadable we fail rather than decode the wrong rows. */
    {
        int rs = ev_r16s(EV_PHASETAB_ADDR + 4u, &ok);
        ev->row_stride = (rs < 0) ? -rs : rs;
        if (ev->row_stride <= 0) ok = 0;
    }

    /* Tile graphics base table address (a constant pointer into the level data
     * region). Range-check it; the renderer dereferences it. */
    ev->gfxtab = EV_GFXTAB_ADDR;
    if (EV_GFXTAB_ADDR + 4u >= RT_MEM_SIZE) ok = 0;

    /* Edges must bracket a real range, and the clamp must leave the camera
     * inside it — a cross-level invariant. A per-level fudge can't satisfy this
     * for all levels, so a violation means the derivation is wrong (RE it),
     * not "nudge a constant". */
    if (ev->level_hi <= ev->level_lo) ok = 0;

    ev->valid = ok;
    return ok;
}
