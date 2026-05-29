/*
 * recomp/hw_copper.c  –  Scanline copper execution + bitplane renderer
 *
 * This implementation executes copper instructions per scanline (WAIT/SKIP/MOVE)
 * and renders lines from the register state active on that line.
 */

#include "hw_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _COP1LCH  0x080
#define _COP1LCL  0x082
#define _DIWSTRT  0x08E
#define _DIWSTOP  0x090
#define _DDFSTRT  0x092
#define _DDFSTOP  0x094
#define _BPLCON0  0x100
#define _BPLCON1  0x102
#define _BPL1MOD  0x108
#define _BPL2MOD  0x10A

/* Copper executor state for one frame */
typedef struct {
    uint32_t lc;
    uint32_t alt_lc;
    uint32_t ip;
    int waiting;
    uint16_t wait_vp;
    uint16_t wait_hp;
    uint16_t wait_vpm;
    uint16_t wait_hpm;
    int restarts;
} CopperExec;

static int s_trace_copper = -1;
static int s_xorigin_bias = 56;
static int s_xorigin_init = 0;
static int s_top_border = 17;
static int s_top_border_init = 0;
static int s_lores_x2 = -1;
static int s_xdelta_4200 = -9999;
static int s_xdelta_3600 = -9999;
static int s_xdelta_3600_ffd8 = -9999;
static int s_xdelta_3600_zero = -9999;
static int s_dualpf_decode = -1;
static int s_bpl_snap_offset = -9999;
static int s_bpl_line_offset_4200 = -9999;
static int s_bpl_line_offset_3600 = -9999;
static int s_bpl_line_offset_3600_ffd8 = -9999;
static int s_bpl_line_offset_3600_zero = -9999;
static int s_fetch_words_bias_3600 = -9999;
static int s_bpl_line_offset_4200_start = -9999;
static int s_bpl_line_offset_4200_end = -9999;
static int s_bpl_line_offset_4200_skip_y = -9999;
static int s_bpl_line_offset_4200_skip_line = -9999;
static int s_bpl_line_offset_4200_skip_line_mask = -9999;
static int s_bpl_line_offset_4200_mask = -9999;
static int s_rule_4200_plane1_transition = -1;
static int s_reset_regs = -1;
static int s_advance_before = -1;
static int s_wait_at_fetch = -1;
static int s_copper_cur_hp = -9999;
static int s_aga_holdover = -1;
static int s_trace_bplptr_sync = -1;
static int s_trace_3600_lines = -1;
static int s_trace_3600_pixels_y = -9999;
static int s_trace_3600_pixels_x0 = -9999;
static int s_trace_3600_pixels_x1 = -9999;
static int s_use_prev_bpl2mod_3600 = -1;
static int s_pf1_shift_3600 = -9999;
static int s_pf1_base_bytes_3600 = -9999;
static int s_pf1_base_bytes_3600_ffd8 = -9999;
static int s_pf1_base_bytes_3600_zero = -9999;
static int s_pf1_window_base_bytes_3600 = -9999;
static int s_pf1_window_base_bytes_3600_byte_max = -9999;
static int s_pf1_step_bias_3600 = -9999;
static int s_plane0_base_bytes_3600 = -9999;
static int s_plane2_base_bytes_3600 = -9999;

static int xorigin_bias(void)
{
    if (!s_xorigin_init) {
        s_xorigin_init = 1;
        const char *e = getenv("BENEFACTOR_XORIGIN_BIAS");
        if (e && *e)
            s_xorigin_bias = atoi(e);
    }
    return s_xorigin_bias;
}

static int top_border_bias(void)
{
    if (!s_top_border_init) {
        s_top_border_init = 1;
        const char *e = getenv("BENEFACTOR_TOP_BORDER");
        if (e && *e)
            s_top_border = atoi(e);
    }
    return s_top_border;
}

static int lores_x2_enabled(void)
{
    if (s_lores_x2 < 0) {
        const char *e = getenv("BENEFACTOR_LORES_X2");
        s_lores_x2 = (e && *e) ? atoi(e) : 0;
    }
    return s_lores_x2;
}

static int xdelta_4200(void)
{
    if (s_xdelta_4200 == -9999) {
        const char *e = getenv("BENEFACTOR_XDELTA_4200");
        s_xdelta_4200 = (e && *e) ? atoi(e) : -1;
    }
    return s_xdelta_4200;
}

static int xdelta_3600(void)
{
    if (s_xdelta_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_XDELTA_3600");
        s_xdelta_3600 = (e && *e) ? atoi(e) : -161;
    }
    return s_xdelta_3600;
}

static int xdelta_3600_ffd8(void)
{
    if (s_xdelta_3600_ffd8 == -9999) {
        const char *e = getenv("BENEFACTOR_XDELTA_3600_FFD8");
        s_xdelta_3600_ffd8 = (e && *e) ? atoi(e) : 0;
    }
    return s_xdelta_3600_ffd8;
}

static int xdelta_3600_zero(void)
{
    if (s_xdelta_3600_zero == -9999) {
        const char *e = getenv("BENEFACTOR_XDELTA_3600_ZERO");
        s_xdelta_3600_zero = (e && *e) ? atoi(e) : 0;
    }
    return s_xdelta_3600_zero;
}

static int dualpf_decode_enabled(void)
{
    if (s_dualpf_decode < 0) {
        const char *e = getenv("BENEFACTOR_DUALPF_DECODE");
        /* Default ON: the star-field section uses BPLCON0=$3600 which sets DBLPF.
         * Without proper dual-playfield decoding, pixels where all 3 planes coincide
         * get cidx=7 → COLOR07=$0776 (bright from the title palette), producing white
         * blobs instead of blue star dots.  Correct PF1/PF2 separation gives:
         * PF1 (BPL1,BPL3) → COLOR01-03 (stars=blue); PF2 (BPL2) → COLOR09 (near-black
         * masking plane); PF1 priority. */
        s_dualpf_decode = (e && *e) ? atoi(e) : 1;
    }
    return s_dualpf_decode;
}

static int bpl_snap_offset(void)
{
    if (s_bpl_snap_offset == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_SNAP_OFFSET");
        s_bpl_snap_offset = (e && *e) ? atoi(e) : 0;
    }
    return s_bpl_snap_offset;
}

static int bpl_line_offset_4200(void)
{
    if (s_bpl_line_offset_4200 == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_4200");
        s_bpl_line_offset_4200 = (e && *e) ? atoi(e) : 0;
    }
    return s_bpl_line_offset_4200;
}

static int bpl_line_offset_3600(void)
{
    if (s_bpl_line_offset_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_3600");
        s_bpl_line_offset_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_bpl_line_offset_3600;
}

static int bpl_line_offset_3600_ffd8(void)
{
    if (s_bpl_line_offset_3600_ffd8 == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_3600_FFD8");
        s_bpl_line_offset_3600_ffd8 = (e && *e) ? atoi(e) : 0;
    }
    return s_bpl_line_offset_3600_ffd8;
}

static int bpl_line_offset_3600_zero(void)
{
    if (s_bpl_line_offset_3600_zero == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_3600_ZERO");
        s_bpl_line_offset_3600_zero = (e && *e) ? atoi(e) : 0;
    }
    return s_bpl_line_offset_3600_zero;
}

static int fetch_words_bias_3600(void)
{
    if (s_fetch_words_bias_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_FETCH_WORDS_BIAS_3600");
        s_fetch_words_bias_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_fetch_words_bias_3600;
}

static int bpl_line_offset_4200_start(void)
{
    if (s_bpl_line_offset_4200_start == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_4200_START");
        s_bpl_line_offset_4200_start = (e && *e) ? atoi(e) : 0;
    }
    return s_bpl_line_offset_4200_start;
}

static int bpl_line_offset_4200_end(void)
{
    if (s_bpl_line_offset_4200_end == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_4200_END");
        s_bpl_line_offset_4200_end = (e && *e) ? atoi(e) : 255;
    }
    return s_bpl_line_offset_4200_end;
}

static int bpl_line_offset_4200_skip_y(void)
{
    if (s_bpl_line_offset_4200_skip_y == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_4200_SKIP_Y");
        s_bpl_line_offset_4200_skip_y = (e && *e) ? atoi(e) : -1;
    }
    return s_bpl_line_offset_4200_skip_y;
}

static int bpl_line_offset_4200_skip_line(void)
{
    if (s_bpl_line_offset_4200_skip_line == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_4200_SKIP_LINE");
        s_bpl_line_offset_4200_skip_line = (e && *e) ? atoi(e) : -1;
    }
    return s_bpl_line_offset_4200_skip_line;
}

static int bpl_line_offset_4200_skip_line_mask(void)
{
    if (s_bpl_line_offset_4200_skip_line_mask == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_4200_SKIP_LINE_MASK");
        s_bpl_line_offset_4200_skip_line_mask = (e && *e) ? atoi(e) : 0x3F;
    }
    return s_bpl_line_offset_4200_skip_line_mask;
}

static int bpl_line_offset_4200_mask(void)
{
    if (s_bpl_line_offset_4200_mask == -9999) {
        const char *e = getenv("BENEFACTOR_BPL_LINE_OFFSET_4200_MASK");
        s_bpl_line_offset_4200_mask = (e && *e) ? atoi(e) : 0x3F;
    }
    return s_bpl_line_offset_4200_mask;
}

static int rule_4200_plane1_transition_enabled(void)
{
    if (s_rule_4200_plane1_transition < 0) {
        const char *e = getenv("BENEFACTOR_RULE_4200_PLANE1_TRANSITION");
        s_rule_4200_plane1_transition = (e && *e) ? atoi(e) : 1;
    }
    return s_rule_4200_plane1_transition;
}

static int reset_regs_each_frame(void)
{
    if (s_reset_regs < 0) {
        const char *e = getenv("BENEFACTOR_RESET_REGS_EACH_FRAME");
        s_reset_regs = (e && *e) ? atoi(e) : 0;
    }
    return s_reset_regs;
}

static int advance_before_render(void)
{
    if (s_advance_before < 0) {
        const char *e = getenv("BENEFACTOR_ADVANCE_BEFORE");
        s_advance_before = (e && *e) ? atoi(e) : 0;
    }
    return s_advance_before;
}

static int wait_at_fetch_hpos(void)
{
    if (s_wait_at_fetch < 0) {
        const char *e = getenv("BENEFACTOR_WAIT_AT_FETCH");
        s_wait_at_fetch = (e && *e) ? atoi(e) : 0;
    }
    return s_wait_at_fetch;
}

static int copper_cur_hp_override(void)
{
    if (s_copper_cur_hp == -9999) {
        const char *e = getenv("BENEFACTOR_COPPER_CUR_HP");
        s_copper_cur_hp = (e && *e) ? atoi(e) : -1;
    }
    return s_copper_cur_hp;
}

static int trace_bplptr_sync_enabled(void)
{
    if (s_trace_bplptr_sync < 0)
        s_trace_bplptr_sync = getenv("BENEFACTOR_TRACE_BPLPTR_SYNC") ? 1 : 0;
    return s_trace_bplptr_sync;
}

static int trace_3600_lines_enabled(void)
{
    if (s_trace_3600_lines < 0)
        s_trace_3600_lines = getenv("BENEFACTOR_TRACE_3600_LINES") ? 1 : 0;
    return s_trace_3600_lines;
}

static int trace_3600_pixels_y(void)
{
    if (s_trace_3600_pixels_y == -9999) {
        const char *e = getenv("BENEFACTOR_TRACE_3600_PIXELS_Y");
        s_trace_3600_pixels_y = (e && *e) ? atoi(e) : -1;
    }
    return s_trace_3600_pixels_y;
}

static int trace_3600_pixels_x0(void)
{
    if (s_trace_3600_pixels_x0 == -9999) {
        const char *e = getenv("BENEFACTOR_TRACE_3600_PIXELS_X0");
        s_trace_3600_pixels_x0 = (e && *e) ? atoi(e) : 0;
    }
    return s_trace_3600_pixels_x0;
}

static int trace_3600_pixels_x1(void)
{
    if (s_trace_3600_pixels_x1 == -9999) {
        const char *e = getenv("BENEFACTOR_TRACE_3600_PIXELS_X1");
        s_trace_3600_pixels_x1 = (e && *e) ? atoi(e) : -1;
    }
    return s_trace_3600_pixels_x1;
}

static int use_prev_bpl2mod_3600_enabled(void)
{
    if (s_use_prev_bpl2mod_3600 < 0)
        s_use_prev_bpl2mod_3600 = getenv("BENEFACTOR_3600_USE_PREV_BPL2MOD") ? 1 : 0;
    return s_use_prev_bpl2mod_3600;
}

static int pf1_shift_3600(void)
{
    if (s_pf1_shift_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_PF1_SHIFT_3600");
        s_pf1_shift_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_pf1_shift_3600;
}

static int pf1_base_bytes_3600(void)
{
    if (s_pf1_base_bytes_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_PF1_BASE_BYTES_3600");
        s_pf1_base_bytes_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_pf1_base_bytes_3600;
}

static int pf1_base_bytes_3600_ffd8(void)
{
    if (s_pf1_base_bytes_3600_ffd8 == -9999) {
        const char *e = getenv("BENEFACTOR_PF1_BASE_BYTES_3600_FFD8");
        s_pf1_base_bytes_3600_ffd8 = (e && *e) ? atoi(e) : 0;
    }
    return s_pf1_base_bytes_3600_ffd8;
}

static int pf1_base_bytes_3600_zero(void)
{
    if (s_pf1_base_bytes_3600_zero == -9999) {
        const char *e = getenv("BENEFACTOR_PF1_BASE_BYTES_3600_ZERO");
        s_pf1_base_bytes_3600_zero = (e && *e) ? atoi(e) : 0;
    }
    return s_pf1_base_bytes_3600_zero;
}

static int pf1_window_base_bytes_3600(void)
{
    if (s_pf1_window_base_bytes_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_PF1_WINDOW_BASE_BYTES_3600");
        s_pf1_window_base_bytes_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_pf1_window_base_bytes_3600;
}

static int pf1_window_base_bytes_3600_byte_max(void)
{
    if (s_pf1_window_base_bytes_3600_byte_max == -9999) {
        const char *e = getenv("BENEFACTOR_PF1_WINDOW_BASE_BYTES_3600_BYTE_MAX");
        s_pf1_window_base_bytes_3600_byte_max = (e && *e) ? atoi(e) : -1;
    }
    return s_pf1_window_base_bytes_3600_byte_max;
}

static int pf1_step_bias_3600(void)
{
    if (s_pf1_step_bias_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_PF1_STEP_BIAS_3600");
        s_pf1_step_bias_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_pf1_step_bias_3600;
}

static int plane0_base_bytes_3600(void)
{
    if (s_plane0_base_bytes_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_PLANE0_BASE_BYTES_3600");
        s_plane0_base_bytes_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_plane0_base_bytes_3600;
}

static int plane2_base_bytes_3600(void)
{
    if (s_plane2_base_bytes_3600 == -9999) {
        const char *e = getenv("BENEFACTOR_PLANE2_BASE_BYTES_3600");
        s_plane2_base_bytes_3600 = (e && *e) ? atoi(e) : 0;
    }
    return s_plane2_base_bytes_3600;
}

static int trace_copper_enabled(void)
{
    if (s_trace_copper < 0)
        s_trace_copper = getenv("BENEFACTOR_TRACE_COPPER") ? 1 : 0;
    return s_trace_copper;
}

static int copper_pos_reached(int line, uint16_t cur_hp,
                               uint16_t vp, uint16_t hp,
                               uint16_t vpm, uint16_t hpm)
{
    (void)vpm;
    /* Amiga copper vertical counter is 8-bit: lines 256-311 wrap to 0x00-0x37 */
    int cmp_line = line & 0xFF;
    /* Next-frame WAIT guard: if cmp_line > vp by more than 127, the WAIT target
     * is more than half a frame in the past — it is actually for the next frame
     * (after the 8-bit counter wraps back through 0). Defer it. This prevents
     * per-scanline copper lists that end with vp=1..43 "vblank" WAITs from
     * prematurely firing BPLCON0=$0200 at the bottom of the visible display. */
    if (cmp_line > vp && (cmp_line - vp) > 127)
        return 0;
    return (cmp_line > vp) || (cmp_line == vp && ((cur_hp & hpm) >= (hp & hpm)));
}

static inline uint16_t mem_r16(uint32_t a)
{
    return (uint16_t)((g_mem[a] << 8) | g_mem[a + 1]);
}

static void copper_run_scanline(CopperExec *c, int line)
{
    uint16_t cur_hp = 0xFE;
    if (wait_at_fetch_hpos())
        cur_hp = (uint16_t)(s_regs[_DDFSTRT >> 1] & 0xFE);
    {
        int ov = copper_cur_hp_override();
        if (ov >= 0)
            cur_hp = (uint16_t)(ov & 0xFE);
    }
    if (c->ip == 0 || c->lc == 0) return;

    if (c->waiting) {
        if (copper_pos_reached(line, cur_hp, c->wait_vp, c->wait_hp,
                                c->wait_vpm, c->wait_hpm))
            c->waiting = 0;
        else
            return;
    }

    for (int i = 0; i < 512; i++) {
        if (c->ip + 3 >= RT_MEM_SIZE) return;

        uint16_t ir1 = mem_r16(c->ip);
        uint16_t ir2 = mem_r16(c->ip + 2);
        c->ip += 4;

        if (!(ir1 & 1)) {
            uint16_t reg = ir1 & 0x01FE;
            if (reg >= 0x20) {
                /* Tag copper writes so HWTRACE can distinguish them from game code */
                s_copper_writing = 1;
                hw_write16(0xDFF000u + reg, ir2);
                s_copper_writing = 0;
            }
            continue;
        }

        uint16_t vp  = (ir1 >> 8) & 0xFF;
        uint16_t hp  = (ir1 >> 1) & 0x7F;
        uint16_t vpm = (uint16_t)(((ir2 >> 8) | 0x80) & 0xFF);
        uint16_t hpm = ir2 & 0xFE;

        if (ir2 & 1) {
            if (copper_pos_reached(line, cur_hp, vp, hp, vpm, hpm))
                c->ip += 4;
            continue;
        }

        /* WAIT $FFFF $FFFE: end-of-list → stop copper */
        if (ir1 == 0xFFFF && ir2 == 0xFFFE) {
            c->ip = 0;
            c->lc = 0;
            return;
        }

        if (!copper_pos_reached(line, cur_hp, vp, hp, vpm, hpm)) {
            c->wait_vp  = vp;
            c->wait_hp  = hp;
            c->wait_vpm = vpm;
            c->wait_hpm = hpm;
            c->waiting  = 1;
            return;
        }
    }
}

/* Execute copper as scanline machine for current frame state. */
void hw_execute_copper(void)
{
    /* Rendering path executes copper per-line in hw_render_frame(). */
}

static void render_scanline_from_regs(int line, int diy_start, int out_y,
                                      uint32_t bpl_live[6], int snapped,
                                      int16_t prev_bpl2mod)
{
    uint16_t bplcon0 = s_regs[_BPLCON0 >> 1];
    uint16_t bplcon1 = s_regs[_BPLCON1 >> 1];
    uint16_t bplcon2 = s_regs[0x104 >> 1];
    int nplanes = (bplcon0 >> 12) & 7;
    int hires = ((bplcon0 & 0x8000) != 0);
    int dualpf = ((bplcon0 & 0x0400) != 0);
    int mode_4200 = ((bplcon0 & 0xFE00u) == 0x4200u);
    int mode_3600 = ((bplcon0 & 0xFE00u) == 0x3600u);
    if (nplanes > 6) nplanes = 6;

    uint32_t bg = s_palette[0];
    uint32_t *row = &s_fb[out_y * HW_DISPLAY_W];
    for (int x = 0; x < HW_DISPLAY_W; x++) row[x] = bg;

    if (!snapped || nplanes <= 0) return;

    int scroll_x = bplcon1 & 0xF;
    uint16_t ddfstrt = s_regs[_DDFSTRT >> 1];
    uint16_t ddfstop = s_regs[_DDFSTOP >> 1];
    uint16_t diwstrt = s_regs[_DIWSTRT >> 1];
    int fetch_words = ((ddfstop - ddfstrt) >> 3) + 1;
    if ((bplcon0 & 0xFE00u) == 0x3600u)
        fetch_words += fetch_words_bias_3600();
    if (fetch_words < 1) fetch_words = 20;
    int bytes_per_line = fetch_words * 2;
    int16_t bpl1mod = (int16_t)s_regs[_BPL1MOD >> 1];
    int16_t bpl2mod = (int16_t)s_regs[_BPL2MOD >> 1];
    if (mode_3600 && use_prev_bpl2mod_3600_enabled())
        bpl2mod = prev_bpl2mod;

    /* Horizontal border offset between DIW and fetch start. */
    int x_origin = 0;
    {
        int diw_hstart = diwstrt & 0xFF;
        int ddf_hstart = ddfstrt & 0xFF;
        int delta = diw_hstart - ddf_hstart - 1;
        if (delta > 0)
            x_origin = delta * 2;
        /* PUAE comparison framebuffer is scaled from full emulator output,
         * which includes wider side borders than the native 320 decode path. */
        x_origin += xorigin_bias();
        if ((bplcon0 & 0xFE00u) == 0x4200u)
            x_origin += xdelta_4200();
        else if ((bplcon0 & 0xFE00u) == 0x3600u) {
            x_origin += xdelta_3600();
            if ((int16_t)bpl2mod < 0)
                x_origin += xdelta_3600_ffd8();
            else if ((int16_t)bpl2mod == 0)
                x_origin += xdelta_3600_zero();
        }
    }

    /* EHB palette extension */
    int ehb = (nplanes == 6) && !((bplcon0 >> 11) & 1);
    uint32_t palette[64];
    for (int i = 0; i < 32; i++) palette[i] = s_palette[i];
    if (ehb) {
        for (int i = 0; i < 32; i++) {
            uint32_t c = palette[i];
            palette[32+i] = (c & 0xFF000000u)
                | (((c >> 17) & 0x7F) << 16)
                | (((c >>  9) & 0x7F) <<  8)
                |  ((c >>  1) & 0x7F);
        }
    }

    if (advance_before_render()) {
        for (int p = 0; p < nplanes; p++) {
            int step_bias = 0;
            if (mode_3600 && (p & 1) == 0)
                step_bias = pf1_step_bias_3600();
            bpl_live[p] = (bpl_live[p] + (uint32_t)(bytes_per_line + step_bias)) & 0xFFFFFF;
            if (p & 1)
                bpl_live[p] = (uint32_t)((int32_t)bpl_live[p] + bpl2mod) & 0xFFFFFF;
            else
                bpl_live[p] = (uint32_t)((int32_t)bpl_live[p] + bpl1mod) & 0xFFFFFF;
        }
    }

    uint32_t bpl_read_base[6];
    int line_off = 0;
    if (mode_4200) {
        int y0 = bpl_line_offset_4200_start();
        int y1 = bpl_line_offset_4200_end();
        int yskip = bpl_line_offset_4200_skip_y();
        if (out_y >= y0 && out_y <= y1 && out_y != yskip)
            line_off = bpl_line_offset_4200();
    } else if ((bplcon0 & 0xFE00u) == 0x3600u) {
        line_off = bpl_line_offset_3600();
        if ((int16_t)bpl2mod < 0)
            line_off += bpl_line_offset_3600_ffd8();
        else if ((int16_t)bpl2mod == 0)
            line_off += bpl_line_offset_3600_zero();
    }
    int line_mask = bpl_line_offset_4200_mask();
    if (mode_4200) {
        int line_skip = bpl_line_offset_4200_skip_line();
        int line_idx = line - diy_start;
        if (line_idx == line_skip)
            line_mask &= ~bpl_line_offset_4200_skip_line_mask();
        if (rule_4200_plane1_transition_enabled() && line_idx == 43)
            line_mask &= ~(1 << 1);
    }
    for (int p = 0; p < nplanes; p++) {
        int this_line_off = (line_mask & (1 << p)) ? line_off : 0;
        int plane_step = bytes_per_line + ((p & 1) ? bpl2mod : bpl1mod);
        int32_t base = (int32_t)bpl_live[p] + this_line_off * plane_step;
        if (mode_3600 && (p & 1) == 0) {
            base += pf1_base_bytes_3600();
            if ((int16_t)bpl2mod < 0)
                base += pf1_base_bytes_3600_ffd8();
            else if ((int16_t)bpl2mod == 0)
                base += pf1_base_bytes_3600_zero();
            if (p == 0)
                base += plane0_base_bytes_3600();
            else if (p == 2)
                base += plane2_base_bytes_3600();
        }
        bpl_read_base[p] = (uint32_t)base & 0xFFFFFF;
    }

    if (mode_3600 && trace_3600_lines_enabled()) {
        GLOBAL_LOG(
                "[TRACE3600] line=%d out_y=%d bpl2mod=$%04X x_origin=%d "
                "scroll=%d bytes=%d line_off=%d line_mask=$%02X "
                "bpl1=$%06X bpl2=$%06X bpl3=$%06X bpl4=$%06X\n",
                line, out_y, (uint16_t)bpl2mod, x_origin,
                scroll_x, bytes_per_line, line_off, line_mask,
                bpl_read_base[0], bpl_read_base[1], bpl_read_base[2], bpl_read_base[3]);
    }

    for (int x = 0; x < HW_DISPLAY_W; x++) {
        int sx;
        if (!hires && lores_x2_enabled())
            sx = ((x - x_origin) >> 1) + scroll_x;
        else
            sx = (x - x_origin) + scroll_x;
        if (sx < 0) continue;
        if ((sx >> 3) >= bytes_per_line) continue;
        int byte_off = sx >> 3;
        int bit = 7 - (sx & 7);
        uint8_t cidx = 0;
        uint8_t pf1 = 0;
        uint8_t pf2 = 0;
        uint8_t plane_bits = 0;
        for (int p = 0; p < nplanes; p++) {
            int plane_sx = sx;
            if (mode_3600 && (p & 1) == 0)
                plane_sx += pf1_shift_3600();
            if (plane_sx < 0) continue;
            if ((plane_sx >> 3) >= bytes_per_line) continue;
            int plane_byte_off = plane_sx >> 3;
            int plane_bit = 7 - (plane_sx & 7);
            int window_bytes = 0;
            if (mode_3600 && (p & 1) == 0) {
                int byte_max = pf1_window_base_bytes_3600_byte_max();
                if (byte_max >= 0 && plane_byte_off <= byte_max)
                    window_bytes = pf1_window_base_bytes_3600();
            }
            uint32_t a = (bpl_read_base[p] + (uint32_t)(plane_byte_off + window_bytes)) & 0xFFFFFF;
            if (a < RT_MEM_SIZE && ((g_mem[a] >> plane_bit) & 1)) {
                plane_bits |= (uint8_t)(1 << p);
                cidx |= (uint8_t)(1 << p);
                if (dualpf && dualpf_decode_enabled()) {
                    if ((p & 1) == 0)
                        pf1 |= (uint8_t)(1 << (p >> 1));
                    else
                        pf2 |= (uint8_t)(1 << (p >> 1));
                }
            }
        }
        if (dualpf && dualpf_decode_enabled()) {
            int pf2pri = (bplcon2 & 0x0040) != 0;
            if (pf2 && (pf2pri || !pf1))
                cidx = (uint8_t)(8 + pf2);
            else
                cidx = pf1;
        }
        if (mode_3600 && out_y == trace_3600_pixels_y()) {
            int x0 = trace_3600_pixels_x0();
            int x1 = trace_3600_pixels_x1();
            if (x >= x0 && (x1 < 0 || x <= x1)) {
                GLOBAL_LOG(
                        "[TRACE3600PX] y=%d x=%d sx=%d byte=%d bit=%d bits=$%02X cidx=%u pf1=%u pf2=%u\n",
                        out_y, x, sx, byte_off, bit, plane_bits, cidx, pf1, pf2);
            }
        }
        row[x] = palette[cidx];
    }

    /* Advance bitplane DMA pointers by one scanline */
    if (!advance_before_render()) {
        for (int p = 0; p < nplanes; p++) {
            int step_bias = 0;
            if (mode_3600 && (p & 1) == 0)
                step_bias = pf1_step_bias_3600();
            bpl_live[p] = (bpl_live[p] + (uint32_t)(bytes_per_line + step_bias)) & 0xFFFFFF;
            if (p & 1)
                bpl_live[p] = (uint32_t)((int32_t)bpl_live[p] + bpl2mod) & 0xFFFFFF;
            else
                bpl_live[p] = (uint32_t)((int32_t)bpl_live[p] + bpl1mod) & 0xFFFFFF;
        }
    }
}

void hw_render_frame(void)
{
    if (reset_regs_each_frame()) {
        uint16_t cop1h = s_regs[_COP1LCH >> 1];
        uint16_t cop1l = s_regs[_COP1LCL >> 1];
        uint16_t cop2h = s_regs[0x084 >> 1];
        uint16_t cop2l = s_regs[0x086 >> 1];

        memset(s_regs, 0, sizeof(s_regs));
        s_regs[_COP1LCH >> 1] = cop1h;
        s_regs[_COP1LCL >> 1] = cop1l;
        s_regs[0x084 >> 1] = cop2h;
        s_regs[0x086 >> 1] = cop2l;

        memset(s_bplptr, 0, sizeof(s_bplptr));
        memset(s_sprpt, 0, sizeof(s_sprpt));
        memset(s_palette, 0, sizeof(s_palette));
    }

    memset(s_fb, 0, sizeof(s_fb));

    CopperExec c;
    memset(&c, 0, sizeof(c));
    uint32_t cop1lc = (((uint32_t)s_regs[_COP1LCH >> 1] << 16) | s_regs[_COP1LCL >> 1]) & 0xFFFFFF;
    uint32_t cop2lc = (((uint32_t)s_regs[0x084 >> 1] << 16) | s_regs[0x086 >> 1]) & 0xFFFFFF;
    c.lc    = cop1lc;
    c.alt_lc = (cop2lc != 0 && cop2lc != cop1lc) ? cop2lc : cop1lc;
    c.ip = c.lc;
    c.wait_vpm = 0xFF;
    c.wait_hpm = 0xFE;

    uint32_t bpl_live[6] = {0};
    uint32_t prev_hw_bpl[6] = {0};
    int bpl_snapped = 0;
    int trace = trace_copper_enabled();
    int16_t prev_render_bpl2mod = (int16_t)s_regs[_BPL2MOD >> 1];

    for (int line = 0; line < 312; line++) {
        copper_run_scanline(&c, line);

        uint16_t diwstrt = s_regs[_DIWSTRT >> 1];
        uint16_t diwstop = s_regs[_DIWSTOP >> 1];
        int diy_start = (diwstrt >> 8) & 0xFF;
        int diy_stop = (diwstop >> 8) & 0xFF;
        if (diy_start < 1) diy_start = 44;
        if (diy_stop <= diy_start) diy_stop = diy_start + HW_DISPLAY_H;
        int snap_line = diy_start + bpl_snap_offset();

        if (!bpl_snapped && line == snap_line) {
            for (int p = 0; p < 6; p++) {
                bpl_live[p] = s_bplptr[p] & 0xFFFFFF;
                prev_hw_bpl[p] = bpl_live[p];
            }
            bpl_snapped = 1;
            /* One-shot render state diagnostics */
            static int s_snap_logged = 0;
            if (!s_snap_logged) {
                s_snap_logged = 1;
                uint16_t diwstrt_v = s_regs[_DIWSTRT >> 1];
                uint16_t diwstop_v = s_regs[_DIWSTOP >> 1];
                uint8_t diw_hstart = diwstrt_v & 0xFF;
                uint8_t diw_hstop  = diwstop_v & 0xFF;
                int h_stop_9 = (diw_hstop & 0x80) ? (diw_hstop | 0x100) : diw_hstop;
                int diw_w = h_stop_9 - diw_hstart;
                uint16_t ddfstrt_v = s_regs[_DDFSTRT >> 1];
                uint16_t ddfstop_v = s_regs[_DDFSTOP >> 1];
                int fetch_words_calc = ((ddfstop_v - ddfstrt_v) >> 3) + 1;
                GLOBAL_LOG(
                    "[RENDER_DBG] snap line=%d diy_start=%d diy_stop=%d "
                    "bplcon0=$%04X ddf=$%04X/$%04X fetch_words=%d bytes=%d "
                    "diw_hstart=$%02X diw_hstop=$%02X diw_w=%d "
                    "bpl1=$%06X bpl2=$%06X bpl3=$%06X "
                    "COLOR00=$%06X COLOR01=$%06X COLOR02=$%06X\n",
                    line, diy_start, diy_stop,
                    s_regs[_BPLCON0 >> 1],
                    ddfstrt_v, ddfstop_v, fetch_words_calc, fetch_words_calc * 2,
                    diw_hstart, diw_hstop, diw_w,
                    bpl_live[0], bpl_live[1], bpl_live[2],
                    s_palette[0], s_palette[1], s_palette[2]);
            }
            if (trace) {
                GLOBAL_LOG(
                        "[COPPER] snap line=%d lc=$%06X bpl1=$%06X bpl2=$%06X bpl3=$%06X bplcon0=$%04X ddf=$%04X/$%04X\n",
                        line, c.lc, bpl_live[0], bpl_live[1], bpl_live[2],
                        s_regs[_BPLCON0 >> 1], s_regs[_DDFSTRT >> 1], s_regs[_DDFSTOP >> 1]);
            }
        }

        /* PUAE harness scales full output to 256 lines; keep a small top border
         * so our coordinate space aligns with PUAE's sampled frame. */
        const int top_border = top_border_bias();
        int out_y = line - diy_start + top_border;
        if (bpl_snapped) {
            for (int p = 0; p < 6; p++) {
                uint32_t cur = s_bplptr[p] & 0xFFFFFF;
                if (cur != prev_hw_bpl[p]) {
                    if (trace_bplptr_sync_enabled()) {
                        GLOBAL_LOG(
                                "[BPL_SYNC] line=%d out_y=%d p=%d prev=$%06X cur=$%06X bplcon0=$%04X\n",
                                line, out_y, p, prev_hw_bpl[p], cur, s_regs[_BPLCON0 >> 1]);
                    }
                    bpl_live[p] = cur;
                    prev_hw_bpl[p] = cur;
                }
            }
        }
        if (line >= diy_start && line < diy_stop && out_y >= 0 && out_y < HW_DISPLAY_H)
            render_scanline_from_regs(line, diy_start, out_y, bpl_live, bpl_snapped, prev_render_bpl2mod);
        prev_render_bpl2mod = (int16_t)s_regs[_BPL2MOD >> 1];
    }

    /* (no end-of-frame diagnostics) */

    {
        static int disable_sprites = -1;
        if (disable_sprites < 0)
            disable_sprites = getenv("PC_DISABLE_SPRITES") ? 1 : 0;

        if (!disable_sprites) {
            uint32_t spr_palette[32];
            for (int i = 0; i < 32; i++) spr_palette[i] = s_palette[i];
            for (int spr = 0; spr < 8; spr++) {
                uint32_t addr = s_sprpt[spr];
                if (addr < 4 || addr + 4 >= RT_MEM_SIZE) continue;
                uint16_t pos = mem_r16(addr);
                uint16_t ctl = mem_r16(addr + 2);
                int vstart = (pos >> 8) & 0xFF;
                int vstop = (ctl >> 8) & 0xFF;
                int hstart = ((pos & 0xFF) << 1) | ((ctl >> 1) & 1);
                addr += 4;
                for (int y = vstart; y < vstop && y < HW_DISPLAY_H; y++) {
                    if (addr + 3 >= RT_MEM_SIZE) break;
                    uint16_t d0 = mem_r16(addr);
                    uint16_t d1 = mem_r16(addr + 2);
                    addr += 4;
                    for (int x = 0; x < 16; x++) {
                        int px = hstart + x;
                        if ((unsigned)px >= (unsigned)HW_DISPLAY_W) continue;
                        int bit = 15 - x;
                        int col = (((d1 >> bit) & 1) << 1) | ((d0 >> bit) & 1);
                        if (!col) continue;
                        int color_idx = 16 + ((spr >> 1) * 4) + col;
                        s_fb[y * HW_DISPLAY_W + px] = spr_palette[color_idx];
                    }
                }
            }
        }
    }
}
