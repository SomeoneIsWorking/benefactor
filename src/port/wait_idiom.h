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
    /* `btst #0,$5(a6)` / `beq` then the same with `bne`: hold until the beam
     * is past line 256, then hold until it has wrapped back above it. That
     * pair spans exactly one frame — the guest's own vertical-blank wait. */
    PC_WAIT_FRAME,
    /* Half of it. BELOW means "waiting for the beam to come DOWN past line
     * 256" (the branch loops while bit 8 is clear); ABOVE is the other half. */
    PC_WAIT_BEAM_BELOW,
    PC_WAIT_BEAM_ABOVE,
    /* `btst #6,(a6)` / `bne`: DMACONR bit 6, BBUSY. This port's blitter is
     * synchronous, so the bit is never set. */
    PC_WAIT_BLITTER,
} PcWaitIdiomKind;

typedef struct {
    PcWaitIdiomKind kind;
    uint32_t resume; /* the address after the loop (or loops) */
} PcWaitIdiom;

/* `btst #0,$5(a6)` — bit 8 of VPOSR ($DFF004), with a6 = $DFF000. */
#define PC_WAIT_OP_VPOSR_0 0x082Eu
#define PC_WAIT_OP_VPOSR_1 0x0000u
#define PC_WAIT_OP_VPOSR_2 0x0003u
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

/* Is there a poll on the beam's bit 8 at `at`? 1 = beq form, 2 = bne form. */
static inline int pc_wait_vposr_poll(const uint8_t *memory, uint32_t size, uint32_t at) {
    if (at + 8u > size)
        return 0;
    if (pc_wait_word(memory, at) != PC_WAIT_OP_VPOSR_0 ||
        pc_wait_word(memory, at + 2u) != PC_WAIT_OP_VPOSR_1 ||
        pc_wait_word(memory, at + 4u) != PC_WAIT_OP_VPOSR_2)
        return 0;
    return pc_wait_branch_back(memory, at + 6u, at);
}

/* Recognise the busy-wait that begins at `addr`, if there is one. */
static inline PcWaitIdiom pc_wait_idiom_at(const uint8_t *memory, uint32_t size, uint32_t addr) {
    PcWaitIdiom found = {PC_WAIT_NONE, 0u};
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
    if (addr + 6u <= size && pc_wait_word(memory, addr) == PC_WAIT_OP_BBUSY_0 &&
        pc_wait_word(memory, addr + 2u) == PC_WAIT_OP_BBUSY_1 &&
        pc_wait_branch_back(memory, addr + 4u, addr) == 2) {
        found.kind = PC_WAIT_BLITTER;
        found.resume = addr + 6u;
        return found;
    }
    return found;
}
