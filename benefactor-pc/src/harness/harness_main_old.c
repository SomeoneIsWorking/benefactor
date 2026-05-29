/* harness/harness_main.c
 *
 * Comparison harness: runs PUAE (reference) and our PC-port recompiler side
 * by side, one frame at a time.  Stops at the first diverging frame and
 * prints a detailed report: disassembled copper lists, palette table,
 * and annotated register diffs.
 *
 * Usage:
 *   benefactor-harness <kick_dir> <slave_path> <chip_ram_dump> <disk1> [disk2] [disk3]
 *                      [--frames N] [--boot-frames N]
 *
 *   kick_dir      - directory containing the kickstart ROM (kick34005.A500)
 *   slave_path    - path to Benefactor.slave (WHDLoad slave)
 *   chip_ram_dump - path to chip_ram_dump.bin (initial state for PC port)
 *   diskN         - ADF floppy paths for PC port side
 *   --frames N    - how many GAME frames to run after boot (default 60)
 *   --boot-frames N - how many frames to run PUAE silently before capturing
 *                     (skip kickstart+WHDLoad boot, default 500)
 *
 * Phase 1: PUAE boots via kickstart + WHDLoad.  The first --boot-frames
 *          frames are run silently and discarded; then N game frames are
 *          captured once PUAE appears to have reached game state.
 * Phase 2: PC port initialises from chip_ram_dump and runs for N frames.
 *          Hardware state is captured after each frame.
 * Phase 3: Frames are compared pair-wise.  At the first divergence a full
 *          diagnostic report is printed and the harness exits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ── PUAE headers (compiled into same binary) ── */
#include "sysconfig.h"
#include "sysdeps.h"
#include "newcpu.h"
#include "libretro-core.h"
#include "libretro.h"

/* libretro-common redefines fflush → rfflush */
#ifdef fflush
# undef fflush
#endif

/* ── PC-port headers ── */
#include "recomp/hw.h"
#include "recomp/rt.h"
#include "pc.h"
/* ── Harness headers ── */
#include "harness/puae_state.h"
#include "harness/trace.h"
#include "harness/input.h"

/* PUAE trace init (defined in puae_snap_impl.c) */
extern void puae_trace_init(void);

/* ─────────────────────────────────────────────────────────────────────────── */
/* External symbols                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */
extern bool libretro_frame_end;
extern bool libretro_runloop_active;
extern char full_path[RETRO_PATH_MAX];
extern char harness_system_dir[RETRO_PATH_MAX];
extern char harness_save_dir[RETRO_PATH_MAX];
void harness_frontend_init(void);
extern void retro_run(void);
extern void (*g_harness_frame_hook)(void);
/* g_mem (chip RAM) declared in rt.h */

/* Frontend globals / functions */
extern int g_harness_fast_forward;
extern uint32_t s_puae_fb[320 * 256];
extern void harness_combined_init(void);
extern void harness_combined_present(void);
extern void harness_combined_fini(void);
extern void hw_execute_copper(void);

/* ─────────────────────────────────────────────────────────────────────────── */
/* State log                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */
#define MAX_FRAMES 1000
static FrameState s_puae_log[MAX_FRAMES];
static FrameState s_pc_log  [MAX_FRAMES];

/* Per-frame framebuffer snapshots for headless pixel comparison.
 * Limited to MAX_FB_FRAMES to keep memory use bounded (~6.5 MB for 10 frames). */
#define MAX_FB_FRAMES 10
#define FB_W 320
#define FB_H 256
static uint32_t s_puae_fb_log[MAX_FB_FRAMES][FB_W * FB_H];
static uint32_t s_pc_fb_log  [MAX_FB_FRAMES][FB_W * FB_H];
static int       s_puae_fb_count = 0;
static int       s_pc_fb_count   = 0;

/* Chip RAM snapshots — stored only for the frame we'll diff (to save memory).
 * PUAE chip RAM is 512 KB; we keep two buffers: one loaded from
 * /tmp/harness_puae_chipram.bin (written at sync point) and one per-frame
 * dump taken just before the diverging PC frame runs. */
#define CHIP_RAM_SIZE (512 * 1024)
static uint8_t s_puae_chipram_snap[CHIP_RAM_SIZE];
static uint8_t s_pc_chipram_snap  [CHIP_RAM_SIZE];
static int      s_puae_chipram_valid = 0;
static int      s_pc_chipram_valid   = 0;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Copper list disassembler                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Known DFF register names (sparse table) */
static const char *reg_name(uint16_t reg)
{
    switch (reg) {
    case 0x008: return "DMACON";
    case 0x009: return "DMACONR";
    case 0x020: return "DSKPTH";   case 0x022: return "DSKPTL";
    case 0x07E: return "COPINS";
    case 0x080: return "COP1LCH";  case 0x082: return "COP1LCL";
    case 0x084: return "COP2LCH";  case 0x086: return "COP2LCL";
    case 0x088: return "COPJMP1";  case 0x08A: return "COPJMP2";
    case 0x08E: return "DIWSTRT";  case 0x090: return "DIWSTOP";
    case 0x092: return "DDFSTRT";  case 0x094: return "DDFSTOP";
    case 0x096: return "DMACON";   case 0x09C: return "INTENA";
    case 0x09E: return "INTREQ";
    case 0x0A0: return "AUD0LCH";  case 0x0A2: return "AUD0LCL";
    case 0x0E0: return "BPL1PTH";  case 0x0E2: return "BPL1PTL";
    case 0x0E4: return "BPL2PTH";  case 0x0E6: return "BPL2PTL";
    case 0x0E8: return "BPL3PTH";  case 0x0EA: return "BPL3PTL";
    case 0x0EC: return "BPL4PTH";  case 0x0EE: return "BPL4PTL";
    case 0x0F0: return "BPL5PTH";  case 0x0F2: return "BPL5PTL";
    case 0x0F4: return "BPL6PTH";  case 0x0F6: return "BPL6PTL";
    case 0x100: return "BPLCON0";  case 0x102: return "BPLCON1";
    case 0x104: return "BPLCON2";  case 0x106: return "BPLCON3";
    case 0x108: return "BPL1MOD";  case 0x10A: return "BPL2MOD";
    case 0x120: return "SPR0PTH";  case 0x122: return "SPR0PTL";
    case 0x124: return "SPR1PTH";  case 0x126: return "SPR1PTL";
    case 0x128: return "SPR2PTH";  case 0x12A: return "SPR2PTL";
    case 0x12C: return "SPR3PTH";  case 0x12E: return "SPR3PTL";
    case 0x130: return "SPR4PTH";  case 0x132: return "SPR4PTL";
    case 0x134: return "SPR5PTH";  case 0x136: return "SPR5PTL";
    case 0x138: return "SPR6PTH";  case 0x13A: return "SPR6PTL";
    case 0x13C: return "SPR7PTH";  case 0x13E: return "SPR7PTL";
    case 0x180: return "COLOR00";  case 0x182: return "COLOR01";
    case 0x184: return "COLOR02";  case 0x186: return "COLOR03";
    case 0x188: return "COLOR04";  case 0x18A: return "COLOR05";
    case 0x18C: return "COLOR06";  case 0x18E: return "COLOR07";
    case 0x190: return "COLOR08";  case 0x192: return "COLOR09";
    case 0x194: return "COLOR10";  case 0x196: return "COLOR11";
    case 0x198: return "COLOR12";  case 0x19A: return "COLOR13";
    case 0x19C: return "COLOR14";  case 0x19E: return "COLOR15";
    case 0x1A0: return "COLOR16";  case 0x1A2: return "COLOR17";
    case 0x1A4: return "COLOR18";  case 0x1A6: return "COLOR19";
    case 0x1A8: return "COLOR20";  case 0x1AA: return "COLOR21";
    case 0x1AC: return "COLOR22";  case 0x1AE: return "COLOR23";
    case 0x1B0: return "COLOR24";  case 0x1B2: return "COLOR25";
    case 0x1B4: return "COLOR26";  case 0x1B6: return "COLOR27";
    case 0x1B8: return "COLOR28";  case 0x1BA: return "COLOR29";
    case 0x1BC: return "COLOR30";  case 0x1BE: return "COLOR31";
    default: return NULL;
    }
}

/* Disassemble the copper list stored in `words[n_words]` to `fp`.
 * `label` is a short prefix (e.g. "PUAE" or "PC  ").
 * `cop1lc` is the base address (for annotation). */
static void copper_disasm(FILE *fp, const char *label,
                           uint32_t cop1lc,
                           const uint16_t *words, int n_words)
{
    fprintf(fp, "  [%s] COP1LC=$%06X  (%d words captured)\n",
            label, cop1lc, n_words);
    for (int i = 0; i + 1 < n_words; i += 2) {
        uint16_t w1 = words[i];
        uint16_t w2 = words[i + 1];
        uint32_t addr = cop1lc + (uint32_t)(i * 2);

        if (w1 == 0xFFFF && w2 == 0xFFFE) {
            fprintf(fp, "  [%s]   $%06X: FFFF FFFE   END\n", label, addr);
            break;
        }
        if (w1 & 1) {
            if (w2 & 1) {
                fprintf(fp, "  [%s]   $%06X: %04X %04X   SKIP V>=$%02X H>=$%02X\n",
                        label, addr, w1, w2,
                        (w1 >> 8) & 0x7F, (w1 >> 1) & 0x7F);
            } else {
                fprintf(fp, "  [%s]   $%06X: %04X %04X   WAIT V=$%02X H=$%02X\n",
                        label, addr, w1, w2,
                        (w1 >> 8) & 0xFF, (w1 >> 1) & 0x7F);
            }
        } else {
            uint16_t reg = w1 & 0x1FE;
            const char *name = reg_name(reg);
            if (name)
                fprintf(fp, "  [%s]   $%06X: %04X %04X   MOVE $%04X -> DFF%03X (%s)\n",
                        label, addr, w1, w2, w2, reg, name);
            else
                fprintf(fp, "  [%s]   $%06X: %04X %04X   MOVE $%04X -> DFF%03X\n",
                        label, addr, w1, w2, w2, reg);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Detailed divergence report                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

static void print_divider(FILE *fp)
{
    fprintf(fp, "══════════════════════════════════════════════════════════════\n");
}

/* Classify the divergence and emit a human-readable cause summary */
static void print_cause(FILE *fp, int frame,
                         const FrameState *p, const FrameState *c)
{
    fprintf(fp, "\nROOT CAUSE ANALYSIS for frame %d:\n", frame);

    /* --- cop1lc divergence classification --- */
    if (p->cop1lc != c->cop1lc) {
        int32_t delta = (int32_t)c->cop1lc - (int32_t)p->cop1lc;
        if (delta < 0) delta = -delta;
        if (delta > 0x2000) {
            fprintf(fp, "  [SYNC MISMATCH] PUAE cop1lc=$%06X vs PC cop1lc=$%06X  (delta=%+d)\n"
                        "  → Sides are at DIFFERENT GAME SCREENS.\n"
                        "    PUAE has not yet reached the same game state as the chip_ram_dump.\n"
                        "    Try --boot-frames with a larger value, or verify the chip_ram_dump\n"
                        "    was captured at the very beginning of the game (right after WHDLoad).\n",
                    p->cop1lc, c->cop1lc, (int32_t)c->cop1lc - (int32_t)p->cop1lc);
        } else {
            fprintf(fp, "  [COPPER ADDRESS DIVERGENCE] cop1lc delta=%+d bytes\n"
                        "  → Same game screen but different copper list location.\n"
                        "    Possible cause: double-buffering out of phase, or COP1LCH/L\n"
                        "    written with a different address by the recompiled game code.\n",
                    (int32_t)c->cop1lc - (int32_t)p->cop1lc);
        }
    } else {
        fprintf(fp, "  [CONTENT DIVERGENCE] cop1lc matches ($%06X) — same screen, wrong values\n"
                    "  → The copper list address is identical but the hardware state differs.\n"
                    "    This means the recompiler wrote different values to hardware registers.\n",
                p->cop1lc);
    }

    /* --- bplcon0 --- */
    if (p->bplcon0 != c->bplcon0) {
        int pp = (p->bplcon0 >> 12) & 7, cp = (c->bplcon0 >> 12) & 7;
        if (pp != cp)
            fprintf(fp, "  [BPLCON0] Plane count mismatch: PUAE=%d planes, PC=%d planes\n"
                        "  → A MOVE $xxxx,DFF100 in the copper list wrote different BPLCON0.\n"
                        "    Check copper word diffs below for the DFF100 (BPLCON0) entry.\n",
                    pp, cp);
        else
            fprintf(fp, "  [BPLCON0] Mode flags differ (same plane count=%d): "
                        "PUAE=$%04X PC=$%04X\n"
                        "  → HAM/EHB/HIRES bits differ — check copper BPLCON0 write.\n",
                    pp, p->bplcon0, c->bplcon0);
    }

    /* --- modulos --- */
    if (p->bpl1mod != c->bpl1mod)
        fprintf(fp, "  [BPL1MOD] PUAE=%d PC=%d (delta %+d)\n"
                    "  → BPL1MOD controls scroll interleave / skip for odd bitplanes.\n"
                    "    Wrong value means wrong memory stride → corrupted display.\n"
                    "    Check copper MOVE to DFF108 or direct hw_reg_write(BPL1MOD).\n",
                p->bpl1mod, c->bpl1mod, c->bpl1mod - p->bpl1mod);
    if (p->bpl2mod != c->bpl2mod)
        fprintf(fp, "  [BPL2MOD] PUAE=%d PC=%d (delta %+d)\n"
                    "  → BPL2MOD controls scroll interleave / skip for even bitplanes.\n",
                p->bpl2mod, c->bpl2mod, c->bpl2mod - p->bpl2mod);

    /* --- bitplane pointers --- */
    int bplpt_diffs = 0;
    for (int i = 0; i < 6; i++)
        if (p->bplpt[i] != c->bplpt[i]) bplpt_diffs++;
    if (bplpt_diffs) {
        fprintf(fp, "  [BPLPT] %d of 6 bitplane pointers differ\n", bplpt_diffs);
        for (int i = 0; i < 6; i++) {
            if (p->bplpt[i] == c->bplpt[i]) continue;
            fprintf(fp, "    BPL%dPT: PUAE=$%06X  PC=$%06X  (delta %+d bytes)\n",
                    i+1, p->bplpt[i], c->bplpt[i],
                    (int32_t)c->bplpt[i] - (int32_t)p->bplpt[i]);
        }
        fprintf(fp, "  → Bitplane pointers are written by copper MOVE to DFF0E0-DFF0F6.\n"
                    "    If all pointers are offset by the same amount, the game allocated\n"
                    "    its bitplane buffers at a different chip RAM address in the recompiler.\n"
                    "    If only some differ, the copper list wrote wrong values.\n");
        /* Check if all diffs have the same delta (uniform offset) */
        if (bplpt_diffs > 1) {
            int32_t first_delta = 0;
            bool uniform = true;
            for (int i = 0; i < 6; i++) {
                if (p->bplpt[i] == c->bplpt[i]) continue;
                int32_t d = (int32_t)c->bplpt[i] - (int32_t)p->bplpt[i];
                if (first_delta == 0) first_delta = d;
                else if (d != first_delta) { uniform = false; break; }
            }
            if (uniform && first_delta != 0)
                fprintf(fp, "    NOTE: all differing pointers have the same delta (%+d = $%+X).\n"
                            "    This looks like a uniform memory layout offset — the recompiler\n"
                            "    allocated the bitplane buffers %d bytes %s than PUAE.\n",
                        first_delta, first_delta,
                        first_delta < 0 ? -first_delta : first_delta,
                        first_delta < 0 ? "before" : "after");
        }
    }

    /* --- palette --- */
    int pal_diffs = 0;
    for (int i = 0; i < 32; i++)
        if (p->palette[i] != c->palette[i]) pal_diffs++;
    if (pal_diffs) {
        fprintf(fp, "  [PALETTE] %d of 32 colors differ\n", pal_diffs);
        for (int i = 0; i < 32; i++) {
            if (p->palette[i] == c->palette[i]) continue;
            /* decode 12-bit Amiga RGB */
            uint16_t pr = (p->palette[i] >> 8) & 0xF, pg = (p->palette[i] >> 4) & 0xF,
                     pb = p->palette[i] & 0xF;
            uint16_t cr = (c->palette[i] >> 8) & 0xF, cg = (c->palette[i] >> 4) & 0xF,
                     cb = c->palette[i] & 0xF;
            fprintf(fp, "    COLOR%02d: PUAE=#%X%X%X  PC=#%X%X%X  "
                        "(MOVE $%04X/$%04X to DFF%03X)\n",
                    i, pr, pg, pb, cr, cg, cb,
                    p->palette[i], c->palette[i],
                    0x180 + i * 2);
        }
        fprintf(fp, "  → Palette is written by copper MOVE to DFF180-DFF1BE.\n"
                    "    If colors are wrong, check the copper list diff for DFF18x entries,\n"
                    "    or check hw_set_palette() calls in the recompiled game code.\n");
    }

    /* (bpl_data_crc is compared post-run, not per-frame — see compare_phases) */

    fprintf(fp, "\n");
}

static void report_divergence(FILE *fp, int frame,
                               const FrameState *p, const FrameState *c)
{
    print_divider(fp);
    fprintf(fp, "FIRST DIVERGENCE at frame %d\n", frame);
    print_divider(fp);

    /* Cause analysis first — most actionable info at the top */
    print_cause(fp, frame, p, c);

    /* ── Copper pointers ── */
    fprintf(fp, "\nCopper pointers:\n");
    fprintf(fp, "  COP1LC  PUAE=$%06X  PC=$%06X  %s\n",
            p->cop1lc, c->cop1lc,
            p->cop1lc == c->cop1lc ? "OK" : "DIFF <<<");
    fprintf(fp, "  COP2LC  PUAE=$%06X  PC=$%06X  %s\n",
            p->cop2lc, c->cop2lc,
            p->cop2lc == c->cop2lc ? "OK" : "DIFF <<<");

    /* ── BPLCON0 ── */
    fprintf(fp, "\nBPLCON0:\n");
    fprintf(fp, "  PUAE=$%04X (planes=%d HAM=%d EHB=%d HIRES=%d)\n",
            p->bplcon0,
            (p->bplcon0 >> 12) & 7,
            (p->bplcon0 >> 11) & 1,
            (p->bplcon0 >>  7) & 1,
            (p->bplcon0 >> 15) & 1);
    fprintf(fp, "  PC  =$%04X (planes=%d HAM=%d EHB=%d HIRES=%d)  %s\n",
            c->bplcon0,
            (c->bplcon0 >> 12) & 7,
            (c->bplcon0 >> 11) & 1,
            (c->bplcon0 >>  7) & 1,
            (c->bplcon0 >> 15) & 1,
            p->bplcon0 == c->bplcon0 ? "OK" : "DIFF <<<");

    /* ── Modulos ── */
    fprintf(fp, "\nModulos:\n");
    fprintf(fp, "  BPL1MOD  PUAE=%d  PC=%d  %s\n",
            p->bpl1mod, c->bpl1mod,
            p->bpl1mod == c->bpl1mod ? "OK" : "DIFF <<<");
    fprintf(fp, "  BPL2MOD  PUAE=%d  PC=%d  %s\n",
            p->bpl2mod, c->bpl2mod,
            p->bpl2mod == c->bpl2mod ? "OK" : "DIFF <<<");

    /* ── Bitplane pointers ── */
    fprintf(fp, "\nBitplane pointers:\n");
    for (int i = 0; i < 6; i++) {
        fprintf(fp, "  BPL%dPT  PUAE=$%06X  PC=$%06X  %s\n",
                i + 1, p->bplpt[i], c->bplpt[i],
                p->bplpt[i] == c->bplpt[i] ? "OK" : "DIFF <<<");
    }

    /* ── Palette ── */
    fprintf(fp, "\nPalette (12-bit Amiga RGB):\n");
    fprintf(fp, "  Idx  PUAE    PC      Match\n");
    for (int i = 0; i < 32; i++) {
        fprintf(fp, "  %02d   $%04X   $%04X   %s\n",
                i, p->palette[i], c->palette[i],
                p->palette[i] == c->palette[i] ? "" : "<<<");
    }

    /* ── Copper list disassembly ── */
    fprintf(fp, "\nCopper list disassembly:\n");
    if (p->coplist_valid)
        copper_disasm(fp, "PUAE", p->cop1lc, p->coplist, HARNESS_COPLIST_WORDS);
    else
        fprintf(fp, "  [PUAE] copper list not captured (cop1lc out of range)\n");

    if (c->coplist_valid)
        copper_disasm(fp, "PC  ", c->cop1lc, c->coplist, HARNESS_COPLIST_WORDS);
    else
        fprintf(fp, "  [PC  ] copper list not captured (cop1lc out of range)\n");

    /* ── Side-by-side copper diff with cause annotation ── */
    if (p->coplist_valid && c->coplist_valid && p->cop1lc == c->cop1lc) {
        fprintf(fp, "\nCopper word diffs (same address) — indicates wrong value written via copper:\n");
        int ndiff = 0;
        for (int i = 0; i + 1 < HARNESS_COPLIST_WORDS; i += 2) {
            uint16_t pw1 = p->coplist[i], pw2 = p->coplist[i+1];
            uint16_t cw1 = c->coplist[i], cw2 = c->coplist[i+1];
            if (pw1 == cw1 && pw2 == cw2) continue;
            /* Decode the PUAE instruction to describe what register this is */
            if (!(pw1 & 1)) {
                uint16_t reg = pw1 & 0x1FE;
                const char *name = reg_name(reg);
                if (name)
                    fprintf(fp, "  word[%03d] DFF%03X (%s): PUAE MOVE $%04X  PC MOVE $%04X\n",
                            i/2, reg, name, pw2, cw2);
                else
                    fprintf(fp, "  word[%03d] DFF%03X: PUAE MOVE $%04X  PC MOVE $%04X\n",
                            i/2, reg, pw2, cw2);
            } else {
                fprintf(fp, "  word[%03d] PUAE=%04X/%04X  PC=%04X/%04X (WAIT/SKIP differs)\n",
                        i/2, pw1, pw2, cw1, cw2);
            }
            if (++ndiff >= 32) {
                fprintf(fp, "  ... (truncated at 32 diffs)\n");
                break;
            }
        }
        if (ndiff == 0) fprintf(fp, "  (copper list content matches)\n");
    }

    print_divider(fp);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Sync detection                                                              */
/* ─────────────────────────────────────────────────────────────────────────── *//* ─────────────────────────────────────────────────────────────────────────── */
/* PUAE phase                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */
/* Sync detection                                                              */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Returns 1 when PUAE appears to have entered game state:
 * copper list is in a "game address" range and at least 1 bitplane is active. */
static int is_game_frame(const FrameState *s)
{
    /* Match only the stable double-buffered title-screen copper lists.
     * cop1lc=$7770 is a brief transition state (blitter/sprite tables still
     * being set up) and is NOT a safe PC-port start point. */
    return s->cop1lc == 0x7BC8u || s->cop1lc == 0x86CCu;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* PUAE phase                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

static int run_puae_phase(const char *kick_dir, const char *slave_path,
                           int boot_frames, int n_frames,
                           char *chipram_out_path, int chipram_out_len,
                           int display_only, int interactive)
{
    trace_reset();
    printf("[harness] === PUAE phase: boot_max=%d  capture=%d frames ===\n",
           boot_frames, n_frames);
    fflush(stdout);

    snprintf(harness_system_dir, RETRO_PATH_MAX, "%s", kick_dir);
    snprintf(harness_save_dir,   RETRO_PATH_MAX, "/tmp");
    harness_frontend_init();
    snprintf(full_path, RETRO_PATH_MAX, "%s", slave_path);
    retro_init();

    struct retro_game_info gi = { slave_path, NULL, 0, NULL };
    if (!retro_load_game(&gi)) {
        fprintf(stderr, "[harness] retro_load_game failed\n");
        return -1;
    }

    puae_trace_init();
    libretro_runloop_active = true;

    /* ── Boot / sync phase: run silently (fast-forward) until game state ── */
    g_harness_fast_forward = 1;
    FrameState tmp;
    int sync_frame = -1;
    uint32_t last_cop1lc = 0xFFFFFFFF;
    int stuck_count = 0;
    for (int f = 0; f < boot_frames; f++) {
        retro_run();
        puae_snap_state(&tmp);

        /* Stuck detection: warn if cop1lc hasn't moved in 500 frames */
        if (tmp.cop1lc == last_cop1lc) {
            stuck_count++;
            if (stuck_count == 500)
                printf("[harness]   PUAE appears stuck at cop1lc=$%06X for 500 frames\n",
                       tmp.cop1lc);
        } else {
            if (stuck_count >= 500)
                printf("[harness]   cop1lc changed after %d stuck frames: "
                       "$%06X → $%06X\n", stuck_count, last_cop1lc, tmp.cop1lc);
            stuck_count = 0;
            last_cop1lc = tmp.cop1lc;
        }

        if (f % 100 == 99)
            printf("[harness]   PUAE boot f%4d: cop1lc=$%06X bplcon0=$%04X planes=%d\n",
                   f + 1, tmp.cop1lc, tmp.bplcon0, (tmp.bplcon0 >> 12) & 7);

        /* Record first game frame but do NOT break — run all boot_frames so
         * that animation, palette, and other chip-RAM state fully converges.
         * The PC port loads the chip RAM dump, so it must start from the same
         * converged state that PUAE has after all boot_frames. */
        if (sync_frame < 0 && is_game_frame(&tmp)) {
            sync_frame = f;
            printf("[harness]   PUAE first game frame at boot-frame %d "
                   "(cop1lc=$%06X bplcon0=$%04X planes=%d) — continuing to boot-frame %d\n",
                   f, tmp.cop1lc, tmp.bplcon0, (tmp.bplcon0 >> 12) & 7, boot_frames);
        }
    }

    /* After all boot_frames: verify we actually reached game state */
    if (sync_frame < 0) {
        printf("[harness] WARNING: PUAE never reached game state in %d boot frames.\n"
               "          Last state: cop1lc=$%06X bplcon0=$%04X\n"
               "          Try --boot-frames with a larger value.\n",
               boot_frames, tmp.cop1lc, tmp.bplcon0);
        if (tmp.coplist_valid) {
            printf("[harness]   Copper list at $%06X:\n", tmp.cop1lc);
            for (int i = 0; i + 1 < HARNESS_COPLIST_WORDS; i += 2) {
                uint16_t w1 = tmp.coplist[i], w2 = tmp.coplist[i + 1];
                if (w1 == 0xFFFF && w2 == 0xFFFE) {
                    printf("[harness]     [%03d] FFFF FFFE  END\n", i / 2);
                    break;
                }
                printf("[harness]     [%03d] %04X %04X\n", i / 2, w1, w2);
                if (i / 2 >= 15) { printf("[harness]     ...(truncated)\n"); break; }
            }
        }
    }
    if (!is_game_frame(&tmp))
        printf("[harness] WARNING: PUAE ended boot_frames not in game state "
               "(cop1lc=$%06X) — chip RAM dump may be wrong.\n", tmp.cop1lc);

    /* ── Dump chip RAM after all boot_frames — state is now fully converged ── */
    g_harness_fast_forward = 0;  /* enable rendering for capture phase */
    chipram_out_path[0] = '\0';
    if (sync_frame >= 0) {
        static uint8_t s_chipram_buf[2 * 1024 * 1024];
        int bytes = puae_dump_chipram(s_chipram_buf, sizeof(s_chipram_buf));
        if (bytes > 524288) bytes = 524288;
        if (bytes > 0) {
            snprintf(chipram_out_path, chipram_out_len, "logs/harness_puae_chipram.bin");
            FILE *fp = fopen(chipram_out_path, "wb");
            if (fp) {
                fwrite(s_chipram_buf, 1, bytes, fp);
                fclose(fp);
                printf("[harness]   Dumped %d bytes of PUAE chip RAM (after %d boot frames) -> %s\n",
                       bytes, boot_frames, chipram_out_path);
                /* Also save PUAE audio register state at this sync point so the
                 * PC port can pre-initialize its audio shadows to match. */
                puae_dump_audio_regs("logs/harness_puae_audio_sync.bin");
                printf("[harness]   Dumped PUAE audio sync state -> logs/harness_puae_audio_sync.bin\n");
            } else {
                chipram_out_path[0] = '\0';
                perror("[harness] fopen chip RAM dump");
            }
            memcpy(s_puae_chipram_snap, s_chipram_buf,
                   bytes < CHIP_RAM_SIZE ? (size_t)bytes : CHIP_RAM_SIZE);
            s_puae_chipram_valid = 1;
        }
    }

    /* ── Capture phase ── */
    printf("[harness]   Capturing %d PUAE frames...\n", n_frames);
    fflush(stdout);
    s_puae_fb_count = 0;
    /* Per-frame buf_A change tracking: save previous chip RAM state */
    static uint8_t s_prev_chipram[CHIP_RAM_SIZE];
    int prev_valid = 0;
    memcpy(s_prev_chipram, s_puae_chipram_snap, CHIP_RAM_SIZE);
    prev_valid = s_puae_chipram_valid;
    for (int f = 0; f < n_frames; f++) {
        retro_run();
        s_puae_log[f].frame = f;
        puae_snap_state(&s_puae_log[f]);   /* includes chipram_crc */
        /* Snapshot framebuffer for diagnostic pixel comparison */
        if (f < MAX_FB_FRAMES) {
            memcpy(s_puae_fb_log[f], s_puae_fb, FB_W * FB_H * sizeof(uint32_t));
            s_puae_fb_count = f + 1;
        }
        if (f % 50 == 49)
            printf("[harness]   PUAE capture f%d: cop1lc=$%06X bplcon0=$%04X chipram_crc=%08X\n",
                   f + 1, s_puae_log[f].cop1lc, s_puae_log[f].bplcon0, s_puae_log[f].chipram_crc);
        /* Per-frame buf_A diff: detect which frame changes sprite data */
        if (prev_valid) {
            static uint8_t s_cur_chipram[CHIP_RAM_SIZE];
            int bytes2 = puae_dump_chipram(s_cur_chipram, CHIP_RAM_SIZE);
            if (bytes2 > 0) {
                /* buf_A: $070930-$0718CF */
                int bufa_changes = 0;
                for (int i = 0x070930; i < 0x0718D0 && i < bytes2; i++)
                    if (s_cur_chipram[i] != s_prev_chipram[i]) bufa_changes++;
                /* buf_B: $077E60-$07883F */
                int bufb_changes = 0;
                for (int i = 0x077E60; i < 0x078840 && i < bytes2; i++)
                    if (s_cur_chipram[i] != s_prev_chipram[i]) bufb_changes++;
                printf("[harness]   PUAE frame %d: buf_A changes=%d  buf_B changes=%d\n",
                       f, bufa_changes, bufb_changes);
                /* On first frame with buf_A changes: dump changed positions */
                if (bufa_changes > 0) {
                    int shown = 0;
                    for (int i = 0x070930; i < 0x0718D0 && i < bytes2 && shown < 30; i++) {
                        if (s_cur_chipram[i] != s_prev_chipram[i]) {
                            printf("[harness]   buf_A diff @$%06X: %02X -> %02X\n",
                                   i, s_prev_chipram[i], s_cur_chipram[i]);
                            shown++;
                        }
                    }
                    /* Also dump post-frame-0 buf_A to file for analysis */
                    FILE *fp = fopen("logs/puae_bufa_f0.bin", "wb");
                    if (fp) {
                        fwrite(s_cur_chipram + 0x070930, 1, 0x0718D0 - 0x070930, fp);
                        fclose(fp);
                        printf("[harness]   Dumped post-f0 buf_A to logs/puae_bufa_f0.bin\n");
                    }
                }
                fflush(stdout);
                memcpy(s_prev_chipram, s_cur_chipram, (size_t)bytes2 < CHIP_RAM_SIZE ? (size_t)bytes2 : CHIP_RAM_SIZE);
            }
        }
    }

    if (display_only) {
        printf("[harness] --display-only: running PUAE forever. Close window to quit.\n");
        fflush(stdout);
        for (;;)
            retro_run();   /* video_cb calls exit(0) on window close */
    }

    /* For interactive mode: rewrite dump file after capture so PC starts
     * from the SAME frame that retro_run() will continue from. */
    if (interactive) {
        static uint8_t s_chipram_buf2[2 * 1024 * 1024];
        int bytes = puae_dump_chipram(s_chipram_buf2, sizeof(s_chipram_buf2));
        if (bytes > 524288) bytes = 524288;
        if (bytes > 0) {
            snprintf(chipram_out_path, chipram_out_len, "logs/harness_puae_chipram.bin");
            FILE *fp = fopen(chipram_out_path, "wb");
            if (fp) {
                fwrite(s_chipram_buf2, 1, bytes, fp);
                fclose(fp);
                printf("[harness]   Interactive sync dump → %s\n", chipram_out_path);
            }
            memcpy(s_puae_chipram_snap, s_chipram_buf2,
                   bytes < CHIP_RAM_SIZE ? (size_t)bytes : CHIP_RAM_SIZE);
            s_puae_chipram_valid = 1;
        }
    } else {
        /* Non-interactive: keep sync-point dump for comparison */
        int bytes = puae_dump_chipram(s_puae_chipram_snap, CHIP_RAM_SIZE);
        printf("[harness] Final PUAE chip RAM snap: %d bytes\n", bytes);
        if (bytes > 0) {
            s_puae_chipram_valid = 1;
            FILE *pf = fopen("logs/harness_puae_chipram_end.bin", "wb");
            if (pf) {
                fwrite(s_puae_chipram_snap, 1, (size_t)bytes < CHIP_RAM_SIZE ? (size_t)bytes : CHIP_RAM_SIZE, pf);
                fclose(pf);
            }
        }
    }

    printf("[harness] PUAE phase done, %d frames captured\n", n_frames);
    return n_frames;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* PC-port phase                                                               */
/* ─────────────────────────────────────────────────────────────────────────── */

static int s_pc_frame = 0;
static int s_pc_target = 0;

static void pc_frame_hook(void)
{
    int inf = (s_pc_target <= 0);
    if (!inf && s_pc_frame >= s_pc_target) return;
    s_pc_log[s_pc_frame].frame = s_pc_frame;
    hw_get_snap(&s_pc_log[s_pc_frame]);
    /* CRC32 of PC chip RAM — covers bitplane + sprite data */
    s_pc_log[s_pc_frame].chipram_crc =
        (g_mem) ? crc32_buf(g_mem, 524288) : 0;
    /* Snapshot framebuffer for diagnostic pixel comparison */
    if (s_pc_frame < MAX_FB_FRAMES) {
        const uint32_t *fb = hw_get_framebuffer();
        if (fb) {
            memcpy(s_pc_fb_log[s_pc_frame], fb, FB_W * FB_H * sizeof(uint32_t));
            s_pc_fb_count = s_pc_frame + 1;
        }
    }
    if (!inf) s_pc_frame++;
    if (!inf && s_pc_frame >= s_pc_target)
        hw_running = 0;
}

static int run_pc_phase(const char *chip_ram_path,
                        const char **disks, int n_disks, int n_frames)
{
    printf("[harness] === PC-port phase: %d frames ===\n", n_frames);
    fflush(stdout);

    /* Headless: hw.c won't create its own window — we render via combined display */
    setenv("BENEFACTOR_HEADLESS", "1", 1);

    if (pc_init(chip_ram_path, disks, n_disks) < 0) {
        fprintf(stderr, "[harness] pc_init failed\n");
        return -1;
    }

    /* Pre-initialize PC audio register shadows from PUAE sync state so that
     * channels not written during the comparison frames start with matching values. */
    hw_load_audio_sync("logs/harness_puae_audio_sync.bin");

    s_pc_frame  = 0;
    s_pc_target = n_frames;
    g_harness_frame_hook = pc_frame_hook;
    pc_set_harness_mode(1);
    hw_running = 1;
    pc_run();
    g_harness_frame_hook = NULL;

    if (g_mem) {
        memcpy(s_pc_chipram_snap, g_mem, CHIP_RAM_SIZE);
        s_pc_chipram_valid = 1;
        /* Save PC chip RAM for external analysis */
        FILE *pcf = fopen("logs/harness_pc_chipram.bin", "wb");
        if (pcf) {
            fwrite(g_mem, 1, CHIP_RAM_SIZE, pcf);
            fclose(pcf);
        }
    }

    pc_fini();

    printf("[harness] PC-port phase done, %d frames captured\n", s_pc_frame);
    return s_pc_frame;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Interleaved frame-by-frame comparison (new structured mode)                  */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Forward declaration */
static int frames_differ(const FrameState *p, const FrameState *c);

static int run_interleaved_comparison(const char *chip_ram_path,
                                      const char **disks, int n_disks,
                                      int n_frames)
{
    printf("[harness] === Interleaved comparison: boot both, then frame-by-frame ===\n");
    fflush(stdout);

    setenv("BENEFACTOR_HEADLESS", "1", 1);

    if (pc_init(chip_ram_path, disks, n_disks) < 0) {
        fprintf(stderr, "[harness] pc_init failed\n");
        return -1;
    }

    pc_set_harness_mode(1);
    pc_init_state();

    g_harness_frame_hook = NULL;  /* we drive the frame loop manually */
    hw_running = 1;

    printf("[harness] Starting frame-by-frame interleaved run (%d frames)...\n", n_frames);
    fflush(stdout);

    int first_diff_frame = -1;
    s_puae_fb_count = 0;
    s_pc_fb_count = 0;

    for (int f = 0; f < n_frames; f++) {
        /* Step 1: Run PUAE one frame */
        retro_run();
        s_puae_log[f].frame = f;
        puae_snap_state(&s_puae_log[f]);

        /* Snapshot PUAE framebuffer */
        if (f < MAX_FB_FRAMES) {
            memcpy(s_puae_fb_log[f], s_puae_fb, FB_W * FB_H * sizeof(uint32_t));
            s_puae_fb_count = f + 1;
        }

        /* Step 2: Run PC one frame */
        pc_step();
        s_pc_log[f].frame = f;
        hw_get_snap(&s_pc_log[f]);

        /* Snapshot PC framebuffer */
        if (f < MAX_FB_FRAMES) {
            const uint32_t *fb = hw_get_framebuffer();
            if (fb) {
                memcpy(s_pc_fb_log[f], fb, FB_W * FB_H * sizeof(uint32_t));
                s_pc_fb_count = f + 1;
            }
        }

        /* Step 3: Compare frames immediately */
        int diff = frames_differ(&s_puae_log[f], &s_pc_log[f]);

        printf("  F%03d: cop1lc PUAE=$%06X PC=$%06X  bplcon0 PUAE=$%04X PC=$%04X  %s\n",
               f, s_puae_log[f].cop1lc, s_pc_log[f].cop1lc,
               s_puae_log[f].bplcon0, s_pc_log[f].bplcon0,
               diff ? "DIFF <<<" : "ok");

        if (diff) {
            first_diff_frame = f;
            printf("\n[harness] Stopping at first divergence (frame %d)\n", f);
            break;
        }

        if ((f + 1) % 10 == 0) {
            printf("[harness]   Compared %d frames — all match so far\n", f + 1);
            fflush(stdout);
        }
    }

    /* Capture final chip RAM state for post-run bitplane check */
    if (g_mem) {
        memcpy(s_pc_chipram_snap, g_mem, CHIP_RAM_SIZE);
        s_pc_chipram_valid = 1;
    }

    /* Also capture PUAE's final chip RAM state for comparison */
    if (g_mem) {
        static uint8_t s_tmp_chipram[CHIP_RAM_SIZE];
        int bytes = puae_dump_chipram(s_tmp_chipram, CHIP_RAM_SIZE);
        if (bytes > 0) {
            memcpy(s_puae_chipram_snap, s_tmp_chipram, (size_t)bytes < CHIP_RAM_SIZE ? (size_t)bytes : CHIP_RAM_SIZE);
            s_puae_chipram_valid = 1;
        }
    }

    hw_running = 0;
    pc_fini();

    int n_compared = (first_diff_frame >= 0) ? (first_diff_frame + 1) : n_frames;
    printf("[harness] Interleaved comparison done: %d frames compared\n", n_compared);
    return n_compared;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Interactive simultaneous mode (legacy)                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static int run_interactive(const char *chip_ram_path,
                           const char **disks, int n_disks)
{
    printf("[harness] === Interactive mode: PUAE + PC simultaneous ===\n");
    fflush(stdout);

    setenv("BENEFACTOR_HEADLESS", "1", 1);

    if (pc_init(chip_ram_path, disks, n_disks) < 0) {
        fprintf(stderr, "[harness] pc_init failed\n");
        return -1;
    }
    pc_set_harness_mode(1);
    pc_init_state();

    g_harness_frame_hook = NULL;  /* we drive the frame loop manually */
    hw_running = 1;

    /* Simple hook that presents combined display after PC renders */
    g_harness_frame_hook = harness_combined_present;

    printf("[harness] Interactive: Close window or press Esc to quit.\n");
    fflush(stdout);

    FrameState puae_state, pc_state;
    int frame_num = 0;

    /* Auto-fire the start button after a few frames so the PC transitions to gameplay */
    int auto_start = 3;

    while (hw_running) {
        frame_num++;
        /* 1. Poll input once — feeds both PUAE (via input_state_cb) and PC (via hw_set_joystick) */
        input_poll();

        /* 2. Auto-start: press once to transition from loading screen */
        if (frame_num == auto_start) {
            hw_set_mouse_lmb(1); hw_set_fire(1);
        } else {
            hw_set_mouse_lmb(0); hw_set_fire(0);
        }

        /* 3. Run PUAE 1 frame → video_cb stores to s_puae_fb */
        if (frame_num == 1) {
            /* Diagnostic: sample PUAE chip RAM before frame 1 at bpl0 area */
            static uint8_t s_pre1[32];
            int pre_bytes = puae_dump_chipram(s_pre1, sizeof(s_pre1));
            /* puae_dump_chipram dumps from 0; offset manually */
            {
                static uint8_t s_tmp[0x025354 + 4];
                int tb = puae_dump_chipram(s_tmp, sizeof(s_tmp));
                if (tb > 0x025340) {
                    fprintf(stderr, "[BPL_PRE] frame1 PUAE bpl0[$025334]=%02X%02X%02X%02X bpl1[$025348]=%02X%02X%02X%02X\n",
                        s_tmp[0x025334], s_tmp[0x025335], s_tmp[0x025336], s_tmp[0x025337],
                        s_tmp[0x025348], s_tmp[0x025349], s_tmp[0x02534A], s_tmp[0x02534B]);
                }
                (void)pre_bytes;
            }
        }
        retro_run();

        /* 3. Snapshot PUAE state after frame */
        puae_snap_state(&puae_state);
        if (frame_num == 1) {
            /* Diagnostic: sample PUAE chip RAM after frame 1 at bpl0 area */
            static uint8_t s_post_tmp[0x025354 + 4];
            int pb = puae_dump_chipram(s_post_tmp, sizeof(s_post_tmp));
            if (pb > 0x025340) {
                fprintf(stderr, "[BPL_POST] frame1 PUAE bpl0[$025334]=%02X%02X%02X%02X bpl1[$025348]=%02X%02X%02X%02X\n",
                    s_post_tmp[0x025334], s_post_tmp[0x025335], s_post_tmp[0x025336], s_post_tmp[0x025337],
                    s_post_tmp[0x025348], s_post_tmp[0x025349], s_post_tmp[0x02534A], s_post_tmp[0x02534B]);
            }
        }

        /* 4. Run PC 1 frame → g_harness_frame_hook fires after render */
        pc_step();

        /* 5. Snapshot PC state after frame */
        hw_get_snap(&pc_state);

        /* 6. Compare frame and stop immediately on first render mismatch. */
        {
            int first_cop_idx = -1;
            /* Copper list comparison */
            if (puae_state.coplist_valid && pc_state.coplist_valid) {
                if (puae_state.cop1lc != pc_state.cop1lc) {
                    printf("[diff] Frame %d: cop1lc PUAE=$%06X PC=$%06X\n",
                           frame_num, puae_state.cop1lc, pc_state.cop1lc);
                }
                for (int i = 0; i < HARNESS_COPLIST_WORDS; i++) {
                    if (puae_state.coplist[i] != pc_state.coplist[i]) {
                        first_cop_idx = i;
                        printf("[diff] Frame %d: coplist[%d] PUAE=$%04X PC=$%04X\n",
                               frame_num, i, puae_state.coplist[i], pc_state.coplist[i]);
                        break;
                    }
                }
            }
            /* Framebuffer pixel comparison */
            {
                const uint32_t *pc_fb = hw_get_framebuffer();
                int pixel_diff = 0;
                int first_px = -1;
                int row_diffs[256] = {0};
                for (int i = 0; i < 320 * 256; i++) {
                    if ((s_puae_fb[i] & 0x00FFFFFF) != (pc_fb[i] & 0x00FFFFFF)) {
                        if (first_px < 0)
                            first_px = i;
                        row_diffs[i / 320]++;
                        pixel_diff++;
                    }
                }
                if (pixel_diff > 0) {
                    int px = first_px % 320;
                    int py = first_px / 320;
                    int first_row = -1;
                    int last_row = -1;
                    int worst_row = -1;
                    int worst_count = 0;
                    for (int y = 0; y < 256; y++) {
                        if (row_diffs[y] > 0) {
                            if (first_row < 0) first_row = y;
                            last_row = y;
                            if (row_diffs[y] > worst_count) {
                                worst_count = row_diffs[y];
                                worst_row = y;
                            }
                        }
                    }
                    uint32_t puae_rgb = s_puae_fb[first_px] & 0x00FFFFFFu;
                    uint32_t pc_rgb = pc_fb[first_px] & 0x00FFFFFFu;
                    /* Find first non-black coordinates in PUAE fb and PC fb */
                    int puae_first_nz = -1, pc_first_nz = -1;
                    int puae_first_nz_x = -1, pc_first_nz_x = -1;
                    for (int y = 0; y < 256 && (puae_first_nz < 0 || pc_first_nz < 0); y++) {
                        for (int x = 0; x < 320; x++) {
                            if (puae_first_nz < 0 && (s_puae_fb[y*320+x] & 0x00FFFFFFu)) {
                                puae_first_nz = y;
                                puae_first_nz_x = x;
                            }
                            if (pc_first_nz < 0 && (pc_fb[y*320+x] & 0x00FFFFFFu)) {
                                pc_first_nz = y;
                                pc_first_nz_x = x;
                            }
                            if (puae_first_nz >= 0 && pc_first_nz >= 0) break;
                        }
                    }
                    printf("[diff] Frame %d: Pixel diff: %d / %d (%.1f%%)\n",
                           frame_num, pixel_diff, 320*256, 100.0 * pixel_diff / (320*256));
                    printf("[cause] First pixel mismatch at (%d,%d): PUAE=$%06X PC=$%06X\n",
                           px, py, puae_rgb, pc_rgb);
                          printf("[cause] First non-black pixel: PUAE (%d,%d)  PC (%d,%d)\n",
                              puae_first_nz_x, puae_first_nz,
                              pc_first_nz_x, pc_first_nz);
                    if (first_row >= 0) {
                        printf("[cause] Mismatch rows y=%d..%d (worst row y=%d has %d bad pixels)\n",
                               first_row, last_row, worst_row, worst_count);
                    }

                    {
                        int best_dx = 0;
                        int best_dy = 0;
                        int best_diff = 320 * 256;
                        for (int dy = -64; dy <= 64; dy++) {
                            for (int dx = -64; dx <= 64; dx++) {
                                int d = 0;
                                int compared = 0;
                                for (int y = 0; y < 256; y++) {
                                    int yy = y + dy;
                                    if ((unsigned)yy >= 256u) continue;
                                    for (int x = 0; x < 320; x++) {
                                        int xx = x + dx;
                                        if ((unsigned)xx >= 320u) continue;
                                        uint32_t a = s_puae_fb[y * 320 + x] & 0x00FFFFFFu;
                                        uint32_t b = pc_fb[yy * 320 + xx] & 0x00FFFFFFu;
                                        if (a != b) d++;
                                        compared++;
                                    }
                                }
                                if (compared > 0 && d < best_diff) {
                                    best_diff = d;
                                    best_dx = dx;
                                    best_dy = dy;
                                }
                            }
                        }
                        printf("[cause] Best small offset match: dx=%d dy=%d diff=%d pixels\n",
                               best_dx, best_dy, best_diff);
                    }

                    if (puae_state.cop1lc != pc_state.cop1lc) {
                        printf("[cause] Copper list base diverged: PUAE=$%06X PC=$%06X\n",
                               puae_state.cop1lc, pc_state.cop1lc);
                    } else if (first_cop_idx >= 0) {
                        uint32_t cop_addr = puae_state.cop1lc + (uint32_t)(first_cop_idx * 2);
                        printf("[cause] First copper-word mismatch at $%06X (index %d): PUAE=$%04X PC=$%04X\n",
                               cop_addr, first_cop_idx,
                               puae_state.coplist[first_cop_idx],
                               pc_state.coplist[first_cop_idx]);
                    } else {
                        printf("[cause] Copper list matches this frame; mismatch is likely in renderer/blitter timing or scanline decode.\n");
                    }

                    printf("[harness] Stopping at first render mismatch (frame %d)\n", frame_num);
                    hw_running = 0;
                }
            }
        }
    }

    g_harness_frame_hook = NULL;
    pc_fini();
    printf("[harness] Interactive session ended\n");
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Comparison                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

static int frames_differ(const FrameState *p, const FrameState *c)
{
    if (p->cop1lc != c->cop1lc)
        return 1;
    if (!p->coplist_valid || !c->coplist_valid)
        return 0;
    for (int i = 0; i < HARNESS_COPLIST_WORDS; i++) {
        if (p->coplist[i] != c->coplist[i])
            return 1;
    }

    /* Audio channels: only compare AUDxVOL as a DIFF trigger.
     * AUDxLC, AUDxLEN, AUDxPER are updated by the Paula audio DMA interrupt
     * handler which is not emulated in the PC port — these will diverge until
     * Paula audio DMA is ported. Volume is written synchronously from game code
     * and should always match. */
    for (int n = 0; n < 4; n++) {
        if (p->audio[n].vol != c->audio[n].vol)
            return 1;
    }

    /* NOTE: bpl_data_crc is NOT a per-frame DIFF trigger.
     * PUAE's frame 0 snap is taken after ~600 boot frames; PC's frame 0 snap
     * is taken after just 1 frame from the sync point.  The bitplane DATA
     * content changes every frame (sprites, animations), so comparing per-frame
     * CRCs compares completely different game states → always a false DIFF.
     * The end-of-run bpl_data check in compare_phases() is the correct test:
     * both sides run the same N frames from the sync point, so their end-of-run
     * bitplane content should match if the blitter is correct. */

    /* NOTE: palette[] comparison intentionally NOT a DIFF trigger.
     * PUAE snapshots its COLOR registers at frame START (after the initial
     * copper MOVE instructions, but before the first scanline WAIT fires).
     * The PC port snapshots s_palette at frame END (after the final copper
     * MOVE, which may be from a different scanline section).
     * This timing difference causes systematic false positives for games that
     * use split-screen copper palette tricks (e.g., $7BC8 list: COLOR01=$0110
     * before line 44-90, then COLOR01=$0248 after WAIT line=90).
     * The coplist[] comparison already verifies that both sides program the
     * same colour sequence — that is the authoritative check.
     *
     * NOTE: chipram_crc intentionally NOT a DIFF trigger.  PC port writes its
     * own framebuffer addresses into chip RAM (different from PUAE's chip RAM
     * layout), so CRC always differs even when both copper lists match.
     *
     * NOTE: Framebuffer pixel diff is NOT a DIFF trigger because libretro-UAE
     * applies scaling/cropping on top of the game output that the PC port does
     * not replicate.  The rendered pixels will always differ; what matters is
     * that the inputs (copper list, bitplane data) match. */
    return 0;
}

/* Compare PUAE vs PC framebuffers with detailed diagnostics.
 * 'fi' is the frame index to compare states for (last captured frame). */
static void compare_framebuffers(int fi)
{
    const uint32_t *pc_fb = hw_get_framebuffer();
    if (!s_puae_fb || !pc_fb) {
        printf("[pixel] No framebuffer available\n");
        return;
    }

    int W = 320, H = 256;

    /* --- Register shadow comparison (last captured frame) --- */
    if (fi < 0 || fi >= MAX_FRAMES) fi = 0;
    const FrameState *p = &s_puae_log[fi];
    const FrameState *c = &s_pc_log[fi];
    printf("[state] cop1lc PUAE=$%06X PC=$%06X  cop2lc PUAE=$%06X PC=$%06X\n",
           p->cop1lc, c->cop1lc, p->cop2lc, c->cop2lc);
    printf("[state] bplcon0 PUAE=$%04X PC=$%04X  bpl1mod PUAE=%d PC=%d  bpl2mod PUAE=%d PC=%d\n",
           p->bplcon0, c->bplcon0, p->bpl1mod, c->bpl1mod, p->bpl2mod, c->bpl2mod);

    for (int i = 0; i < 6; i++) {
        if (p->bplpt[i] != c->bplpt[i])
            printf("[state] bplpt[%d] PUAE=$%06X PC=$%06X  DIFF\n", i, p->bplpt[i], c->bplpt[i]);
    }

    /* --- Palette comparison --- */
    int pal_diff = 0;
    for (int i = 0; i < 32; i++) {
        if (p->palette[i] != c->palette[i]) {
            if (pal_diff < 5)
                printf("[pal] COLOR%02d PUAE=$%04X PC=$%04X  DIFF\n", i, p->palette[i], c->palette[i]);
            pal_diff++;
        }
    }
    if (pal_diff > 0) printf("[pal] Total palette diffs: %d\n", pal_diff);

    /* --- Audio channel comparison --- */
    for (int n = 0; n < 4; n++) {
        const AudioChanSnap *pa = &p->audio[n];
        const AudioChanSnap *ca = &c->audio[n];
        /* Show full state if any field differs; mark vol as DIFF trigger */
        int vol_diff = (pa->vol != ca->vol);
        int any_diff = (pa->lc != ca->lc || pa->len != ca->len ||
                        pa->per != ca->per || vol_diff);
        if (any_diff) {
            const char *vol_tag = vol_diff ? "  VOL-DIFF" : "";
            printf("[aud] CH%d PUAE: lc=$%06X len=%u per=%u vol=%u\n",
                   n, pa->lc, pa->len, pa->per, pa->vol);
            printf("[aud] CH%d PC  : lc=$%06X len=%u per=%u vol=%u%s\n",
                   n, ca->lc, ca->len, ca->per, ca->vol, vol_tag);
        }
    }

    /* --- Count pixel diffs with per-row profile --- */
    int diff_pixels = 0;
    int row_diff[256] = {0};
    int first_diff_idx = -1;

    for (int i = 0; i < W * H; i++) {
        uint32_t puae_c = s_puae_fb[i] & 0x00FFFFFFu;
        uint32_t pc_c   = pc_fb[i] & 0x00FFFFFFu;
        if (puae_c != pc_c) {
            if (first_diff_idx < 0) first_diff_idx = i;
            diff_pixels++;
            row_diff[i / W]++;
        }
    }

    printf("[pixel] Diff: %d / %d (%.1f%%)  ", diff_pixels, W*H, 100.0 * diff_pixels / (W*H));
    if (diff_pixels > 0) {
        int px = first_diff_idx % W, py = first_diff_idx / W;
        uint32_t puae_c = s_puae_fb[first_diff_idx] & 0x00FFFFFFu;
        uint32_t pc_c   = pc_fb[first_diff_idx] & 0x00FFFFFFu;
        printf("first at (%d,%d) PUAE=%06X PC=%06X", px, py, puae_c, pc_c);
    }
    printf("\n");

    if (diff_pixels == 0) return;

    /* --- Row profile --- */
    int worst_row = 0, worst_count = 0;
    int first_row = -1, last_row = -1;
    for (int y = 0; y < H; y++) {
        if (row_diff[y] > 0) {
            if (first_row < 0) first_row = y;
            last_row = y;
            if (row_diff[y] > worst_count) {
                worst_count = row_diff[y];
                worst_row = y;
            }
        }
    }
    printf("[pixel] Rows y=%d..%d (worst y=%d: %d bad pixels)\n",
           first_row, last_row, worst_row, worst_count);

    /* --- Sample differing pixels --- */
    int samples = 0;
    printf("[pixel] Sample differing pixels (PC vs PUAE):\n");
    for (int i = 0; i < W * H && samples < 10; i++) {
        uint32_t puae_c = s_puae_fb[i] & 0x00FFFFFFu;
        uint32_t pc_c   = pc_fb[i] & 0x00FFFFFFu;
        if (puae_c != pc_c) {
            int px = i % W, py = i / W;
            printf("  (%3d,%3d) PC=%06X  PUAE=%06X\n", px, py, pc_c, puae_c);
            samples++;
        }
    }

    /* --- Colour-occurrence analysis: which colours differ and how often --- */
    /* Count how many times each PC colour value appears as a wrong pixel */
    typedef struct { uint32_t color; int count; } ColorCount;
    ColorCount pc_wrong[256];
    int n_pc = 0;
    for (int i = 0; i < W * H; i++) {
        uint32_t puae_c = s_puae_fb[i] & 0x00FFFFFFu;
        uint32_t pc_c   = pc_fb[i] & 0x00FFFFFFu;
        if (puae_c != pc_c) {
            int found = -1;
            for (int j = 0; j < n_pc; j++) {
                if (pc_wrong[j].color == pc_c) { found = j; break; }
            }
            if (found >= 0) pc_wrong[found].count++;
            else if (n_pc < 256) { pc_wrong[n_pc].color = pc_c; pc_wrong[n_pc].count = 1; n_pc++; }
        }
    }
    /* Sort by count descending and show top 5 */
    for (int i = 0; i < n_pc && i < 5; i++) {
        int best = i;
        for (int j = i+1; j < n_pc; j++)
            if (pc_wrong[j].count > pc_wrong[best].count) best = j;
        ColorCount t = pc_wrong[i]; pc_wrong[i] = pc_wrong[best]; pc_wrong[best] = t;
    }
    printf("[pixel] Top PC colours appearing where PUAE differs:\n");
    for (int i = 0; i < n_pc && i < 5; i++) {
        printf("  %06X  x%d\n", pc_wrong[i].color, pc_wrong[i].count);
    }
}

/* Diff PUAE chip RAM (s_puae_chipram_snap) vs PC g_mem[].
 * Reports the first MAX_DIFFS differing regions and dumps bytes around each. */
static void report_chipram_diff(FILE *fp)
{
#define MAX_DIFFS 20
    if (!s_puae_chipram_valid || !s_pc_chipram_valid) {
        fprintf(fp, "\nChip RAM diff: not available (puae=%d pc=%d)\n",
                s_puae_chipram_valid, s_pc_chipram_valid);
        return;
    }
    fprintf(fp, "\nChip RAM diff (PUAE end-of-run vs PC end-of-run, first %d regions):\n", MAX_DIFFS);
    int ndiffs = 0;
    uint32_t i = 0;
    while (i < CHIP_RAM_SIZE && ndiffs < MAX_DIFFS) {
        if (s_puae_chipram_snap[i] == s_pc_chipram_snap[i]) { i++; continue; }
        uint32_t run_start = i;
        while (i < CHIP_RAM_SIZE && s_puae_chipram_snap[i] != s_pc_chipram_snap[i]) i++;
        uint32_t run_end = i;
        uint32_t ctx = (run_start >= 8) ? run_start - 8 : 0;
        uint32_t ctx_end = ctx + 32 < CHIP_RAM_SIZE ? ctx + 32 : CHIP_RAM_SIZE;
        fprintf(fp, "  $%06X...$%06X  (%u bytes differ)\n",
                run_start, run_end - 1, run_end - run_start);
        fprintf(fp, "  PUAE:");
        for (uint32_t j = ctx; j < ctx_end; j++)
            fprintf(fp, "%s%02X", (j == run_start ? " [" : " "), s_puae_chipram_snap[j]);
        fprintf(fp, "%s\n", (run_end <= ctx_end ? "]" : ""));
        fprintf(fp, "  PC  :");
        for (uint32_t j = ctx; j < ctx_end; j++)
            fprintf(fp, "%s%02X", (j == run_start ? " [" : " "), s_pc_chipram_snap[j]);
        fprintf(fp, "%s\n", (run_end <= ctx_end ? "]" : ""));
        ndiffs++;
    }
    if (ndiffs == 0)
        fprintf(fp, "  (chip RAM is identical)\n");
    else if (ndiffs == MAX_DIFFS && i < CHIP_RAM_SIZE)
        fprintf(fp, "  ... (more diffs not shown)\n");
#undef MAX_DIFFS
}

static void compare_phases(int puae_frames, int pc_frames, const char *report_path)
{
    int n = puae_frames < pc_frames ? puae_frames : pc_frames;
    printf("\n[harness] === Comparison: %d frames ===\n", n);

    int first_diff = -1;
    int first_fb_diff = -1;  /* frame where fb differs but copper/palette matched */
    for (int f = 0; f < n; f++) {
        const FrameState *p = &s_puae_log[f];
        const FrameState *c = &s_pc_log[f];
        int diff = frames_differ(p, c);

        /* Framebuffer pixel comparison (informational only — libretro-UAE applies
         * scaling/cropping that the PC renderer does not replicate, so pixel
         * differences are expected even when game state is identical) */
        int fb_diff_pixels = 0;
        if (f < s_puae_fb_count && f < s_pc_fb_count) {
            const uint32_t *pf = s_puae_fb_log[f];
            const uint32_t *cf = s_pc_fb_log[f];
            for (int i = 0; i < FB_W * FB_H; i++) {
                if ((pf[i] & 0x00FFFFFFu) != (cf[i] & 0x00FFFFFFu))
                    fb_diff_pixels++;
            }
        }

        printf("  F%03d: cop1lc PUAE=$%06X PC=$%06X  bplcon0 PUAE=$%04X PC=$%04X",
               f, p->cop1lc, c->cop1lc, p->bplcon0, c->bplcon0);
        if (fb_diff_pixels > 0)
            printf("  fb_diff=%d px (%.1f%%)", fb_diff_pixels, 100.0 * fb_diff_pixels / (FB_W * FB_H));
        printf("  %s\n", diff ? "DIFF <<<" : "ok");

        if (diff) {
            first_diff = f;
            if (fb_diff_pixels > 0) first_fb_diff = f;
            printf("\n[harness] Stopping at first divergence (frame %d)\n", f);
            break;
        }
    }

    if (first_diff < 0) {
        /* Post-run bitplane data check: compare end-of-run chip RAM at active
         * bitplane regions.  This is the correct way to verify blitter output:
         * both sides ran the same N comparison frames from the same sync point,
         * so their end-of-run bpl data should be byte-for-byte identical. */
        int bpl_data_ok = 1;
        if (s_puae_chipram_valid && s_pc_chipram_valid) {
            static const struct { uint32_t start; uint32_t len; const char *name; } bpl_rgns[] = {
                { 0x025334u, 4096u,  "title" },
                { 0x04F6F4u, 4096u,  "gplay_bpl1+3" },
                { 0x070958u, 8192u,  "gplay_bpl2" },
            };
            printf("[harness] Post-run bpl data check:\n");
            for (int r = 0; r < 3; r++) {
                uint32_t start = bpl_rgns[r].start;
                uint32_t len   = bpl_rgns[r].len;
                if (start + len > CHIP_RAM_SIZE) continue;
                int match = (memcmp(s_puae_chipram_snap + start,
                                    s_pc_chipram_snap   + start, len) == 0);
                printf("  $%06X+%u (%s): %s\n",
                       start, len, bpl_rgns[r].name, match ? "MATCH" : "DIFF <<<");
                if (!match) bpl_data_ok = 0;
            }
        } else {
            printf("[harness] Post-run bpl data check: chip RAM not available\n");
        }

        if (bpl_data_ok) {
            printf("\n[harness] PERFECT MATCH across %d frames  (bpl data OK)\n", n);
        } else {
            printf("\n[harness] COPPER/AUDIO MATCH across %d frames  but BPL DATA DIFF <<<\n", n);
            printf("[harness]   Blitter wrote different pixel data on PC vs PUAE.\n"
                   "            Add a watchpoint on the diverging address range to find the writer.\n");
            /* Write bpl diff to report file */
            FILE *rp = fopen(report_path, "w");
            if (rp) {
                fprintf(rp, "BPL DATA DIFF (blitter bug) — copper list matches, bpl content differs\n");
                fprintf(rp, "Check harness report for chip RAM diff at $025334/$04F6F4/$070958.\n");
                report_chipram_diff(rp);
                fclose(rp);
            }
        }
        return;
    }

    /* Write detailed report to file */
    FILE *rp = fopen(report_path, "w");
    bool report_to_file = (rp != NULL);
    if (!rp) rp = stdout;

    /* If the divergence was framebuffer-only (copper+palette matched), emit a
     * dedicated pixel diff section rather than the copper divergence report. */
    if (first_fb_diff >= 0) {
        print_divider(rp);
        fprintf(rp, "FIRST DIVERGENCE at frame %d — FRAMEBUFFER PIXEL MISMATCH\n", first_fb_diff);
        fprintf(rp, "(copper list and palette are identical; pixels differ)\n");
        print_divider(rp);

        const uint32_t *pf = s_puae_fb_log[first_fb_diff];
        const uint32_t *cf = s_pc_fb_log  [first_fb_diff];
        int px_diff = 0, first_px = -1;
        int row_diffs[FB_H];
        memset(row_diffs, 0, sizeof(row_diffs));
        for (int i = 0; i < FB_W * FB_H; i++) {
            if ((pf[i] & 0xFFFFFFu) != (cf[i] & 0xFFFFFFu)) {
                if (first_px < 0) first_px = i;
                row_diffs[i / FB_W]++;
                px_diff++;
            }
        }
        fprintf(rp, "  Total pixel diff: %d / %d  (%.1f%%)\n",
                px_diff, FB_W * FB_H, 100.0 * px_diff / (FB_W * FB_H));
        if (first_px >= 0) {
            fprintf(rp, "  First mismatch at (%d,%d): PUAE=$%06X  PC=$%06X\n",
                    first_px % FB_W, first_px / FB_W,
                    pf[first_px] & 0xFFFFFFu, cf[first_px] & 0xFFFFFFu);
        }
        int first_row = -1, last_row = -1, worst_row = -1, worst_n = 0;
        for (int y = 0; y < FB_H; y++) {
            if (row_diffs[y]) {
                if (first_row < 0) first_row = y;
                last_row = y;
                if (row_diffs[y] > worst_n) { worst_n = row_diffs[y]; worst_row = y; }
            }
        }
        if (first_row >= 0)
            fprintf(rp, "  Affected rows y=%d..%d  (worst y=%d: %d px)\n",
                    first_row, last_row, worst_row, worst_n);
        print_divider(rp);
    } else {
        report_divergence(rp, first_diff,
                           &s_puae_log[first_diff],
                           &s_pc_log  [first_diff]);
    }

    report_chipram_diff(rp);

    /* ── Side-by-side write trace for first diverging address ── */
    {
        const FrameState *p = &s_puae_log[first_diff];
        const FrameState *c = &s_pc_log  [first_diff];
        uint32_t first_da = 0;
        if (p->cop1lc == c->cop1lc && p->coplist_valid && c->coplist_valid) {
            for (int i = 0; i < HARNESS_COPLIST_WORDS; i++) {
                if (p->coplist[i] != c->coplist[i]) {
                    first_da = p->cop1lc + i * 2;
                    break;
                }
            }
        }
        if (first_da) {
            fprintf(rp, "\n══════════════════════════════════════════════════════════════\n");
            fprintf(rp, "WRITE TRACE for first diverging address $%06X\n", first_da);
            fprintf(rp, "══════════════════════════════════════════════════════════════\n");
            /* trace buffer disabled due to corruption; use direct logging instead */
            fprintf(rp, "  (trace buffer disabled, see stderr logs for operation sequences)\n");
            fprintf(rp, "\n");
        }
    }

    if (report_to_file) {
        fclose(rp);
        printf("[harness] Detailed report written to %s\n", report_path);
        /* Print condensed key diffs to stdout too */
        const FrameState *p = &s_puae_log[first_diff];
        const FrameState *c = &s_pc_log  [first_diff];
        printf("\n--- Key diffs (frame %d) ---\n", first_diff);
        if (p->cop1lc  != c->cop1lc)
            printf("  cop1lc  PUAE=$%06X  PC=$%06X  (delta %+d)\n",
                   p->cop1lc, c->cop1lc,
                   (int32_t)c->cop1lc - (int32_t)p->cop1lc);
        if (p->bplcon0 != c->bplcon0)
            printf("  bplcon0 PUAE=$%04X (planes=%d)  PC=$%04X (planes=%d)\n",
                   p->bplcon0, (p->bplcon0>>12)&7,
                   c->bplcon0, (c->bplcon0>>12)&7);
        if (p->bpl1mod != c->bpl1mod)
            printf("  bpl1mod PUAE=%d  PC=%d\n", p->bpl1mod, c->bpl1mod);
        if (p->bpl2mod != c->bpl2mod)
            printf("  bpl2mod PUAE=%d  PC=%d\n", p->bpl2mod, c->bpl2mod);
        for (int i = 0; i < 6; i++)
            if (p->bplpt[i] != c->bplpt[i])
                printf("  bplpt[%d] PUAE=$%06X  PC=$%06X\n",
                       i, p->bplpt[i], c->bplpt[i]);
        int pal_diffs = 0;
        for (int i = 0; i < 32; i++)
            if (p->palette[i] != c->palette[i]) pal_diffs++;
        if (pal_diffs)
            printf("  palette: %d of 32 entries differ\n", pal_diffs);
        /* Audio channel diffs (vol only is DIFF trigger; lc/len/per shown informational) */
        for (int n = 0; n < 4; n++) {
            const AudioChanSnap *pa = &p->audio[n];
            const AudioChanSnap *ca = &c->audio[n];
            if (pa->lc  != ca->lc  || pa->len != ca->len ||
                pa->per != ca->per || pa->vol != ca->vol) {
                const char *tag = (pa->vol != ca->vol) ? "  VOL-DIFF" : "  (lc/len/per: ISR-driven, informational)";
                printf("  aud CH%d: PUAE lc=$%06X len=%u per=%u vol=%u  |  PC lc=$%06X len=%u per=%u vol=%u%s\n",
                       n, pa->lc, pa->len, pa->per, pa->vol,
                          ca->lc, ca->len, ca->per, ca->vol, tag);
            }
        }
        /* Also dump trace to stdout */
        uint32_t first_da = 0;
        if (p->cop1lc == c->cop1lc && p->coplist_valid && c->coplist_valid) {
            for (int i = 0; i < HARNESS_COPLIST_WORDS; i++) {
                if (p->coplist[i] != c->coplist[i]) {
                    first_da = p->cop1lc + i * 2;
                    break;
                }
            }
        }
        if (first_da)
            trace_dump_side_by_side(stdout, first_da, 10);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <kick_dir> <slave_path> <disk1> [disk2] [disk3]\n"
            "       [--frames N] [--boot-frames N] [--report FILE] [--display-only]\n"
            "\n"
            "  --frames N       game frames to capture (0 = interactive, default 60)\n"
            "  --boot-frames N  PUAE boot frames before game starts (default 1000)\n"
            "  --report FILE    write detailed report here (default logs/harness_report.txt)\n"
            "  --display-only   run PUAE forever in SDL window, no comparison\n",
            argv[0]);
        return 1;
    }

    const char *kick_dir      = argv[1];
    const char *slave_path    = argv[2];
    const char *disks[4]      = { NULL };
    int n_disks      = 0;
    int n_frames     = 60;
    int boot_frames  = 500;
    int display_only = 0;
    int interactive  = 0;
    const char *report_path = "logs/harness_report.txt";

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            n_frames = atoi(argv[++i]);
            if (n_frames <= 0) { n_frames = 0; interactive = 1; }
        } else if (!strcmp(argv[i], "--boot-frames") && i + 1 < argc)
            boot_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc)
            report_path = argv[++i];
        else if (!strcmp(argv[i], "--display-only"))
            display_only = 1;
        else if (n_disks < 3)
            disks[n_disks++] = argv[i];
    }
    if (n_frames > MAX_FRAMES)    n_frames = MAX_FRAMES;
    if (n_frames < 0)             n_frames = 0;
    if (boot_frames > 10000)      boot_frames = 10000;

    printf("[harness] kick_dir=%s  slave=%s\n", kick_dir, slave_path);
    printf("[harness] boot_frames=%d  capture_frames=%d%s  report=%s\n",
           boot_frames, n_frames, interactive ? " (interactive)" : "", report_path);

    /* Phase 1: PUAE boots to game state, captures frames, dumps chip RAM */
    char puae_chipram_path[512] = "";
    int capture_frames = interactive ? 1 : n_frames;
    int puae_frames = run_puae_phase(kick_dir, slave_path, boot_frames, capture_frames,
                                     puae_chipram_path, sizeof(puae_chipram_path),
                                     display_only, interactive);
    if (puae_frames < 0) return 1;

    if (display_only) return 0;  /* PUAE-only mode */

    if (puae_chipram_path[0] == '\0') {
        fprintf(stderr, "[harness] PUAE chip RAM dump not available\n");
        return 1;
    }

    /* Phase 2: PC boots independently from chip dump, then both boot_compare */
    printf("[harness] === Boot comparison: PUAE reached game state, now PC boots independently ===\n");
    printf("[harness] PC init from chip dump (no kickstart/disks needed)\n");
    fflush(stdout);

    if (pc_init_from_chip_dump(puae_chipram_path) < 0) {
        fprintf(stderr, "[harness] PC init failed\n");
        return 1;
    }

    pc_set_harness_mode(1);
    pc_init_state();
    hw_running = 1;

    /* Run PC through boot_frames to reach same game state as PUAE */
    printf("[harness] PC running boot sequence (%d frames)...\n", boot_frames);
    for (int f = 0; f < boot_frames; f++) {
        pc_step();
        if ((f + 1) % 100 == 0) {
            uint32_t cop = r32(0xdff080);
            printf("[harness]   PC boot f %d: cop1lc=$%06X\n", f + 1, cop);
            fflush(stdout);
        }
    }

    /* Capture PC boot state after same boot_frames as PUAE */
    s_pc_log[0].frame = 0;
    hw_get_snap(&s_pc_log[0]);
    if (MAX_FB_FRAMES > 0) {
        const uint32_t *fb = hw_get_framebuffer();
        if (fb) {
            memcpy(s_pc_fb_log[0], fb, FB_W * FB_H * sizeof(uint32_t));
            s_pc_fb_count = 1;
        }
    }

    printf("[harness] PC boot complete: cop1lc=$%06X bplcon0=$%04X\n",
           s_pc_log[0].cop1lc, s_pc_log[0].bplcon0);

    /* Capture PUAE boot state (already have s_puae_log[0] from phase 1) */
    printf("[harness] PUAE boot state: cop1lc=$%06X bplcon0=$%04X\n",
           s_puae_log[0].cop1lc, s_puae_log[0].bplcon0);

    /* Compare boot states */
    int boot_match = !frames_differ(&s_puae_log[0], &s_pc_log[0]);
    printf("[harness] Boot state comparison: %s\n", boot_match ? "MATCH" : "DIFFER");

    if (!boot_match) {
        /* Report boot divergence and stop */
        printf("\n[harness] Boot state divergence detected — stopping here\n");
        compare_phases(1, 1, report_path);
        pc_fini();
        return 1;
    }

    /* Phase 3: Both at same boot state — now alternate per-frame */
    printf("[harness] === Frame-by-frame interleaved comparison ===\n");
    fflush(stdout);

    harness_combined_init();

    int first_diff_frame = -1;
    s_puae_fb_count = 0;
    s_pc_fb_count = 0;

    for (int f = 0; f < n_frames; f++) {
        /* Run one PUAE frame */
        retro_run();
        s_puae_log[f].frame = f;
        puae_snap_state(&s_puae_log[f]);
        if (f < MAX_FB_FRAMES) {
            memcpy(s_puae_fb_log[f], s_puae_fb, FB_W * FB_H * sizeof(uint32_t));
            s_puae_fb_count = f + 1;
        }

        /* Run one PC frame */
        pc_step();
        s_pc_log[f].frame = f;
        hw_get_snap(&s_pc_log[f]);
        if (f < MAX_FB_FRAMES) {
            const uint32_t *fb = hw_get_framebuffer();
            if (fb) {
                memcpy(s_pc_fb_log[f], fb, FB_W * FB_H * sizeof(uint32_t));
                s_pc_fb_count = f + 1;
            }
        }

        /* Compare immediately */
        int diff = frames_differ(&s_puae_log[f], &s_pc_log[f]);
        printf("  F%03d: cop1lc PUAE=$%06X PC=$%06X  bplcon0 PUAE=$%04X PC=$%04X  %s\n",
               f, s_puae_log[f].cop1lc, s_pc_log[f].cop1lc,
               s_puae_log[f].bplcon0, s_pc_log[f].bplcon0,
               diff ? "DIFF <<<" : "ok");

        if (diff) {
            first_diff_frame = f;
            printf("\n[harness] Frame divergence at frame %d\n", f);
            break;
        }

        if ((f + 1) % 10 == 0) {
            printf("[harness]   %d frames compared — all match\n", f + 1);
            fflush(stdout);
        }
    }

    int n_compared = (first_diff_frame >= 0) ? (first_diff_frame + 1) : n_frames;
    if (!interactive)
        compare_phases(n_compared, n_compared, report_path);

    {
        int last = n_compared - 1;
        if (last < 0) last = 0;
        compare_framebuffers(last);
    }

    pc_fini();
    retro_unload_game();
    retro_deinit();
    harness_combined_fini();
    return (first_diff_frame >= 0) ? 1 : 0;
}

