/* src/engine/gameplay_handoff.c — native reconstruction of the retail $150
 * loader body's low-memory initialisation (see gameplay_handoff.h). */
#include "engine/gameplay_handoff.h"

#include "engine/hw.h"
#include "runtime/guest_runtime.h" /* g_mem */

#include <stdint.h>

/* ── Guest low-memory the $150 body initialises ───────────────────────────── */
#define GP_DISPLAY_SENTINEL_PRIMARY 0x0000003Eu   /* long: display-list tail ptr  */
#define GP_DISPLAY_SENTINEL_SECONDARY 0x00000184u /* long: mirror of the above    */
#define GP_DISPLAY_SENTINEL_VALUE 0x00000A68u     /* fixed $150 immediate         */

#define GP_MODE_WORD 0x0000001Eu /* word: gameplay mode / difficulty */
#define GP_DIFFICULTY_EASY 0x0001u
#define GP_DIFFICULTY_NORMAL 0x0002u
#define GP_DIFFICULTY_HARD 0x0004u

/* ── Guest memory access (big-endian 68000 view of g_mem) ─────────────────── */
static uint16_t guest_read_word(uint32_t address) {
    return (uint16_t)(((uint16_t)g_mem[address] << 8) | g_mem[address + 1]);
}

static void guest_write_word(uint32_t address, uint16_t value) {
    g_mem[address] = (uint8_t)(value >> 8);
    g_mem[address + 1] = (uint8_t)value;
}

static void guest_write_long(uint32_t address, uint32_t value) {
    g_mem[address + 0] = (uint8_t)(value >> 24);
    g_mem[address + 1] = (uint8_t)(value >> 16);
    g_mem[address + 2] = (uint8_t)(value >> 8);
    g_mem[address + 3] = (uint8_t)value;
}

/* ── $150 loader body reconstruction ─────────────────────────────────────── */
static void write_card_display_sentinels(void) {
    guest_write_long(GP_DISPLAY_SENTINEL_PRIMARY, GP_DISPLAY_SENTINEL_VALUE);
    guest_write_long(GP_DISPLAY_SENTINEL_SECONDARY, GP_DISPLAY_SENTINEL_VALUE);
}

static int is_valid_difficulty(uint16_t mode_word) {
    return mode_word == GP_DIFFICULTY_EASY || mode_word == GP_DIFFICULTY_NORMAL ||
           mode_word == GP_DIFFICULTY_HARD;
}

/* Preserve a menu-selected difficulty; replace any other value (0, the attract
 * / game-over marker 8, or leftover garbage) with NORMAL — the same value the
 * engine's own inter-level path writes at $57720C. */
static void normalise_gameplay_mode_word(void) {
    if (!is_valid_difficulty(guest_read_word(GP_MODE_WORD)))
        guest_write_word(GP_MODE_WORD, GP_DIFFICULTY_NORMAL);
}

void gameplay_handoff_prepare_low_memory(void) {
    write_card_display_sentinels();
    normalise_gameplay_mode_word();
    /* Clear exception vectors and INTENA across overlay switch so stale
     * vectors from the previous screen cannot fire before the new overlay
     * sets up its own handlers. */
    guest_write_long(0x6Cu, 0u);
    guest_write_long(0x78u, 0u);
    hw_write16(0xDFF09Au, 0x7FFFu);
}
