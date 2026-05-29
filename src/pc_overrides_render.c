/* pc_overrides_render.c — Render pipeline hook overrides
 *
 * These are stable hook points in the render pipeline, currently delegating
 * to the recompiled generated code.  As the native port matures each wrapper
 * will be replaced with a full PC-native implementation.
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
#include "pc_internal.h"

void native_text_sprite_render(M68KCtx *ctx)
{
    rt_call_generated(ctx, 0x00405Cu);
}
void native_dispatch_table    (M68KCtx *ctx) { rt_call_generated(ctx, 0x0040B6u); }
void native_item_dispatch_1   (M68KCtx *ctx) { rt_call_generated(ctx, 0x0040B8u); }
void native_item_dispatch_2   (M68KCtx *ctx) { rt_call_generated(ctx, 0x0040BAu); }
void native_item_dispatch_3   (M68KCtx *ctx) { rt_call_generated(ctx, 0x0040BCu); }
void native_item_decrement    (M68KCtx *ctx) { rt_call_generated(ctx, 0x0040BEu); }
void native_item_scroll       (M68KCtx *ctx) { rt_call_generated(ctx, 0x0040CCu); }
void native_item_position     (M68KCtx *ctx) { rt_call_generated(ctx, 0x004102u); }
void native_item_blitter      (M68KCtx *ctx) { rt_call_generated(ctx, 0x00412Eu); }
void native_blit_row_callback(M68KCtx *ctx)
{
    rt_call_generated(ctx, 0x004236u);
}

void native_post_blit_handler(M68KCtx *ctx)
{
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
        rt_call_generated(ctx, 0x0052F0u);
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

void native_timer_interrupt(M68KCtx *ctx)
{
    /* Call $0055A0 once per frame to match PUAE's CIA-B timer rate.
     * Confirmed by harness: PUAE fires the timer interrupt once per VBL
     * (ctr sequence: 1→8→7→6 over 3 frames). Two calls per frame causes
     * the PC counter to run at double rate (reaching 3 by frame 3 vs 6 for
     * PUAE), diverging the animation table and copper list BPL pointers. */
    static int s_calls_per_frame = 1;
    static int s_trace = -1;
    if (s_trace < 0) s_trace = getenv("TIMER_TRACE") ? 1 : 0;
    for (int i = 0; i < s_calls_per_frame; i++) {
        if (s_trace) {
            uint16_t before = r16(0x0067D2u);
            uint8_t  d5before = r8(0x0067D5u);
            uint32_t copptr_before = r32(0x0069DCu);
            GLOBAL_LOG("[timer] call %d/%d  ctr=$%04X  d5=$%02X  copptr=$%08X  a5=$%06X a6=$%06X\n",
                       i+1, s_calls_per_frame, before, d5before, copptr_before, ctx->A[5], ctx->A[6]);
        }
        rt_call_generated(ctx, 0x0055A0u);
        if (s_trace) {
            uint16_t after = r16(0x0067D2u);
            uint32_t copptr_after = r32(0x0069DCu);
            GLOBAL_LOG("[timer] call %d/%d  ctr after=$%04X  copptr after=$%08X\n",
                       i+1, s_calls_per_frame, after, copptr_after);
        }
    }
    /* NOTE: do NOT zero $0069F0-$006AE9 here.  $0055A0 writes palette animation
     * state to this region each frame (CIA-B timer modulation tables).  Clearing
     * it after the call destroys the state that the NEXT frame's timer interrupt
     * reads to compute the correct palette modulation — causing COLOR01-COLOR09
     * to diverge from PUAE every frame. */
}
