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

/* ── Functions provided by hw_audio.c ─────────────────────────────────────── */
void hw_audio_trigger(int ch);          /* AUDxDAT write (one-shot kick)        */
void hw_audio_dma_kick(int ch);         /* AUDxLEN write — start DMA stream     */
void hw_audio_resync(void);             /* restart all channels from registers  */
void hw_audio_callback(void *userdata, Uint8 *stream, int len);
int  hw_audio_open(void);
void hw_audio_close(void);

/* ── Functions provided by hw_copper.c ────────────────────────────────────── */
/* (hw_execute_copper and hw_render_frame are in hw.h) */
