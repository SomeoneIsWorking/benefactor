/* Guest time determines the beam position; recognized guest waits determine
 * when gameplay can yield a completed frame. The wait idioms are owned by
 * src/port/wait_idiom.h and src/port/overrides/wait_idioms.c. */
#include "engine/hw_private.h"

#include "port/port.h"
#include "runtime/guest_runtime.h"

/* The two halves of the guest's own vertical-blank poll, as host waits. The
 * guest spins on VPOSR bit 8 — first until the beam has come DOWN past line
 * 256, then until it has wrapped back ABOVE it — and the pair together is one
 * frame, which is hw_vblank_wait below. Alone, each is a partial wait: move
 * the guest clock to the position it is waiting for instead of interpreting
 * the spin.
 *
 * Only on the game thread, for the same reason hw_vblank_wait rounds only
 * there: an interrupt handler must not push the clock forward under the flow
 * it interrupted. Off the flow these are no-ops, which is what the oracle's
 * injected waits did off-thread too. */
#define BEAM_V8_LINE 256u

void hw_beam_wait_below(void) {
    if (!pc_on_game_thread())
        return;
    const uint64_t per_frame = (uint64_t)BEAM_CYCLES_PER_LINE * BEAM_LINES_PER_FRAME;
    const uint64_t into = rt_get_guest_cycles() % per_frame;
    const uint64_t target = (uint64_t)BEAM_V8_LINE * BEAM_CYCLES_PER_LINE;
    if (into < target)
        rt_add_guest_cycles(target - into);
}

void hw_beam_wait_above(void) {
    if (!pc_on_game_thread())
        return;
    const uint64_t per_frame = (uint64_t)BEAM_CYCLES_PER_LINE * BEAM_LINES_PER_FRAME;
    const uint64_t into = rt_get_guest_cycles() % per_frame;
    const uint64_t target = (uint64_t)BEAM_V8_LINE * BEAM_CYCLES_PER_LINE;
    if (into < target)
        return; /* already above the line: nothing to wait for */
    /* The wrap IS a frame boundary, so this half ends where a frame ends. */
    rt_add_guest_cycles(per_frame - into);
    if (g_hw_vblank_yield)
        (void)g_hw_vblank_yield();
}

void hw_beam_wait_scanline(uint8_t line) {
    if (!pc_on_game_thread())
        return;
    const uint64_t per_frame = (uint64_t)BEAM_CYCLES_PER_LINE * BEAM_LINES_PER_FRAME;
    const uint64_t into = rt_get_guest_cycles() % per_frame;
    const uint64_t target = (uint64_t)line * BEAM_CYCLES_PER_LINE;
    if (into < target) {
        rt_add_guest_cycles(target - into);
    } else {
        rt_add_guest_cycles((per_frame - into) + target);
        if (g_hw_vblank_yield)
            (void)g_hw_vblank_yield();
    }
}

/* A native body waiting for the vertical blank stands in for guest code that
 * would have spun for a frame — so it must COST the guest a frame, the same way
 * a blit costs the guest the bus cycles it would have occupied. The beam is
 * derived from consumed guest cycles, and native code consumes none: without
 * this charge, a native body that waits 32 times in a row advances the beam not
 * at all and every wait returns immediately.
 *
 * That is what flattened the boot logo fade. Its palette animation is 16 passes
 * of two frames each; it asked for 32 frames, got 1, and all sixteen colour
 * steps landed in a single displayed frame.
 *
 * This charge applies only to NATIVE callers. Guest code that spins on VPOSR
 * itself (the intro crawl at $003732) is interpreted instruction by
 * instruction and already pays for its own spin — charging that too, or
 * replacing it with this call, gives the crawl a second clock and it runs
 * ~165x too fast. See docs/issues/0008. */
void hw_vblank_wait(void) {
    if (pc_on_game_thread()) {
        const uint64_t per_frame = (uint64_t)BEAM_CYCLES_PER_LINE * BEAM_LINES_PER_FRAME;
        rt_add_guest_cycles(per_frame - rt_get_guest_cycles() % per_frame);
    }
    /* Disk-boot coroutine mode: this is the per-frame yield point — hand control
     * back to the frame driver (render + input + IRQs), then resume the game.
     * Otherwise a no-op (the snapshot path drives frames from src/port/game_loop.c). */
    if (g_hw_vblank_yield)
        (void)g_hw_vblank_yield();
}
