/*
 * recomp/hw_blitter.c  –  Amiga OCS blitter simulation
 *
 * Implements hw_do_blit(), triggered by BLTSIZE writes in hw.c.
 * Evaluates minterm logic functions and copies/fills chip RAM regions.
 *
 * A-shift register note: PUAE resets the A shift register to 0 at every blit
 * start (blitter_start_init), so it does NOT persist across blits. PC matches
 * that (reset per blit) by default. BLIT_CARRY=1 re-enables cross-blit
 * persistence for comparison testing only.
 *
 * Trace facility: set BLIT_WATCH=<hex_addr> to write blit_trace_pc.txt
 * logging every blit that touches that address.
 */

#include "hw_private.h"
#ifdef HARNESS_BUILD
#include "harness/trace.h"
#endif
#include <stdlib.h>

/* Helper: build a 24-bit pointer from a pair of shadow registers */
static inline uint32_t _bplptr_from(int hi_reg, int lo_reg)
{
    return (((uint32_t)s_regs[hi_reg >> 1] << 16) | s_regs[lo_reg >> 1]) & 0xFFFFFF;
}

/* OCS register offsets (local copies of hw.c defines) */
#define _BLTCON0  0x040
#define _BLTCON1  0x042
#define _BLTSIZE  0x058
#define _BLTDPTH  0x054
#define _BLTDPTL  0x056
#define _BLTAPTH  0x050
#define _BLTAPTL  0x052
#define _BLTBPTH  0x04C
#define _BLTBPTL  0x04E
#define _BLTCPTH  0x048
#define _BLTCPTL  0x04A
#define _BLTDMOD  0x066
#define _BLTAMOD  0x064
#define _BLTBMOD  0x062
#define _BLTCMOD  0x060
#define _BLTAFWM  0x044
#define _BLTALWM  0x046

/* PUAE resets the A-shift register to 0 at every blit start (blitter.c:2003,
 * blitter_start_init: blt_info.bltaold = 0) — it does NOT persist the A shift
 * register across blits. The old "OCS carries across blits" assumption was
 * WRONG and corrupted the title-text compositing, so the A register starts at
 * 0 every blit (no carry, matching PUAE). */

/* ── Trace facility ─────────────────────────────────────────────────────── */

static uint32_t  s_watch_addr  = 0xFFFFFFFF;
static FILE     *s_trace_log   = NULL;
static int       s_blit_serial = 0;
static int       s_trace_ready = 0;

static void _trace_init(void)
{
    if (s_trace_ready) return;
    s_trace_ready = 1;
    const char *env = getenv("BLIT_WATCH");
    if (!env) return;
    s_watch_addr = (uint32_t)strtoul(env, NULL, 16);
    s_trace_log  = fopen("blit_trace_pc.txt", "w");
    if (s_trace_log)
        fprintf(s_trace_log,
                "# PC blit trace — watching $%06X\n"
                "# cols: serial frame dpt apt cpt con0 con1 bltsize afwm alwm "
                "amod bmod cmod dmod prev_a_at_x0 watch_before watch_after\n",
                s_watch_addr);
}

static inline uint16_t _mem16(uint32_t a)
{
    if (a + 1 < RT_MEM_SIZE)
        return (uint16_t)((g_mem[a] << 8) | g_mem[a + 1]);
    return 0;
}

/* ── Main blit engine ───────────────────────────────────────────────────── */

void hw_do_blit(void)
{
    _trace_init();

    uint16_t bltcon0 = s_regs[_BLTCON0 >> 1];
    uint16_t bltcon1 = s_regs[_BLTCON1 >> 1];
    uint16_t bltsize = s_regs[_BLTSIZE >> 1];
    uint16_t afwm    = s_regs[_BLTAFWM >> 1];
    uint16_t alwm    = s_regs[_BLTALWM >> 1];

    int width_words  = (bltsize & 0x3F) ? (bltsize & 0x3F) : 64;
    int height       = (bltsize >> 6)  ? (bltsize >> 6)  : 1024;

    /* The OCS blitter is WORD-addressed: it ignores bit 0 of the channel
     * pointers (chip-RAM DMA is word-granular). Game code can hand it a byte-odd
     * pointer and expect it word-aligned — e.g. the title-car bob uses
     * dpt = base + (carX>>3) (a *byte* offset that is often odd) and puts the
     * sub-word position in BLTCON1 BSH; on hardware the odd byte is dropped and
     * BSH (0-15) supplies the fine X, giving smooth motion. Honoring the odd
     * byte here would shift the bob 8px on odd frames (the car "stutter"). Mask
     * bit 0 to match the hardware. */
    uint32_t apt = _bplptr_from(_BLTAPTH, _BLTAPTL) & ~1u;
    uint32_t bpt = _bplptr_from(_BLTBPTH, _BLTBPTL) & ~1u;
    uint32_t cpt = _bplptr_from(_BLTCPTH, _BLTCPTL) & ~1u;
    uint32_t dpt = _bplptr_from(_BLTDPTH, _BLTDPTL) & ~1u;


    int16_t amod = (int16_t)s_regs[_BLTAMOD >> 1];
    int16_t bmod = (int16_t)s_regs[_BLTBMOD >> 1];
    int16_t cmod = (int16_t)s_regs[_BLTCMOD >> 1];
    int16_t dmod = (int16_t)s_regs[_BLTDMOD >> 1];

    /* BLIT_LOG: append every blit (source/dest/dims) to logs/blit_log.txt so we
     * can see the full set of graphics PC actually blits during gameplay and
     * compare it to the level object records' gfx pointers (an object whose gfx
     * is never a blit source isn't being drawn). */
    if (getenv("BLIT_LOG")) {
        static FILE *bl = NULL; static long n = 0;
        if (!bl) bl = fopen("logs/blit_log.txt", "w");
        if (bl) { fprintf(bl, "%ld apt=%06X bpt=%06X cpt=%06X dpt=%06X w=%d h=%d con0=%04X\n",
                          n++, apt, bpt, cpt, dpt, width_words, height, bltcon0); fflush(bl); }
    }

    /* GP_BLIT_TRACE: log destination regions of gameplay blits (diagnostic). */
    if (getenv("GP_BLIT_TRACE")) {
        static int seen[256] = {0};
        uint32_t reg = (dpt >> 16) & 0xFF;
        if (((bltcon0 >> 8) & 1) && !seen[reg]) {
            seen[reg] = 1;
            printf("[gp-blit] dest region $%02X0000 (dpt=$%06X bltsize=%04X)\n",
                   reg, dpt, bltsize);
        }
    }

    int desc  = (bltcon1 >> 1) & 1;   /* BLTCON1 bit1 = DDOWN */
    int use_a = (bltcon0 >> 11) & 1;
    int use_b = (bltcon0 >> 10) & 1;
    int use_c = (bltcon0 >>  9) & 1;
    int use_d = (bltcon0 >>  8) & 1;
    int a_shift = (bltcon0 >> 12) & 0xF;
    int b_shift = (bltcon1 >> 12) & 0xF;
    int dir     = desc ? -1 : 1;            /* descending: walk high→low */

    /* Check if this blit touches the watch address */
    int watch_this = 0;
    uint16_t watch_before = 0, watch_after_val = 0;
    uint16_t prev_a_at_x0 = 0;
    if (s_trace_log && use_d) {
        /* Conservative range check: dpt .. dpt + height*(width_words*2 + |dmod|) */
        int32_t row_span = (int32_t)(width_words * 2) + dmod;
        uint32_t d_end   = (row_span > 0)
                           ? dpt + (uint32_t)(height * row_span)
                           : dpt + (uint32_t)(width_words * 2);
        if (s_watch_addr >= dpt && s_watch_addr < d_end + 2)
            watch_this = 1;
        if (watch_this)
            watch_before = _mem16(s_watch_addr);
    }

    /* A shift register starts at 0 every blit (PUAE resets bltaold per blit). */
    uint16_t blit_init_prev_a = 0;
    uint16_t last_a_raw = blit_init_prev_a; /* updated throughout blit */
    uint16_t last_b = 0;

    int serial = ++s_blit_serial;

    /* Per-blit trace (env BLIT_TRACE_ALL=1) — checksum accumulated only when on,
     * so the native port pays nothing when tracing is disabled. */
    static int s_bta_en = -1;
    static FILE *s_bta_f = NULL;
    if (s_bta_en < 0) {
        s_bta_en = (getenv("BLIT_TRACE_ALL") != NULL) ? 1 : 0;
        if (s_bta_en) {
            const char *d = getenv("BLIT_TRACE_DIR");
            char p[300]; snprintf(p, sizeof p, "%s/pc_blit_trace.txt", d ? d : "logs");
            s_bta_f = fopen(p, "w");
        }
    }
    uint32_t blit_cksum = 0;

    for (int y = 0; y < height; y++) {
        uint16_t prev_a = (y == 0) ? blit_init_prev_a : last_a_raw;
        uint16_t prev_b = (y == 0) ? 0 : last_b;

        for (int x = 0; x < width_words; x++) {
            uint16_t a_raw = 0, b_raw = 0, c = 0, d = 0;
            int32_t wo = dir * (int32_t)(x * 2);   /* forward / backward */

            if (use_a && (int32_t)apt + wo >= 0 && (uint32_t)((int32_t)apt+wo)+1 < RT_MEM_SIZE)
                a_raw = _mem16((uint32_t)((int32_t)apt + wo));
            if (use_b && (int32_t)bpt + wo >= 0 && (uint32_t)((int32_t)bpt+wo)+1 < RT_MEM_SIZE)
                b_raw = _mem16((uint32_t)((int32_t)bpt + wo));
            if (use_c && (int32_t)cpt + wo >= 0 && (uint32_t)((int32_t)cpt+wo)+1 < RT_MEM_SIZE)
                c     = _mem16((uint32_t)((int32_t)cpt + wo));

            uint16_t a_masked = a_raw;
            if (x == 0)                  a_masked &= afwm;
            if (x == width_words - 1)    a_masked &= alwm;

            uint16_t a, b;
            if (desc) {
                a = (uint16_t)((((uint32_t)a_masked << 16) | prev_a) >> (16 - a_shift));
                b = (uint16_t)((((uint32_t)b_raw    << 16) | prev_b) >> (16 - b_shift));
            } else {
                a = (uint16_t)(((uint32_t)prev_a << 16 | a_masked) >> a_shift);
                b = (uint16_t)(((uint32_t)prev_b << 16 | b_raw) >> b_shift);
            }

            if (y == 0 && x == 0) prev_a_at_x0 = prev_a;

            prev_a  = a_masked;  last_a_raw = a_masked;
            prev_b  = b_raw;     last_b     = b_raw;

            /* Evaluate minterm LF = BLTCON0[7:0] */
            uint8_t lf = (uint8_t)(bltcon0 & 0xFF);
            d = 0;
            for (int bit = 0; bit < 16; bit++) {
                int idx = ((a >> (15-bit)) & 1) << 2 |
                          ((b >> (15-bit)) & 1) << 1 |
                          ((c >> (15-bit)) & 1);
                if ((lf >> idx) & 1)
                    d |= (uint16_t)(1 << (15-bit));
            }
            if (s_bta_en) blit_cksum = blit_cksum * 31u + d;

            if (use_d && (int32_t)dpt + wo >= 0 && (uint32_t)((int32_t)dpt+wo)+1 < RT_MEM_SIZE) {
                uint32_t da = (uint32_t)((int32_t)dpt + wo);
                int is_game_copper = (da >= 0x86CCu && da < 0x91D4u);
                if (!is_game_copper) {
                    g_mem[da]     = (uint8_t)(d >> 8);
                    g_mem[da + 1] = (uint8_t)(d & 0xFF);
                }
#ifdef HARNESS_BUILD
                if ((da >= 0x86CCu && da < 0x88CCu) ||
                    (da >= 0x7C80u && da < 0x7CA0u) ||
                    (da >= 0x04F6F4u && da < 0x0546F4u) ||
                    (da >= 0x070930u && da < 0x075930u) ||
                    (da >= 0x04F71Cu && da < 0x05471Cu) ||
                    (da >= 0x025370u && da < 0x02A370u)) {
                    uint16_t old_v = (uint16_t)((g_mem[da] << 8) | g_mem[da + 1]);
                    trace_write_pc(da, old_v, d, 2, 0, 1);
                }
#endif
            }
        }

        if (use_a) apt = (uint32_t)((int32_t)apt + dir*(amod + width_words*2));
        if (use_b) bpt = (uint32_t)((int32_t)bpt + dir*(bmod + width_words*2));
        if (use_c) cpt = (uint32_t)((int32_t)cpt + dir*(cmod + width_words*2));
        if (use_d) dpt = (uint32_t)((int32_t)dpt + dir*(dmod + width_words*2));
    }

    /* Full per-blit trace (env BLIT_TRACE_ALL=1) → logs/pc_blit_trace.txt, for
     * comparison against the PUAE side to find the first diverging blit. */
    {
        if (s_bta_en && s_bta_f) {
            int _bta_frame = 0;
#ifdef HARNESS_BUILD
            extern int g_harness_compared_frame;
            _bta_frame = g_harness_compared_frame;
#endif
            fprintf(s_bta_f, "f=%d ", _bta_frame);
            fprintf(s_bta_f,
                    "dpt=%06X apt=%06X bpt=%06X cpt=%06X con0=%04X con1=%04X size=%04X "
                    "afwm=%04X alwm=%04X carryIn=%04X cksum=%08X\n",
                    _bplptr_from(_BLTDPTH, _BLTDPTL), _bplptr_from(_BLTAPTH, _BLTAPTL),
                    _bplptr_from(_BLTBPTH, _BLTBPTL), _bplptr_from(_BLTCPTH, _BLTCPTL),
                    bltcon0, bltcon1, bltsize, afwm, alwm,
                    blit_init_prev_a, blit_cksum);
        }
    }

    /* Emit trace record if this blit touched the watch address */
    if (watch_this && s_trace_log) {
        watch_after_val = _mem16(s_watch_addr);
        extern int hw_get_frame_num(void);
        fprintf(s_trace_log,
                "%d %d $%06X $%06X $%06X $%04X $%04X $%04X $%04X $%04X "
                "%d %d %d %d $%04X $%04X->$%04X\n",
                serial, hw_get_frame_num(),
                _bplptr_from(_BLTDPTH, _BLTDPTL),
                _bplptr_from(_BLTAPTH, _BLTAPTL),
                _bplptr_from(_BLTCPTH, _BLTCPTL),
                bltcon0, bltcon1, bltsize, afwm, alwm,
                (int)amod, (int)bmod, (int)cmod, (int)dmod,
                prev_a_at_x0,
                watch_before, watch_after_val);
        fflush(s_trace_log);
    }

    /* OCS auto-advances the blitter channel pointers as the blit runs; after
     * completion BLTxPT hold the address just past the last word accessed.
     * Software relies on this to chain back-to-back blits without re-loading
     * the pointers — e.g. the gp cover-art draw ($3424 then $3430 in
     * gfn_gp_003330) writes BLTSIZE twice while only loading BLTAPT/BLTDPT once,
     * expecting the 2nd blit to continue into the bottom half of $49000. The
     * loop above advanced the local apt/bpt/cpt/dpt but left the shadow
     * registers untouched, so the 2nd blit restarted at $49000 and the bottom
     * half stayed black. Write the advanced pointers back so the next blit
     * resumes where this one ended. */
    if (use_a) { s_regs[_BLTAPTH>>1] = (uint16_t)((apt>>16)&0xFFFF); s_regs[_BLTAPTL>>1] = (uint16_t)(apt&0xFFFF); }
    if (use_b) { s_regs[_BLTBPTH>>1] = (uint16_t)((bpt>>16)&0xFFFF); s_regs[_BLTBPTL>>1] = (uint16_t)(bpt&0xFFFF); }
    if (use_c) { s_regs[_BLTCPTH>>1] = (uint16_t)((cpt>>16)&0xFFFF); s_regs[_BLTCPTL>>1] = (uint16_t)(cpt&0xFFFF); }
    if (use_d) { s_regs[_BLTDPTH>>1] = (uint16_t)((dpt>>16)&0xFFFF); s_regs[_BLTDPTL>>1] = (uint16_t)(dpt&0xFFFF); }

    s_blt_bzero = 1;
}
