/* Tests for the guest busy-wait recogniser (src/port/wait_idiom.h).
 *
 * The retired offline translator that used to play this game replaced the guest's
 * custom-register busy-waits with a host frame wait, so its frame boundaries
 * only ever landed at those points. The interpreter has to recognise the same
 * loops to land its boundaries in the same places. Getting the recognition
 * wrong in the permissive direction would hand a native wait to code that is
 * not a wait at all, so these tests are mostly about what is NOT an idiom.
 */
#include "port/wait_idiom.h"

#include <assert.h>
#include <string.h>

#define MEM_SIZE 0x200u
static uint8_t memory[MEM_SIZE];

static void put(uint32_t at, const char *bytes, uint32_t n) {
    memcpy(memory + at, bytes, n);
}

/* btst #0,$5(a6) — VPOSR bit 8, the beam past line 256 */
#define BTST_V8 "\x08\x2E\x00\x00\x00\x03"
/* btst #6,(a6) — DMACONR bit 6, BBUSY */
#define BTST_BBUSY "\x08\x16\x00\x06"
#define BEQ_BACK_8 "\x67\xF8"
#define BNE_BACK_8 "\x66\xF8"
#define BNE_BACK_6 "\x66\xFA"
/* `tst.b $BFE001` — CIA-A PRA, the joystick fire button. Measured at $5772D6 in
 * the decrunched level-1 gameplay image, whose loop body is the 8 bytes below
 * (the level-complete banner's "press fire to continue"). */
#define TST_CIA_A_PRA "\x4A\x39\x00\xBF\xE0\x01"
#define BMI_BACK_8 "\x6B\xF8"
#define BPL_BACK_8 "\x6A\xF8"

int main(void) {
    /* The full frame wait: hold until the beam is past line 256, then hold
     * until it has wrapped back above it. That pair IS one frame. */
    memset(memory, 0, sizeof memory);
    put(0x100, BTST_V8 BEQ_BACK_8 BTST_V8 BNE_BACK_8, 16);
    PcWaitIdiom found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_FRAME);
    assert(found.resume == 0x110);

    /* Half of it on its own is half a wait, and says so. */
    memset(memory, 0, sizeof memory);
    put(0x100, BTST_V8 BEQ_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_BEAM_BELOW);
    assert(found.resume == 0x108);

    memset(memory, 0, sizeof memory);
    put(0x100, BTST_V8 BNE_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_BEAM_ABOVE);

    /* The blitter poll. This port's blitter is synchronous, so BBUSY is never
     * set and the loop falls through on its first test anyway — recognising it
     * only saves the interpretation. Form 1 (a6=$DFF002) and Form 2 (a6=$DFF000). */
    memset(memory, 0, sizeof memory);
    put(0x100, BTST_BBUSY BNE_BACK_6, 6);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_BLITTER);
    assert(found.resume == 0x106);

    memset(memory, 0, sizeof memory);
    put(0x100, "\x08\x2E\x00\x06\x00\x02" BNE_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_BLITTER);
    assert(found.resume == 0x108);

    /* The gameplay scanline poll (cmpi.b #line,$6(a6); bne.s self). */
    memset(memory, 0, sizeof memory);
    put(0x100, "\x0C\x2E\x00\x3B\x00\x06" BNE_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_SCANLINE);
    assert(found.resume == 0x108);

    /* VPOSR poll with a6=$DFF000 (displacement $0005). */
    memset(memory, 0, sizeof memory);
    put(0x100, "\x08\x2E\x00\x00\x00\x05" BEQ_BACK_8 "\x08\x2E\x00\x00\x00\x05" BNE_BACK_8, 16);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_FRAME);
    assert(found.resume == 0x110);

    /* The fire wait, with its real bytes: hold the frame until the player
     * presses the button. `bmi` branches while the input bit reads high, i.e.
     * while fire is up, so this one waits for a press. */
    memset(memory, 0, sizeof memory);
    put(0x100, TST_CIA_A_PRA BMI_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_FIRE);
    assert(found.resume == 0x108);
    assert(found.wait_pressed == 1);

    /* The other polarity waits for the player to let go. */
    memset(memory, 0, sizeof memory);
    put(0x100, TST_CIA_A_PRA BPL_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_FIRE);
    assert(found.wait_pressed == 0);

    /* NOT idioms. */
    memset(memory, 0, sizeof memory);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* A branch that goes somewhere else is a loop over a BODY, not a poll. */
    memset(memory, 0, sizeof memory);
    put(0x100, BTST_V8 "\x67\xF6", 8); /* -10: targets before the btst */
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* A forward branch leaves the loop; the code after it is not ours. */
    memset(memory, 0, sizeof memory);
    put(0x100, BTST_V8 "\x67\x08", 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* An unconditional branch back is a hang, not a wait on the hardware. */
    memset(memory, 0, sizeof memory);
    put(0x100, BTST_V8 "\x60\xF8", 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* A poll on a register that is not the beam or the blitter. */
    memset(memory, 0, sizeof memory);
    put(0x100, "\x08\x2E\x00\x00\x00\x1F" BEQ_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* A joystick read followed by a branch that is not a bodyless loop is not
     * a fire wait: the input read itself is ordinary gameplay. */
    memset(memory, 0, sizeof memory);
    put(0x100, TST_CIA_A_PRA "\x6B\x08", 8); /* forward: the test is passed through */
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    memset(memory, 0, sizeof memory);
    put(0x100, TST_CIA_A_PRA "\x4E\x71" BMI_BACK_8, 10); /* a body between them */
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* A conditional loop over a byte test that is not this register: the value
     * is data, and waiting on it is not waiting for the player. */
    memset(memory, 0, sizeof memory);
    put(0x100, "\x4A\x39\x00\xDF\xF0\x00" BMI_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* `beq`/`bne` on the register byte would be testing whether the whole port
     * read is zero, which is not a button state and not this idiom. */
    memset(memory, 0, sizeof memory);
    put(0x100, TST_CIA_A_PRA BEQ_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, 0x100);
    assert(found.kind == PC_WAIT_NONE);

    /* The end of memory is not a licence to read past it. */
    memset(memory, 0, sizeof memory);
    put(MEM_SIZE - 8, BTST_V8 BEQ_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, MEM_SIZE - 8);
    assert(found.kind == PC_WAIT_BEAM_BELOW);
    found = pc_wait_idiom_at(memory, MEM_SIZE, MEM_SIZE - 4);
    assert(found.kind == PC_WAIT_NONE);

    /* The fire poll reads eight bytes and must not read past the end either. */
    memset(memory, 0, sizeof memory);
    put(MEM_SIZE - 8, TST_CIA_A_PRA BMI_BACK_8, 8);
    found = pc_wait_idiom_at(memory, MEM_SIZE, MEM_SIZE - 8);
    assert(found.kind == PC_WAIT_FIRE);
    found = pc_wait_idiom_at(memory, MEM_SIZE, MEM_SIZE - 6);
    assert(found.kind == PC_WAIT_NONE);

    return 0;
}
