/* src/port/overrides/render.c — Render pipeline hook overrides
 *
 * These are stable hook points in the render pipeline, currently delegating
 * to the original guest routines through the interpreter. Each wrapper can become
 * a binary-grounded native implementation independently.
 *
 * Hook addresses and their roles in the frame render sequence:
 *
 *   $00405C  native_text_sprite_render  — outer render entry; updates BPLPTRs
 *   $0040B6  native_dispatch_table      — render dispatch table entry
 *   $0040B8  native_item_dispatch_1     — dispatch item type 1
 *   $0040BA  native_item_dispatch_2     — dispatch item type 2
 *   $0040BC  native_item_dispatch_3     — dispatch item type 3
 *   $0040BE  native_item_decrement      — item counter decrement
 *   $0040CC  native_item_scroll         — scrolling item handler
 *   $004102  native_item_position       — item position update
 *   $00412E  native_item_blitter        — item blit operation
 *   $004236  native_blit_row_callback   — per-row blit callback (3 blits/row)
 *   $0052A4  native_post_blit_handler   — post-blit: animation advance, zero fill
 *   $0055A0  native_timer_interrupt     — CIA-B timer B: palette animation tick
 */
#include "port/port_internal.h"

void native_text_sprite_render(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x00405Cu); }
void native_dispatch_table(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x0040B6u); }
void native_item_dispatch_1(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x0040B8u); }
void native_item_dispatch_2(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x0040BAu); }
void native_item_dispatch_3(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x0040BCu); }
void native_item_decrement(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x0040BEu); }
void native_item_scroll(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x0040CCu); }
void native_item_position(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x004102u); }
void native_item_blitter(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x00412Eu); }
void native_blit_row_callback(M68KCtx *ctx) { rt_call_original(ctx, ctx->image, 0x004236u); }

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
        rt_call_original(ctx, ctx->image, 0x0052F0u);
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
 * so the wrapper was a second caller, and a harmful one: rt_call_original runs
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
