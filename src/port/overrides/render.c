/* src/port/overrides/render.c — Render pipeline hook overrides
 *
 * What is left here is native code that actually does something. There used to
 * be ten more "hook points" in this file, each a function whose whole body was
 * `rt_call(ctx, ctx->image, <its own address>)` — $00405C, $0040B6,
 * $0040B8, $0040BA, $0040BC, $0040BE, $0040CC, $004102, $00412E and $004236,
 * plus $003488 in copper.c. Registering an override that only calls the
 * original is behaviourally identical to not registering one, so they were
 * removed: they were placeholders from the retired translator, kept for a
 * native implementation that a later reading of the render path never needed.
 *
 * An override that does nothing cannot get its boundary wrong if it does not
 * exist. See the override table in CLAUDE.md before adding one back.
 *
 *   $0052A4  native_post_blit_handler   — post-blit: animation advance, zero fill
 *
 * The level-6 timer leaf $0055A0 has NO native owner, deliberately — see the
 * note further down this file. Do not re-wrap it.
 */
#include "port/port_internal.h"

void native_post_blit_handler(M68KCtx *ctx) {
    ctx->A[1] = ctx->D[0];
    ctx->A[2] = r32(ctx->A[5] - 0x101Eu);
    ctx->A[4] = ctx->A[5] - 0x1020u;
    ctx->A[1] += 0xFA0u;

    uint16_t prev = r16(ctx->A[4]);
    uint16_t count = (uint16_t)(prev + 1u);
    w16(ctx->A[4], count);
    if (count != 0x10u) {
        /* NON-RESET: call $0052F0 immediately.  The fill blit completes
         * instantly on PC so the blitter-wait inside $0052F0 returns at once —
         * no stall occurs.  PUAE harness also runs the blit instantly (v=19
         * compositing in the same retro_run), so deferral is incorrect. */
        rt_call(ctx, ctx->image, 0x0052F0u);
        return;
    }

    w16(ctx->A[4], 0xFFFFu);
    ctx->A[2] += 0x14u;
    if (r8(ctx->A[2]) == 0) {
        ctx->A[2] = 0x442Eu;
        w8(ctx->A[5] - 0x1CB4u, 0xFFu);
    }

    w32(ctx->A[5] - 0x101Eu, ctx->A[2]);
    hw_write32(ctx->A[6] + 0x52u, ctx->A[1]);
    hw_write32(ctx->A[6] + 0x3Eu, 0x01000000u);
    hw_write16(ctx->A[6] + 0x64u, 0);
    hw_write16(ctx->A[6] + 0x56u, 0x54u);
}

/* The level-6 timer leaf $0055A0 has NO native owner, deliberately.
 *
 * It used to be wrapped here to control how often it ran, back when the host
 * called it directly. The guest's own vector wrapper ($003160) already does
 * `bsr $55A0` once per delivery — the same thing the reference product does —
 * so the wrapper was a second caller, and a harmful one: a raw call runs
 * without stopping at an RTE, and $55A0's chain ends at one ($005892). Past
 * that RTE the run carried straight on into the code the interrupt had
 * interrupted — a million instructions of the intro crawl inside one timer
 * interrupt, then a rollback of the lot. The music player, which is what the
 * interrupt is FOR, advanced once per twenty frames.
 *
 * Do not re-wrap it. A native owner for the timer must be entered as the
 * vector (so the delivery's own RTE boundary applies), not as a wrapper around
 * a routine the guest is already calling.
 *
 * Its palette-animation state at $0069F0-$006AE9 must also survive between
 * deliveries: $55A0 writes the CIA-B timer modulation tables there and the
 * next delivery reads them back. Clearing them made COLOR01-COLOR09 diverge
 * from the reference every frame. */
