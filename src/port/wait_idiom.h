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
} PcWaitIdiomKind;

typedef struct {
    PcWaitIdiomKind kind;
    uint32_t resume; /* the address after the loop (or loops) */
    uint8_t scanline;
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

static inline uint32_t pc_wait_word(const uint8_t *memory, uint32_t at) {
    return (uint32_t)memory[at] << 8 | memory[at + 1u];
}

/* A `beq.s`/`bne.s` whose 8-bit displacement lands exactly back on `top`.
 * Returns 1 for beq, 2 for bne, 0 for anything else. */
static inline int pc_wait_branch_back(const uint8_t *memory, uint32_t at, uint32_t top) {
    const uint32_t op = pc_wait_word(memory, at);
    const uint32_t displacement = op & 0xFFu;
    if (displacement == 0u || displacement == 0xFFu)
        return 0; /* 16- or 32-bit displacement forms: not this shape */
    const int32_t offset = (int32_t)(int8_t)(uint8_t)displacement;
    if ((uint32_t)((int32_t)at + 2 + offset) != top)
        return 0;
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

/* Recognise the busy-wait that begins at `addr`, if there is one. */
static inline PcWaitIdiom pc_wait_idiom_at(const uint8_t *memory, uint32_t size, uint32_t addr) {
    PcWaitIdiom found = {PC_WAIT_NONE, 0u, 0u};
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
    return found;
}
