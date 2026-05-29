#pragma once
/*
 * amiga/memory.h  –  Amiga 24-bit address-space layout and access helpers.
 *
 * Address map (OCS/ECS A500 model):
 *   000000–1FFFFF  Chip RAM (up to 2 MB, we allocate 2 MB always)
 *   BFD000–BFDFFF  CIA-B
 *   BFE001–BFEFFF  CIA-A (odd bytes)
 *   DFF000–DFFFFF  OCS/ECS custom chip registers
 *   F80000–FFFFFF  Kickstart ROM (not needed; trap accesses)
 */

#include <stdint.h>
#include <stddef.h>

#define CHIP_RAM_SIZE   (2 * 1024 * 1024)   /* 2 MB chip RAM */
#define CHIP_RAM_BASE   0x000000u
#define CHIP_RAM_END    0x1FFFFFu

#define CIAB_BASE       0xBFD000u
#define CIAB_END        0xBFDFFFu
#define CIAA_BASE       0xBFE001u   /* odd bytes only */
#define CIAA_END        0xBFEFFFu

#define CUSTOM_BASE     0xDFF000u
#define CUSTOM_END      0xDFFDFFu

#define ROM_BASE        0xF80000u
#define ROM_END         0xFFFFFFu

/* Initialise memory subsystem.  chip_ram_size bytes are allocated. */
void mem_init(void);
void mem_fini(void);

/* Bulk load bytes into chip RAM (used by disk loader). */
void mem_load(uint32_t addr, const uint8_t *src, uint32_t len);

/* Raw chip-RAM pointer for bulk operations (copper, blitter). */
uint8_t *mem_chip_ptr(uint32_t addr);

/* Generic bus read / write – used by Musashi callbacks. */
uint32_t mem_read8 (uint32_t addr);
uint32_t mem_read16(uint32_t addr);
uint32_t mem_read32(uint32_t addr);
void     mem_write8 (uint32_t addr, uint32_t val);
void     mem_write16(uint32_t addr, uint32_t val);
void     mem_write32(uint32_t addr, uint32_t val);
