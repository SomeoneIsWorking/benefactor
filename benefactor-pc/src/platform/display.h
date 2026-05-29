#pragma once
/*
 * platform/display.h  –  SDL2 display output.
 *
 * The Amiga OCS display uses bitplanes (planar format) with a copper-
 * controlled palette.  We convert each frame to RGBA and blit it to
 * an SDL2 texture.
 *
 * Assumed display geometry (standard PAL low-res):
 *   320 × 256 pixels, up to 5 bitplanes (32 colours), or EHB (64 colours).
 */

#include <stdint.h>
#include "SDL2/SDL.h"

#define DISPLAY_W 320
#define DISPLAY_H 256

/* Create the SDL2 window and renderer. */
int  display_init(const char *title);

/* Render one scanline (Amiga line number 0..311) using current custom-chip
 * state.  Must be called once per scanline, in order, after the copper has
 * run for that line.  Call display_present() at end of frame (line 311). */
void display_render_scanline(int amiga_line);

/* Render one frame from current custom-chip state (legacy; use scanline API). */
void display_render_frame(void);

/* Swap (present) the completed frame. */
void display_present(void);

/* Destroy window / renderer. */
void display_fini(void);

/* Toggle full-screen. */
void display_toggle_fullscreen(void);
