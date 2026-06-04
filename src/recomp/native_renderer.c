/* native_renderer.c — Copper-walking native renderer
 *
 * Reads the copper list directly from chip RAM each frame, builds per-scanline
 * register state, and decodes bitplane pixel data without any Amiga hardware
 * emulation.  Replaces hw_render_frame() / hw_copper.c rendering path.
 *
 * Coordinate system:
 *   PUAE renders 256 display lines starting from PAL raster FIRST_VISIBLE_LINE=25.
 *   A copper WAIT VP maps to display line: display_y = VP - FIRST_VISIBLE_LINE.
 *   VPs below 25 are invisible (top border); we fill those lines with COLOR00.
 *
 * Title screen layout after coordinate correction (from copper list at $7BC8):
 *   Display lines 0-18:   Border (before DIWSTRT VP=44 → display line 19)
 *   Display lines 19-64:  BPLCON0=$4200 (4-plane lores non-DPF, copper VP 44-89)
 *                          160px wide centred at x=80; fetch=20B/plane; stride=80B/line
 *   Display lines 65-254: BPLCON0=$3600 (3-plane DPF lores, copper VP 90+)
 *                          320px; PF1=BPL1+BPL3 (odd), PF2=BPL2 (even); fetch=40B/plane
 *                          BPL1MOD=40 (constant); BPL2MOD varies per copper WAIT
 */
#include "hw_private.h"   /* pulls in rt.h → g_mem, and amiga_to_argb(), s_regs, etc. */
#include <string.h>
#include <stdlib.h>

/* Bitplane/copper source for the renderer. The Amiga blitter is timed (writes
 * become visible ~1 frame after the blit is kicked); the PC blitter is
 * synchronous, so disk-boot would display each frame's freshly-blitted scroll
 * — AND the copper-list bitplane pointers $0041A4 just wrote — a frame early,
 * which is the intro text stutter. When g_native_render_delay is set, render
 * from a one-frame-delayed snapshot of chip RAM for BOTH the copper-list/
 * pointers (walk_copper) and the bitplane pixel data (decode_*), keeping them
 * in lockstep so PC shows the buffer state the Amiga's timed blitter exposes
 * at this vblank. (cop1lc itself is read live from s_regs and already matches
 * the Amiga frame-for-frame.) */
int g_native_render_delay = 0;                /* snapshot depth (frames) when >0 */
#define RENDER_SNAP_RING 4
static uint8_t s_snap_ring[RENDER_SNAP_RING][0x80000];
static int     s_snap_widx = 0;               /* next write slot */
static int     s_snap_count = 0;              /* valid snapshots so far */
static const uint8_t *s_render_src = NULL;   /* chip-RAM source the renderer reads */

static inline uint16_t chip_r16(uint32_t addr)
{
    addr &= 0xFFFFEu;  /* word-align, stay within 512 KB */
    if (addr + 1 >= 0x80000u) return 0;
    const uint8_t *m = s_render_src ? s_render_src : g_mem;
    return (uint16_t)((m[addr] << 8) | m[addr + 1]);
}

/* ── Register index helpers ────────────────────────────────────────── */
#define RIDX_COP1LCH  (0x080u >> 1)   /* s_regs[0x40] */
#define RIDX_COP1LCL  (0x082u >> 1)   /* s_regs[0x41] */

/* ── DFF register offsets (from copper MOVE word1 & 0x01FE) ───────── */
#define DFF_BPLCON0  0x100u
#define DFF_BPLCON1  0x102u
#define DFF_DDFSTRT  0x092u
#define DFF_DDFSTOP  0x094u
#define DFF_BPL1MOD  0x108u
#define DFF_BPL2MOD  0x10Au
#define DFF_BPL1PTH  0x0E0u   /* BPLxPTH/L run $0E0..$0F6 (planes 1..6) */
#define DFF_COLOR00  0x180u
#define DFF_COLOR31  0x1BEu

#define MAX_PLANES 6

/* ── Per-scanline copper state ─────────────────────────────────────── */
typedef struct {
    uint16_t bplcon0;
    uint16_t bplcon1;       /* horizontal scroll (PF1H bits 0-3, PF2H bits 4-7) */
    int16_t  bpl1mod, bpl2mod;
    uint16_t ddfstrt, ddfstop;  /* data-fetch window — sets width + horizontal position */
    uint32_t palette[32];   /* ARGB8888 */
} ScanState;

/* Map DDFSTRT to the on-screen x of the first fetched pixel, in PUAE's
 * framebuffer coordinates. Standard DDFSTRT=$38 lands at fb x=6 (PUAE's left
 * border); each DDFSTRT unit is 2 lores px. Verified: starfield ($38)→6,
 * title box ($60)→86 (matches PUAE's measured 6 / 87). */
#define DDF_TO_X(ddf)  ((int)((ddf) - 0x38) * 2 + 6)

/* BPL pointer anchor: at `first_line` the indicated plane pointers reset */
typedef struct {
    int      first_line;
    uint32_t ptr[MAX_PLANES];
    uint8_t  mask;   /* bit i set → ptr[i] valid */
} BplAnchor;

/* One BPL-pointer re-point per scanline at most: the leaderboard ($00844A)
 * water reflection re-points BPL2PT every reflection line (~78 anchors) to
 * build the rippling mirror. A small cap silently dropped the lower reflection
 * lines, so the animated water effect was missing below the first ~16 lines. */
#define MAX_ANCHORS (HW_DISPLAY_H + 8)

static ScanState s_scan[HW_DISPLAY_H];
static BplAnchor s_anchors[MAX_ANCHORS];
static int       s_nanchors;

/* Per-scanline BPL pointer + geometry snapshot (single-playfield lines), so the wide
 * renderer can decode the engine's page at OFF-SCREEN x (the margins) — testing whether
 * the page buffer holds content beyond the visible fetch window. */
static uint32_t s_line_bplpt[HW_DISPLAY_H][MAX_PLANES];
static int16_t  s_line_xoff[HW_DISPLAY_H], s_line_scr1[HW_DISPLAY_H];
static uint8_t  s_line_bpu[HW_DISPLAY_H];
static int16_t  s_line_width[HW_DISPLAY_H];

/* Title intro: the story-text scroller is reimplemented natively (see
 * draw_title_text) because the recompiled blitter corrupts the composited
 * text buffer ($077E60) via the A-shift carry. On the title we render the
 * starfield (PF1) from chip RAM and draw the text (PF2) cleanly ourselves. */
static uint32_t s_cur_cop1lc  = 0;

/* ── Copper list walker ─────────────────────────────────────────────── */
static void walk_copper(void)
{
    uint32_t cop1lc = ((uint32_t)s_regs[RIDX_COP1LCH] << 16)
                    |  (uint32_t)s_regs[RIDX_COP1LCL];
    cop1lc &= 0xFFFFEu;
    if (cop1lc < 0x1000u || cop1lc >= 0x80000u)
        cop1lc = 0x7BC8u;   /* fallback: title copper list */
    s_cur_cop1lc = cop1lc;

    s_nanchors = 0;

    /* Current display state — use "no planes" for the pre-DIWSTRT border.
     * s_regs[BPLCON0] after hw_execute_copper() holds the copper list's LAST
     * BPLCON0 write (star-field DPF mode), which is wrong for border lines.
     * Start with bpu=0 so border lines render as COLOR00 background only. */
    uint16_t cur_bplcon0 = 0x0200u;    /* BPLCON0 reset: lores, 0 planes */
    uint16_t cur_bplcon1 = 0x0000u;    /* horizontal scroll */
    int16_t  cur_bpl1mod = 0;
    int16_t  cur_bpl2mod = 0;
    uint16_t cur_ddfstrt = 0x38u;      /* standard data-fetch start */
    uint16_t cur_ddfstop = 0xD0u;      /* standard data-fetch stop (320px) */
    uint32_t cur_pal[32];
    /* Seed palette from hardware shadow so COLOR00 for the border matches the
     * hardware state left by the previous frame's copper list. */
    memcpy(cur_pal, s_palette, sizeof cur_pal);

    /* Pending BPL pointer assembly (H word received, waiting for L) */
    uint32_t pend_ptr[MAX_PLANES]   = {0};
    uint8_t  pend_hi[MAX_PLANES]    = {0};   /* H word received for plane p */
    uint8_t  pend_ready[MAX_PLANES] = {0};   /* L word received → ptr[p] complete */
    uint8_t  pend_mask     = 0;     /* which planes have a complete pending ptr */

    /* PAL first visible raster line.  PUAE renders display line 0 from raster 25,
     * so copper WAIT VP maps to display_y = VP - FIRST_VISIBLE_LINE. */
    static const int FIRST_VISIBLE = 27;

    /* cur_line tracks the current display_y (not raw VP). */
    int cur_line = 0;

    /* The copper VP field is 8-bit and wraps at 256. A title list runs past VP=255
     * (e.g. the story-crawl bottom waits VP=255 then VP=1,2,3…, meaning raster lines
     * 257,258,…). Track a wrap base so wrapped VPs map to the correct display line
     * instead of being mistaken for the top border or the end-of-list sentinel. */
    int prev_vp = -1;
    int vp_base = 0;

    for (uint32_t ip = cop1lc; ip + 3u < 0x80000u; ip += 4u) {
        uint16_t w1 = chip_r16(ip);
        uint16_t w2 = chip_r16(ip + 2u);

        if (w1 & 1u) {
            /* WAIT or SKIP instruction */
            if (w1 == 0xFFFFu && w2 == 0xFFFEu) break;  /* true end-of-list sentinel */
            if (w2 & 1u) { ip += 4u; continue; }   /* SKIP: skip next instr */
            int raw_vp = (int)((w1 >> 8) & 0xFFu);
            /* A VP lower than the previous wait is an 8-bit wrap past line 255. */
            if (raw_vp < prev_vp) vp_base += 256;
            prev_vp = raw_vp;
            int vp = raw_vp + vp_base;

            /* Map VP → display_y (negative VPs are in invisible top border) */
            int dy = vp - FIRST_VISIBLE;
            if (dy < 0) dy = 0;
            int lim = (dy < HW_DISPLAY_H) ? dy : HW_DISPLAY_H;

            /* Stamp current state for display lines [cur_line, lim) */
            for (int y = cur_line; y < lim; y++) {
                s_scan[y].bplcon0 = cur_bplcon0;
                s_scan[y].bplcon1 = cur_bplcon1;
                s_scan[y].bpl1mod = cur_bpl1mod;
                s_scan[y].bpl2mod = cur_bpl2mod;
                s_scan[y].ddfstrt = cur_ddfstrt;
                s_scan[y].ddfstop = cur_ddfstop;
                memcpy(s_scan[y].palette, cur_pal, sizeof cur_pal);
            }

            /* Record pending BPL pointer anchor at cur_line (segment start) */
            if (pend_mask && s_nanchors < MAX_ANCHORS) {
                BplAnchor *a = &s_anchors[s_nanchors++];
                a->first_line = cur_line;
                a->mask = pend_mask;
                for (int p = 0; p < MAX_PLANES; p++)
                    a->ptr[p] = pend_ready[p] ? pend_ptr[p] : 0u;
                pend_mask = 0;
                memset(pend_hi,    0, sizeof pend_hi);
                memset(pend_ready, 0, sizeof pend_ready);
            }

            cur_line = lim;
            if (cur_line >= HW_DISPLAY_H) break;
        } else {
            /* MOVE instruction */
            uint32_t reg = (uint32_t)(w1 & 0x01FEu);
            if (reg == DFF_BPLCON0)      { cur_bplcon0 = w2; }
            else if (reg == DFF_BPLCON1) { cur_bplcon1 = w2; }
            else if (reg == DFF_DDFSTRT) { cur_ddfstrt = w2; }
            else if (reg == DFF_DDFSTOP) { cur_ddfstop = w2; }
            else if (reg == DFF_BPL1MOD) { cur_bpl1mod = (int16_t)w2; }
            else if (reg == DFF_BPL2MOD) { cur_bpl2mod = (int16_t)w2; }
            else if (reg >= DFF_BPL1PTH && reg < DFF_BPL1PTH + MAX_PLANES * 4u) {
                /* BPLxPTH/L: $0E0 + plane*4 (+0 = high word, +2 = low word) */
                int idx    = (int)((reg - DFF_BPL1PTH) >> 2);
                int is_low = ((reg - DFF_BPL1PTH) & 2u) != 0;
                if (!is_low) {
                    pend_ptr[idx] = ((uint32_t)w2 << 16) | (pend_ptr[idx] & 0xFFFFu);
                    pend_hi[idx]  = 1;
                } else if (pend_hi[idx]) {
                    pend_ptr[idx]   = (pend_ptr[idx] & 0xFFFF0000u) | w2;
                    pend_ready[idx] = 1;
                    pend_mask      |= (uint8_t)(1u << (unsigned)idx);
                }
            }
            else if (reg >= DFF_COLOR00 && reg <= DFF_COLOR31) {
                cur_pal[(reg - DFF_COLOR00) >> 1] = amiga_to_argb(w2);
            }
        }
    }

    /* Fill any remaining scanlines with the last active state */
    for (int y = cur_line; y < HW_DISPLAY_H; y++) {
        s_scan[y].bplcon0 = cur_bplcon0;
        s_scan[y].bplcon1 = cur_bplcon1;
        s_scan[y].bpl1mod = cur_bpl1mod;
        s_scan[y].bpl2mod = cur_bpl2mod;
        s_scan[y].ddfstrt = cur_ddfstrt;
        s_scan[y].ddfstop = cur_ddfstop;
        memcpy(s_scan[y].palette, cur_pal, sizeof cur_pal);
    }
}

/* ── Pixel decoders ─────────────────────────────────────────────────── */

/* Decode one N-plane (non-DPF) pixel at bit index `bi` (may be negative for
 * BPLCON1 scroll wrap into the previously-fetched word). Returns ARGB8888. */
/* Debug: when BENEFACTOR_RAW_PLANES is set, paint ANY set bitplane bit white,
 * everything else background — bypasses palette/playfield decode so we can see
 * exactly what pixel data is present in the bitplanes regardless of colour. */
static int s_raw_planes = -1;
static inline int raw_planes_on(void)
{
    if (s_raw_planes < 0) s_raw_planes = getenv("BENEFACTOR_RAW_PLANES") ? 1 : 0;
    return s_raw_planes;
}

static inline uint32_t decode_planes(int bi, const uint32_t bplpt[MAX_PLANES],
                                     int nplanes, const uint32_t pal[32])
{
    int byte_off = bi >> 3;            /* arithmetic shift: correct for bi<0 */
    int bit      = 7 - (bi & 7);       /* (bi & 7) is 0..7 even when bi<0 */
    uint8_t cidx = 0;
    for (int p = 0; p < nplanes; p++) {
        uint32_t addr = (bplpt[p] + (uint32_t)byte_off) & 0x7FFFFu;
        if ((s_render_src[addr] >> bit) & 1u)
            cidx |= (uint8_t)(1u << p);
    }
    if (raw_planes_on())
        return cidx ? 0xFFFFFFFFu : pal[0];
    if (nplanes <= 5)
        return pal[cidx & 0x1Fu];          /* up to 5 planes / 32 colours */
    /* 6 planes, non-HAM/non-DPF = Extra-Half-Brite (the gameplay menu/title):
     * planes 1-5 pick COLOR00-31; plane 6 (bit 5) halves the RGB brightness. */
    uint32_t c = pal[cidx & 0x1Fu];
    if (cidx & 0x20u)
        c = (c & 0xFF000000u) | ((c >> 1) & 0x007F7F7Fu);
    return c;
}

/* Decode one dual-playfield pixel at x in [0, 320).
 * PF1 = BPL1 (idx 0) + BPL3 (idx 2), PF2 = BPL2 (idx 1).
 * BPLCON2=$0040 → PF2 priority: show PF2 colour if pf2≠0, else PF1. */
static inline uint32_t decode_dpf(int x, const uint32_t bplpt[4],
                                   const uint32_t pal[32])
{
    int byte_off = x >> 3;
    int bit      = 7 - (x & 7);

    uint8_t pf1 = 0;
    {
        uint32_t a = (bplpt[0] + (uint32_t)byte_off) & 0x7FFFFu;
        if ((s_render_src[a] >> bit) & 1u) pf1 |= 1u;
        a = (bplpt[2] + (uint32_t)byte_off) & 0x7FFFFu;
        if ((s_render_src[a] >> bit) & 1u) pf1 |= 2u;
    }
    uint8_t pf2 = 0;
    {
        uint32_t a = (bplpt[1] + (uint32_t)byte_off) & 0x7FFFFu;
        if ((s_render_src[a] >> bit) & 1u) pf2 = 1u;
    }

    if (raw_planes_on())
        return (pf1 || pf2) ? 0xFFFFFFFFu : pal[0];
    if (pf2) return pal[8u + pf2];   /* COLOR09 */
    if (pf1) return pal[pf1];         /* COLOR01-03 */
    return pal[0];                     /* COLOR00 */
}

/* ── Main entry point ───────────────────────────────────────────────── */
void native_render_frame(void)
{
    /* Select the chip-RAM source (delayed snapshot under the blitter-latency
     * model, else live) BEFORE walking the copper, so the copper list/pointers
     * and the bitplane pixel data are read from the same delayed view. */
    int delay = g_native_render_delay;
    if (delay > RENDER_SNAP_RING - 1) delay = RENDER_SNAP_RING - 1;
    if (delay > 0 && s_snap_count >= delay) {
        int slot = (s_snap_widx - delay + RENDER_SNAP_RING) % RENDER_SNAP_RING;
        s_render_src = s_snap_ring[slot];
    } else {
        s_render_src = g_mem;
    }

    walk_copper();   /* reads copper list + pointers from s_render_src */

    /* Running BPL pointer state — updated from anchors and auto-advanced */
    uint32_t bplpt[MAX_PLANES] = {0};
    int next_anchor = 0;

    for (int y = 0; y < HW_DISPLAY_H; y++) {
        /* Apply any BPL pointer anchors that fire at or before this line.
         * (Anchors are in ascending first_line order from walk_copper.) */
        while (next_anchor < s_nanchors
               && s_anchors[next_anchor].first_line <= y) {
            const BplAnchor *a = &s_anchors[next_anchor++];
            for (int p = 0; p < MAX_PLANES; p++)
                if (a->mask & (uint8_t)(1u << p))
                    bplpt[p] = a->ptr[p];
        }

        const ScanState *st  = &s_scan[y];
        uint32_t        *row = s_fb + y * HW_DISPLAY_W;
        int bpu = (st->bplcon0 >> 12) & 7;
        int dpf = (st->bplcon0 >> 10) & 1;
        uint32_t bg = st->palette[0];
        int x_off = DDF_TO_X(st->ddfstrt);   /* horizontal screen position from DDFSTRT */

        /* Per-line fetch geometry from the data-fetch window (lores): one word
         * (16px) per 8 DDF units, inclusive. Both title sections and gameplay
         * derive their width here. */
        int fetch_words = (((int)st->ddfstop - (int)st->ddfstrt) >> 3) + 1;
        if (fetch_words < 1)  fetch_words = 1;
        if (fetch_words > 64) fetch_words = 64;
        int fetch_bytes = fetch_words * 2;
        int width_px    = fetch_words * 16;
        int scroll1     = st->bplcon1 & 0xF;          /* PF1 (odd planes) H-scroll */
        int scroll2     = (st->bplcon1 >> 4) & 0xF;   /* PF2 (even planes) H-scroll */

        if (bpu == 0) {
            /* No bitplanes — fill with background colour */
            for (int x = 0; x < HW_DISPLAY_W; x++)
                row[x] = bg;

        } else if (dpf) {
            /* Dual-playfield (title star-field): PF1=BPL1+BPL3, PF2=BPL2. */
            for (int x = 0; x < HW_DISPLAY_W; x++) {
                int lx = x - x_off;
                row[x] = (lx >= 0 && lx < width_px)
                        ? decode_dpf(lx, bplpt, st->palette)
                        : bg;
            }
            (void)scroll2;
            int adv_odd  = fetch_bytes + (int)st->bpl1mod;
            int adv_even = fetch_bytes + (int)st->bpl2mod;
            bplpt[0] = (uint32_t)((int32_t)bplpt[0] + adv_odd);
            bplpt[1] = (uint32_t)((int32_t)bplpt[1] + adv_even);
            bplpt[2] = (uint32_t)((int32_t)bplpt[2] + adv_odd);
            bplpt[3] = (uint32_t)((int32_t)bplpt[3] + adv_even);

        } else {
            /* Single playfield, 1-5 planes (title box = 4, gameplay = 5).
             * BPLCON1 delays the playfield by `scroll1` lores px (smooth scroll). */
            for (int p = 0; p < MAX_PLANES; p++) s_line_bplpt[y][p] = bplpt[p];
            s_line_xoff[y] = (int16_t)x_off; s_line_scr1[y] = (int16_t)scroll1;
            s_line_bpu[y] = (uint8_t)bpu;    s_line_width[y] = (int16_t)width_px;
            for (int x = 0; x < HW_DISPLAY_W; x++) {
                int lx = x - x_off - scroll1;
                row[x] = (lx >= 0 && lx < width_px)
                        ? decode_planes(lx, bplpt, bpu, st->palette)
                        : bg;
            }
            /* Advance per plane: odd planes (1,3,5) use BPL1MOD, even use BPL2MOD. */
            for (int p = 0; p < MAX_PLANES; p++) {
                int mod = (p & 1) ? (int)st->bpl2mod : (int)st->bpl1mod;
                bplpt[p] = (uint32_t)((int32_t)bplpt[p] + fetch_bytes + mod);
            }
        }
    }

    /* Push this frame's chip RAM into the snapshot ring for delayed reads. */
    if (g_native_render_delay) {
        memcpy(s_snap_ring[s_snap_widx], g_mem, 0x80000);
        s_snap_widx = (s_snap_widx + 1) % RENDER_SNAP_RING;
        if (s_snap_count < RENDER_SNAP_RING) s_snap_count++;
    }
}

/* ── Widescreen native tile background (Phase 3) ────────────────────────────────
 * The gameplay engine only blits the visible ~336px of the scrolling tilemap into
 * the bitplane buffer, so the off-screen world isn't in chip RAM as pixels. For a
 * wider-than-320 view we draw the WHOLE playfield ourselves, directly from the level
 * tilemap + tile graphics + the camera X — the game's own render logic, widened.
 *
 * The screen<->world mapping is DERIVED from the same copper/camera state the engine
 * uses (NOT tuned): for display x the engine shows bitplane bit-index
 *   lx = x - x_off - scroll1            (x_off = DDF_TO_X(ddfstrt), scroll1 = bplcon1&F)
 * and that bit-index is world pixel  worldX = (camera & ~15) + lx. So our native tile
 * fetch uses the identical formula → it is pixel-exact with the engine, and there is
 * no seam to tune. We render every output column this way and then leave the engine's
 * VALID fetched window (where its bitplanes carry the live objects/player) intact;
 * the engine's off-window border over-fetch is overwritten by our native tiles.
 * Decode spec (harness-verified, see instructions/widescreen-plan.md):
 *   camera   = word @ $57FDBA;  level edges [$57FE8C-$90, $57FE8E-$100]
 *   tilemap  = $552A0, ROW-MAJOR, row stride $F4, col = worldX>>4
 *   tile gfx = read_long($5A539E + (mapword & 0xFFFE)); 160B, 5-plane PLANE-MAJOR
 *              (plane p, row r word = gfx + p*32 + r*2), 16x16 5bpp
 *   palette  = per-scanline copper (already in s_scan[y].palette from walk_copper)
 * Tilemap (~$552A0) and tile gfx (~$5D9xxx, >512KB) are read straight from g_mem
 * (they don't change within a level), NOT via the 512KB chip_r16 window. */
#define WS_TILEMAP  0x000552A0u
#define WS_TILEROW  0xF4u            /* tilemap row stride (bytes) = 122 columns */
#define WS_LAYER_W  2560             /* world object-layer width (px), absolute world X */
/* Persistent world-indexed object/decoration layer (see native_objlayer_update). */
static uint32_t s_objlayer[HW_DISPLAY_H][WS_LAYER_W];   /* ARGB, alpha 0 = transparent */
static int      s_objlayer_init = 0;
#define WS_GFXTAB   0x005A539Eu
#define WS_CAMERA   0x0057FDBAu
#define WS_EDGE_LO  0x0057FE8Cu
#define WS_EDGE_HI  0x0057FE8Eu
#define WS_GAMEPLAY_COP1LC 0x003484u

static void native_render_wide_objects(uint32_t *out, int ow, int margin,
                                       int cam, int pf_top, int pf_bot);
static void native_objlayer_update(int cam, int pf_top, int pf_bot);

static inline uint16_t gmem_r16(const uint8_t *m, uint32_t a)
{ return (uint16_t)((m[a] << 8) | m[a + 1]); }
static inline uint32_t gmem_r32(const uint8_t *m, uint32_t a)
{ return ((uint32_t)m[a] << 24) | ((uint32_t)m[a+1] << 16) | ((uint32_t)m[a+2] << 8) | m[a+3]; }

void native_render_wide_bg(uint32_t *out, int ow, int margin)
{
    if (margin < 0 || s_cur_cop1lc != WS_GAMEPLAY_COP1LC) return;   /* gameplay only (margin 0 = compare-at-352) */
    const uint8_t *M = g_mem;
    if (!M || WS_GFXTAB + 4u >= RT_MEM_SIZE) return;

    int cam    = (int)gmem_r16(M, WS_CAMERA);
    /* World X of the displayed left coarse column. Deriving it from cam arithmetic is
     * direction-asymmetric (the engine's coarse update is hysteretic — verified: a cam
     * formula tears the bg for one frame at boundaries while scrolling LEFT). Instead read
     * the coarse straight from the copper BPL pointer the engine displays — frame-locked
     * and correct in both directions. ptr = page_base + col*2 (page is full level width). */
    int cam16;
    {
        if (s_nanchors < 1) return;
        uint32_t bp0 = s_anchors[0].ptr[0];
        uint32_t pbase = (bp0 >= 0x038628u) ? 0x038628u : 0x02B3ECu;
        cam16 = (int)((bp0 - pbase) / 2u) * 16;
    }
    int cmin   = (int)gmem_r16(M, WS_EDGE_LO) - 0x90;
    int cmax   = (int)gmem_r16(M, WS_EDGE_HI) - 0x100;
    int mincol = (cmin < 0 ? 0 : cmin) >> 4;
    int maxcol = (cmax + 320) >> 4;                      /* last valid level column */

    /* Playfield vertical extent = the scanline span of the FIRST BPL anchor (the
     * scrolling buffer pointers); the next anchor begins the HUD. Both are derived by
     * walk_copper. tilemap row 0 maps to the playfield's first scanline (no vert scroll). */
    /* Playfield top = first scanline the scrolling 5-plane buffer actually displays
     * (the first BPL anchor can fire at the top border before the playfield). Bottom =
     * the next BPL anchor below it (the HUD re-points the pointers there). */
    int pf_top = -1;
    for (int y = 0; y < HW_DISPLAY_H; y++)
        if (((s_scan[y].bplcon0 >> 12) & 7) >= 5 && !((s_scan[y].bplcon0 >> 10) & 1)) { pf_top = y; break; }
    if (pf_top < 0) return;
    int pf_bot = pf_top + 16 * 16;
    for (int a = 0; a < s_nanchors; a++)
        if (s_anchors[a].first_line > pf_top && s_anchors[a].first_line < pf_bot)
            pf_bot = s_anchors[a].first_line;
    if (pf_bot > HW_DISPLAY_H) pf_bot = HW_DISPLAY_H;

    if (getenv("WS_DBG")) {
        int scroll1 = s_scan[pf_top].bplcon1 & 0xF;
        /* Consistency: the engine's displayed fine scroll (copper bplcon1) must equal
         * 15-(cam&15) if my camera source matches the displayed frame. A mismatch during
         * scroll = cam ($57FDBA) is out of sync with the copper that produced s_fb. Also
         * compare cam-coarse vs the BPL-ptr coarse the copper actually points at. */
        uint32_t bp0 = s_anchors[0].ptr[0];
        fprintf(stderr, "[WS_DBG] cam=%d cam&15=%d  copper_scroll1=%d  15-(cam&15)=%d  %s   "
                "cam16=%d bplpt0=%06X x_off=%d pf_top=%d pf_bot=%d\n",
                cam, cam & 15, scroll1, 15 - (cam & 15),
                ((cam & 15) == (15 - scroll1)) ? "OK" : "*** MISMATCH ***",
                cam16, bp0, DDF_TO_X(s_scan[pf_top].ddfstrt), pf_top, pf_bot);
    }

    /* Replay this frame's captured object/decoration blits into the persistent world layer. */
    native_objlayer_update(cam, pf_top, pf_bot);

    for (int y = pf_top; y < pf_bot; y++) {
        int dy = y - pf_top;
        int r  = dy >> 4;
        int ty = dy & 15;
        if (r < 0) continue;
        const ScanState *st = &s_scan[y];
        /* must be the 5-plane single-playfield scanline the scroll buffer drives */
        int bpu = (st->bplcon0 >> 12) & 7;
        if (bpu < 5 || ((st->bplcon0 >> 10) & 1)) continue;   /* skip HUD/dpf lines */
        const uint32_t *pal = st->palette;
        uint32_t *row = out + (size_t)y * ow;

        int x_off   = DDF_TO_X(st->ddfstrt);
        int scroll1 = st->bplcon1 & 0xF;

        /* ONE native renderer: the PC composes the ENTIRE playfield width from the level
         * tilemap — the engine's bitplane/page is NOT used for display. */
        for (int x = 0; x < ow; x++) {
            int lx = (x - margin) - x_off - scroll1;          /* engine bitplane bit index */
            int worldX = cam16 + lx;                          /* identical to engine mapping */
            if (worldX < 0) continue;
            int col = worldX >> 4;
            if (col < mincol || col >= maxcol) continue;
            int tx = worldX & 15;
            uint32_t mo = WS_TILEMAP + (uint32_t)r * WS_TILEROW + (uint32_t)col * 2u;
            if (mo + 1u >= RT_MEM_SIZE) continue;
            uint16_t w = gmem_r16(M, mo);
            uint32_t gfx = gmem_r32(M, WS_GFXTAB + (uint32_t)(w & 0xFFFEu));
            if (gfx < 0x1000u || gfx + 160u > RT_MEM_SIZE) continue;
            int ci = 0;
            for (int p = 0; p < 5; p++)
                if ((gmem_r16(M, gfx + (uint32_t)p * 32u + (uint32_t)ty * 2u) >> (15 - tx)) & 1u)
                    ci |= (1 << p);
            row[x] = pal[ci & 0x1Fu];

            /* Composite the world object/decoration layer on top of the bg tile. */
            if (worldX >= 0 && worldX < WS_LAYER_W) {
                uint32_t o = s_objlayer[y][worldX];
                if (o & 0xFF000000u) row[x] = o;
            }
        }
    }
    (void)native_render_wide_objects;
}

/* Re-draw the engine's captured object sprite blits natively into the wide buffer.
 * Each object is 5 plane-blits (dest steps by the plane stride $2A0C); position is
 * recovered from the dest's offset within its double-buffer page vs the scroll
 * coarse offset, mapped into the same screen coordinate the tile bg uses. Opaque
 * blits (con0 $09F0, D=A) copy all pixels; cookie-cut blits (minterm $CA) draw only
 * where the shared mask plane is set (transparency). */
#define WS_PLANE_STRIDE 0x2A0Cu
#define WS_ROWSTRIDE    46            /* playfield page row stride (bytes) = $2e */

/* Persistent world-indexed object/decoration layer. The engine draws every object AND
 * the static decorations (box, torch) as blits into the page; we replay those blits into
 * this layer in WORLD coordinates (indexed by absolute world X, fixed scanline y): sprite
 * draws SET a pixel, background-restore blits CLEAR it (alpha 0). It persists across frames
 * so column-cross/once-drawn decorations stay, while dynamic objects erase+redraw — exactly
 * mirroring the engine's own draw/restore. Composited over the native tilemap bg. */

/* Process this frame's captured blits into the world layer (called before the bg pass). */
static void native_objlayer_update(int cam, int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    int n = hw_blit_capture_count();
    const BlitRec *rec = hw_blit_capture_recs();
    if (!M) return;
    /* Fresh each frame (world-persistence ghosts under scroll because restore-clear and
     * the original draw use different camera offsets). Cleared, sprites set, restores
     * skipped — clean full-width native objects, no ghost trails. */
    memset(s_objlayer, 0, sizeof s_objlayer);
    s_objlayer_init = 1;

    int cam16 = ((cam + 15) & ~15);
    uint32_t disp_base = (s_anchors[0].ptr[0] >= 0x038628u) ? 0x038628u : 0x02B3ECu;
    int coarse = (int)((s_anchors[0].ptr[0] - disp_base) % WS_ROWSTRIDE);

    int i = 0;
    while (i < n) {
        int g0 = i, np = 1;
        while (i + np < n && np < 5
               && rec[i + np].dpt == rec[i + np - 1].dpt + WS_PLANE_STRIDE
               && rec[i + np].w == rec[g0].w && rec[i + np].h == rec[g0].h) np++;
        const BlitRec *r0 = &rec[g0];
        i += np;

        uint32_t base;
        if (r0->dpt >= 0x038628u && r0->dpt < 0x038628u + WS_PLANE_STRIDE) base = 0x038628u;
        else if (r0->dpt >= 0x02B3ECu && r0->dpt < 0x02B3ECu + WS_PLANE_STRIDE) base = 0x02B3ECu;
        else continue;
        if (base != disp_base) continue;                 /* displayed buffer only */

        int masked  = (r0->mask != 0u);
        int restore = (!masked && r0->src >= 0x045000u && r0->src < 0x054000u);  /* bg-restore */
        uint32_t off = r0->dpt - base;
        int orow = (int)(off / WS_ROWSTRIDE), xbyte = (int)(off % WS_ROWSTRIDE);
        int wpx  = r0->w * 16;
        int srow = r0->w * 2 + r0->smod, mrow = r0->w * 2 + r0->mmod;
        int xbit = ((xbyte - coarse + WS_ROWSTRIDE) % WS_ROWSTRIDE) * 8 + r0->shift;
        int wx0  = cam16 + xbit;                          /* absolute world X of the sprite */

        for (int py = 0; py < r0->h; py++) {
            int sy = pf_top + orow + py;
            if (sy < pf_top || sy >= pf_bot || sy >= HW_DISPLAY_H) continue;
            const uint32_t *pal = s_scan[sy].palette;
            for (int px = 0; px < wpx; px++) {
                int wx = wx0 + px;
                if (wx < 0 || wx >= WS_LAYER_W) continue;
                if (restore) continue;                               /* skip bg-restore */
                if (masked) {
                    uint32_t ma = r0->mask + (uint32_t)py * mrow + (uint32_t)(px >> 3);
                    if (ma + 1u >= RT_MEM_SIZE || !((M[ma] >> (7 - (px & 7))) & 1u)) continue;
                }
                int ci = 0;
                for (int p = 0; p < np; p++) {
                    uint32_t sa = rec[g0 + p].src + (uint32_t)py * srow + (uint32_t)(px >> 3);
                    if (sa + 1u < RT_MEM_SIZE && ((M[sa] >> (7 - (px & 7))) & 1u)) ci |= (1 << p);
                }
                s_objlayer[sy][wx] = 0xFF000000u | (pal[ci & 0x1Fu] & 0xFFFFFFu);   /* set opaque */
            }
        }
    }
}

static void native_render_wide_objects(uint32_t *out, int ow, int margin,
                                        int cam, int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    int n = hw_blit_capture_count();
    const BlitRec *rec = hw_blit_capture_recs();
    if (n <= 0 || !M) return;

    const ScanState *pst = &s_scan[pf_top];
    int x_off   = DDF_TO_X(pst->ddfstrt);
    int scroll1 = pst->bplcon1 & 0xF;
    /* The engine draws each object into BOTH double-buffer pages (at different page
     * offsets, since the buffers are at different scroll states). s_fb shows only the
     * DISPLAYED buffer (the one the copper BPL pointer points at), so draw only that
     * buffer's object blits — otherwise every object is drawn twice at two positions. */
    uint32_t disp_base = (s_anchors[0].ptr[0] >= 0x038628u) ? 0x038628u : 0x02B3ECu;
    /* Displayed-left horizontal byte WITHIN the page row, from the same BPL pointer the
     * bg uses (frame-locked) — NOT cam arithmetic, which drifts vs the bg coarse. */
    int coarse  = (int)((s_anchors[0].ptr[0] - disp_base) % WS_ROWSTRIDE);

    /* Dedup: the engine draws each object into BOTH double-buffer pages, so every
     * sprite appears ~twice. Draw each unique (screen-x, row, w, h) once. */
    struct { int x, y, w, h; } seen[256]; int nseen = 0;

    int i = 0;
    while (i < n) {
        int g0 = i, np = 1;
        while (i + np < n && np < 5
               && rec[i + np].dpt == rec[i + np - 1].dpt + WS_PLANE_STRIDE
               && rec[i + np].w == rec[g0].w && rec[i + np].h == rec[g0].h)
            np++;
        const BlitRec *r0 = &rec[g0];
        i += np;

        uint32_t base;                                  /* plane-0 page origin */
        if (r0->dpt >= 0x038628u && r0->dpt < 0x038628u + WS_PLANE_STRIDE) base = 0x038628u;
        else if (r0->dpt >= 0x02B3ECu && r0->dpt < 0x02B3ECu + WS_PLANE_STRIDE) base = 0x02B3ECu;
        else continue;
        if (base != disp_base) continue;                /* only the displayed buffer */
        int masked = (r0->mask != 0u);
        /* Opaque blits sourcing the bg-SAVE buffer ($045000-$053000) are background
         * RESTORES (the engine erasing old sprites); we redraw bg fresh, so skip them
         * — drawing them paints stale save-buffer rectangles (the "green blocks"). Real
         * opaque OBJECTS (e.g. the metal box) source from elsewhere and must be drawn. */
        if (!masked && r0->src >= 0x045000u && r0->src < 0x054000u) continue;

        uint32_t off = r0->dpt - base;                  /* row*46 + xbyte */
        int orow = (int)(off / WS_ROWSTRIDE);
        int xbyte = (int)(off % WS_ROWSTRIDE);
        int wpx  = r0->w * 16;
        int srow = r0->w * 2 + r0->smod;                /* source plane row stride */
        int mrow = r0->w * 2 + r0->mmod;
        int xbit = ((xbyte - coarse + WS_ROWSTRIDE) % WS_ROWSTRIDE) * 8 + r0->shift;

        int dup = 0;
        for (int s = 0; s < nseen; s++)
            if (seen[s].x == xbit && seen[s].y == orow && seen[s].w == r0->w && seen[s].h == r0->h) { dup = 1; break; }
        if (dup) continue;
        if (nseen < 256) { seen[nseen].x = xbit; seen[nseen].y = orow; seen[nseen].w = r0->w; seen[nseen].h = r0->h; nseen++; }

        if (getenv("WS_DBG"))
            fprintf(stderr, "[WS_OBJ] np=%d %s w=%d h=%d dpt=%06X base=%06X row=%d xbyte=%d "
                    "xbit=%d -> sfx=%d shift=%d con0=%04X src=%06X mask=%06X\n",
                    np, masked?"MASK":"OPAQUE", r0->w, r0->h, r0->dpt, base, orow, xbyte,
                    xbit, xbit + x_off + scroll1, r0->shift, r0->con0, r0->src, r0->mask);

        static int dbgn = 0;
        if (getenv("WS_DBG") && masked && dbgn < 3) {
            dbgn++;
            int hist[32] = {0};
            for (int py = 0; py < r0->h; py++)
              for (int px = 0; px < wpx; px++) {
                uint32_t ma = r0->mask + (uint32_t)py*mrow + (uint32_t)(px>>3);
                if (ma+1u>=RT_MEM_SIZE || !((M[ma]>>(7-(px&7)))&1u)) continue;
                int ci=0; for (int p=0;p<np;p++){uint32_t sa=rec[g0+p].src+(uint32_t)py*srow+(uint32_t)(px>>3); if(sa+1u<RT_MEM_SIZE&&((M[sa]>>(7-(px&7)))&1u))ci|=(1<<p);} hist[ci&0x1F]++;
              }
            fprintf(stderr,"[WS_CI] obj sfx=%d sy=%d  indices:", xbit+x_off+scroll1, pf_top+orow);
            for(int i=0;i<32;i++) if(hist[i]) fprintf(stderr," %d:%dx(pal=%06X)", i, hist[i], s_scan[pf_top+orow+r0->h/2].palette[i]&0xFFFFFF);
            fprintf(stderr,"\n");
        }
        for (int py = 0; py < r0->h; py++) {
            int sy = pf_top + orow + py;
            if (sy < pf_top || sy >= pf_bot || sy >= HW_DISPLAY_H) continue;
            const uint32_t *pal = s_scan[sy].palette;
            uint32_t *row = out + (size_t)sy * ow;
            for (int px = 0; px < wpx; px++) {
                if (masked) {
                    uint32_t ma = r0->mask + (uint32_t)py * mrow + (uint32_t)(px >> 3);
                    if (ma + 1u >= RT_MEM_SIZE || !((M[ma] >> (7 - (px & 7))) & 1u)) continue;
                }
                int ci = 0;
                for (int p = 0; p < np; p++) {
                    uint32_t sa = rec[g0 + p].src + (uint32_t)py * srow + (uint32_t)(px >> 3);
                    if (sa + 1u < RT_MEM_SIZE && ((M[sa] >> (7 - (px & 7))) & 1u)) ci |= (1 << p);
                }
                int x = margin + xbit + px + x_off + scroll1;
                if (x < 0 || x >= ow) continue;
                row[x] = pal[ci & 0x1Fu];
            }
        }
    }
}
