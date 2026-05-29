#pragma once
/*
 * amiga/cpu.h  –  68000 CPU wrapper around the Musashi core.
 *
 * Musashi requires a single translation unit that defines the bus-access
 * callbacks (m68k_read_memory_*, m68k_write_memory_*).  We provide those
 * here and forward them to memory.c.
 */

#include <stdint.h>

/* Initialise the CPU, set PC and SP, then let the caller run. */
void cpu_init(uint32_t initial_pc, uint32_t initial_sp);

/* Execute up to cycles machine cycles; returns cycles actually consumed. */
int cpu_run(int cycles);

/* Raise an autovector interrupt at level 1–7. */
void cpu_irq(int level);

/* Read / write named registers (for debugging). */
uint32_t cpu_get_reg(unsigned int reg_id);  /* m68k_register_t values */
void     cpu_set_reg(unsigned int reg_id, uint32_t val);

/* Soft reset (RESET instruction). */
void cpu_reset(void);
