#include "memory.h"
#include "custom.h"
#include "cia.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Chip RAM ────────────────────────────────────────────────────────────── */

static uint8_t *chip_ram;

void mem_init(void)
{
    chip_ram = (uint8_t *)calloc(1, CHIP_RAM_SIZE);
    if (!chip_ram) {
        fprintf(stderr, "mem_init: out of memory\n");
        exit(1);
    }
}

void mem_fini(void)
{
    free(chip_ram);
    chip_ram = NULL;
}

void mem_load(uint32_t addr, const uint8_t *src, uint32_t len)
{
    if (addr + len > CHIP_RAM_SIZE) {
        fprintf(stderr, "mem_load: address %08x + %u exceeds chip RAM\n", addr, len);
        len = CHIP_RAM_SIZE - addr;
    }
    memcpy(chip_ram + addr, src, len);
}

uint8_t *mem_chip_ptr(uint32_t addr)
{
    if (addr < CHIP_RAM_SIZE)
        return chip_ram + addr;
    return NULL;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8; p[1] = v & 0xFF;
}
static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF; p[3] = v & 0xFF;
}

/* ── Bus read ────────────────────────────────────────────────────────────── */

uint32_t mem_read8(uint32_t addr)
{
    addr &= 0xFFFFFF;   /* 24-bit bus */

    if (addr <= CHIP_RAM_END)
        return chip_ram[addr];

    /* CIA-A  (odd addresses, $BFE001–$BFEFFF) */
    if (addr >= CIAA_BASE && addr <= CIAA_END) {
        int reg = (addr >> 8) & 0xF;
        return ciaa_read(reg);
    }

    /* CIA-B  ($BFD000–$BFDFFF) */
    if (addr >= CIAB_BASE && addr <= CIAB_END) {
        int reg = (addr >> 8) & 0xF;
        return ciab_read(reg);
    }

    /* Custom chips ($DFF000–$DFFFFF) – 16-bit registers, byte read */
    if (addr >= CUSTOM_BASE && addr <= CUSTOM_END) {
        uint32_t reg = addr & 0x1FE;            /* round down to word */
        uint16_t val = custom_read(reg);
        return (addr & 1) ? (val & 0xFF) : (val >> 8);
    }

    /* ROM area – return open bus / 0xFF */
    if (addr >= ROM_BASE)
        return 0xFF;

    return 0;
}

uint32_t mem_read16(uint32_t addr)
{
    addr &= 0xFFFFFF;

    if (addr + 1 <= CHIP_RAM_END)
        return be16(chip_ram + addr);

    if (addr >= CIAA_BASE && addr <= CIAA_END)
        return ciaa_read((addr >> 8) & 0xF);

    if (addr >= CIAB_BASE && addr <= CIAB_END)
        return ciab_read((addr >> 8) & 0xF);

    if (addr >= CUSTOM_BASE && addr <= CUSTOM_END)
        return custom_read(addr & 0x1FE);

    if (addr >= ROM_BASE)
        return 0xFFFF;

    return 0;
}

uint32_t mem_read32(uint32_t addr)
{
    addr &= 0xFFFFFF;

    if (addr + 3 <= CHIP_RAM_END)
        return be32(chip_ram + addr);

    /* Fall back to two 16-bit reads */
    return ((uint32_t)mem_read16(addr) << 16) | mem_read16(addr + 2);
}

/* ── Bus write ───────────────────────────────────────────────────────────── */

void mem_write8(uint32_t addr, uint32_t val)
{
    addr &= 0xFFFFFF;

    if (addr <= CHIP_RAM_END) {
        chip_ram[addr] = (uint8_t)val;
        return;
    }

    if (addr >= CIAA_BASE && addr <= CIAA_END) {
        ciaa_write((addr >> 8) & 0xF, (uint8_t)val);
        return;
    }

    if (addr >= CIAB_BASE && addr <= CIAB_END) {
        ciab_write((addr >> 8) & 0xF, (uint8_t)val);
        return;
    }

    if (addr >= CUSTOM_BASE && addr <= CUSTOM_END) {
        uint32_t reg = addr & 0x1FE;
        uint16_t cur = custom_read(reg);
        if (addr & 1)
            custom_write(reg, (cur & 0xFF00) | (val & 0xFF));
        else
            custom_write(reg, (cur & 0x00FF) | ((val & 0xFF) << 8));
        return;
    }
    /* Writes to ROM area are silently ignored */
}

void mem_write16(uint32_t addr, uint32_t val)
{
    addr &= 0xFFFFFF;

    if (addr + 1 <= CHIP_RAM_END) {
        put_be16(chip_ram + addr, (uint16_t)val);
        return;
    }

    if (addr >= CIAA_BASE && addr <= CIAA_END) {
        ciaa_write((addr >> 8) & 0xF, (uint8_t)val);
        return;
    }

    if (addr >= CIAB_BASE && addr <= CIAB_END) {
        ciab_write((addr >> 8) & 0xF, (uint8_t)val);
        return;
    }

    if (addr >= CUSTOM_BASE && addr <= CUSTOM_END) {
        custom_write(addr & 0x1FE, (uint16_t)val);
        return;
    }
}

void mem_write32(uint32_t addr, uint32_t val)
{
    addr &= 0xFFFFFF;

    if (addr + 3 <= CHIP_RAM_END) {
        put_be32(chip_ram + addr, val);
        return;
    }

    mem_write16(addr,     val >> 16);
    mem_write16(addr + 2, val & 0xFFFF);
}
