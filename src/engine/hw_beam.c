/* hw_beam.c — when is a frame over?
 *
 * Split out of hw.c, which owns the rest of the chip set. This file owns that
 * one question, and the answer has two halves that must not be confused:
 *
 *   - The BEAM comes from consumed guest time. A VPOSR read has to say where
 *     the beam really is, because the game polls it (engine/hw.c derives
 *     s_scanline from rt_get_guest_cycles for exactly that).
 *   - The FRAME ends where the GUEST asks to wait — not where the beam
 *     happens to cross. Crossing raises the boundary; reaching a wait is what
 *     lands it.
 *
 * That second half is the thing the working oracle product did and this one
 * did not, and it is where every divergence measured against it came from
 * (docs/issues/0008). The oracle had no cycle model at all: its frames began
 * and ended only at the host waits its translator had put in place of the
 * guest's busy-wait loops. Deriving the boundary from cycles instead put it
 * wherever a frame's budget ran out — and a blit costs four tenths of a frame,
 * so it landed between a BLTSIZE write and the poll waiting for that blit,
 * with the poster's copper list half rebuilt. Measured: its first frame showed
 * $008182 with null bitplane pointers where the oracle showed the finished
 * $0081D2, and every per-frame counter after it was a tick out.
 *
 * The guest's own wait loops are given native owners by
 * src/port/wait_idiom.h + src/port/overrides/wait_idioms.c, so "reaching a
 * wait" is something this file can actually be told about.
 */
#include "engine/hw_private.h"

#include "port/port.h"
#include "runtime/guest_runtime.h"

/* Boundaries crossed while waiting for the game flow to reach its own wait,
 * and the cap that stops a screen which never waits from never presenting.
 *
 * TWO, and that is measured, not reasoned. A cap of 1 sounds better — holding
 * a second boundary puts two frames of guest work into one presented frame —
 * but the poster's rebuild does not fit inside one held frame, so a cap of 1
 * reproduces the whole fault this file exists to fix: the frame boundary lands
 * back between the BLTSIZE write at $003424 and the BBUSY poll at $00342A,
 * and frame 7160 shows $008182 with null bitplane pointers again, 23548 bytes
 * of the 8 MB differing. At 2 that frame is gone and 7157 frames are
 * byte-identical to the oracle. Raising it further changes nothing measured,
 * so 2 it is: the smallest cap that lets the guest finish what it started. */
volatile uint32_t g_hw_beam_held = 0;

int hw_boundary_hold(void) { return 0; }

void hw_boundary_release(void) {}

int hw_boundary_take_owed(void) { return 0; }

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
