/*
 * recomp/hw_audio.c  –  Amiga OCS audio channel simulation
 *
 * Implements SDL2-backed audio mixing for the 4 Amiga audio DMA channels.
 * Audio is triggered by writes to AUDxDAT in hw.c's hw_write16().
 */

#include "hw_private.h"
#include <string.h>
#include <stdio.h>

/* OCS audio register offsets (local, relative to DFF000) */
#define _AUD0LCH  0x0A0
#define _AUD0LCL  0x0A2
#define _AUD0LEN  0x0A4
#define _AUD0PER  0x0A6
#define _AUD0VOL  0x0A8

/* State definitions (declared extern in hw_private.h, defined in hw.c) */

/* PAL Paula sample clock (Hz) and our output rate. The sample position advances
 * one byte every `period` Paula ticks; one output sample spans PAULA_CLOCK_PAL/
 * OUTPUT_RATE Paula ticks. Accumulate EXACTLY (integer) to avoid pitch drift:
 * per output sample tick += PAULA_CLOCK_PAL, advance while tick >= period*OUTPUT_RATE.
 * (Using a rounded 3546895/22050≈161 drifts ~0.09%, ~20 samples/s — audible desync.) */
#define PAULA_CLOCK_PAL 3546895
#define OUTPUT_RATE     22050

/* Live register reads for one channel (Paula reads these continuously). */
static inline uint32_t aud_lc(int ch)
{
    int b = _AUD0LCH + ch * 0x10;
    return (((uint32_t)s_regs[b >> 1] << 16) | s_regs[(b + 2) >> 1]) & 0xFFFFFF;
}
static inline uint16_t aud_len(int ch) { return s_regs[(_AUD0LCH + ch * 0x10 + 4) >> 1]; }
static inline uint16_t aud_per(int ch) { return s_regs[(_AUD0LCH + ch * 0x10 + 6) >> 1]; }
static inline uint8_t  aud_vol(int ch)
{
    uint8_t v = (uint8_t)(s_regs[(_AUD0LCH + ch * 0x10 + 8) >> 1] & 0x7F);
    return v > 64 ? 64 : v;
}

/* Benefactor's music driver uses DMA-streaming audio: it enables audio DMA once
 * (before our chip-RAM save-state was captured) and thereafter only updates
 * AUDxLC/LEN/PER/VOL — it never writes AUDxDAT.  So we model continuous Paula
 * DMA: a channel plays its current sample, and at each loop boundary reloads the
 * pointer/length from the *live* registers (which the driver keeps swapping to
 * stream the next sample).  Period and volume are read live every callback. */
static void hw_audio_mix(short *buf, int nsamples)
{
    static int s_only_ch = -2;   /* AUDCH_ONLY: render only this channel (debug) */
    if (s_only_ch == -2) { const char *e = getenv("AUDCH_ONLY"); s_only_ch = e ? atoi(e) : -1; }
    for (int ch = 0; ch < 4; ch++) {
        if (s_only_ch >= 0 && ch != s_only_ch) continue;
        AudioChannel *a = &s_audio[ch];
        if (!a->active) continue;

        uint8_t  vol    = aud_vol(ch);
        int      period = aud_per(ch) > 0 ? aud_per(ch) : 1;
        int      total  = (int)a->len * 2;   /* bytes in current sample */
        if (total <= 0) { a->active = 0; continue; }
        int64_t  step   = (int64_t)period * OUTPUT_RATE;

        for (int i = 0; i < nsamples; i++) {
            a->tick += PAULA_CLOCK_PAL;
            while (a->tick >= step) {
                a->tick -= step;
                a->pos++;
                if (a->pos >= total) {
                    /* Loop boundary: reload from live registers (DMA stream). */
                    a->ptr = aud_lc(ch);
                    a->len = aud_len(ch);
                    total  = (int)a->len * 2;
                    a->pos = 0;
                    if (total <= 0) { a->active = 0; break; }
                }
            }
            if (!a->active) break;
            uint32_t raddr = a->ptr + (uint32_t)a->pos;
            if (raddr >= RT_MEM_SIZE) { a->active = 0; break; }
            int8_t sample = (int8_t)g_mem[raddr];
            /* 8-bit signed (±128) * vol(0-64) = ±8192 per channel. Amiga stereo:
             * channels 0 and 3 are routed to the LEFT output, channels 1 and 2
             * to the RIGHT (Paula's fixed hardware panning). Two channels per
             * side sum to ±16384, well within int16. */
            int s = sample * vol;
            int idx = (ch == 0 || ch == 3) ? (i*2+0) : (i*2+1);
            int v = buf[idx] + s;
            if (v >  32767) v =  32767; else if (v < -32768) v = -32768;
            buf[idx] = (short)v;
        }
    }
    /* Stereo-separation blend, matching PUAE's default (sound_stereo_separation=7
     * => mul2=26, mul1=6, /32 — see vendor audio.c stereo_separation_mix). Paula
     * is hard-panned (ch0/3 left, ch1/2 right); without this blend a channel that
     * rests on one side makes that side drop out, while PUAE keeps it cushioned by
     * ~19% bleed from the other side (the gp title's left "break"). */
    for (int i = 0; i < nsamples; i++) {
        int l = buf[i*2+0], r = buf[i*2+1];
        buf[i*2+0] = (short)((l * 26 + r * 6) / 32);
        buf[i*2+1] = (short)((r * 26 + l * 6) / 32);
    }
}

void hw_audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    short *buf = (short *)stream;
    int nsamples = len / (int)sizeof(short) / 2;
    memset(buf, 0, len);
    hw_audio_mix(buf, nsamples);
}

/* Render nsamples of stereo PCM from the current channel state (for offline
 * capture/comparison, e.g. the harness). Same path as the SDL callback. */
void hw_audio_render(short *buf, int nsamples)
{
    memset(buf, 0, (size_t)nsamples * 2 * sizeof(short));
    hw_audio_mix(buf, nsamples);
}

/* Start (or restart) a channel from its current registers. */
static void hw_audio_start(int ch)
{
    AudioChannel *a = &s_audio[ch];
    a->ptr    = aud_lc(ch);
    a->start  = a->ptr;
    a->len    = aud_len(ch);
    a->period = aud_per(ch);
    a->vol    = aud_vol(ch);
    a->pos    = 0;
    a->tick   = 0;
    a->active = 1;
}

/* AUDxLEN write: the driver points the channel at a sample.  If the channel is
 * not already streaming, start it now; if it is, the new LC/LEN are picked up at
 * the next loop boundary (matching Paula's reload-on-completion behavior). */
void hw_audio_dma_kick(int ch)
{
    if (s_audio[ch].active) return;
    if (aud_len(ch) == 0) return;
    hw_audio_start(ch);
}

/* AUDxDAT write: explicit one-shot kick (rarely used by this game). */
void hw_audio_trigger(int ch)
{
    hw_audio_start(ch);
}

/* Restart all channels from the current register shadows. Used after the harness
 * seeds the Paula registers from PUAE's sync-point state, so each channel's live
 * playback pointer/length matches the seeded AUDxLC/LEN. Channels with LEN==0 are
 * left inactive (silent). */
void hw_audio_resync(void)
{
    for (int ch = 0; ch < 4; ch++) {
        if (aud_len(ch) == 0) { s_audio[ch].active = 0; continue; }
        hw_audio_start(ch);
    }
}

int hw_audio_open(void)
{
    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof(want));
    want.freq     = 22050;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 512;
    /* Queue (push) mode: the game loop renders each frame's audio itself and
     * pushes it via hw_audio_queue(), delivering the gp music ISR in sub-frame
     * chunks so the CIA-timer-driven song plays at full tempo (a pull callback
     * renders from the once-per-frame register state and plays it ~7x too slow).
     * The loop is paced to PAL 50Hz, so 441 samples/frame matches 22050Hz. */
    want.callback = NULL;
    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_audio_spec, 0);
    if (s_audio_dev) {
        SDL_PauseAudioDevice(s_audio_dev, 0);
        GLOBAL_LOG( "[audio] opened: %d Hz %d ch\n",
                s_audio_spec.freq, s_audio_spec.channels);
        return 0;
    }
    GLOBAL_LOG( "[audio] not available: %s\n", SDL_GetError());
    return -1;
}

/* Push `nframes` stereo samples to the audio device (queue mode). Drops the
 * frame if the queue is backing up badly (>4 frames) so we never lag the video. */
void hw_audio_queue(const short *buf, int nframes)
{
    if (!s_audio_dev) return;
    if (SDL_GetQueuedAudioSize(s_audio_dev) > (Uint32)(441 * 4 * 2 * (int)sizeof(short)))
        SDL_ClearQueuedAudio(s_audio_dev);
    SDL_QueueAudio(s_audio_dev, buf, (Uint32)(nframes * 2 * (int)sizeof(short)));
}

void hw_audio_close(void)
{
    if (s_audio_dev) {
        SDL_CloseAudioDevice(s_audio_dev);
        s_audio_dev = 0;
    }
}
