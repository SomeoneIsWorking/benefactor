/* wait_idiom.h — recognising the guest's custom-register busy-wait loops.
 *
 * WHY THIS EXISTS. The retired offline translator — the oracle this port is
 * measured against, docs/oracle.md — played this game correctly, and one
 * reason is that it did not translate these loops at all: it replaced each
 * one with a host frame wait. So that product's frame
 * boundaries only ever landed where the GUEST asked to wait. The interpreter
 * executes the loops for real and takes its boundary wherever the frame's
 * cycle budget happens to run out — mid-blit, mid-setup, between two writes
 * that belong together. Every divergence measured against the reference in
 * docs/issues/0008 comes back to that one difference.
 *
 * So this port recognises the same loops and hands them to the host, which
 * puts the boundaries back where the working product had them. The patterns
 * are found by scanning the player's own decrunched image at run time
 * (src/port/overrides/wait_idioms.c) — nothing here comes from the retired
 * translator or anything it emitted.
 *
 * Header-only and free of engine state so it can be tested on its own
 * (tests/test_wait_idiom.c).
 *
 * A recognised loop is a POLL: two instructions, a read of one custom
 * register and a conditional branch straight back to that read. There is no
 * body, so nothing is skipped by not running it. Anything else — a branch
 * that lands somewhere other than the top, a forward branch, an unconditional
 * one — is a loop with a body and is left alone.
 */
#pragma once

#include <stdint.h>

typedef enum {
    PC_WAIT_NONE = 0,
    /* `btst #0,$3/$5(a6)` / `beq` then the same with `bne`: hold until the beam
     * is past line 256, then hold until it has wrapped back above it. That
     * pair spans exactly one frame — the guest's own vertical-blank wait. */
    PC_WAIT_FRAME,
    /* Half of it. BELOW means "waiting for the beam to come DOWN past line
     * 256" (the branch loops while bit 8 is clear); ABOVE is the other half. */
    PC_WAIT_BEAM_BELOW,
    PC_WAIT_BEAM_ABOVE,
    /* `cmpi.b #line,$6(a6)` / `bne`: VHPOSR scanline wait. The steady-gameplay
     * frame boundary in the main loop ($57712E) and level setup ($578472). */
    PC_WAIT_SCANLINE,
    /* `btst #6,(a6)` or `btst #6,$2(a6)` / `bne`: DMACONR bit 6, BBUSY. This port's
     * blitter is synchronous, so the bit is never set. */
    PC_WAIT_BLITTER,
    /* `tst.b $BFE001` / `bmi` (or `bpl`): CIA-A PRA bit 7, the joystick fire
     * button, active low. The guest is waiting for the PLAYER; this port only
     * latches input at a frame boundary, so the loop has to yield one. */
    PC_WAIT_FIRE,
} PcWaitIdiomKind;

typedef struct {
    PcWaitIdiomKind kind;
    uint32_t resume;      /* the address after the loop (or loops) */
    uint8_t scanline;     /* PC_WAIT_SCANLINE: the VHPOSR line polled for */
    uint8_t wait_pressed; /* PC_WAIT_FIRE: hold until fire is pressed (else released) */
} PcWaitIdiom;

/* The retail gameplay main loop's VHPOSR poll. The level-setup card uses a
 * different target ($3A), so only this title-owned poll can close the card to
 * gameplay display-frame handoff. */
#define PC_GAMEPLAY_MAIN_LOOP_SCANLINE 0x3Bu

/* `btst #0,$3(a6)` or `$5(a6)` — bit 8 of VPOSR ($DFF004). */
#define PC_WAIT_OP_VPOSR_0 0x082Eu
#define PC_WAIT_OP_VPOSR_1 0x0000u
/* `btst #6,(a6)` — bit 6 of DMACONR ($DFF002), with a6 = $DFF002. */
#define PC_WAIT_OP_BBUSY_0 0x0816u
#define PC_WAIT_OP_BBUSY_1 0x0006u
/* CIA-A PRA at $BFE001: bit6 = /FIR0, bit7 = /FIR1 (the joystick fire button),
 * both active low. Polled in a bodyless loop whenever a screen waits for the
 * player to press fire — the level-complete banner's "press fire to continue"
 * at $5772D6 (`tst.b $BFE001; bmi.s $5772D6`) among others. */
#define PC_WAIT_CIA_A_PRA 0xBFE001u

static inline uint32_t pc_wait_word(const uint8_t *memory, uint32_t at) {
    return (uint32_t)memory[at] << 8 | memory[at + 1u];
}

static inline uint32_t pc_wait_long(const uint8_t *memory, uint32_t at) {
    return (uint32_t)memory[at] << 24 | (uint32_t)memory[at + 1u] << 16 |
           (uint32_t)memory[at + 2u] << 8 | memory[at + 3u];
}

/* A short conditional branch: is the instruction at `at` one, and where does it
 * go? BRA is excluded (there is no condition for a caller to read) along with
 * the 0x00 and 0xFF displacement values, which continue into a longer encoding
 * rather than being the 8-bit form. */
static inline int pc_wait_branch_target(const uint8_t *memory, uint32_t at, uint32_t *target) {
    const uint32_t op = pc_wait_word(memory, at);
    if ((op & 0xF000u) != 0x6000u)
        return 0;
    if (((op >> 8) & 0x0Fu) == 0u)
        return 0; /* bra: unconditional */
    const uint32_t displacement = op & 0xFFu;
    if (displacement == 0u || displacement == 0xFFu)
        return 0; /* 16- or 32-bit displacement forms: not this shape */
    const int32_t offset = (int32_t)(int8_t)(uint8_t)displacement;
    *target = (uint32_t)((int32_t)at + 2 + offset);
    return 1;
}

/* A `beq.s`/`bne.s` whose 8-bit displacement lands exactly back on `top`.
 * Returns 1 for beq, 2 for bne, 0 for anything else. */
static inline int pc_wait_branch_back(const uint8_t *memory, uint32_t at, uint32_t top) {
    uint32_t target = 0u;
    if (!pc_wait_branch_target(memory, at, &target) || target != top)
        return 0;
    const uint32_t op = pc_wait_word(memory, at);
    if ((op & 0xFF00u) == 0x6700u)
        return 1; /* beq */
    if ((op & 0xFF00u) == 0x6600u)
        return 2; /* bne */
    return 0;
}

/* Is there a poll on the beam's bit 8 at `at`? 1 = beq form, 2 = bne form.
 * Accepts displacement $0003 (when a6=$DFF002) and $0005 (when a6=$DFF000). */
static inline int pc_wait_vposr_poll(const uint8_t *memory, uint32_t size, uint32_t at) {
    if (at + 8u > size)
        return 0;
    if (pc_wait_word(memory, at) != PC_WAIT_OP_VPOSR_0 ||
        pc_wait_word(memory, at + 2u) != PC_WAIT_OP_VPOSR_1)
        return 0;
    const uint32_t disp = pc_wait_word(memory, at + 4u);
    if (disp != 0x0003u && disp != 0x0005u)
        return 0;
    return pc_wait_branch_back(memory, at + 6u, at);
}

/* Is there a scanline poll `cmpi.b #line,$6(a6); bne self` at `at`? */
static inline int pc_wait_scanline_poll(const uint8_t *memory, uint32_t size, uint32_t at) {
    if (at + 8u > size)
        return 0;
    if (pc_wait_word(memory, at) != 0x0C2Eu)
        return 0;
    if ((pc_wait_word(memory, at + 2u) & 0xFF00u) != 0u)
        return 0;
    if (pc_wait_word(memory, at + 4u) != 0x0006u)
        return 0;
    return pc_wait_branch_back(memory, at + 6u, at) == 2;
}

/* Is there a blitter busy poll at `at`? Returns instruction length (6 or 8) or 0. */
static inline uint32_t pc_wait_blitter_poll(const uint8_t *memory, uint32_t size, uint32_t at) {
    /* Form 1 (length 6): btst #6,(a6) ; bne.s self (with a6 = $DFF002) */
    if (at + 6u <= size && pc_wait_word(memory, at) == PC_WAIT_OP_BBUSY_0 &&
        pc_wait_word(memory, at + 2u) == PC_WAIT_OP_BBUSY_1 &&
        pc_wait_branch_back(memory, at + 4u, at) == 2) {
        return 6u;
    }
    /* Form 2 (length 8): btst #6,$2(a6) ; bne.s self (with a6 = $DFF000) */
    if (at + 8u <= size && pc_wait_word(memory, at) == 0x082Eu &&
        pc_wait_word(memory, at + 2u) == 0x0006u && pc_wait_word(memory, at + 4u) == 0x0002u &&
        pc_wait_branch_back(memory, at + 6u, at) == 2) {
        return 8u;
    }
    return 0u;
}

/* Is there a joystick fire poll `tst.b $BFE001; bmi.s self` (or `bpl.s self`)
 * at `at`? Returns 0 for no, 1 when the loop holds until fire is pressed
 * (the branch is taken while the input bit reads high, i.e. while fire is up),
 * and 2 when it holds until fire is released.
 *
 * Both instructions are required: the test alone is an ordinary read of the
 * joystick, and a branch alone could be any conditional loop. `tst.b` tests the
 * whole byte, so bit 7 — the fire button — is the only thing the branch can be
 * reading; a `btst` on the register names a bit instead, and is deliberately
 * not accepted here. */
static inline int pc_wait_fire_poll(const uint8_t *memory, uint32_t size, uint32_t at) {
    if (at + 8u > size)
        return 0;
    if (pc_wait_word(memory, at) != 0x4A39u)
        return 0; /* tst.b <abs>.l */
    if (pc_wait_long(memory, at + 2u) != PC_WAIT_CIA_A_PRA)
        return 0;
    uint32_t target = 0u;
    if (!pc_wait_branch_target(memory, at + 6u, &target) || target != at)
        return 0;
    const uint32_t op = pc_wait_word(memory, at + 6u);
    if ((op & 0xFF00u) == 0x6B00u)
        return 1; /* bmi: branch while the bit is set, i.e. while fire is up */
    if ((op & 0xFF00u) == 0x6A00u)
        return 2; /* bpl: branch while the bit is clear, i.e. while fire is down */
    return 0;
}

/* Recognise the busy-wait that begins at `addr`, if there is one. */
static inline PcWaitIdiom pc_wait_idiom_at(const uint8_t *memory, uint32_t size, uint32_t addr) {
    PcWaitIdiom found = {PC_WAIT_NONE, 0u, 0u, 0u};
    const int beam = pc_wait_vposr_poll(memory, size, addr);
    if (beam != 0) {
        /* The pair is the thing the guest means by "wait for the next frame":
         * down past line 256 first, then back above it. Take both together so
         * the host's own frame wait stands in for the whole of it. */
        if (beam == 1 && pc_wait_vposr_poll(memory, size, addr + 8u) == 2) {
            found.kind = PC_WAIT_FRAME;
            found.resume = addr + 16u;
            return found;
        }
        found.kind = (beam == 1) ? PC_WAIT_BEAM_BELOW : PC_WAIT_BEAM_ABOVE;
        found.resume = addr + 8u;
        return found;
    }
    if (pc_wait_scanline_poll(memory, size, addr)) {
        found.kind = PC_WAIT_SCANLINE;
        found.resume = addr + 8u;
        found.scanline = memory[addr + 3u];
        return found;
    }
    const uint32_t blit_len = pc_wait_blitter_poll(memory, size, addr);
    if (blit_len != 0u) {
        found.kind = PC_WAIT_BLITTER;
        found.resume = addr + blit_len;
        return found;
    }
    const int fire = pc_wait_fire_poll(memory, size, addr);
    if (fire != 0) {
        found.kind = PC_WAIT_FIRE;
        found.resume = addr + 8u;
        found.wait_pressed = (uint8_t)(fire == 1);
        return found;
    }
    return found;
}
