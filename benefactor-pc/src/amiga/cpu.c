/*
 * amiga/cpu.c  –  Musashi 68000 integration.
 *
 * This translation unit OWNS all Musashi bus-callback symbols.
 * Do NOT include m68k.h in any other .c that also defines these.
 */

#include "cpu.h"
#include "memory.h"

/* Musashi public header – must come after our own headers */
#include "m68k.h"

#include <stdio.h>

/* ── Musashi required callbacks ──────────────────────────────────────────── */

unsigned int m68k_read_memory_8(unsigned int address)
{
    return mem_read8(address);
}
unsigned int m68k_read_memory_16(unsigned int address)
{
    return mem_read16(address);
}
unsigned int m68k_read_memory_32(unsigned int address)
{
    return mem_read32(address);
}

/* Disassembler callbacks (same as read, no side effects) */
unsigned int m68k_read_disassembler_8 (unsigned int address) { return mem_read8(address);  }
unsigned int m68k_read_disassembler_16(unsigned int address) { return mem_read16(address); }
unsigned int m68k_read_disassembler_32(unsigned int address) { return mem_read32(address); }

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    mem_write8(address, value);
}
void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    mem_write16(address, value);
}
void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    mem_write32(address, value);
}

/* ── Optional callbacks ──────────────────────────────────────────────────── */

/* Called on RESET instruction. */
void m68k_reset_instr_callback(void)
{
    /* Soft reset: re-read vectors from address 0. */
    uint32_t sp = mem_read32(0);
    uint32_t pc = mem_read32(4);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, pc);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void cpu_init(uint32_t initial_pc, uint32_t initial_sp)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();

    /* Override reset-vector values so the CPU starts at our entry point. */
    m68k_set_reg(M68K_REG_SP, initial_sp);
    m68k_set_reg(M68K_REG_PC, initial_pc);
}

int cpu_run(int cycles)
{
    return m68k_execute(cycles);
}

void cpu_irq(int level)
{
    m68k_set_irq(level);
}

uint32_t cpu_get_reg(unsigned int reg_id)
{
    return m68k_get_reg(NULL, (m68k_register_t)reg_id);
}

void cpu_set_reg(unsigned int reg_id, uint32_t val)
{
    m68k_set_reg((m68k_register_t)reg_id, val);
}

void cpu_reset(void)
{
    m68k_pulse_reset();
}
