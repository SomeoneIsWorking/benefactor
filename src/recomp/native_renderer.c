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
    /* Promote the widescreen object/char captures (built by the game thread this
     * frame) to the renderer-facing lists, once per present. Done here — not at the
     * object-walker — so the GET-READY/GAME-OVER frames (where the walker pauses but
     * the engine keeps re-blitting a static queue) still show their objects, the
     * teleport animation, and the banner text. See native_ws_promote(). */
    { extern void native_ws_promote(void); native_ws_promote(); }

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
#define WS_TILEROW  0xF4u            /* tilemap row stride FALLBACK (L9); real one is per-level */
#define WS_PHASETAB 0x0057F4BCu      /* row-offset/phase table; entry[1].d2adj = row stride */
#define WS_LAYER_W  2560             /* world object-layer width (px), absolute world X */
int g_ws_view_left = 0, g_ws_view_w = 0;   /* last wide-render view mapping (worldX = view_left + x) */
/* s_objlayer holds the per-frame PROJECTION of the page display-list (decorations under
 * objects), in world coords (absolute world X, screen Y). Rebuilt each frame from s_pg. */
static uint32_t s_objlayer[HW_DISPLAY_H][WS_LAYER_W];

/* Per-group diagnostics, filled every frame by native_objlayer_update so the harness
 * `blits` command can expose WHY each captured blit was drawn/skipped (the CAUSE), not
 * just the pixel-diff result. */
typedef struct {
    uint32_t src, dpt, mask;
    int      w, h, np, con0;
    int      wx0, sy0, wpx;
    char     cls;     /* 'D'eco 'O'bj 'R'estore                                */
    char     drop;    /* 0=drawn, 'b'=unknown buffer, 'B'=not displayed buffer */
} WsBlitDiag;
static WsBlitDiag s_blitdiag[512];
static int        s_blitdiag_n = 0;
static int        s_diag_prev_n = 0;        /* s_prev count seen by the last layer update   */
static uint32_t   s_diag_disp_base = 0, s_diag_bplpt0 = 0;

/* Page display-list: the engine's playfield page is content that DRAW blits add and
 * background-restore blits remove. We mirror it as a list of live page-sprites keyed by
 * their page address (camera-independent), re-projected to the screen every frame from the
 * live BPL pointer. This is what lets a once-drawn overlay (the GET READY banner, drawn ~16
 * frames before the gameplay copper is even up) survive until its restore erases it — the
 * old 1-frame capture replay lost it the moment the screen wasn't a renderable playfield. */
typedef struct {
    uint32_t base;            /* double-buffer page this sprite lives in        */
    uint32_t dpt;             /* plane-0 dest pointer (page address)            */
    uint32_t src[5], mask;    /* gfx source per plane; mask plane (0 = opaque)  */
    int      np, w, h, shift, smod, mmod;
    int      deco;            /* 1 = static decoration (baked bg; restore can't erase it) */
    int      pfill[5];        /* per plane: -1 = read src[p]; 0/1 = constant fill (no A/B src) */
    int      pj_wx0, pj_sy0;  /* last projected world-X / screen-Y (diagnostics)          */
} PgSprite;
static int s_diag_cam16 = 0;
#define PG_MAX 512
static PgSprite s_pg[PG_MAX];
static int      s_pgn = 0;
static long     s_pg_adds = 0, s_pg_removes = 0;   /* cumulative, REPL-readable */
static long     s_objup_runs = 0, s_objup_nonzero = 0;  /* update calls; calls that saw n>0 */
void native_pglist_stats(long *adds, long *removes) { *adds = s_pg_adds; *removes = s_pg_removes; }
void native_objup_stats(long *runs, long *nonzero) { *runs = s_objup_runs; *nonzero = s_objup_nonzero; }
int native_pglist_dump(uint32_t *base, uint32_t *dpt, uint32_t *src0, int *w, int *h, int max)
{
    int n = s_pgn < max ? s_pgn : max;
    for (int k = 0; k < n; k++) {
        base[k]=s_pg[k].base; dpt[k]=s_pg[k].dpt; src0[k]=s_pg[k].src[0];
        w[k]=s_pg[k].w; h[k]=s_pg[k].h;
    }
    return s_pgn;
}
/* extended dump: projected screen X (wx0-cam16), screen Y, mask, shift, np, deco */
int native_pglist_dump2(int *sx, int *sy, uint32_t *mask, int *shift, int *np, int *deco, int max)
{
    int n = s_pgn < max ? s_pgn : max;
    for (int k = 0; k < n; k++) {
        sx[k]=s_pg[k].pj_wx0 - s_diag_cam16; sy[k]=s_pg[k].pj_sy0;
        mask[k]=s_pg[k].mask; shift[k]=s_pg[k].shift; np[k]=s_pg[k].np; deco[k]=s_pg[k].deco;
    }
    return s_pgn;
}
int native_blitdiag(const WsBlitDiag **out) { *out = s_blitdiag; return s_blitdiag_n; }
/* expose the page display-list for the REPL `pglist` inspector */
int native_pglist_dump(uint32_t *base, uint32_t *dpt, uint32_t *src0, int *w, int *h, int max);
void native_blitdiag_ctx(int *prev_n, uint32_t *disp_base, uint32_t *bplpt0)
{ *prev_n = s_diag_prev_n; *disp_base = s_diag_disp_base; *bplpt0 = s_diag_bplpt0; }
#define WS_GFXTAB   0x005A539Eu
#define WS_CAMERA   0x0057FDBAu
#define WS_EDGE_LO  0x0057FE8Cu
#define WS_EDGE_HI  0x0057FE8Eu
#define WS_GAMEPLAY_COP1LC 0x003484u

static void native_objlayer_ingest(void);
static void native_objlayer_project(int cam16, int pf_top, int pf_bot);

/* Per-object draw params captured at the engine's $57D8D0 choke point (before its
 * 352px camera-clip) — the faithful, camera-independent source of every gameplay
 * object. See pc_overrides_gameplay.c / instructions/widescreen-plan.md "Phase 4". */
extern int native_wsobj_count(void);
extern int native_wsobj_get(int i, int *x, int *y, int *w, int *h,
                            uint32_t *src, uint32_t *mod);
extern int native_wsplayer_get(int *x, int *y, uint32_t *dbase, uint32_t *mbase, int *black);
extern int native_wschar_count(void);
extern int native_wschar_get(int i, int *x, int *y, int *w, int *h,
                             uint32_t *data, uint32_t *mask, int *rowstride);
extern int native_wsbanner_get(int *row, int *rel, uint32_t *data, uint32_t *mask,
                               int *pstride, int *rs, int *ww, int *rows);
extern int native_wstelanim_get(uint32_t *src, int *rel, int *w, int *h);
extern int native_wstext_get(uint32_t *str, int *rel);
extern int native_wsbanner_ash(void);

static inline uint16_t gmem_r16(const uint8_t *m, uint32_t a)
{ return (uint16_t)((m[a] << 8) | m[a + 1]); }
static inline uint32_t gmem_r32(const uint8_t *m, uint32_t a)
{ return ((uint32_t)m[a] << 24) | ((uint32_t)m[a+1] << 16) | ((uint32_t)m[a+2] << 8) | m[a+3]; }

/* Build s_objlayer from the engine's captured per-object draw list (native_wsobj_*).
 * Each object is a 5-plane plane-major sprite at absolute world (x, y): source word
 * for plane p, row r, hword ω = src + (p*h + r)*w*2 + ω*2 (A-modulo is 0, so planes
 * are tightly packed). Drawn at screen y = pf_top + worldY, absolute world x, with
 * cookie-cut transparency (colour index 0 = see-through). The bg loop composites
 * s_objlayer[y][worldX] over the tile background, sub-pixel-aligned via its own
 * worldX mapping — so objects and terrain share one absolute world coordinate, no
 * tuning constants. This REPLACES the blit-replay page display-list. */
static void native_objlayer_from_capture(int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    memset(s_objlayer, 0, sizeof s_objlayer);
    if (!M || getenv("WS_NOOBJ")) return;
    int n = native_wsobj_count();
    for (int i = 0; i < n; i++) {
        int x, y, w, h; uint32_t src, mod;
        if (!native_wsobj_get(i, &x, &y, &w, &h, &src, &mod)) continue;
        if (w <= 0 || h <= 0 || w > 64 || h > 256) continue;
        uint32_t plane_stride = (uint32_t)w * 2u * (uint32_t)h;   /* A-mod 0 => packed */
        if (src < 0x1000u || src + plane_stride * 5u > RT_MEM_SIZE) continue;
        int wpx = w * 16;
        for (int r = 0; r < h; r++) {
            int sy = pf_top + y + r;
            if (sy < pf_top || sy >= pf_bot || sy >= HW_DISPLAY_H) continue;
            const uint32_t *pal = s_scan[sy].palette;
            for (int c = 0; c < wpx; c++) {
                int worldX = x + c;
                if (worldX < 0 || worldX >= WS_LAYER_W) continue;
                uint32_t base = src + (uint32_t)r * (uint32_t)w * 2u + (uint32_t)(c >> 4) * 2u;
                int bit = 15 - (c & 15), ci = 0;
                for (int p = 0; p < 5; p++)
                    if ((gmem_r16(M, base + (uint32_t)p * plane_stride) >> bit) & 1u)
                        ci |= (1 << p);
                if (ci) s_objlayer[sy][worldX] = 0xFF000000u | (pal[ci & 0x1Fu] & 0x00FFFFFFu);
            }
        }
    }
}

/* Composite the captured PLAYER ($57A666) into s_objlayer. 16px x 16 rows,
 * cookie-cut: 5-plane DATA at dbase (plane stride $2800, row stride $28, word0),
 * 1-plane MASK at mbase (row stride $28). Drawn where the mask bit is set, with
 * the data colour index through the per-scanline copper palette. Placed at
 * absolute world (x..x+15, top y) so it shares the bg's world coordinate. */
#define WS_PLR_DATA_PSTRIDE 0x2800u
#define WS_PLR_ROW_STRIDE   0x28u
static void native_wsplayer_compose(int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    int x, y, black; uint32_t dbase, mbase;
    if (!M || getenv("WS_NOOBJ")) return;
    if (!native_wsplayer_get(&x, &y, &dbase, &mbase, &black)) return;
    if (dbase + WS_PLR_DATA_PSTRIDE * 4u + 16u * WS_PLR_ROW_STRIDE > RT_MEM_SIZE) return;
    if (mbase + 16u * WS_PLR_ROW_STRIDE > RT_MEM_SIZE) return;
    for (int r = 0; r < 16; r++) {
        int sy = pf_top + y + r;
        if (sy < pf_top || sy >= pf_bot || sy >= HW_DISPLAY_H) continue;
        const uint32_t *pal = s_scan[sy].palette;
        uint16_t mw = gmem_r16(M, mbase + (uint32_t)r * WS_PLR_ROW_STRIDE);
        uint16_t dw[5];
        for (int p = 0; p < 5; p++)
            dw[p] = gmem_r16(M, dbase + (uint32_t)p * WS_PLR_DATA_PSTRIDE
                                      + (uint32_t)r * WS_PLR_ROW_STRIDE);
        for (int c = 0; c < 16; c++) {
            int bit = 15 - c;
            if (!((mw >> bit) & 1u)) continue;           /* cookie-cut: mask gates */
            int worldX = x + c;
            if (worldX < 0 || worldX >= WS_LAYER_W) continue;
            /* Damage-blink black frame: the engine fills the mask silhouette with
             * colour 0 ($57A7E6); else the normal 5-plane data colour. */
            int ci = 0;
            if (!black)
                for (int p = 0; p < 5; p++)
                    if ((dw[p] >> bit) & 1u) ci |= (1 << p);
            s_objlayer[sy][worldX] = 0xFF000000u | (pal[ci & 0x1Fu] & 0x00FFFFFFu);
        }
    }
}

/* Composite captured cookie-cut CHARACTERS ($57D3F4 walkers/enemies) into
 * s_objlayer. Each is a 5-plane DATA + 1-plane MASK sprite, both with the same
 * per-row stride `rs` (= w*2 + BMOD; BMOD is usually -2 so rows overlap). DATA
 * plane stride = h*rs (the B-channel auto-advance per plane in the executor). The
 * mask gates transparency (cookie-cut), the data colour index runs through the
 * per-scanline copper palette. Placed at absolute world (x, y) so it shares the
 * background's world coordinate — no tuning constants. See pc_overrides_gameplay.c
 * native_char_capture / widescreen-plan.md "Phase 4 — character draw". */
static void native_wschar_compose(int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    if (!M || getenv("WS_NOOBJ")) return;
    int n = native_wschar_count();
    for (int i = 0; i < n; i++) {
        int x, y, w, h, rs; uint32_t data, mask;
        if (!native_wschar_get(i, &x, &y, &w, &h, &data, &mask, &rs)) continue;
        if (w <= 0 || h <= 0 || w > 64 || h > 256 || rs <= 0) continue;
        uint32_t pstride = (uint32_t)h * (uint32_t)rs;       /* B auto-advance/plane */
        /* DISPLAYED width = rs/2 words, NOT w. Verified from the actual blits
         * (BLIT_LOG fn=$57D688): the blit reads w words/row but the row only
         * ADVANCES `rs` bytes (rs = w*2+BMOD), and the spillover word past rs is
         * killed by BLTALWM=$0000 (the fine-shift guard word) — so only rs/2 words
         * are ever drawn. The data is packed at rs bytes/row (plane stride h*rs =
         * 84 = 21*4 on the L9 walker, matching the measured bpt step $54). Using w
         * drew the masked spillover word as a doubled second body. */
        int ww  = rs / 2; if (ww < 1) ww = 1;
        int wpx = ww * 16;
        if (data + pstride * 5u > RT_MEM_SIZE) continue;
        if (mask + (uint32_t)(h - 1) * (uint32_t)rs + (uint32_t)ww * 2u > RT_MEM_SIZE) continue;
        for (int r = 0; r < h; r++) {
            int sy = pf_top + y + r;
            if (sy < pf_top || sy >= pf_bot || sy >= HW_DISPLAY_H) continue;
            const uint32_t *pal = s_scan[sy].palette;
            for (int c = 0; c < wpx; c++) {
                int wo = c >> 4, bit = 15 - (c & 15);
                uint32_t rowoff = (uint32_t)r * (uint32_t)rs + (uint32_t)wo * 2u;
                uint16_t mw = gmem_r16(M, mask + rowoff);
                if (!((mw >> bit) & 1u)) continue;            /* cookie-cut: mask gates */
                int worldX = x + c;
                if (worldX < 0 || worldX >= WS_LAYER_W) continue;
                int ci = 0;
                for (int p = 0; p < 5; p++)
                    if ((gmem_r16(M, data + (uint32_t)p * pstride + rowoff) >> bit) & 1u)
                        ci |= (1 << p);
                s_objlayer[sy][worldX] = 0xFF000000u | (pal[ci & 0x1Fu] & 0x00FFFFFFu);
            }
        }
    }
}

#define WS_PLANE_STRIDE 0x2A0Cu      /* page bitplane stride (dest step per plane)   */
#define WS_ROWSTRIDE    46           /* playfield page row stride (bytes) = $2e      */

/* Composite the static-placement OBJECTS (the caged "Marry Men" / rescue creatures and
 * similar level decorations) that the $57D8D0 list-A and $57D3F4 char captures DON'T
 * cover. RE (logs/savestate.bin L1 + scratch/bin/caged_l1.bin, disasm tools/disasm2.py):
 * these are NOT characters — they are placement records at $5A4562 (clean world coords:
 * worldX=rec+2, worldY=rec+4), processed per-frame by the heavyweight object compositor
 * $57B0B4 (per-record re-entry at $57B0FC), which builds a cookie-cut blit descriptor
 * into queue $5A39EC (data/mask/con0/BLTSIZE/dst/BMOD), played by executor $57D6C4. The
 * per-frame CHARACTER builder $57D3F4 never sees them (the freed/walking marry man IS a
 * normal $05xxxx char and IS captured there) — that is why widescreen lost them.
 *
 * We walk that object-only queue $5A39EC directly and draw each cookie-cut descriptor
 * (mask 1-plane gates, data 5-plane plane-stride h*rs, transparent colour-0) at the
 * dst-projected world position — exactly like the player / char / list-A passes. No
 * hw_blit_capture page reverse-projection (the old, deleted native_wsmissedchar_compose
 * re-drew every page blit → ghosting/dedup, projected the 368px circular page → wrap
 * phantoms, drew colour-0 opaque → red outline). Queue $5A39EC is object-only (the char
 * builder writes a separate queue), so there is no per-frame-sprite ghosting.
 *
 * LIMITATION (open): the engine only builds a descriptor when the object is within ~the
 * 320px window, and the page is only 368px wide, so an object scrolled into the WIDE
 * margin has no descriptor → not drawn there (the caged Marry Men still "cull" in the
 * far margin, same family as remaining-issues #4). Full margin coverage needs the gfx
 * resolved from the placement record $5A4562 (stride 64) via the compositor's gfx/
 * collision tables — a larger port, not yet done. */
/* The static-object queue base (executor $57D6C4 plays it; built by $57B0B4). Fixed
 * literal in the gameplay bank ($57D5C6 / $57B0CC: lea $5a39ec,a0/a2). 24-byte
 * descriptors, terminated by a zero BLTSIZE word. */
#define WS_STATIC_QUEUE 0x5A39ECu
#define WS_STATIC_DESC  24

/* native_wsstatic_compose: per-frame inspector counts (REPL `wsstatic`). */
static int s_wsstatic_drawn = 0;       /* descriptors drawn from the live queue */
static int s_wsstatic_scanned = 0;     /* descriptors in the queue this frame   */
static int s_wsstatic_cached = 0;      /* distance-culled objects redrawn from cache */
static uint32_t s_wsstatic_dbg_bp0 = 0, s_wsstatic_dbg_first = 0;
int native_wsstatic_drawn(void) { return s_wsstatic_drawn; }
int native_wsstatic_scanned(void) { return s_wsstatic_scanned; }
int native_wsstatic_cached(void) { return s_wsstatic_cached; }
uint32_t native_wsstatic_dbg_bp0(void) { return s_wsstatic_dbg_bp0; }
uint32_t native_wsstatic_dbg_first(void) { return s_wsstatic_dbg_first; }

/* Decode one cookie-cut static sprite (mask 1-plane gates; data 5-plane, plane stride
 * h*rs; colour 0 transparent) into s_objlayer at absolute world (wx0, worldY). */
static void ws_draw_static(const uint8_t *M, int pf_top, int pf_bot,
                           int wx0, int worldY, int h, int rs, uint32_t data, uint32_t mask)
{
    int ww = rs / 2; if (ww < 1) ww = 1;             /* BLTALWM=0 kills the spillover word */
    int wpx = ww * 16;
    uint32_t pstride = (uint32_t)h * (uint32_t)rs;
    if (data + pstride * 4u + (uint32_t)(h - 1) * rs + 2u > RT_MEM_SIZE) return;
    if (mask + (uint32_t)(h - 1) * rs + 2u > RT_MEM_SIZE) return;
    for (int py = 0; py < h; py++) {
        int sy = pf_top + worldY + py;
        if (sy < pf_top || sy >= pf_bot || sy >= HW_DISPLAY_H) continue;
        const uint32_t *pal = s_scan[sy].palette;
        for (int c = 0; c < wpx; c++) {
            int wo = c >> 4, bit = 15 - (c & 15);
            uint32_t rowoff = (uint32_t)py * (uint32_t)rs + (uint32_t)wo * 2u;
            if (!((gmem_r16(M, mask + rowoff) >> bit) & 1u)) continue;  /* cookie-cut */
            int wx = wx0 + c;
            if (wx < 0 || wx >= WS_LAYER_W) continue;
            int ci = 0;
            for (int p = 0; p < 5; p++)
                if ((gmem_r16(M, data + (uint32_t)p * pstride + rowoff) >> bit) & 1u)
                    ci |= (1 << p);
            if (ci) s_objlayer[sy][wx] = 0xFF000000u | (pal[ci & 0x1Fu] & 0x00FFFFFFu);
        }
    }
}

/* Persistence cache (see the LIMITATION note above): the engine only builds a $5A39EC
 * descriptor while the object is within ~the 320px window, and the 368px page can't hold
 * the wide margin, so a caged Marry Man scrolled into the margin would vanish. We cache
 * each static object's gfx + ABSOLUTE world position while it's in view and keep drawing
 * it once distance-culled; an object that disappears while still INSIDE the engine window
 * is treated as removed (rescued/picked up) and dropped. Cache cleared on level change. */
typedef struct { int wx, wy, h, rs; uint32_t data, mask; int valid, age; } WsScObj;
#define WS_SC_MAX 24
static WsScObj s_sc[WS_SC_MAX];
static int s_sc_sig = -1;

static void native_wsstatic_compose(int pf_top, int pf_bot, int cam16)
{
    const uint8_t *M = g_mem;
    s_wsstatic_drawn = 0; s_wsstatic_scanned = 0; s_wsstatic_cached = 0;
    if (!M || getenv("WS_NOOBJ") || s_nanchors < 1) return;
    uint32_t bp0 = s_anchors[0].ptr[0];
    s_wsstatic_dbg_bp0 = bp0; s_wsstatic_dbg_first = gmem_r32(M, WS_STATIC_QUEUE + 0x10u);
    /* The object queue is built into the BACK buffer, so a descriptor's dst is usually
     * in the OTHER page than the displayed bp0 (1-frame double-buffer skew). Project
     * relative to the descriptor's OWN buffer base, shifted by the displayed page's
     * coarse-scroll offset (bp0 - its base) — NOT by bp0 directly (cross-buffer would
     * give a garbage delta). On a static level both pages share the scroll, so this is
     * exact; while scrolling it can be a few px off (the known edge-camera skew). */
    uint32_t disp_buf = (bp0 >= 0x038628u) ? 0x038628u : 0x02B3ECu;
    int scroll_off = (int)((int32_t)bp0 - (int32_t)disp_buf);

    /* Level-change clear (level-edge signature a5+$107a/$107c = $57FE8C/$57FE8E). */
    int sig = (int)(((uint32_t)gmem_r16(M, 0x57FE8Cu) << 16) | gmem_r16(M, 0x57FE8Eu));
    if (sig != s_sc_sig) { memset(s_sc, 0, sizeof s_sc); s_sc_sig = sig; }
    for (int i = 0; i < WS_SC_MAX; i++) if (s_sc[i].valid) s_sc[i].age++;

    /* Walk the object-only queue $5A39EC. Each 24-byte descriptor (the layout the
     * executor $57D6C4 reads with a6=$dff000): +0 data(B,5-plane), +4 mask(A,1-plane,
     * cookie-cut), +8 con0/con1, +C BLTAMOD/BLTDMOD (== BLTBMOD for these), +10 dst,
     * +14 BLTALWM.w, +16 BLTSIZE.w (==0 terminates). data/mask plane stride = h*rs. */
    for (int gi = 0; gi < 48; gi++) {
        uint32_t q = WS_STATIC_QUEUE + (uint32_t)gi * WS_STATIC_DESC;
        if (q + WS_STATIC_DESC > RT_MEM_SIZE) break;
        uint16_t bltsize = gmem_r16(M, q + 0x16u);
        if (bltsize == 0) break;                         /* queue terminator */
        s_wsstatic_scanned++;
        uint32_t data = gmem_r32(M, q + 0x00u);
        uint32_t mask = gmem_r32(M, q + 0x04u);
        uint16_t con0 = gmem_r16(M, q + 0x08u);
        int16_t  bmod = (int16_t)gmem_r16(M, q + 0x0Cu); /* BLTAMOD; BLTBMOD identical here */
        uint32_t dpt  = gmem_r32(M, q + 0x10u) & ~1u;    /* OCS ignores ptr bit 0 */
        int w = bltsize & 0x3F, h = bltsize >> 6;
        int shift = (con0 >> 12) & 0xF;                  /* BLTCON0 ASH */
        if (w <= 0 || h <= 0 || w > 64 || h > 256) continue;
        int rs = w * 2 + bmod;                           /* packed row stride (rows overlap) */
        if (rs <= 0) continue;

        /* dst -> on-screen base. worldY = page row (Y never scrolls). worldX from the dst
         * byte column is correct IN-VIEW (ambiguous mod the 368px page only when wrapped). */
        uint32_t desc_buf = (dpt >= 0x038628u) ? 0x038628u : 0x02B3ECu;
        if (dpt < desc_buf || dpt >= desc_buf + WS_PLANE_STRIDE) continue;
        int32_t delta = (int32_t)(dpt - desc_buf) - scroll_off;
        if (delta < 0) continue;                         /* wrapped/off the displayed page */
        int worldY = delta / WS_ROWSTRIDE;
        int xrel = (int)(delta - worldY * WS_ROWSTRIDE);
        int wx0  = cam16 + xrel * 8 + shift;             /* ABSOLUTE world X of source bit 0 */
        if (worldY + h <= 0 || worldY >= (pf_bot - pf_top)) continue;

        s_wsstatic_drawn++;
        ws_draw_static(M, pf_top, pf_bot, wx0, worldY, h, rs, data, mask);

        /* Upsert into the persistence cache (match by absolute position; static objects
         * don't move, so this refreshes the same slot every frame they're in view). */
        int slot = -1;
        for (int i = 0; i < WS_SC_MAX; i++)
            if (s_sc[i].valid && abs(s_sc[i].wx - wx0) <= 12 && abs(s_sc[i].wy - worldY) <= 6) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < WS_SC_MAX; i++) if (!s_sc[i].valid) { slot = i; break; }
        if (slot >= 0) s_sc[slot] = (WsScObj){ wx0, worldY, h, rs, data, mask, 1, 0 };
    }

    /* Redraw distance-culled cache entries (age>0 = not in this frame's queue). An entry
     * absent but still well INSIDE the engine window was removed (rescued) → drop it. */
    int rm_lo = cam16, rm_hi = cam16 + 304;
    for (int i = 0; i < WS_SC_MAX; i++) {
        if (!s_sc[i].valid || s_sc[i].age == 0) continue;        /* age==0 = just drawn from queue */
        if (s_sc[i].wx >= rm_lo && s_sc[i].wx <= rm_hi && s_sc[i].age > 4) { s_sc[i].valid = 0; continue; }
        ws_draw_static(M, pf_top, pf_bot, s_sc[i].wx, s_sc[i].wy, s_sc[i].h, s_sc[i].rs, s_sc[i].data, s_sc[i].mask);
        s_wsstatic_cached++;
    }
}

/* Composite the GET READY / GAME OVER banner as a CENTERED top UI overlay drawn
 * straight into `out` (after the playfield + objects, so nothing draws over it). Three
 * captured elements (pc_overrides_gameplay.c): the box ($578974, cookie-cut), the
 * teleport animation ($578B94, opaque 5-plane sprite in the box's right circle), and
 * the text ($578860/$57889C, an 8px column-major font in colour 16). The anim and text
 * are placed relative to the box via their page-offset DELTA from the box (camera
 * cancels), decomposed into (row, col) at the page row stride (46 bytes). The box is
 * horizontally centered in the (possibly wide) output; everything else hangs off it. */
#define WS_PAGE_RS 46                          /* playfield page row stride (bytes)     */
#define WS_TEL_PSTRIDE 0x0A80u                 /* teleport-anim plane stride            */
#define WS_FONT_BASE 0x5A0E00u                 /* banner font (column-major, 8px)       */
#define WS_FONT_RS   0x56u                     /* font glyph row stride                 */

/* The banner is screen-fixed UI, drawn directly into `out` ON TOP of everything (after
 * the playfield/objects) so nothing covers it. Its base X is the VANILLA screen
 * position `box_worldX - cam` (the object-coordinate that overlays the vanilla engine
 * render EXACTLY — verifiable to 100% with `bannercmp`) for the 352 compare; for a wide
 * output it is CENTERED. (NB: the playfield bg uses cam16 from the BPL pointer, which
 * differs from the object camera `cam`=$57FDBA by the known few-px alignment bug, so the
 * banner is anchored on `cam` to match vanilla, not on the bg.) The anim/text hang off
 * the box by their page-offset deltas. */
static void wsbanner_put(uint32_t *out, int ow, int sy, int ox, uint32_t argb)
{
    if (sy >= 0 && sy < HW_DISPLAY_H && ox >= 0 && ox < ow)
        out[(size_t)sy * ow + ox] = argb;
}
static void native_wsbanner_overlay(uint32_t *out, int ow, int cam, int pf_top)
{
    const uint8_t *M = g_mem;
    int row, brel, pstride, rs, ww, rows; uint32_t data, mask;
    if (!M || getenv("WS_NOBANNER")) return;
    if (!native_wsbanner_get(&row, &brel, &data, &mask, &pstride, &rs, &ww, &rows)) return;
    int wpx = ww * 16;
    int box_worldX = ((cam >> 4) + 3) * 16;
    int bx0 = (ow > HW_DISPLAY_W) ? (ow - wpx) / 2     /* wide: centered                  */
                                  : box_worldX - cam - 1; /* 352 compare: vanilla screen (object-coord → s_fb pixel is -1) */
    int boy = pf_top + row;
    /* The box art is blitted with a 10px A-shift; the anim/text are not (see
     * native_wsbanner_ash). We draw the box at its calibrated visual left bx0, so the
     * children — placed by their page DELTA from the box dest — subtract that shift. */
    int box_ash = native_wsbanner_ash();

    /* ── box (cookie-cut: mask gates which pixels are box; clear pixels = transparent,
     *    incl. the right-circle hole the teleport animation draws into below) ── */
    if (data + (uint32_t)pstride * 4u + (uint32_t)(rows - 1) * (uint32_t)rs
              + (uint32_t)ww * 2u < RT_MEM_SIZE &&
        mask + (uint32_t)(rows - 1) * (uint32_t)rs + (uint32_t)ww * 2u < RT_MEM_SIZE) {
        for (int r = 0; r < rows; r++) {
            int sy = boy + r;
            if (sy < 0 || sy >= HW_DISPLAY_H) continue;
            const uint32_t *pal = s_scan[sy].palette;
            for (int c = 0; c < wpx; c++) {
                int wo = c >> 4, bit = 15 - (c & 15);
                uint32_t roff = (uint32_t)r * (uint32_t)rs + (uint32_t)wo * 2u;
                if ((gmem_r16(M, mask + roff) >> bit) & 1u) {
                    int ci = 0;
                    for (int p = 0; p < 5; p++)
                        if ((gmem_r16(M, data + (uint32_t)p * (uint32_t)pstride + roff) >> bit) & 1u)
                            ci |= (1 << p);
                    wsbanner_put(out, ow, sy, bx0 + c, pal[ci & 0x1Fu]);
                }
            }
        }
    }

    /* ── teleport animation: opaque 32x28 4-plane sprite (packed src rows) ── */
    { uint32_t tsrc; int trel, tw, th;
      if (native_wstelanim_get(&tsrc, &trel, &tw, &th)) {
        int delta = trel - brel;
        int drow  = (delta >= 0) ? delta / WS_PAGE_RS : -(((-delta) + WS_PAGE_RS - 1) / WS_PAGE_RS);
        int dcol  = delta - drow * WS_PAGE_RS;
        int txx = bx0 + dcol * 8 - box_ash, tsy0 = boy + drow, twpx = tw * 16;
        uint32_t trs = (uint32_t)tw * 2u;             /* source rows PACKED (amod=0)   */
        if (tsrc + WS_TEL_PSTRIDE * 4u + (uint32_t)(th - 1) * trs + (uint32_t)tw * 2u
                < RT_MEM_SIZE) {
            for (int r = 0; r < th; r++) {
                int sy = tsy0 + r;
                if (sy < 0 || sy >= HW_DISPLAY_H) continue;
                const uint32_t *pal = s_scan[sy].palette;
                for (int c = 0; c < twpx; c++) {
                    int wo = c >> 4, bit = 15 - (c & 15);
                    uint32_t roff = (uint32_t)r * trs + (uint32_t)wo * 2u;
                    int ci = 0;
                    for (int p = 0; p < 4; p++)         /* anim is 4 planes (moveq #3,d6) */
                        if ((gmem_r16(M, tsrc + (uint32_t)p * WS_TEL_PSTRIDE + roff) >> bit) & 1u)
                            ci |= (1 << p);
                    /* The engine blits only planes 0-3; the circle background has plane 4
                     * SET, so the displayed colour is ci|16 (verified: page index = src+16). */
                    wsbanner_put(out, ow, sy, txx + c, pal[(ci | 16) & 0x1Fu]);   /* opaque */
                }
            }
        }
      } }

    /* ── text: 8px column-major font ($5A0E00), colour 16, one char per 8px ── */
    { uint32_t str; int xrel;
      if (native_wstext_get(&str, &xrel)) {
        int delta = xrel - brel;
        int drow  = (delta >= 0) ? delta / WS_PAGE_RS : -(((-delta) + WS_PAGE_RS - 1) / WS_PAGE_RS);
        int dcol  = delta - drow * WS_PAGE_RS;
        int txx = bx0 + dcol * 8 - box_ash, tsy0 = boy + drow;
        for (int k = 0; k < 40; k++) {
            uint32_t ca = str + (uint32_t)k;
            if (ca >= RT_MEM_SIZE) break;
            uint8_t ch = M[ca];
            if (ch == 0) break;
            if (ch < 0x20) continue;
            uint32_t glyph = WS_FONT_BASE + (uint32_t)(ch - 0x20);
            for (int r = 0; r < 16; r++) {
                int sy = tsy0 + r;
                if (sy < 0 || sy >= HW_DISPLAY_H) continue;
                uint32_t ga = glyph + (uint32_t)r * WS_FONT_RS;
                if (ga >= RT_MEM_SIZE) continue;
                uint8_t gb = M[ga];
                if (!gb) continue;
                const uint32_t *pal = s_scan[sy].palette;
                for (int px = 0; px < 8; px++)
                    if ((gb >> (7 - px)) & 1u) wsbanner_put(out, ow, sy, txx + k * 8 + px, pal[16]);
            }
        }
      } }
}

void native_render_wide_bg(uint32_t *out, int ow, int margin)
{
    if (margin < 0 || s_cur_cop1lc != WS_GAMEPLAY_COP1LC) return;   /* gameplay only (margin 0 = compare-at-352) */
    const uint8_t *M = g_mem;
    if (!M || WS_GFXTAB + 4u >= RT_MEM_SIZE) return;

    int cam    = (int16_t)gmem_r16(M, WS_CAMERA);   /* camera is SIGNED (can be -16 etc.) */

    /* Tilemap row stride is PER-LEVEL (narrow levels have fewer columns/row), so it
     * must NOT be hardcoded. The engine stores a row-offset table at $57F4BC whose
     * entry[k].d2adj = k * rowstride (16-byte fine-scroll phase table, {d2adj.w,
     * a3adj.w} x 16). entry[1].d2adj = the per-level row stride in bytes (L1=64,
     * L9=244=$F4). Hardcoding $F4 made every non-L9 level decode the wrong rows. */
    int rowstride = (int16_t)gmem_r16(M, WS_PHASETAB + 4);
    if (rowstride < 0) rowstride = -rowstride;
    if (rowstride <= 0) rowstride = (int)WS_TILEROW;   /* fallback to L9 if unreadable */
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

    /* Wide camera (the margin>0 = real-widescreen path). The view must stay CLAMPED to
     * the level's world bounds (never reveal void past an edge) and, when the level is
     * NARROWER than the view, the level is CENTERED (equal black margins) — both per the
     * user. worldX maps 1:1 to output x (worldX = view_left + x); the tilemap, objects and
     * player are all keyed by absolute worldX so they track automatically, and smooth
     * scroll comes from the signed camera's fine bits (no page coarse/fine hysteresis since
     * we read the tilemap directly). The margin==0 COMPARE path keeps the exact
     * engine-aligned mapping in the loop below so wsdiff stays a valid pixel check. */
    int eng_left = cam + 16;                              /* engine's displayed left world X */
    int level_lo = mincol * 16;
    int level_hi = maxcol * 16;
    int level_w  = level_hi - level_lo;
    int view_left;
    if (level_w <= ow)
        view_left = level_lo - (ow - level_w) / 2;        /* center the narrow level */
    else {
        view_left = eng_left - (ow - 320) / 2;            /* follow player, extend both sides */
        if (view_left < level_lo)      view_left = level_lo;
        if (view_left > level_hi - ow) view_left = level_hi - ow;
    }

    { extern int g_ws_view_left, g_ws_view_w; g_ws_view_left = view_left; g_ws_view_w = ow; }

    /* Playfield vertical extent = the scanline span of the FIRST BPL anchor (the
     * scrolling buffer pointers); the next anchor begins the HUD. Both are derived by
     * walk_copper. tilemap row 0 maps to the playfield's first scanline (no vert scroll). */
    /* Playfield top = first scanline the scrolling 5-plane buffer actually displays
     * (the first BPL anchor can fire at the top border before the playfield). Bottom =
     * the next BPL anchor below it (the HUD re-points the pointers there). */
    int pf_top = -1;
    for (int y = 0; y < HW_DISPLAY_H; y++)
        if (((s_scan[y].bplcon0 >> 12) & 7) >= 5 && !((s_scan[y].bplcon0 >> 10) & 1)) { pf_top = y; break; }
    int pf_bot = 0;
    if (pf_top >= 0) {
        pf_bot = pf_top + 16 * 16;
        for (int a = 0; a < s_nanchors; a++)
            if (s_anchors[a].first_line > pf_top && s_anchors[a].first_line < pf_bot)
                pf_bot = s_anchors[a].first_line;
        if (pf_bot > HW_DISPLAY_H) pf_bot = HW_DISPLAY_H;
    }

    if (pf_top < 0) return;
    /* Fill the object layer from the engine's captured per-object draw list, then
     * composite the player (drawn by its own routine $57A666, not the object loop). */
    native_objlayer_from_capture(pf_top, pf_bot);
    native_wschar_compose(pf_top, pf_bot);
    native_wsplayer_compose(pf_top, pf_bot);
    native_wsstatic_compose(pf_top, pf_bot, cam16);   /* caged Marry Men + static-placement objects */

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

        /* ONE continuous native renderer: EVERY output pixel maps to an absolute world X
         * (cam16 + bitplane index) and is decoded straight from the level tilemap — there
         * is NO centre/margin split and NO clip to the engine's narrow data-fetch window
         * (that clip is what created a seam at the centre/margin boundary). The only clip
         * is the real level edge (mincol/maxcol = where the camera physically stops). The
         * engine's bitplane/page is NOT used for the playfield. */
        for (int x = 0; x < ow; x++) {
            int worldX;
            if (margin > 0) {
                worldX = view_left + x;                       /* wide: 1:1, centered/clamped */
            } else {
                int lx = (x - margin) - x_off - scroll1;      /* compare: engine bitplane index */
                worldX = cam16 + lx;                          /* engine-aligned mapping */
            }
            if (worldX < 0) continue;
            int col = worldX >> 4;
            if (col < mincol || col >= maxcol) continue;
            int tx = worldX & 15;
            uint32_t mo = WS_TILEMAP + (uint32_t)r * (uint32_t)rowstride + (uint32_t)col * 2u;
            if (mo + 1u >= RT_MEM_SIZE) continue;
            uint16_t w = gmem_r16(M, mo);
            uint32_t gfx = gmem_r32(M, WS_GFXTAB + (uint32_t)(w & 0xFFFEu));
            if (gfx < 0x1000u || gfx + 160u > RT_MEM_SIZE) continue;
            int ci = 0;
            for (int p = 0; p < 5; p++)
                if ((gmem_r16(M, gfx + (uint32_t)p * 32u + (uint32_t)ty * 2u) >> (15 - tx)) & 1u)
                    ci |= (1 << p);
            row[x] = pal[ci & 0x1Fu];

            /* Composite the captured gameplay objects (s_objlayer, keyed by absolute
             * world X — same coordinate as the tile bg) on top of the terrain. */
            if (worldX >= 0 && worldX < WS_LAYER_W) {
                uint32_t o = s_objlayer[y][worldX];
                if (o & 0xFF000000u) row[x] = o;
            }
        }
    }

    /* Banner (GET READY / GAME OVER) — screen-fixed UI on top of everything. */
    native_wsbanner_overlay(out, ow, cam, pf_top);
}

/* Re-draw the engine's captured object sprite blits natively into the wide buffer.
 * Each object is 5 plane-blits (dest steps by the plane stride $2A0C); position is
 * recovered from the dest's offset within its double-buffer page vs the scroll
 * coarse offset, mapped into the same screen coordinate the tile bg uses. Opaque
 * blits (con0 $09F0, D=A) copy all pixels; cookie-cut blits (minterm $CA) draw only
 * where the shared mask plane is set (transparency). */
/* WS_PLANE_STRIDE / WS_ROWSTRIDE defined above (near native_wsstatic_compose). */

/* Page display-list model. The engine's playfield page is built by DRAW blits (objects,
 * the GET READY banner) and torn down by background-RESTORE blits. We mirror it as a list
 * of live page-sprites (s_pg), keyed by their page address — camera-independent — and
 * re-project the whole list to the screen every frame from the live BPL pointer. Static
 * decorations (box, torch) are baked into the saved bg, so a restore must NOT remove them;
 * those keep living in the persistent world-pixel s_decolayer exactly as before. */

/* plane-0 page rectangle of a sprite/restore in page-relative (row, byte-col) space. */
static void pg_rect(uint32_t base, uint32_t dpt, int w, int h,
                    int *r0, int *c0, int *r1, int *c1)
{
    int off = (int)(dpt - base);
    *r0 = off / WS_ROWSTRIDE; *r1 = *r0 + h;
    *c0 = off % WS_ROWSTRIDE; *c1 = *c0 + w * 2;
}
static int pg_overlap(const PgSprite *s, uint32_t base, uint32_t dpt, int w, int h)
{
    if (s->base != base) return 0;
    int ar0,ac0,ar1,ac1, br0,bc0,br1,bc1;
    pg_rect(s->base, s->dpt, s->w, s->h, &ar0,&ac0,&ar1,&ac1);
    pg_rect(base,    dpt,    w,    h,    &br0,&bc0,&br1,&bc1);
    return !(ar1 <= br0 || br1 <= ar0 || ac1 <= bc0 || bc1 <= ac0);
}

/* Ingest this frame's captured blits into the page display-list. GEOMETRY-FREE, so it can
 * (and must) run on EVERY present — even during the copper transition before the gameplay
 * playfield exists. The GET READY banner is drawn ~16 frames before the gameplay copper
 * comes up; the old code gated the whole update behind the gameplay-copper check, so the
 * banner was wiped from the 1-frame capture long before it could be displayed. Restore
 * blits remove the (non-decoration) sprites they overwrite; object/decoration draws are
 * added or refreshed in place. Keyed by page address → camera-independent. */
static void native_objlayer_ingest(void)
{
    int n = hw_blit_capture_count();
    const BlitRec *rec = hw_blit_capture_recs();
    if (!g_mem) return;
    s_diag_prev_n = n;
    s_diag_bplpt0 = (s_nanchors >= 1) ? s_anchors[0].ptr[0] : 0u;
    s_diag_disp_base = (s_diag_bplpt0 >= 0x038628u) ? 0x038628u : 0x02B3ECu;
    s_blitdiag_n = 0;
    s_objup_runs++; if (n > 0) s_objup_nonzero++;

    int i = 0;
    while (i < n) {
        int g0 = i, np = 1;
        while (i + np < n && np < 5
               && rec[i + np].dpt == rec[i + np - 1].dpt + WS_PLANE_STRIDE
               && rec[i + np].w == rec[g0].w && rec[i + np].h == rec[g0].h) np++;
        const BlitRec *r0 = &rec[g0];
        i += np;

        WsBlitDiag *dg = (s_blitdiag_n < (int)(sizeof s_blitdiag / sizeof s_blitdiag[0]))
                         ? &s_blitdiag[s_blitdiag_n++] : NULL;
        if (dg) { *dg = (WsBlitDiag){0}; dg->src=r0->src; dg->dpt=r0->dpt; dg->mask=r0->mask;
                  dg->w=r0->w; dg->h=r0->h; dg->np=np; dg->con0=r0->con0; }

        uint32_t base;
        if (r0->dpt >= 0x038628u && r0->dpt < 0x038628u + WS_PLANE_STRIDE) base = 0x038628u;
        else if (r0->dpt >= 0x02B3ECu && r0->dpt < 0x02B3ECu + WS_PLANE_STRIDE) base = 0x02B3ECu;
        else { if (dg) dg->drop = 'b'; continue; }

        /* Skip pure fills (no A/B source channel, e.g. the full-page clear con0=$0100): they
         * are page background prep, not sprites — projecting one paints a giant rectangle. */
        if (!((r0->con0 >> 11) & 1) && !((r0->con0 >> 10) & 1)) { if (dg) dg->drop = 'f'; continue; }

        int masked  = (r0->mask != 0u);
        int restore = (!masked && r0->src >= 0x045000u && r0->src < 0x054000u);  /* bg-restore */
        int deco    = (r0->src >= 0x054000u && r0->src < 0x060000u);             /* baked-in   */
        if (dg) dg->cls = deco ? 'D' : (restore ? 'R' : 'O');

        if (restore) {                            /* remove the (non-deco) sprites it overwrites */
            for (int k = 0; k < s_pgn; )
                if (!s_pg[k].deco && pg_overlap(&s_pg[k], base, r0->dpt, r0->w, r0->h))
                    { s_pg[k] = s_pg[--s_pgn]; s_pg_removes++; }
                else k++;
            continue;
        }
        /* object/banner/decoration: add, or refresh an entry already at this page address. */
        PgSprite *ps = NULL;
        for (int k = 0; k < s_pgn; k++)
            if (s_pg[k].base == base && s_pg[k].dpt == r0->dpt) { ps = &s_pg[k]; break; }
        if (!ps && s_pgn < PG_MAX) { ps = &s_pg[s_pgn++]; s_pg_adds++; }
        if (ps) {
            ps->base=base; ps->dpt=r0->dpt; ps->mask=r0->mask; ps->np=np; ps->deco=deco;
            ps->w=r0->w; ps->h=r0->h; ps->shift=r0->shift; ps->smod=r0->smod; ps->mmod=r0->mmod;
            for (int p = 0; p < np; p++) {
                ps->src[p] = rec[g0 + p].src;
                /* A plane with no A/B source channel is a constant fill (minterm output for
                 * all-zero inputs = con0 bit0), e.g. a "set this plane to 1 everywhere" used
                 * to push a sprite into the bright COLOR16-31 range. Reading it as gfx would
                 * corrupt that plane's bit. */
                uint16_t c = rec[g0 + p].con0;
                ps->pfill[p] = (((c >> 11) & 1) || ((c >> 10) & 1)) ? -1 : (int)(c & 1);
            }
        }
    }
}

/* Project the displayed buffer's live page-list into s_objlayer (decorations first, then
 * objects on top), using the live BPL pointer + camera. Requires a renderable playfield. */
static void native_objlayer_project(int cam16, int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    if (s_nanchors < 1) return;
    uint32_t disp_base = (s_anchors[0].ptr[0] >= 0x038628u) ? 0x038628u : 0x02B3ECu;
    memset(s_objlayer, 0, sizeof s_objlayer);
    s_diag_cam16 = cam16;
    for (int pass = 1; pass >= 0; pass--) {            /* pass 1 = decorations, pass 0 = objects */
        for (int k = 0; k < s_pgn; k++) {
            PgSprite *ps = &s_pg[k];
            if (ps->base != disp_base || ps->deco != pass) continue;
            int32_t delta = (int32_t)(ps->dpt - s_anchors[0].ptr[0]);
            int orow = (delta >= 0) ? (delta / WS_ROWSTRIDE)
                                    : -(((-delta) + WS_ROWSTRIDE - 1) / WS_ROWSTRIDE);
            int xrel = (int)(delta - orow * WS_ROWSTRIDE);
            int wpx = ps->w * 16, srow = ps->w * 2 + ps->smod, mrow = ps->w * 2 + ps->mmod;
            int masked = (ps->mask != 0u), wx0 = cam16 + xrel * 8 + ps->shift;
            /* The OCS barrel-shifter shifts the source right by `shift` within the fixed w
             * dest words; the rightmost `shift` source columns are shifted past the last
             * word and dropped. Draw only those columns (drawing all wpx paints `shift`
             * phantom columns at the right edge). */
            int draww = wpx - ps->shift;
            ps->pj_wx0 = wx0; ps->pj_sy0 = pf_top + orow;
            for (int py = 0; py < ps->h; py++) {
                int sy = pf_top + orow + py;
                if (sy < pf_top || sy >= pf_bot || sy >= HW_DISPLAY_H) continue;
                const uint32_t *pal = s_scan[sy].palette;
                for (int px = 0; px < draww; px++) {
                    int wx = wx0 + px; if (wx < 0 || wx >= WS_LAYER_W) continue;
                    if (masked) {
                        uint32_t ma = ps->mask + (uint32_t)py * mrow + (uint32_t)(px >> 3);
                        if (ma + 1u >= RT_MEM_SIZE || !((M[ma] >> (7 - (px & 7))) & 1u)) continue;
                    }
                    int ci = 0;
                    for (int p = 0; p < ps->np; p++) {
                        if (ps->pfill[p] >= 0) { if (ps->pfill[p]) ci |= (1 << p); continue; }
                        uint32_t sa = ps->src[p] + (uint32_t)py * srow + (uint32_t)(px >> 3);
                        if (sa + 1u < RT_MEM_SIZE && ((M[sa] >> (7 - (px & 7))) & 1u)) ci |= (1 << p);
                    }
                    s_objlayer[sy][wx] = 0xFF000000u | (pal[ci & 0x1Fu] & 0xFFFFFFu);
                }
            }
        }
    }
}

