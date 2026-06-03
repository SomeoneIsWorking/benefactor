/* pc_overrides_audio.c — native port of the gameplay AUDIO engine (staged).
 *
 * See instructions/audio-engine.md for the full RE map. The engine is a unified
 * music-replayer + SFX system; we port it in stages as native C we own, keeping the
 * recompiled bodies alive (A/B via dispatch) so each step is verifiable vs PUAE.
 *
 * Stage 1 (this file, so far): the SFX TRIGGER ($58656E). Faithful 1:1 translation.
 */
#include "pc_internal.h"

/* For confirming the override actually fires during gameplay (gpl bank). */
unsigned long g_native_sfx_trigger_hits = 0;

/* $58656E — request a sound effect.
 *
 * Entry: a3 = pointer to a sound DESCRIPTOR (in the $5Bxxxx sound-def table). Layout
 * (mirrors the active descriptor copied to $57fe50):
 *   +$0  long  sample read pointer (current chunk)
 *   +$4  word  period (pitch)
 *   +$6  word  volume   (also a priority tiebreak)
 *   +$8  word  length (words) of the streamed chunk
 *   +$10 word  priority
 *
 * Arbitration: if a sound is already active ($57fe4e != 0), only replace it when the
 * new priority is higher, or equal-priority with >= volume. On win: reset the
 * ping-pong buffer toggle, save the base sample ptr ($57fe78), copy the 20-byte
 * descriptor to the active slot ($57fe50), mark ch0 pending ($57fe4e=$FF), and clear
 * ch0 audio DMA so the streamer ($586612) restarts cleanly from the new sound.
 */
void native_sfx_trigger(M68KCtx *ctx)
{
    g_native_sfx_trigger_hits++;

    const uint32_t src = ctx->A[3];      /* &new sound descriptor             */
    const uint32_t cur = 0x57fe50u;      /* active descriptor slot            */
    const char *log = getenv("SFX_NATIVE_LOG");
    extern int hw_get_frame_num(void);

    if (MR8(0x57fe4e)) {                 /* a sound is currently playing      */
        uint16_t new_pri = MR16(src + 0x10);
        uint16_t cur_pri = MR16(cur + 0x10);
        if (new_pri < cur_pri ||         /* lower → keep / tie+quieter → keep */
            (new_pri == cur_pri && MR16(src + 6) < MR16(cur + 6))) {
            if (log) fprintf(stderr, "[native-sfx] f%d #%lu a3=$%06X pri=%u vol=%u "
                             "REJECTED (active pri=%u)\n", hw_get_frame_num(),
                             g_native_sfx_trigger_hits, (unsigned)src,
                             (unsigned)new_pri, (unsigned)MR16(src+6), (unsigned)cur_pri);
            return;
        }
    }
    if (log) fprintf(stderr, "[native-sfx] f%d #%lu a3=$%06X pri=%u vol=%u PLAY\n",
                     hw_get_frame_num(), g_native_sfx_trigger_hits, (unsigned)src,
                     (unsigned)MR16(src+0x10), (unsigned)MR16(src+6));

    MW16(0x586610u, 0);                  /* reset ping-pong buffer selector   */
    MW32(0x57fe78u, MR32(src));          /* stable base sample ptr            */
    for (int i = 0; i < 20; i++)         /* copy the 5-long descriptor        */
        MW8(cur + i, MR8(src + i));
    MW8(0x57fe4e, 0xFFu);                /* ch0 pending                       */
    MW16(0xdff096u, 0x0001u);            /* DMACON: clear ch0 DMA (restart)   */

    /* Own the rendering: hand the WHOLE sample to a native ch0 voice instead of
     * letting hw_audio chase the streamer's per-frame chunks (which truncated +
     * quieted SFX). Descriptor layout (mirrors $586612): +0 base ptr, +4 period,
     * +6 vol, +a chunk(longs)-1, +c chunk count, +e loop add, +12 loop chunks. */
    uint32_t base       = MR32(src + 0);
    int      period     = (int)MR16(src + 4);
    uint8_t  vol        = (uint8_t)MR16(src + 6);
    int      chunk_b    = ((int)MR16(src + 0xa) + 1) * 4;   /* bytes per chunk */
    int      chunks     = (int)MR16(src + 0xc);
    uint16_t loop_add   = MR16(src + 0xe);
    int      loop_chunks= (int)MR16(src + 0x12);
    int      total_b    = chunks * chunk_b;
    uint32_t loop_ptr   = loop_add ? base + loop_add : 0;
    int      loop_b     = loop_add ? loop_chunks * chunk_b : 0;
    hw_audio_sfx_play(0, base, total_b, period, vol, loop_ptr, loop_b);
}
