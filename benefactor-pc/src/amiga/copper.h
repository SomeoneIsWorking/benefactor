#pragma once
/*
 * amiga/copper.h  –  Copper coprocessor.
 *
 * The Copper executes a simple instruction list synchronised to the beam.
 * It can write to any custom chip register and wait for beam positions.
 */

#include <stdint.h>

void copper_set_lc(int n, uint32_t addr);   /* set copper list 1 or 2 */
void copper_jump(int n);                     /* COPJMP1 / COPJMP2 written */

/* Execute copper instructions for scanline `line`.
 * `regs` is the full custom-chip register shadow (512 × uint16_t). */
void copper_run_scanline(int line, uint16_t *regs);
