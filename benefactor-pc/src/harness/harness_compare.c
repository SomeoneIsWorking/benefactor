/* harness_compare.c – Divergence comparison and reporting
 * Extracted from harness_main.c to reduce monolithic size
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "harness/puae_state.h"
#include "harness/harness_internal.h"
#include "harness/trace.h"
#include "recomp/rt.h"   /* g_mem */
#include "recomp/hw.h"   /* g_mem size constant */

/* Called at the first detected divergence. Set a GDB breakpoint here. */
void __attribute__((noinline)) harness_on_diverge(int frame, const char *reason)
{
    (void)frame; (void)reason;
    /* GDB: break harness_on_diverge */
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Copper list register name lookup */

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

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Comparison: frames_differ checks if two frames match */

int frames_differ(const FrameState *p, const FrameState *c)
{
    if (p->cop1lc != c->cop1lc) return 1;

    /* BPL data CRC is NOT checked here; harness_main.c compares PUAE post-frame
     * CRC against PC pre-render CRC (one frame earlier in PC's pipeline) to
     * account for PC's synchronous blitter.  Checking c->bpl_data_crc here
     * would use PC's post-frame value which is 1 frame ahead of PUAE. */

    /* Palette comparison: the PC starts from a fresh hw register state (copper
     * initialises it at frame 0) while PUAE has been running animated palette
     * for 285 boot frames.  Skip palette on the boot frame (frame==0); for
     * lockstep frames (frame>=1) the palette must match because both sides ran
     * the same $0055A0 timer interrupt from the same seeded starting state.
     *
     * bplpt[], bplcon0, bpl1mod/bpl2mod are shadow registers that differ due
     * to snapshot timing — the coplist words capture the correct intended values.
     * Audio vol is NOT compared: Paula DMA vs register shadow semantics differ. */
    if (p->frame > 0) {
        for (int i = 0; i < 32; i++) {
            if (p->palette[i] != c->palette[i]) {
                harness_on_diverge(p->frame, "palette");
                return 1;
            }
        }
    }

    /* Copper list content is the authoritative comparison for BPL/sprite setup. */
    if (!p->coplist_valid || !c->coplist_valid)
        return 0;
    for (int i = 0; i < HARNESS_COPLIST_WORDS; i++) {
        if (p->coplist[i] != c->coplist[i]) {
            uint32_t cop_addr = p->cop1lc + (uint32_t)(i * 2);
            int insn = i / 2;
            int reg_word_idx = insn * 2;
            uint16_t reg_word = p->coplist[reg_word_idx];
            uint16_t reg = reg_word & 0x1FEu;
            const char *name = reg_name(reg);
            harness_on_diverge(p->frame, "coplist");
            return 1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Framebuffer comparison */

/* Walk a copper list and find the first register write whose scanline <= target_y
 * that differs between plist[] and clist[].
 * Reports: [pixel-cause] line Y register REGNAME: PUAE=$XXXX PC=$XXXX
 */
static void report_pixel_cause(int px, int py,
                                const uint16_t *plist, const uint16_t *clist,
                                int nwords)
{
    /* Convert framebuffer row to Amiga scanline.
     * The Benefactor framebuffer starts at display line 0 (row 0 = scanline 0x00).
     * Each framebuffer row = one Amiga scanline. */
    int target_line = py;   /* scanline that contains the first differing pixel */

    int cur_line = -1;  /* -1 = before any WAIT, meaning all register writes apply */
    const char *cause_name = NULL;
    uint16_t cause_puae = 0, cause_pc = 0;
    int cause_cop_idx = -1;

    for (int i = 0; i + 1 < nwords; i += 2) {
        uint16_t pw1 = plist[i], pw2 = plist[i+1];
        uint16_t cw1 = clist[i], cw2 = clist[i+1];

        /* End of list marker */
        if (pw1 == 0xFFFF && pw2 == 0xFFFE) break;

        if (pw1 & 1) {
            /* WAIT instruction: bits 15-8 of pw1 = line (vpos) */
            int wait_line = (pw1 >> 8) & 0xFF;
            if (wait_line > target_line) break;  /* past the target scanline */
            cur_line = wait_line;
        } else {
            /* MOVE instruction: pw1 bits 8-1 = register offset */
            uint16_t reg = pw1 & 0x1FE;

            /* Only registers that affect pixel output matter */
            int affects_pixels =
                (reg >= 0x180 && reg <= 0x1BE) ||  /* COLORxx */
                reg == 0x100 ||  /* BPLCON0 */
                reg == 0x102 ||  /* BPLCON1 */
                (reg >= 0x0E0 && reg <= 0x0F6) ||  /* BPLxPTH/L */
                reg == 0x108 || reg == 0x10A;       /* BPL1MOD/BPL2MOD */

            if (affects_pixels && pw2 != cw2) {
                /* Record first differing entry at or before target scanline */
                if (cause_cop_idx < 0) {
                    cause_name = reg_name(reg);
                    cause_puae = pw2;
                    cause_pc   = cw2;
                    cause_cop_idx = i / 2;
                }
            }

            /* Also note if the instruction word itself differs (pw1 != cw1 would be
             * a structural difference, which we report but don't use for cause) */
        }
    }

    if (cause_cop_idx >= 0) {
        printf("[pixel-cause] first diff pixel (%d,%d): "
               "cop[%03d] line~%d %s: PUAE=$%04X PC=$%04X\n",
               px, py, cause_cop_idx, cur_line,
               cause_name ? cause_name : "???",
               cause_puae, cause_pc);
    } else {
        printf("[pixel-cause] first diff pixel (%d,%d): "
               "no differing copper register found before scanline %d "
               "(coplist may match — blitter/bitplane data differs)\n",
               px, py, target_line);
    }
}

static void dump_suspect_state_traces(void)
{
    static const struct {
        uint32_t start;
        uint32_t end;
        const char *label;
    } ranges[] = {
        { 0x003800u, 0x003806u, "phases-3800" },
        { 0x003B92u, 0x003C2Eu, "3B92-thresh" },
        { 0x007CF4u, 0x007D50u, "title-copper-bplmod" },
        { 0x00419Au, 0x0041A4u, "title-swap" },
        { 0x0042FCu, 0x004308u, "focus-state" },
        { 0x0069F0u, 0x006AEAu, "timer-state" },
        { 0x07FFA2u, 0x080000u, "stack-dispatch" },
    };

    printf("[frame-trace] suspect state ranges:\n");
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        printf("[frame-trace] %s $%06X-$%06X\n",
               ranges[i].label, ranges[i].start, ranges[i].end - 1);
        trace_dump_range(stdout, ranges[i].start, ranges[i].end, 24);
    }
}

int framebuffers_differ(int fi)
{
    if (fi >= s_puae_fb_count || fi >= s_pc_fb_count) return 0;
    const uint32_t *pf = s_puae_fb_log[fi];
    const uint32_t *cf = s_pc_fb_log[fi];
    for (int i = 0; i < FB_W * FB_H; i++) {
        if ((pf[i] & 0xFFFFFFu) != (cf[i] & 0xFFFFFFu)) return 1;
    }
    return 0;
}

void compare_framebuffers(int fi)
{
    if (fi >= s_puae_fb_count || fi >= s_pc_fb_count) return;

    const uint32_t *pf = s_puae_fb_log[fi];
    const uint32_t *cf = s_pc_fb_log[fi];
    int px_diff = 0;
    int first_px = -1;
    int row_diff[FB_H];
    struct {
        uint32_t p;
        uint32_t c;
        int n;
    } pairs[8];
    int pair_count = 0;

    memset(pairs, 0, sizeof(pairs));
    memset(row_diff, 0, sizeof(row_diff));

    for (int i = 0; i < FB_W * FB_H; i++) {
        uint32_t pv = pf[i] & 0xFFFFFFu;
        uint32_t cv = cf[i] & 0xFFFFFFu;
        if (pv != cv) {
            if (first_px < 0) first_px = i;
            px_diff++;
            row_diff[i / FB_W]++;

            int found = 0;
            for (int k = 0; k < pair_count; k++) {
                if (pairs[k].p == pv && pairs[k].c == cv) {
                    pairs[k].n++;
                    found = 1;
                    break;
                }
            }
            if (!found && pair_count < (int)(sizeof(pairs) / sizeof(pairs[0]))) {
                pairs[pair_count].p = pv;
                pairs[pair_count].c = cv;
                pairs[pair_count].n = 1;
                pair_count++;
            }
        }
    }

    printf("[pixel] Diff: %d / %d (%.1f%%)", px_diff, FB_W * FB_H, 100.0 * px_diff / (FB_W * FB_H));
    if (px_diff > 0) {
        int fpx = first_px % FB_W, fpy = first_px / FB_W;
        printf("  first at (%d,%d) PUAE=%06X PC=%06X\n",
               fpx, fpy, pf[first_px] & 0xFFFFFFu, cf[first_px] & 0xFFFFFFu);

        {
            FILE *fp = fopen("logs/harness_puae_fb_diff.bin", "wb");
            if (fp) {
                fwrite(pf, sizeof(uint32_t), FB_W * FB_H, fp);
                fclose(fp);
            }
            fp = fopen("logs/harness_pc_fb_diff.bin", "wb");
            if (fp) {
                fwrite(cf, sizeof(uint32_t), FB_W * FB_H, fp);
                fclose(fp);
            }
        }

        if (pair_count > 0) {
            printf("[pixel-pairs] top pairs (up to %d unique):\n", pair_count);
            for (int k = 0; k < pair_count; k++) {
                printf("  PUAE=%06X PC=%06X count=%d\n",
                       pairs[k].p, pairs[k].c, pairs[k].n);
            }
        }

        /* Map the first differing pixel back to the active copper instruction */
        if (fi < (int)(sizeof(s_puae_log)/sizeof(s_puae_log[0])) &&
            s_puae_log[fi].coplist_valid && s_pc_log[fi].coplist_valid) {
            report_pixel_cause(fpx, fpy,
                                s_puae_log[fi].coplist,
                                s_pc_log[fi].coplist,
                                HARNESS_COPLIST_WORDS);
        }

        {
            int spr_diffs = 0;
            for (int i = 0; i < 8; i++) {
                if (s_puae_log[fi].sprpt[i] != s_pc_log[fi].sprpt[i]) {
                    if (spr_diffs == 0)
                        printf("[sprite-ptrs] mismatches:\n");
                    printf("  spr%d PUAE=$%06X PC=$%06X\n",
                           i,
                           s_puae_log[fi].sprpt[i] & 0xFFFFFFu,
                           s_pc_log[fi].sprpt[i] & 0xFFFFFFu);
                    spr_diffs++;
                }
            }
            if (spr_diffs == 0)
                printf("[sprite-ptrs] all 8 match\n");
        }

        dump_suspect_state_traces();
    } else {
        printf("  MATCH ✓\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Main divergence report */

void compare_phases(int puae_frames, int pc_frames, const char *report_path)
{
    int n = puae_frames < pc_frames ? puae_frames : pc_frames;

    printf("\n[harness] === Comparison: %d frames ===\n", n);

    int first_diff = -1;
    for (int i = 0; i < n; i++) {
        if (frames_differ(&s_puae_log[i], &s_pc_log[i])) {
            first_diff = i;
            break;
        }
    }

    if (first_diff < 0) {
        printf("[harness] PERFECT MATCH across all %d frames\n", n);
        return;
    }

    printf("[harness] First divergence at frame %d\n", s_puae_log[first_diff].frame);

    /* Report BPL data CRC across all frames to show where it first diverges */
    for (int i = 0; i < n; i++) {
        uint32_t pc = s_puae_log[i].bpl_data_crc;
        uint32_t cc = s_pc_log[i].bpl_data_crc;
        if (pc && cc && pc != cc)
            printf("[bpl-crc] frame=%d PUAE=$%08X PC=$%08X  DIFF\n",
                   s_puae_log[i].frame, pc, cc);
    }

    /* Dump trace for the entire $86CC gameplay copper list region so we can
     * see exactly what writes what values there on the PC side. */
    printf("\n[trace] PC writes to gameplay copper list ($86CC-$88CC):\n");
    trace_dump_range(stdout, 0x86CCu, 0x88CCu, 64);
    const FrameState *p = &s_puae_log[first_diff];
    const FrameState *c = &s_pc_log[first_diff];
    printf("  cop1lc  PUAE=$%06X  PC=$%06X  %s\n",
           p->cop1lc, c->cop1lc, p->cop1lc == c->cop1lc ? "OK" : "DIFF <<<");
    printf("  bplcon0 PUAE=$%04X  PC=$%04X  %s\n",
           p->bplcon0, c->bplcon0, p->bplcon0 == c->bplcon0 ? "OK" : "DIFF <<<");
    if (p->bplcon1 != c->bplcon1)
        printf("  bplcon1 PUAE=$%04X  PC=$%04X  DIFF <<<\n", p->bplcon1, c->bplcon1);
    if (p->bplcon2 != c->bplcon2)
        printf("  bplcon2 PUAE=$%04X  PC=$%04X  DIFF <<<\n", p->bplcon2, c->bplcon2);
    if (p->diwstrt != c->diwstrt || p->diwstop != c->diwstop)
        printf("  diw PUAE=$%04X/$%04X  PC=$%04X/$%04X  DIFF <<<\n",
               p->diwstrt, p->diwstop, c->diwstrt, c->diwstop);
    if (p->ddfstrt != c->ddfstrt || p->ddfstop != c->ddfstop)
        printf("  ddf PUAE=$%04X/$%04X  PC=$%04X/$%04X  DIFF <<<\n",
               p->ddfstrt, p->ddfstop, c->ddfstrt, c->ddfstop);

    for (int i = 0; i < 6; i++) {
        if (p->bplpt[i] != c->bplpt[i])
            printf("  bplpt[%d] PUAE=$%06X  PC=$%06X  DIFF <<<\n",
                   i, p->bplpt[i], c->bplpt[i]);
    }

    for (int i = 0; i < 8; i++) {
        if (p->sprpt[i] != c->sprpt[i])
            printf("  sprpt[%d] PUAE=$%06X  PC=$%06X  DIFF <<<\n",
                   i, p->sprpt[i] & 0xFFFFFFu, c->sprpt[i] & 0xFFFFFFu);
    }

    int pal_diffs = 0;
    for (int i = 0; i < 32; i++)
        if (p->palette[i] != c->palette[i]) pal_diffs++;
    if (pal_diffs) {
        printf("  palette: %d of 32 entries differ\n", pal_diffs);
        for (int i = 0; i < 32; i++) {
            if (p->palette[i] != c->palette[i])
                printf("    COLOR%02d PUAE=$%04X  PC=$%04X\n", i, p->palette[i], c->palette[i]);
        }
    }

    /* Print coplist word diffs — the authoritative divergence indicator */
    if (p->coplist_valid && c->coplist_valid && p->cop1lc == c->cop1lc) {
        int ndiff = 0;
        for (int i = 0; i + 1 < HARNESS_COPLIST_WORDS; i += 2) {
            uint16_t pw1 = p->coplist[i], pw2 = p->coplist[i+1];
            uint16_t cw1 = c->coplist[i], cw2 = c->coplist[i+1];
            if (pw1 == cw1 && pw2 == cw2) continue;
            if (!(pw1 & 1)) {
                uint16_t reg = pw1 & 0x1FE;
                const char *name = reg_name(reg);
                if (name)
                    printf("  cop[%03d] %s: PUAE=$%04X  PC=$%04X\n",
                           i/2, name, pw2, cw2);
                else
                    printf("  cop[%03d] DFF%03X: PUAE=$%04X  PC=$%04X\n",
                           i/2, reg, pw2, cw2);
            } else {
                printf("  cop[%03d] WAIT/SKIP: PUAE=%04X/%04X  PC=%04X/%04X\n",
                       i/2, pw1, pw2, cw1, cw2);
            }
            if (++ndiff >= 32) {
                printf("  ... (truncated at 32 diffs)\n");
                break;
            }
        }
        if (ndiff == 0) printf("  [coplist content matches]\n");
    } else if (!p->coplist_valid || !c->coplist_valid) {
        printf("  [coplist not captured: PUAE valid=%d, PC valid=%d]\n",
               p->coplist_valid, c->coplist_valid);
    }

    /* Audio channel diffs — compare the full Paula state per channel
     * (sample pointer, length, period, volume), not just volume. */
    for (int i = 0; i < 4; i++) {
        if (p->audio[i].lc  != c->audio[i].lc)
            printf("  audio[%d].lc  PUAE=$%06X PC=$%06X  DIFF <<<\n",
                   i, p->audio[i].lc, c->audio[i].lc);
        if (p->audio[i].len != c->audio[i].len)
            printf("  audio[%d].len PUAE=$%04X  PC=$%04X  DIFF <<<\n",
                   i, p->audio[i].len, c->audio[i].len);
        if (p->audio[i].per != c->audio[i].per)
            printf("  audio[%d].per PUAE=$%04X  PC=$%04X  DIFF <<<\n",
                   i, p->audio[i].per, c->audio[i].per);
        if (p->audio[i].vol != c->audio[i].vol)
            printf("  audio[%d].vol PUAE=$%04X  PC=$%04X  DIFF <<<\n",
                   i, p->audio[i].vol, c->audio[i].vol);
    }

    compare_framebuffers(first_diff);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Bitplane data diff: compare chip RAM at BPL1-4 addresses */

/* Extract a 24-bit Amiga pointer from consecutive BPLxPTH/BPLxPTL MOVE
 * instructions in a copper list.  reg_h is the high-word register offset
 * (e.g. 0x0E0 for BPL1PTH).  Returns -1 if not found.
 * cop_bpl_ptr       — returns LAST occurrence (final value after all writes)
 * cop_bpl_ptr_first — returns FIRST occurrence (value used by renderer at
 *                     snap_line, before any mid-list pointer updates) */
static int32_t cop_bpl_ptr(const uint16_t *coplist, int nwords, uint16_t reg_h)
{
    uint16_t reg_l = reg_h + 2;
    int got_h = 0, got_l = 0;
    uint16_t hi = 0, lo = 0;
    for (int i = 0; i + 1 < nwords; i += 2) {
        uint16_t inst = coplist[i];
        if (inst == 0xFFFF) break;
        if (inst & 1) continue;  /* WAIT */
        uint16_t reg = inst & 0x1FEu;
        if (reg == reg_h) { hi = coplist[i+1]; got_h = 1; }
        if (reg == reg_l) { lo = coplist[i+1]; got_l = 1; }
    }
    if (!got_h || !got_l) return -1;
    return ((uint32_t)hi << 16) | lo;
}

static int32_t cop_bpl_ptr_first(const uint16_t *coplist, int nwords, uint16_t reg_h)
{
    uint16_t reg_l = reg_h + 2;
    int got_h = 0, got_l = 0;
    uint16_t hi = 0, lo = 0;
    for (int i = 0; i + 1 < nwords; i += 2) {
        uint16_t inst = coplist[i];
        if (inst == 0xFFFF) break;
        if (inst & 1) continue;
        uint16_t reg = inst & 0x1FEu;
        if (reg == reg_h && !got_h) { hi = coplist[i+1]; got_h = 1; }
        if (reg == reg_l && !got_l) { lo = coplist[i+1]; got_l = 1; }
        if (got_h && got_l) break;
    }
    if (!got_h || !got_l) return -1;
    return ((uint32_t)hi << 16) | lo;
}

/* Full chip RAM diff: report every contiguous range that differs. */
static void compare_chipram_full(void)
{
    if (!s_puae_chipram_valid || !s_pc_chipram_valid) return;

    int total_diff = 0, n_ranges = 0;
    int range_start = -1, range_count = 0;

    for (int i = 0; i <= CHIP_RAM_SIZE; i++) {
        int differs = (i < CHIP_RAM_SIZE) &&
                      (s_puae_chipram_snap[i] != s_pc_chipram_snap[i]);
        if (differs) {
            if (range_start < 0) { range_start = i; range_count = 0; }
            range_count++;
            total_diff++;
        } else if (range_start >= 0) {
            if (n_ranges < 64) {
                int end = range_start + range_count - 1;
                printf("[chipram-diff]   $%06X–$%06X (%d byte%s)  PUAE=%02X  PC=%02X\n",
                       range_start, end, range_count, range_count > 1 ? "s" : "",
                       s_puae_chipram_snap[range_start],
                       s_pc_chipram_snap[range_start]);
            } else if (n_ranges == 64) {
                printf("[chipram-diff]   ... (truncated at 64 ranges)\n");
            }
            n_ranges++;
            range_start = -1;
        }
    }

    if (total_diff == 0)
        printf("[chipram-diff] PERFECT MATCH (%d bytes)\n", CHIP_RAM_SIZE);
    else
        printf("[chipram-diff] %d byte%s differ in %d range%s\n",
               total_diff, total_diff > 1 ? "s" : "",
               n_ranges,  n_ranges  > 1 ? "s" : "");
}

static int16_t cop_reg16(const uint16_t *coplist, int nwords, uint16_t reg_want)
{
    for (int i = 0; i + 1 < nwords; i += 2) {
        uint16_t inst = coplist[i];
        if (inst == 0xFFFF) break;
        if (inst & 1) continue;
        if ((inst & 0x1FEu) == reg_want) return (int16_t)coplist[i+1];
    }
    return 0;
}

void compare_bitplanes(const FrameState *p)
{
    if (!s_puae_chipram_valid || !s_pc_chipram_valid) {
        printf("[bpl-diff] chip RAM snapshots not available\n");
        return;
    }
    if (!p->coplist_valid) {
        printf("[bpl-diff] PUAE copper list not captured\n");
        return;
    }

    /* Pre-render BPL CRC: compare what the PC renderer actually saw vs what
     * PUAE's renderer saw (approximated by PUAE post-frame BPL CRC).
     * A mismatch here means the blitter skip in native_sprite_blitter_setup
     * caused BPL data to differ at render time even if post-frame state matches. */
    if (s_pc_prerender_bpl_crc_valid) {
        uint32_t puae_crc = p->bpl_data_crc;
        printf("[bpl-prerender] PC pre-render BPL CRC=$%08X  PUAE post-frame BPL CRC=$%08X  %s\n",
               s_pc_prerender_bpl_crc, puae_crc,
               s_pc_prerender_bpl_crc == puae_crc ? "MATCH" : "DIFF ← blitter skip suspect");
    }

    /* BPL1–BPL4 high-word register offsets */
    static const uint16_t bpl_regs_h[4] = { 0x0E0, 0x0E4, 0x0E8, 0x0EC };

    int16_t bpl1mod = cop_reg16(p->coplist, HARNESS_COPLIST_WORDS, 0x108);
    int16_t bpl2mod = cop_reg16(p->coplist, HARNESS_COPLIST_WORDS, 0x10A);

    /* Fetch stride: 10 words = 20 bytes per scanline (standard for this game).
     * BPL1MOD applied after each odd-plane line, BPL2MOD after even-plane line. */
    const int fetch_bytes = 20;
    int stride[4];
    for (int pl = 0; pl < 4; pl++)
        stride[pl] = fetch_bytes + (int)((pl % 2 == 0) ? bpl1mod : bpl2mod);

    const int height = 256;

    printf("[bpl-diff] BPL1MOD=%d BPL2MOD=%d  fetch=%d bytes/line  height=%d\n",
           (int)bpl1mod, (int)bpl2mod, fetch_bytes, height);

    /* Check BOTH the first AND last copper BPL pointer for each plane.
     * The first pointer is what the renderer uses at snap_line (early copper
     * section, before any mid-list WAIT+pointer updates); the last pointer is
     * what conventional cop_bpl_ptr returns and may be a different address
     * (e.g. late section $04F6F4 vs early section $025334 on the title screen). */
    for (int pass = 0; pass < 2; pass++) {
        const char *pass_label = pass ? "last-ptr" : "first-ptr";
        for (int pl = 0; pl < 4; pl++) {
            int32_t base32 = pass
                ? cop_bpl_ptr      (p->coplist, HARNESS_COPLIST_WORDS, bpl_regs_h[pl])
                : cop_bpl_ptr_first(p->coplist, HARNESS_COPLIST_WORDS, bpl_regs_h[pl]);
            if (base32 < 0) {
                if (pass == 0)
                    printf("[bpl-diff] BPL%d (%s): not found in copper list\n", pl + 1, pass_label);
                continue;
            }
            uint32_t base = (uint32_t)base32;

            /* Skip duplicate: if first == last, only report once */
            if (pass == 1) {
                int32_t first32 = cop_bpl_ptr_first(p->coplist, HARNESS_COPLIST_WORDS, bpl_regs_h[pl]);
                if (first32 == base32) continue;
            }

            int scan_bytes = stride[pl] > 0 ? stride[pl] * height : fetch_bytes * height;
            if ((int64_t)base + scan_bytes > CHIP_RAM_SIZE)
                scan_bytes = (int)(CHIP_RAM_SIZE - base);

            int first_off = -1, n_diff = 0;
            for (int j = 0; j < scan_bytes; j++) {
                if (s_puae_chipram_snap[base + j] != s_pc_chipram_snap[base + j]) {
                    if (first_off < 0) first_off = j;
                    n_diff++;
                }
            }

            if (first_off < 0) {
                printf("[bpl-diff] BPL%d (%s) @ $%06X–$%06X: MATCH\n",
                       pl + 1, pass_label, base, base + (uint32_t)scan_bytes - 1);
            } else {
                uint32_t da = base + (uint32_t)first_off;
                int safe_stride = stride[pl] > 0 ? stride[pl] : fetch_bytes;
                int line = first_off / safe_stride;
                int col  = first_off % safe_stride;
                printf("[bpl-diff] BPL%d (%s) @ $%06X: first diff +%d ($%06X) line=%d col=%d  "
                       "PUAE=%02X PC=%02X  (%d/%d bytes differ)\n",
                       pl + 1, pass_label, base, first_off, da, line, col,
                       s_puae_chipram_snap[da], s_pc_chipram_snap[da],
                       n_diff, scan_bytes);
                printf("[bpl-diff]   PUAE:");
                for (int k = 0; k < 8 && (da + k) < (uint32_t)CHIP_RAM_SIZE; k++)
                    printf(" %02X", s_puae_chipram_snap[da + k]);
                printf("\n[bpl-diff]   PC  :");
                for (int k = 0; k < 8 && (da + k) < (uint32_t)CHIP_RAM_SIZE; k++)
                    printf(" %02X", s_pc_chipram_snap[da + k]);
                printf("\n");

                printf("[bpl-trace] Writes near first diff for BPL%d (%s) ($%06X-$%06X):\n",
                       pl + 1, pass_label,
                       da >= 0x10u ? da - 0x10u : 0u,
                       da + 0x10u);
                trace_dump_range(stdout,
                                 da >= 0x10u ? da - 0x10u : 0u,
                                 da + 0x11u,
                                 32);
            }
        }
    }

    /* Full chip RAM diff: show every differing region, not just BPL1-4. */
    printf("[chipram-diff] Full chip RAM diff (post-frame):\n");
    compare_chipram_full();

    /* Save both snapshots so the Python analysis script can read them */
    {
        FILE *fp;
        fp = fopen("logs/harness_puae_after_frame.bin", "wb");
        if (fp) { fwrite(s_puae_chipram_snap, 1, CHIP_RAM_SIZE, fp); fclose(fp); }
        fp = fopen("logs/harness_pc_after_frame.bin", "wb");
        if (fp) { fwrite(s_pc_chipram_snap, 1, CHIP_RAM_SIZE, fp); fclose(fp); }
        printf("[bpl-diff] Saved chip RAM snapshots → logs/harness_puae_after_frame.bin  logs/harness_pc_after_frame.bin\n");
    }
}

