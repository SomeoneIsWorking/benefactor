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

#include "engine/hw_private.h"
#ifdef HARNESS_BUILD
#include "harness/trace.h"
#endif
#include <stdlib.h>

/* Helper: build a 24-bit pointer from a pair of shadow registers */
static inline uint32_t _bplptr_from(int hi_reg, int lo_reg)
{
    return (((uint32_t)s_regs[hi_reg >> 1] << 16) | s_regs[lo_reg >> 1]) & 0xFFFFFF;
}

/* ── Object-blit capture (consumed by native_render_wide_bg) ───────────────────
 * Double-buffered with a 1-FRAME DELAY: the engine blits this frame's objects into
 * the BACK page, which only becomes the displayed (front) page NEXT frame — while the
 * renderer's bg (and s_fb) shows the current front page = LAST frame's objects. So the
 * renderer replays the PREVIOUS frame's capture (s_prev), which lives in the now-
 * displayed buffer; the current frame fills s_blitcap, promoted to s_prev at reset. */
#define BLIT_CAP_MAX 768
static BlitRec s_blitcap[BLIT_CAP_MAX];   /* current frame, being filled */
static int     s_blitcap_n  = 0;
static BlitRec s_prev[BLIT_CAP_MAX];      /* previous frame, replayed by the renderer */
static int     s_prev_n     = 0;
static int     s_blitcap_on = 1;

/* REPL-readable per-frame log of EVERY blit + the capture decision/reason — the in-REPL
 * replacement for the old BLIT_LOG env file, so divergences can be root-caused live (e.g.
 * "why was this blit not captured?"). Double-buffered like s_blitcap so a STEP_PC leaves
 * the just-rendered frame's log readable. reason: 'C'=captured, 'u'=dest-disabled,
 * 'p'=dest-not-in-pages, 'g'=gfx-src-too-high, 'o'=capture-off, 'f'=buffer-full. */
typedef struct { uint32_t apt,bpt,cpt,dpt; uint16_t con0,con1; int w,h; char reason;
                 uint32_t fn; /* g_rt_last_call: recompiled fn that issued the blit */ } BlitLogRec;
static BlitLogRec s_blitlog[BLIT_CAP_MAX];      int s_blitlog_n = 0;
static BlitLogRec s_blitlog_prev[BLIT_CAP_MAX]; int s_blitlog_prev_n = 0;
int               hw_blitlog_count(void)        { return s_blitlog_prev_n; }
const BlitLogRec *hw_blitlog_recs(void)         { return s_blitlog_prev; }

void hw_blit_capture_reset(void)          /* end of frame: promote current → prev */
{
    memcpy(s_prev, s_blitcap, sizeof(BlitRec) * (size_t)s_blitcap_n);
    s_prev_n = s_blitcap_n;
    s_blitcap_n = 0;
    memcpy(s_blitlog_prev, s_blitlog, sizeof(BlitLogRec) * (size_t)s_blitlog_n);
    s_blitlog_prev_n = s_blitlog_n;
    s_blitlog_n = 0;
}
int            hw_blit_capture_count(void) { return s_prev_n; }
const BlitRec *hw_blit_capture_recs(void)  { return s_prev; }

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

/* Data registers + BLTCON1 mode bits (line + fill), mirroring PUAE. */
#define _BLTCDAT  0x070
#define _BLTBDAT  0x072
#define _BLTADAT  0x074
#define BLT1_LINE 0x0001   /* line mode */
#define BLT1_SING 0x0002   /* single dot per line */
#define BLT1_AUL  0x0004
#define BLT1_SUL  0x0008
#define BLT1_SUD  0x0010
#define BLT1_FCI  0x0004   /* area mode: fill carry-in (same bit as AUL in line mode) */
#define BLT1_IFE  0x0008   /* area mode: inclusive fill enable */
#define BLT1_EFE  0x0010   /* area mode: exclusive fill enable */
#define BLT1_SIGN 0x0040
#define BLT1_DESC 0x0002   /* area mode: descending */

static inline void _mem16_w(uint32_t a, uint16_t v)
{
    if (a + 1 < RT_MEM_SIZE) { g_mem[a] = (uint8_t)(v >> 8); g_mem[a + 1] = (uint8_t)v; }
}

/* Evaluate the 8-bit minterm logic function LF on per-bit (A,B,C). */
static inline uint16_t _minterm(uint16_t a, uint16_t b, uint16_t c, uint8_t lf)
{
    uint16_t d = 0;
    for (int bit = 0; bit < 16; bit++) {
        int idx = ((a >> (15 - bit)) & 1) << 2 |
                  ((b >> (15 - bit)) & 1) << 1 |
                  ((c >> (15 - bit)) & 1);
        if ((lf >> idx) & 1) d |= (uint16_t)(1 << (15 - bit));
    }
    return d;
}

/* Area-fill lookup: blit_filltable[byte][mode][0]=filled byte, [1]=carry-out.
 * mode bit0 = carry-in, mode bit1 = inclusive(1)/exclusive(0). Built once,
 * identical to PUAE's build_blitfilltable. */
static uint8_t s_filltable[256][4][2];
static int s_filltable_ready = 0;
static void _build_filltable(void)
{
    if (s_filltable_ready) return;
    s_filltable_ready = 1;
    for (unsigned d = 0; d < 256; d++)
        for (int i = 0; i < 4; i++) {
            int fc = i & 1;
            unsigned data = d;
            for (unsigned m = 1; m != 0x100; m <<= 1) {
                unsigned tmp = data;
                if (fc) { if (i & 2) data |= m; else data ^= m; }
                if (tmp & m) fc = !fc;
            }
            s_filltable[d][i][0] = (uint8_t)data;
            s_filltable[d][i][1] = (uint8_t)fc;
        }
}

/* Amiga OCS blitter LINE mode (Bresenham), ported from PUAE blitter.c
 * (actually_do_blit line path). Draws a 1px line; the game uses it for the
 * chandelier chains and similar. C is the read/write channel (D has no effect
 * on line draw), BLTAPT is the error accumulator (not a pointer), BLTADAT is
 * the single pen bit ($8000). */
static void hw_do_line(uint16_t bltcon0, uint16_t bltcon1, uint16_t bltsize)
{
    int length = (bltsize >> 6) ? (bltsize >> 6) : 1024;
    uint32_t cpt   = _bplptr_from(_BLTCPTH, _BLTCPTL) & ~1u;
    int16_t  cmod  = (int16_t)s_regs[_BLTCMOD >> 1];
    int16_t  amod  = (int16_t)s_regs[_BLTAMOD >> 1];
    int16_t  bmod  = (int16_t)s_regs[_BLTBMOD >> 1];
    int32_t  apt   = (int16_t)s_regs[_BLTAPTL >> 1];   /* accumulator (low word, signed) */
    uint16_t adat  = s_regs[_BLTADAT >> 1];
    uint16_t bdat  = s_regs[_BLTBDAT >> 1];
    uint16_t afwm  = s_regs[_BLTAFWM >> 1];
    uint8_t  lf    = (uint8_t)(bltcon0 & 0xFF);
    int      usec  = (bltcon0 >> 9) & 1;
    int      sing  = (bltcon1 & BLT1_SING) != 0;
    int      sign  = (bltcon1 & BLT1_SIGN) != 0;
    int      ashift = (bltcon0 >> 12) & 0xF;
    int      bshift = (bltcon1 >> 12) & 0xF;
    uint16_t blineb = (uint16_t)((bdat >> bshift) | (bdat << (16 - bshift)));
    int      onedot = 0;

    for (int i = 0; i < length; i++) {
        apt += sign ? (int16_t)bmod : (int16_t)amod;

        int draw = !sing || !onedot;
        onedot = 1;
        if (draw && usec) {
            uint16_t c     = _mem16(cpt);
            uint16_t ahold = (uint16_t)((adat & afwm) >> ashift);
            uint16_t bbit  = (blineb & 1) ? 0xFFFF : 0;
            _mem16_w(cpt, _minterm(ahold, bbit, c, lf));
        }

        /* X movement (octant) — mirrors PUAE proc_cpt_x. incx/decx wrap ASH and
         * step the word pointer when it crosses a word boundary. */
        if ((!sign && !(bltcon1 & BLT1_SUD)) || (bltcon1 & BLT1_SUD)) {
            int dec = (!sign && !(bltcon1 & BLT1_SUD)) ? (bltcon1 & BLT1_SUL)
                                                       : (bltcon1 & BLT1_AUL);
            if (dec) { if (ashift == 0)  cpt -= 2; ashift = (ashift - 1) & 15; }
            else     { if (ashift == 15) cpt += 2; ashift = (ashift + 1) & 15; }
        }
        /* Y movement (octant) — mirrors PUAE proc_cpt_y. */
        if ((!sign && (bltcon1 & BLT1_SUD)) || !(bltcon1 & BLT1_SUD)) {
            int dec = (!sign && (bltcon1 & BLT1_SUD)) ? (bltcon1 & BLT1_SUL)
                                                      : (bltcon1 & BLT1_AUL);
            cpt = (uint32_t)((int32_t)cpt + (dec ? -cmod : cmod));
            onedot = 0;
        }

        sign = ((int16_t)apt) < 0;
        bshift = (bshift - 1) & 15;
        blineb = (uint16_t)((bdat >> bshift) | (bdat << (16 - bshift)));
    }

    /* Auto-advance C/D pointers to where the line ended (channel chaining). */
    s_regs[_BLTCPTH >> 1] = (uint16_t)((cpt >> 16) & 0xFFFF);
    s_regs[_BLTCPTL >> 1] = (uint16_t)(cpt & 0xFFFF);
    s_regs[_BLTDPTH >> 1] = (uint16_t)((cpt >> 16) & 0xFFFF);
    s_regs[_BLTDPTL >> 1] = (uint16_t)(cpt & 0xFFFF);
    /* Carry ASH/BSH/SIGN back so chained line segments continue cleanly. */
    s_regs[_BLTCON0 >> 1] = (uint16_t)((bltcon0 & 0x0FFF) | (ashift << 12));
    uint16_t nc1 = (uint16_t)((bltcon1 & 0x0FFF) | (bshift << 12));
    if (sign) nc1 |= BLT1_SIGN; else nc1 &= (uint16_t)~BLT1_SIGN;
    s_regs[_BLTCON1 >> 1] = nc1;
    s_blt_bzero = 1;
}

/* ── Main blit engine ───────────────────────────────────────────────────── */

/* DIAGNOSTIC ablation: when set (via REPL `blitskip <fn>`), drop every blit whose
 * issuing routine (g_rt_last_call) matches — used to confirm which fn draws a given
 * on-screen sprite (e.g. the Marry Man) by watching it vanish from the page render. */
uint32_t g_blit_skip_fn = 0;

void hw_do_blit(void)
{
    _trace_init();

    if (g_blit_skip_fn) {
        extern volatile uint32_t g_rt_last_call;
        if ((g_rt_last_call & 0xFFFFFFu) == (g_blit_skip_fn & 0xFFFFFFu)) return;
    }

    uint16_t bltcon0 = s_regs[_BLTCON0 >> 1];
    uint16_t bltcon1 = s_regs[_BLTCON1 >> 1];
    uint16_t bltsize = s_regs[_BLTSIZE >> 1];

    /* LINE mode (BLTCON1 bit0) is a completely different engine (Bresenham). */
    if (bltcon1 & BLT1_LINE) { hw_do_line(bltcon0, bltcon1, bltsize); return; }

    /* Area FILL mode (BLTCON1 bit3=inclusive / bit4=exclusive). Carry-in = bit2. */
    int fill_inclusive = (bltcon1 & BLT1_IFE) != 0;
    int fill_exclusive = (bltcon1 & BLT1_EFE) != 0;
    int fill_on        = fill_inclusive || fill_exclusive;
    int fill_fci       = (bltcon1 & BLT1_FCI) != 0;
    if (fill_on) _build_filltable();
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

    /* ── Object-blit capture for the native widescreen renderer ──────────────
     * Record blits that draw object sprites into the playfield pages so the wide
     * renderer can re-draw them itself. Object gfx lives low ($0xxxxx); the tile
     * scroll sources from the high tile-gfx region ($5Exxxx) — exclude those. */
    {
        int usea = (bltcon0 >> 11) & 1, useb = (bltcon0 >> 10) & 1, used = (bltcon0 >> 8) & 1;
        uint32_t gsrc = useb ? bpt : apt;            /* gfx source: B if masked, else A */
        /* Playfield double-buffer pages: $2B3EC (5 planes to ~$36xxx) and $38628 (5
         * planes to ~$43xxx, stepping by the plane stride $2A0C). Must cover all 5
         * planes of both buffers (else sprites lose bitplanes → wrong colours) but stop
         * below the bg save-buffer ($045000+), which is page↔save shuffle, not sprites. */
        int dest_in_pages = (dpt >= 0x028000u && dpt < 0x044000u);
        char why = !s_blitcap_on ? 'o' : !used ? 'u' : !dest_in_pages ? 'p'
                 : !(gsrc < 0x400000u) ? 'g' : (s_blitcap_n >= BLIT_CAP_MAX) ? 'f' : 'C';
        if (s_blitlog_n < BLIT_CAP_MAX) {
            BlitLogRec *L = &s_blitlog[s_blitlog_n++];
            L->apt=apt; L->bpt=bpt; L->cpt=cpt; L->dpt=dpt; L->con0=bltcon0; L->con1=bltcon1;
            L->w=width_words; L->h=height; L->reason=why;
            { extern volatile uint32_t g_rt_last_call; L->fn = g_rt_last_call; }
        }
        if (s_blitcap_on && used && dest_in_pages && gsrc < 0x400000u
            && s_blitcap_n < BLIT_CAP_MAX) {
            BlitRec *r = &s_blitcap[s_blitcap_n++];
            r->src   = gsrc;
            r->mask  = (useb && usea) ? apt : 0u;    /* cookie-cut mask = A channel */
            r->dpt   = dpt;
            r->w     = width_words;
            r->h     = height;
            r->smod  = useb ? bmod : amod;
            r->mmod  = amod;
            r->shift = (bltcon0 >> 12) & 0xF;
            r->con0  = bltcon0;
        }
    }

    /* BLIT_LOG: append every blit (source/dest/dims) to logs/blit_log.txt so we
     * can see the full set of graphics PC actually blits during gameplay and
     * compare it to the level object records' gfx pointers (an object whose gfx
     * is never a blit source isn't being drawn). */
    if (getenv("BLIT_LOG")) {
        static FILE *bl = NULL; static long n = 0;
        if (!bl) bl = fopen("logs/blit_log.txt", "w");
        extern volatile uint32_t g_rt_last_call;
        if (bl) { fprintf(bl, "%ld apt=%06X bpt=%06X cpt=%06X dpt=%06X w=%d h=%d con0=%04X con1=%04X amod=%d bmod=%d cmod=%d dmod=%d afwm=%04X alwm=%04X fn=%06X\n",
                          n++, apt, bpt, cpt, dpt, width_words, height, bltcon0, bltcon1, amod, bmod, cmod, dmod, afwm, alwm, (unsigned)g_rt_last_call); fflush(bl); }
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
        int fc = fill_fci;   /* area-fill carry resets at the start of every row */

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

            /* Area FILL: propagate a carry through each word (low byte then high,
             * LSB→MSB), resetting per row. Software always fills in descending
             * mode, so x ascending here walks the row right→left as required. */
            if (fill_on) {
                int incl = fill_inclusive ? 2 : 0;
                int il = (fc ? 1 : 0) | incl;
                uint8_t flo = s_filltable[d & 0xFF][il][0]; fc = s_filltable[d & 0xFF][il][1];
                int ih = (fc ? 1 : 0) | incl;
                uint8_t fhi = s_filltable[(d >> 8) & 0xFF][ih][0]; fc = s_filltable[(d >> 8) & 0xFF][ih][1];
                d = (uint16_t)((fhi << 8) | flo);
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
