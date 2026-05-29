#pragma once
/*
 * amiga/blitter.h  –  Blitter chip interface.
 *
 * The blitter performs fast memory-fill, copy, and logic operations
 * in chip RAM using four channels (A, B, C → D).
 */

#include <stdint.h>

/* Start a blit using the current register state (OCS BLTSIZE write). */
void blitter_start(const uint16_t *regs);

/* Start a blit using ECS BLTSIZV/BLTSIZH style. */
void blitter_start_ecs(const uint16_t *regs);
