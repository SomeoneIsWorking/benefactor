/* wait_idioms.c — fold the guest's hardware busy-waits into native bodies.
 *
 * Two idioms in this game spin on a hardware register that our host has
 * already satisfied, so the spin can never end on its own merits and only
 * burns time. The retired offline translator recognised them in the
 * disassembly and emitted a host call in their place; the interpreter
 * executes them literally instead. Do what the translator did, at image-load
 * time rather than offline: scan the loaded code for the exact encodings and
 * register a native override on the first instruction of each match. The
 * override performs the host wait and continues the guest past the idiom.
 *
 *   btst #6,(a6) ; bne.s self     the blitter BBUSY poll. Our blitter finishes
 *                                 inside the BLTSIZE write, so BBUSY is never
 *                                 seen set and the loop is pure overhead —
 *                                 overhead charged to whoever is running,
 *                                 which during the intro is the level-6 timer
 *                                 interrupt. That is what starved the music:
 *                                 one delivery held the beam for ~20 frames,
 *                                 so the player advanced once per 20 frames
 *                                 and channels 1-3 never got a period at all.
 *
 *   tst.b $BFE001 ; b(mi|pl) self the fire-button wait. Nothing on the game
 *                                 thread updates the button, so the literal
 *                                 spin never ends; hw_wait_fire yields a frame
 *                                 per check so input can arrive.
 *
 * The translator also folded the VPOSR frame wait (btst #0,$3(a6) spun until
 * V8 sets, then until it clears) into hw_vblank_wait(). That one is NOT folded
 * here, and the difference is the beam. The translator had no beam: its host
 * call WAS the frame clock. Ours derives the beam from consumed guest cycles,
 * so the guest's own spin is what carries the intro crawl from one frame to
 * the next — replace it and the crawl loses its only clock and runs ~165x too
 * fast (measured: the crawl ended after 38 displayed frames instead of 6290).
 */
#include "port/port_internal.h"

/* ── Encodings ───────────────────────────────────────────────────────────────
 * btst #6,(a6)                     0816 0006                         4 bytes
 * bne.s -6                         66FA                              2 bytes
 * tst.b $BFE001                    4A39 00BF E001                    6 bytes
 * bmi.s -8 / bpl.s -8              6BF8 | 6AF8                       2 bytes
 * ────────────────────────────────────────────────────────────────────────── */
#define BLTBUSY_LEN 6u  /* btst #6,(a6); bne  */
#define FIREWAIT_LEN 8u /* tst.b; b(mi|pl)    */

static uint16_t rd16(const uint8_t *m, uint32_t a) { return (uint16_t)((m[a] << 8) | m[a + 1]); }

/* The callers pass the whole two-byte branch word, displacement included, so
 * the displacement is read from the opcode and the memory word must equal it. */
static int is_self_branch(const uint8_t *m, uint32_t a, uint16_t opcode, uint32_t target) {
    if (rd16(m, a) != opcode)
        return 0;
    const int8_t disp = (int8_t)(opcode & 0xFFu);
    return (uint32_t)((int32_t)a + 2 + disp) == target;
}

/* Each override runs at the FIRST instruction of its idiom, so the guest PC is
 * the match address and the continuation is that plus the idiom's length.
 *
 * Read the PC BEFORE waiting, never after. A wait can park the game thread,
 * the host then delivers an interrupt on this same register file, and
 * rt_get_pc() comes back pointing wherever that interrupt finished — resuming
 * from it sent the guest into low memory and tripped the $150 loader hand-off. */
static void resume_past(M68KCtx *ctx, uint32_t pc, uint32_t length) {
    rt_jump(ctx, ctx->image, pc + length);
}

static void native_wait_blitter_busy(M68KCtx *ctx) {
    const uint32_t pc = rt_get_pc();
    hw_blitter_sync();
    resume_past(ctx, pc, BLTBUSY_LEN);
}

static void native_wait_fire_press(M68KCtx *ctx) {
    const uint32_t pc = rt_get_pc();
    hw_wait_fire(1);
    resume_past(ctx, pc, FIREWAIT_LEN);
}

static void native_wait_fire_release(M68KCtx *ctx) {
    const uint32_t pc = rt_get_pc();
    hw_wait_fire(0);
    resume_past(ctx, pc, FIREWAIT_LEN);
}

int pc_register_wait_idioms(uint32_t image_mask, uint32_t lo, uint32_t hi) {
    const uint8_t *m = g_mem;
    int found = 0;
    lo = (lo + 1u) & ~1u;
    if (hi < FIREWAIT_LEN)
        return 0;
    for (uint32_t a = lo; a + FIREWAIT_LEN <= hi; a += 2u) {
        /* btst #6,(a6) ; bne.s self — BBUSY poll, already satisfied. */
        if (rd16(m, a) == 0x0816u && rd16(m, a + 2u) == 0x0006u &&
            is_self_branch(m, a + 4u, 0x66FAu, a)) {
            rt_register_native(image_mask, a, native_wait_blitter_busy);
            found++;
            a += BLTBUSY_LEN - 2u;
            continue;
        }
        /* tst.b $BFE001 ; bmi/bpl self — CIA-A PRA bit 7 is /FIR1, active low,
         * so bmi loops while fire is UP (wait for a press) and bpl while it is
         * DOWN (wait for the release). */
        if (rd16(m, a) == 0x4A39u && rd16(m, a + 2u) == 0x00BFu && rd16(m, a + 4u) == 0xE001u) {
            if (is_self_branch(m, a + 6u, 0x6BF8u, a)) {
                rt_register_native(image_mask, a, native_wait_fire_press);
                found++;
                a += FIREWAIT_LEN - 2u;
            } else if (is_self_branch(m, a + 6u, 0x6AF8u, a)) {
                rt_register_native(image_mask, a, native_wait_fire_release);
                found++;
                a += FIREWAIT_LEN - 2u;
            }
        }
    }
    benefactor_log_write(
        BENEFACTOR_LOG_INFO, "override",
        "wait idioms: %d folded into native bodies in $%06X..$%06X (image mask %u)", found, lo, hi,
        image_mask);
    return found;
}

/* The code regions of a freshly loaded image: the decrunched main/overlay code
 * in chip RAM, and the gameplay overlay's fast-RAM bank at $577000. */
void pc_fold_wait_idioms(uint32_t image_mask) {
    (void)pc_register_wait_idioms(image_mask, 0x000400u, 0x080000u);
    (void)pc_register_wait_idioms(image_mask, 0x570000u, 0x600000u);
}
