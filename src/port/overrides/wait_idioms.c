/* wait_idioms.c — native owners for the guest's custom-register busy-waits.
 *
 * The recogniser and the reasoning are in src/port/wait_idiom.h. This file is
 * the two halves that need the engine: the native bodies, and the scan that
 * finds every one of these loops in the player's own decrunched image and
 * registers a body at it.
 *
 * Registration is by ADDRESS, so a pattern that happens to match bytes the
 * guest never executes as an instruction is inert — the body only ever runs if
 * the PC really arrives there.
 */
#include "port/wait_idiom.h"

#include "common/log.h"
#include "port/port_internal.h"

/* One body for every kind: which loop this is gets asked again at run time,
 * from the address the PC is standing on. Registering a different function per
 * kind would mean four registrations to keep in step with the recogniser. */
static void native_wait_idiom(M68KCtx *ctx) {
    const uint32_t at = rt_get_pc();
    const PcWaitIdiom found = pc_wait_idiom_at(g_mem, (uint32_t)RT_MEM_SIZE, at);
    const int frame_before = hw_get_frame_num();
    switch (found.kind) {
    case PC_WAIT_FRAME:
        /* The whole point: the guest is asking for the next frame, so say so to
         * the HOST. That parks the game flow and presents here, which is where
         * the oracle's own injected wait presented too. */
        hw_vblank_wait();
        break;
    case PC_WAIT_SCANLINE:
        hw_beam_wait_scanline(found.scanline);
        break;
    case PC_WAIT_BEAM_BELOW:
        hw_beam_wait_below();
        break;
    case PC_WAIT_BEAM_ABOVE:
        hw_beam_wait_above();
        break;
    case PC_WAIT_BLITTER:
        /* Synchronous blitter: BBUSY is never set, so the guest's loop would
         * fall through on its first test. Nothing to wait for. */
        break;
    case PC_WAIT_NONE:
    default:
        /* The bytes changed under us — a screen was loaded over them. Let the
         * guest run its own code rather than guessing what it now means. */
        return;
    }
    benefactor_log_write(BENEFACTOR_LOG_TRACE, "wait", "$%06X kind=%u frame=%d->%d resume=$%06X",
                         at, (unsigned)found.kind, frame_before, hw_get_frame_num(), found.resume);
    rt_jump(ctx, ctx->image, found.resume);
}

/* Find every busy-wait in [low, high) and give it a native owner. Called once
 * per decrunched image, with that image's mask, so an address the next screen
 * reuses for something else is not still owned by this. */
void pc_register_wait_idioms(uint32_t image_mask, uint32_t low, uint32_t high) {
    if (!g_mem || high > (uint32_t)RT_MEM_SIZE || low >= high)
        return;
    /* Registrations accumulate, so an image whose bytes have already been
     * scanned must not be scanned again — a level restart reloads the same
     * gameplay bank, and a second pass would install every owner twice. */
    static uint32_t scanned = 0u;
    if ((scanned & image_mask) == image_mask)
        return;
    scanned |= image_mask;
    unsigned frames = 0, halves = 0, blits = 0, scanlines = 0;
    for (uint32_t at = low; at + 6u <= high; at += 2u) {
        const PcWaitIdiom found = pc_wait_idiom_at(g_mem, (uint32_t)RT_MEM_SIZE, at);
        if (found.kind == PC_WAIT_NONE)
            continue;
        rt_register_native(image_mask, at, native_wait_idiom);
        if (found.kind == PC_WAIT_FRAME) {
            frames++;
            /* The second half of a pair is a poll in its own right and the
             * guest can branch straight into it; give it its own owner, but
             * step the scan past the first half only, so that is what happens. */
        } else if (found.kind == PC_WAIT_SCANLINE) {
            scanlines++;
        } else if (found.kind == PC_WAIT_BLITTER) {
            blits++;
        } else {
            halves++;
        }
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "override",
                         "[waits] image mask %u, $%06X-$%06X: %u frame waits, %u scanlines, "
                         "%u beam halves, %u blitter waits now native\n",
                         image_mask, low, high, frames, scanlines, halves, blits);
}
