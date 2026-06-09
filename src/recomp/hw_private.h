/*
 * recomp/hw_private.h  –  Internal shared state for hw subsystem
 *
 * This header is ONLY for use by hw.c, hw_blitter.c, hw_audio.c,
 * hw_copper.c.  Do NOT include from outside the hw subsystem.
 */
#pragma once
#include <stdint.h>
#include <SDL2/SDL.h>
#include "hw.h"
#include "rt.h"
/* All hw register shadows (s_regs, s_dmacon/intena/intreq, s_bplcon0, s_bplptr,
 * s_sprpt, s_palette, s_diwstrt/stop, s_blt_bzero, the s_ciab_* timer state,
 * s_audio[]) and the AudioChannel typedef now live on g_state — see
 * game_state.h. */
#include "../game_state.h"

/* ── Framebuffer ───────────────────────────────────────────────────────────── */
extern uint32_t s_fb[HW_DISPLAY_W * HW_DISPLAY_H];

/* ── Copper write tag (set during copper MOVE execution) ────────────────────── */
extern int s_copper_writing;

extern SDL_AudioDeviceID s_audio_dev;
extern SDL_AudioSpec     s_audio_spec;

/* ── Colour conversion helper ──────────────────────────────────────────────── */
static inline uint32_t amiga_to_argb(uint16_t c)
{
    /* Match PUAE harness path configured as 16-bit (RGB565):
     * RGB12 -> RGB565 quantization -> RGB888 expansion. */
    uint8_t r4 = (uint8_t)((c >> 8) & 0xF);
    uint8_t g4 = (uint8_t)((c >> 4) & 0xF);
    uint8_t b4 = (uint8_t)(c & 0xF);

    uint8_t r5 = (uint8_t)((r4 << 1) | (r4 >> 3));
    uint8_t g6 = (uint8_t)((g4 << 2) | (g4 >> 2));
    uint8_t b5 = (uint8_t)((b4 << 1) | (b4 >> 3));

    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ── Functions provided by hw_blitter.c ───────────────────────────────────── */
void hw_do_blit(void);

/* Per-frame capture of OBJECT sprite blits (for the native widescreen renderer).
 * The engine draws every object as blitter copies into the playfield pages; we
 * capture those blits as the engine issues them, then re-draw the sprites natively
 * into the wide buffer (native_render_wide_bg). One record per plane-blit; a sprite
 * is 5 consecutive records whose dest steps by the plane stride. con0 bit USEB
 * ($0400) → cookie-cut (src=bpt, mask=apt); else opaque copy (src=apt). */
typedef struct {
    uint32_t src;     /* gfx source plane ptr (apt opaque / bpt masked)         */
    uint32_t mask;    /* mask plane ptr (masked only; 0 = opaque)               */
    uint32_t dpt;     /* dest plane ptr (in a playfield page)                   */
    int      w, h;    /* width in words, height in lines                        */
    int16_t  smod;    /* source-plane modulo (bytes)                            */
    int16_t  mmod;    /* mask modulo (bytes; masked only)                       */
    int      shift;   /* ASH fine x-shift (con0>>12)                            */
    uint16_t con0;
} BlitRec;
void           hw_blit_capture_reset(void);   /* clear, start a fresh frame      */
int            hw_blit_capture_count(void);
const BlitRec *hw_blit_capture_recs(void);

/* ── Functions provided by hw_audio.c ─────────────────────────────────────── */
void hw_audio_trigger(int ch);          /* AUDxDAT write (one-shot kick)        */
void hw_audio_dma_kick(int ch);         /* AUDxLEN write — start DMA stream     */
void hw_audio_resync(void);             /* restart all channels from registers  */
void hw_audio_callback(void *userdata, Uint8 *stream, int len);
int  hw_audio_open(void);
void hw_audio_close(void);
