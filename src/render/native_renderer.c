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
#include "engine/hw_private.h"   /* pulls in rt.h → g_mem, and amiga_to_argb(), s_regs, etc. */
#include "render/engine_view.h"  /* the wide renderer's only engine-state input (the firewall) */
#include "render/scene.h"        /* the per-frame gameplay draw list (renderer/backend seam) */
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
     * s_regs[BPLCON0] holds the copper list's LAST BPLCON0 write (star-field DPF
     * mode), which is wrong for border lines, so we don't seed from it here.
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
#define WS_LAYER_W  2560             /* world object-layer width (px), absolute world X */
int g_ws_view_left = 0, g_ws_view_w = 0;   /* last wide-render view mapping (worldX = view_left + x) */
/* ============================================================================
 * WIDESCREEN GAMEPLAY SPRITE/OBJECT PIPELINE
 *
 * native_render_wide_bg() composes the whole gameplay frame natively across the wide
 * output: the tilemap background (decoded by absolute world X) + every sprite category,
 * drawn into a shared world-space object layer (s_objlayer: absolute world X, screen Y),
 * then blitted to the output for the visible window. We do NOT read the engine's 368px
 * page (it can't hold a wide view); each sprite category is captured at its engine choke
 * point (camera-INDEPENDENT, before the engine's own clip) or resolved from level data,
 * then drawn at its true world position. The capture overrides live in
 * pc_overrides_gameplay.c; the compose passes are below. Categories:
 *
 *   list-A objects  $57D8D0  capture native_objdraw_capture  -> native_wsobj_compose
 *                            (platforms, pickups, ladders, box; walker cull widened #4)
 *   characters      $57D3F4  capture native_char_capture     -> native_wschar_compose
 *                            (walkers, enemies, FREED marry men)
 *   player          $57A666  capture native_player_capture   -> native_wsplayer_compose
 *   static objects  $5A4562  (resolved from level data)      -> native_wsstatic_compose
 *                            (caged Marry Men + queue $5A39EC objects; live anim, no cull)
 *   banner          $578974+ capture native_banner_capture   -> native_wsbanner_overlay
 *                            (GET READY / GAME OVER, drawn last as a centered UI overlay)
 *
 * (A superseded "page display-list" that reverse-projected raw blits — s_pg / *_ingest /
 * *_project — was removed; it could not represent the wide margins and is not the model.)
 * ============================================================================ */
static uint32_t s_objlayer[HW_DISPLAY_H][WS_LAYER_W];

/* The per-frame gameplay draw list (Phase 1 of the GPU-renderer plan): each
 * sprite pass emits index-bitmap quads here instead of resolving ARGB into
 * s_objlayer directly. The CPU rasterizer (scene_composite_argb) reproduces the
 * old pixels byte-for-byte; the Vulkan/SDL backends will draw the SAME list.
 * See instructions/gpu-renderer-plan.md. */
static Scene s_scene;
static int   s_scene_ylo = 0, s_scene_yhi = 0;   /* playfield row span the scene targets */

/* Accessors for the per-sprite backends / headless verification (scene_sdl,
 * vk consumer). The scene is populated by native_render_wide_bg (BenRen). */
int native_render_line_info(int y, uint32_t pt[5], int *xoff, int *scr1, int *width)
{
    if (y < 0 || y >= HW_DISPLAY_H) return 0;
    for (int p = 0; p < MAX_PLANES; p++) pt[p] = s_line_bplpt[y][p];
    if (xoff)  *xoff  = s_line_xoff[y];
    if (scr1)  *scr1  = s_line_scr1[y];
    if (width) *width = s_line_width[y];
    return s_line_bpu[y];
}

const Scene *native_render_scene(void) { return &s_scene; }
void native_render_scene_yrange(int *lo, int *hi) { if (lo) *lo = s_scene_ylo; if (hi) *hi = s_scene_yhi; }
void native_render_scene_dims(int *w, int *h) { if (w) *w = WS_LAYER_W; if (h) *h = HW_DISPLAY_H; }

/* Scene freshness: the windowed per-sprite present must only consume a scene
 * built THIS frame (menus / the level card leave the previous gameplay scene
 * stale). hw_compose_output invalidates before the BenRen compose; the compose
 * marks it ready once it has built this frame's draw list. */
static int s_scene_ready = 0;
void native_render_scene_invalidate(void) { s_scene_ready = 0; }
int  native_render_scene_ready(void)      { return s_scene_ready; }

/* Copy walk_copper's per-scanline palette into the scene's per-row colour LUT
 * (this engine's palette is copper-driven, so colours are per output row). */
static void scene_load_palrows(void)
{
    for (int y = 0; y < HW_DISPLAY_H; y++)
        memcpy(s_scene.pal_rows[y], s_scan[y].palette, sizeof s_scene.pal_rows[y]);
}

#define WS_GFXTAB   0x005A539Eu
#define WS_GAMEPLAY_COP1LC 0x003484u

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

/* The wide view's clamped world-left for output width `ow` — the SINGLE SOURCE of the
 * wide-camera mapping (worldX = view_left + screen_x). Used by both the renderer
 * (native_render_wide_bg) and the object-cull overrides (native_objstep[_b]) so the cull
 * window can never drift from what's actually on screen. Narrow level → centered; else
 * follow the engine camera, clamped to the level edges. */
int ws_view_left(int ow)
{
    EngineView ev;
    if (!engine_view_capture(&ev)) return 0;   /* no sourced state -> no view (no fudge) */
    /* World column span the camera can ever reveal, from the engine's own clamp
     * range (ev.level_lo/hi already include the engine clamp biases). level_lo can
     * be slightly negative at the left edge; the visible world starts at column 0. */
    int level_lo = (ev.level_lo < 0 ? 0 : ev.level_lo) & ~15;
    int level_hi = (ev.level_hi + 320) & ~15;
    int level_w  = level_hi - level_lo;
    if (level_w <= ow) return level_lo - (ow - level_w) / 2;   /* center narrow level */
    /* FREE CAM (src/port/freecam.c): a detached, user-panned follow-point
     * replaces the engine camera. Same clamps below, so it can't leave the
     * level; the object-cull overrides share this view so margins re-derive. */
    extern int pc_freecam_active(void), pc_freecam_x(void);
    int follow = pc_freecam_active() ? pc_freecam_x() : ev.camera + 16;
    int vl = follow - (ow - 320) / 2;                          /* follow player/cam */
    if (vl < level_lo)      vl = level_lo;
    if (vl > level_hi - ow) vl = level_hi - ow;
    return vl;
}

/* Tilemap BACKGROUND on the draw list: emit each visible level tile as an OPAQUE
 * 16x16 colour-index quad (world X = col*16, screen Y = pf_top + r*16), so the whole
 * gameplay frame — background + sprites — is one scene draw list (the precondition for
 * a per-sprite SDL/Vulkan present). Decoded exactly as the old per-pixel loop (tilemap
 * word -> $5A539E gfx -> 5-plane decode), so output stays byte-identical; the consumer
 * resolves the per-row palette. Emitted FIRST (before the sprite passes) so sprites
 * composite over the terrain. Tiles outside [mincol,maxcol) or with no gfx emit no quad
 * -> the void the main loop paints black. */
static void native_wstiles_compose(int pf_top, int pf_bot, int rowstride, int mincol, int maxcol)
{
    const uint8_t *M = g_mem;
    if (!M || getenv("WS_NOTILES")) return;
    int nrows = (pf_bot - pf_top + 15) >> 4;
    for (int r = 0; r < nrows; r++) {
        for (int col = mincol; col < maxcol; col++) {
            int wx = col * 16;
            if (wx < 0 || wx + 16 > WS_LAYER_W) continue;
            uint32_t mo = WS_TILEMAP + (uint32_t)r * (uint32_t)rowstride + (uint32_t)col * 2u;
            if (mo + 1u >= RT_MEM_SIZE) continue;
            uint16_t w   = gmem_r16(M, mo);
            uint32_t gfx = gmem_r32(M, WS_GFXTAB + (uint32_t)(w & 0xFFFEu));
            if (gfx < 0x1000u || gfx + 160u > RT_MEM_SIZE) continue;   /* void: no tile here */
            uint8_t *idx = scene_alloc_idx(&s_scene, 16u * 16u);
            if (!idx) continue;
            for (int ty = 0; ty < 16; ty++) {
                uint16_t pl[5];
                for (int p = 0; p < 5; p++)
                    pl[p] = gmem_r16(M, gfx + (uint32_t)p * 32u + (uint32_t)ty * 2u);
                uint8_t *irow = idx + (size_t)ty * 16;
                for (int tx = 0; tx < 16; tx++) {
                    int bit = 15 - tx, ci = 0;
                    for (int p = 0; p < 5; p++) if ((pl[p] >> bit) & 1u) ci |= (1 << p);
                    irow[tx] = (uint8_t)ci;            /* opaque index (0..31); no cookie-cut */
                }
            }
            scene_add_quad(&s_scene, wx, pf_top + r * 16, 16, 16, idx, 16);
        }
    }
}

/* Build s_objlayer from the engine's captured per-object draw list (native_wsobj_*).
 * Each object is a 5-plane plane-major sprite at absolute world (x, y): source word
 * for plane p, row r, hword ω = src + (p*h + r)*w*2 + ω*2 (A-modulo is 0, so planes
 * are tightly packed). Drawn at screen y = pf_top + worldY, absolute world x, with
 * cookie-cut transparency (colour index 0 = see-through). The bg loop composites
 * s_objlayer[y][worldX] over the tile background, sub-pixel-aligned via its own
 * worldX mapping — so objects and terrain share one absolute world coordinate, no
 * tuning constants. This REPLACES the blit-replay page display-list. */
static void native_wsobj_compose(int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    (void)pf_bot;
    if (!M || getenv("WS_NOOBJ")) return;
    int n = native_wsobj_count();
    for (int i = 0; i < n; i++) {
        int x, y, w, h; uint32_t src, mod;
        if (!native_wsobj_get(i, &x, &y, &w, &h, &src, &mod)) continue;
        if (w <= 0 || h <= 0 || w > 64 || h > 256) continue;
        uint32_t plane_stride = (uint32_t)w * 2u * (uint32_t)h;   /* A-mod 0 => packed */
        if (src < 0x1000u || src + plane_stride * 5u > RT_MEM_SIZE) continue;
        int wpx = w * 16;
        /* Decode the sprite into a colour-index bitmap (0..31, 0xFF = transparent
         * where the colour index is 0) and emit it as a draw-list quad at the
         * object's absolute world (x, pf_top+y). The CPU rasterizer composites it
         * into s_objlayer below — same pixels as the old direct write. */
        uint8_t *idx = scene_alloc_idx(&s_scene, (uint32_t)wpx * (uint32_t)h);
        if (!idx) continue;
        for (int r = 0; r < h; r++) {
            uint8_t *irow = idx + (size_t)r * wpx;
            for (int c = 0; c < wpx; c++) {
                uint32_t base = src + (uint32_t)r * (uint32_t)w * 2u + (uint32_t)(c >> 4) * 2u;
                int bit = 15 - (c & 15), ci = 0;
                for (int p = 0; p < 5; p++)
                    if ((gmem_r16(M, base + (uint32_t)p * plane_stride) >> bit) & 1u)
                        ci |= (1 << p);
                irow[c] = ci ? (uint8_t)ci : SCENE_TRANSPARENT;
            }
        }
        scene_add_quad(&s_scene, x, pf_top + y, wpx, h, idx, wpx);
    }
}

/* ANIMATED PAGE PATCHES (water surface line) — the object walker's multi-tile path
 * $57D81C CPU-writes a 16px x 2-row, 5-plane patch per record straight into the page
 * (word order p0r0,p0r1,p1r0,p1r1,...), bypassing the blitter AND culled to the
 * vanilla window — BenRen missed it entirely. native_anim_patch (gameplay.c) captures
 * every record PRE-cull with the engine's own source/dest math: worldX (the record's
 * cull coordinate, same family as the $57D8D0 object worldX), page row (= playfield-
 * relative worldY, no vertical scroll) and the resolved animated source pointer. The
 * patch is an OPAQUE overwrite in the page (plain move.w, no mask), so index 0 stays
 * colour 0 here. Drawn over the tiles, under objects (page write order). */
static void native_wswater_compose(int pf_top, int pf_bot)
{
    const uint8_t *M = g_mem;
    (void)pf_bot;
    if (!M || getenv("WS_NOWATER")) return;
    extern int native_wswater_count(void);
    extern int native_wswater_get(int, int*, int*, int*, uint32_t*);
    int n = native_wswater_count();
    for (int i = 0; i < n; i++) {
        int wx, row, col; uint32_t src;
        if (!native_wswater_get(i, &wx, &row, &col, &src)) continue;
        if (src < 0x1000u || src + 20u > RT_MEM_SIZE) continue;
        uint8_t *idx = scene_alloc_idx(&s_scene, 16u * 2u);
        if (!idx) continue;
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 16; c++) {
                int ci = 0;
                for (int p = 0; p < 5; p++)
                    if ((gmem_r16(M, src + (uint32_t)(p * 2 + r) * 2u) >> (15 - c)) & 1u)
                        ci |= (1 << p);
                idx[r * 16 + c] = (uint8_t)ci;
            }
        scene_add_quad(&s_scene, wx, pf_top + row, 16, 2, idx, 16);
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
    (void)pf_bot;
    int x, y, black; uint32_t dbase, mbase;
    if (!M || getenv("WS_NOOBJ")) return;
    if (!native_wsplayer_get(&x, &y, &dbase, &mbase, &black)) return;
    if (dbase + WS_PLR_DATA_PSTRIDE * 4u + 16u * WS_PLR_ROW_STRIDE > RT_MEM_SIZE) return;
    if (mbase + 16u * WS_PLR_ROW_STRIDE > RT_MEM_SIZE) return;
    uint8_t *idx = scene_alloc_idx(&s_scene, 16u * 16u);
    if (!idx) return;
    for (int r = 0; r < 16; r++) {
        uint8_t *irow = idx + (size_t)r * 16;
        uint16_t mw = gmem_r16(M, mbase + (uint32_t)r * WS_PLR_ROW_STRIDE);
        uint16_t dw[5];
        for (int p = 0; p < 5; p++)
            dw[p] = gmem_r16(M, dbase + (uint32_t)p * WS_PLR_DATA_PSTRIDE
                                      + (uint32_t)r * WS_PLR_ROW_STRIDE);
        for (int c = 0; c < 16; c++) {
            int bit = 15 - c;
            if (!((mw >> bit) & 1u)) { irow[c] = SCENE_TRANSPARENT; continue; }  /* cookie-cut */
            /* Damage-blink black frame: the engine fills the mask silhouette with
             * colour 0 ($57A7E6); else the normal 5-plane data colour. */
            int ci = 0;
            if (!black)
                for (int p = 0; p < 5; p++)
                    if ((dw[p] >> bit) & 1u) ci |= (1 << p);
            irow[c] = (uint8_t)ci;
        }
    }
    scene_add_quad(&s_scene, x, pf_top + y, 16, 16, idx, 16);
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
    (void)pf_bot;
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
        uint8_t *idx = scene_alloc_idx(&s_scene, (uint32_t)wpx * (uint32_t)h);
        if (!idx) continue;
        for (int r = 0; r < h; r++) {
            uint8_t *irow = idx + (size_t)r * wpx;
            for (int c = 0; c < wpx; c++) {
                int wo = c >> 4, bit = 15 - (c & 15);
                uint32_t rowoff = (uint32_t)r * (uint32_t)rs + (uint32_t)wo * 2u;
                uint16_t mw = gmem_r16(M, mask + rowoff);
                if (!((mw >> bit) & 1u)) { irow[c] = SCENE_TRANSPARENT; continue; }  /* cookie-cut */
                int ci = 0;
                for (int p = 0; p < 5; p++)
                    if ((gmem_r16(M, data + (uint32_t)p * pstride + rowoff) >> bit) & 1u)
                        ci |= (1 << p);
                irow[c] = (uint8_t)ci;
            }
        }
        scene_add_quad(&s_scene, x, pf_top + y, wpx, h, idx, wpx);
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
 * far margin). Full margin coverage needs the gfx
 * resolved from the placement record $5A4562 (stride 64) via the compositor's gfx/
 * collision tables — a larger port, not yet done. */
/* The static-object queue base (executor $57D6C4 plays it; built by $57B0B4). Fixed
 * literal in the gameplay bank ($57D5C6 / $57B0CC: lea $5a39ec,a0/a2). 24-byte
 * descriptors, terminated by a zero BLTSIZE word. */
#define WS_STATIC_QUEUE 0x5A39ECu
#define WS_STATIC_DESC  24

/* native_wsstatic_compose: per-frame inspector counts (REPL `wsstatic`). */
static int s_wsstatic_drawn = 0;       /* non-Marry-Man descriptors drawn from the queue */
static int s_wsstatic_scanned = 0;     /* descriptors in the queue this frame   */
static int s_wsstatic_cached = 0;      /* Marry Men resolved+drawn from placement records */
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
    (void)pf_bot;
    int ww = rs / 2; if (ww < 1) ww = 1;             /* BLTALWM=0 kills the spillover word */
    int wpx = ww * 16;
    uint32_t pstride = (uint32_t)h * (uint32_t)rs;
    if (data + pstride * 4u + (uint32_t)(h - 1) * rs + 2u > RT_MEM_SIZE) return;
    if (mask + (uint32_t)(h - 1) * rs + 2u > RT_MEM_SIZE) return;
    uint8_t *idx = scene_alloc_idx(&s_scene, (uint32_t)wpx * (uint32_t)h);
    if (!idx) return;
    for (int py = 0; py < h; py++) {
        uint8_t *irow = idx + (size_t)py * wpx;
        for (int c = 0; c < wpx; c++) {
            int wo = c >> 4, bit = 15 - (c & 15);
            uint32_t rowoff = (uint32_t)py * (uint32_t)rs + (uint32_t)wo * 2u;
            if (!((gmem_r16(M, mask + rowoff) >> bit) & 1u)) { irow[c] = SCENE_TRANSPARENT; continue; }  /* cookie-cut */
            int ci = 0;
            for (int p = 0; p < 5; p++)
                if ((gmem_r16(M, data + (uint32_t)p * pstride + rowoff) >> bit) & 1u)
                    ci |= (1 << p);
            irow[c] = ci ? (uint8_t)ci : SCENE_TRANSPARENT;  /* colour 0 also transparent here */
        }
    }
    scene_add_quad(&s_scene, wx0, pf_top + worldY, wpx, h, idx, wpx);
}

/* Caged "Marry Men" — resolved from the engine's own BUILD-ENTRY capture, NOT by walking
 * the placement records + replaying the anim table here. The override in
 * pc_overrides_gameplay.c hooks the compositor's build entries ($57B19E red / $57B856 blind,
 * both reached from the per-record handler $57C13A) and snapshots {worldX=d1, worldY=d2,
 * frame=d3, flags=d4, variant} for EVERY Marry Man each frame — in view AND culled off-view.
 * That is the key property: $57C13A reads the top anim table ($5d5a(a5)+cursor) into d3, then
 * dispatches by pose to a per-pose sub-handler ($526a/$545a(a5) → $57C194/…) which OVERWRITES
 * d3 with the pose's own sub-sequence frame before it reaches the build. Because we capture d3
 * AT THE BUILD ENTRY (post-sub-handler), the captured frame already carries the full per-pose
 * animation — there is nothing to replay here. Facing (d4 bit1) and variant (blind) are the
 * engine's own values too. So this resolver just re-blits the captured frame at the true world
 * position across the wide view, applying:
 *   - frame2 = frame + ($55 when d4 bit1 CLEAR)        (facing block; see below)
 *   - gfx entry = $4a72(a5)+frame2*8 → {data_off, mask_off, yoff, BLTSIZE}; cookie-cut, BMOD=-2
 *   - data = data_off + $EEFA (+ $4C38 if blind);  mask = mask_off + $12E7E (+ $4C38 if blind)
 *   - left = worldX-8,  top = clamp(worldY,$D7)+yoff
 *   BLIND/gray = RED gfx + a hardcoded $4C38 (RE-grounded, 2026-06-09): $57B856 is byte-identical
 *   to $57B4F6 except it adds $13B32/$17AB6 where red adds $EEFA/$12E7E — both deltas = $4C38.
 *   (The earlier "per-level +$4C38/+$6AB0" claim was FALSIFIED: that was a different frame-$3a
 *   special-case swapping to a fixed page #$585138. $4C38 is a real constant, not per-level.)
 * VERIFIED 2026-06-09 (harness L9, REPL `wsmm`): two Marry Men — red @worldX 322 and BLIND
 * @worldX 1474 (far off the ~320px narrow view) — both captured, both ANIMATING (frame cycled
 * 50→53), both flagged + faced from the engine's d3/d4. The off-view blind variant + facing +
 * per-pose animation all render correctly; the old "only idle animates / replay the top table"
 * note described the superseded record-walking resolver and is RETIRED. (Untested: a Marry Man
 * in a pose whose sub-handler never reaches the build — it would be absent both on-screen and
 * off, matching vanilla.) Remaining sprite gap is the separate GET READY setup queue, not this. */
#define WS_REC_BASE   0x5A4562u
#define WS_REC_STRIDE 64
#define WS_MM_HANDLER 0x57C13Au
#define WS_GFX_TABLE  0x4A72u          /* a5-relative: $583884 */
#define WS_ANIM_TABLE 0x5D5Au          /* a5-relative: $584B6C ($57C13A anim) */
#define WS_GFX_DATA_ADD 0xEEFAu
#define WS_GFX_MASK_ADD 0x12E7Eu
#define WS_GFX_BLIND_ADD 0x4C38u       /* blind/gray = red gfx + $4C38 (RE: $57B856 vs $57B4F6) */

/* Build-entry capture of the Marry Men (pc_overrides_gameplay.c $57B19E/$57B856). */
extern int native_wsbuild_count(void);
extern int native_wsbuild_get(int i, int *x, int *y, int *frame, int *flags, int *blind);

static void native_wsstatic_compose(int pf_top, int pf_bot, int cam16)
{
    const uint8_t *M = g_mem;
    s_wsstatic_drawn = 0; s_wsstatic_scanned = 0; s_wsstatic_cached = 0;
    if (!M || getenv("WS_NOOBJ")) return;
    (void)cam16;
    s_wsstatic_dbg_bp0 = (s_nanchors >= 1) ? s_anchors[0].ptr[0] : 0u;
    s_wsstatic_dbg_first = 0;
    uint32_t a5 = 0x57EE12u, gtab = a5 + WS_GFX_TABLE;

    /* Resolve each captured Marry Man EXACTLY as the engine does (RE'd in pc_overrides_
     * gameplay.c wsbuild_capture): the build indexes $4a72(a5) at (frame + facing), where
     * facing = +$55 frames when d4 bit1 is CLEAR, and adds a per-variant constant. So the
     * frame, facing AND variant are all the engine's own (captured at the build entry for
     * EVERY Marry Man — in view and culled), and we draw the resolved sprite at its true
     * world position across the wide view. No queue, no worldY pairing, no per-level offset. */
    int n = native_wsbuild_count();
    s_wsstatic_scanned = n;
    for (int i = 0; i < n; i++) {
        int worldX, worldY, frame, flags, blind;
        if (!native_wsbuild_get(i, &worldX, &worldY, &frame, &flags, &blind)) continue;
        int frame2 = frame + ((flags & 2) ? 0 : 0x55);     /* facing: +$55 when d4 bit1 clear */
        if (frame2 < 0) continue;
        uint32_t e = gtab + (uint32_t)frame2 * 8u;
        if (e + 8u > RT_MEM_SIZE) continue;
        uint16_t bsz = gmem_r16(M, e + 6u);
        int w = bsz & 0x3F, h = bsz >> 6;
        if (w <= 0 || h <= 0 || w > 64 || h > 256) continue;
        int rs = w * 2 - 2; if (rs <= 0) continue;          /* BMOD = -2 */
        int yoff = (int16_t)gmem_r16(M, e + 4u);
        uint32_t bdelta = blind ? WS_GFX_BLIND_ADD : 0u;    /* blind/gray = red + $4C38 */
        uint32_t data = (gmem_r16(M, e)      + WS_GFX_DATA_ADD + bdelta) & 0xFFFFFFu;
        uint32_t mask = (gmem_r16(M, e + 2u) + WS_GFX_MASK_ADD + bdelta) & 0xFFFFFFu;
        if (worldY > 0xD7) worldY = 0xD7;                   /* engine clamps d2 to $D7 */
        ws_draw_static(M, pf_top, pf_bot, worldX - 8, worldY + yoff, h, rs, data, mask);
        s_wsstatic_drawn++;
    }
    s_wsstatic_cached = n;
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
/* ROPES — the engine draws these with the blitter in LINE mode into the scrolling page
 * ($57DD42), which the wide renderer ignores, so they were missing. We draw them from the
 * UNCLIPPED segments captured at the engine's shared clip/emit entry $57DCD4 (native_wsrope_*
 * in gameplay.c) — captured BEFORE the engine clips/culls each segment to the vanilla window
 * [cam,cam+0x170], so ropes show across the full wide view (incl. creators off the vanilla
 * view). {x0,y0,x1,y1} are WORLD coords; worldY is playfield-relative (no vertical scroll).
 * Drawn into s_objlayer keyed by absolute worldX / screen Y = pf_top + worldY, so the main
 * composite loop maps + clips (in_view) them like every other gameplay sprite, in both the
 * wide and the 352 compare path. Colour = the rope brown (WS_ROPE_CI) via the per-scanline
 * palette. NOTE: the blitter draws a textured 2-tone rope (palette 17/19/20/22); this renders
 * a solid 1px line in the main brown (19) — visible + world-correct; the twist texture is a
 * later refinement. */
#define WS_ROPE_CI  19              /* $9C5521 — the rope's main brown (row-35 palette) */
static void native_wsrope_compose(int pf_top, int pf_bot)
{
    (void)pf_bot;
    if (getenv("WS_NOROPE")) return;
    extern int  native_wsrope_count(void);
    extern void native_wsrope_get(int i, int *x0, int *y0, int *x1, int *y1);
    int nseg = native_wsrope_count();
    for (int i = 0; i < nseg; i++) {
        int x0, y0, x1, y1;
        native_wsrope_get(i, &x0, &y0, &x1, &y1);
        /* Emit the segment as one world-space quad: a transparent bounding box
         * with the Bresenham line texels set to the rope colour index. The
         * compositor clips to the playfield rows / layer width exactly as the
         * old direct s_objlayer writes did. */
        int bx = x0 < x1 ? x0 : x1, by = y0 < y1 ? y0 : y1;
        int bw = (x1 > x0 ? x1 - x0 : x0 - x1) + 1;
        int bh = (y1 > y0 ? y1 - y0 : y0 - y1) + 1;
        uint8_t *idx = scene_alloc_idx(&s_scene, (uint32_t)bw * (uint32_t)bh);
        if (!idx) continue;
        memset(idx, SCENE_TRANSPARENT, (size_t)bw * (size_t)bh);
        int dx =  (bw - 1), sx = x0 < x1 ? 1 : -1;
        int dy = -(bh - 1), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy, x = x0, y = y0;
        for (;;) {
            idx[(size_t)(y - by) * bw + (x - bx)] = WS_ROPE_CI;
            if (x == x1 && y == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
        scene_add_quad(&s_scene, bx, pf_top + by, bw, bh, idx, bw);
    }
}

static void native_wsbanner_compose(int ow, int cam, int pf_top)
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
        uint8_t *idx = scene_alloc_idx(&s_scene, (uint32_t)wpx * (uint32_t)rows);
        if (idx) {
            for (int r = 0; r < rows; r++) {
                uint8_t *irow = idx + (size_t)r * wpx;
                for (int c = 0; c < wpx; c++) {
                    int wo = c >> 4, bit = 15 - (c & 15);
                    uint32_t roff = (uint32_t)r * (uint32_t)rs + (uint32_t)wo * 2u;
                    if (!((gmem_r16(M, mask + roff) >> bit) & 1u)) { irow[c] = SCENE_TRANSPARENT; continue; }
                    int ci = 0;
                    for (int p = 0; p < 5; p++)
                        if ((gmem_r16(M, data + (uint32_t)p * (uint32_t)pstride + roff) >> bit) & 1u)
                            ci |= (1 << p);
                    irow[c] = (uint8_t)ci;
                }
            }
            scene_add_quad_screen(&s_scene, bx0, boy, wpx, rows, idx, wpx);
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
            uint8_t *idx = scene_alloc_idx(&s_scene, (uint32_t)twpx * (uint32_t)th);
            if (idx) {
                for (int r = 0; r < th; r++) {
                    uint8_t *irow = idx + (size_t)r * twpx;
                    for (int c = 0; c < twpx; c++) {
                        int wo = c >> 4, bit = 15 - (c & 15);
                        uint32_t roff = (uint32_t)r * trs + (uint32_t)wo * 2u;
                        int ci = 0;
                        for (int p = 0; p < 4; p++)     /* anim is 4 planes (moveq #3,d6) */
                            if ((gmem_r16(M, tsrc + (uint32_t)p * WS_TEL_PSTRIDE + roff) >> bit) & 1u)
                                ci |= (1 << p);
                        /* The engine blits only planes 0-3; the circle background has plane 4
                         * SET, so the displayed colour is ci|16 (verified: page index = src+16). */
                        irow[c] = (uint8_t)((ci | 16) & 0x1F);   /* opaque */
                    }
                }
                scene_add_quad_screen(&s_scene, txx, tsy0, twpx, th, idx, twpx);
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
        /* One screen quad for the whole string: 8px per char, 16 rows, colour
         * index 16 where a glyph bit is set, transparent elsewhere. */
        uint8_t *idx = scene_alloc_idx(&s_scene, 40u * 8u * 16u);
        int nk = 0;
        if (idx) {
            memset(idx, SCENE_TRANSPARENT, 40u * 8u * 16u);
            for (int k = 0; k < 40; k++) {
                uint32_t ca = str + (uint32_t)k;
                if (ca >= RT_MEM_SIZE) break;
                uint8_t ch = M[ca];
                if (ch == 0) break;
                nk = k + 1;
                if (ch < 0x20) continue;
                uint32_t glyph = WS_FONT_BASE + (uint32_t)(ch - 0x20);
                for (int r = 0; r < 16; r++) {
                    uint32_t ga = glyph + (uint32_t)r * WS_FONT_RS;
                    if (ga >= RT_MEM_SIZE) continue;
                    uint8_t gb = M[ga];
                    if (!gb) continue;
                    uint8_t *irow = idx + (size_t)r * (40 * 8) + (size_t)k * 8;
                    for (int px = 0; px < 8; px++)
                        if ((gb >> (7 - px)) & 1u) irow[px] = 16;
                }
            }
            if (nk > 0)
                scene_add_quad_screen(&s_scene, txx, tsy0, nk * 8, 16, idx, 40 * 8);
        }
      } }
}

void native_render_wide_bg(uint32_t *out, int ow, int margin)
{
    if (margin < 0 || s_cur_cop1lc != WS_GAMEPLAY_COP1LC) return;   /* gameplay only (margin 0 = compare-at-352) */
    const uint8_t *M = g_mem;
    EngineView ev;
    if (!M || !engine_view_capture(&ev)) return;   /* firewall: engine state only, no fudge */

    int cam       = ev.camera;        /* signed screen-left world X */
    int rowstride = ev.row_stride;    /* per-level tilemap row stride, sourced (no L9 fallback) */

    /* World X of the displayed left coarse column, DERIVED FROM THE CAMERA — not
     * reverse-projected from the displayed copper BPL pointer. The engine sets that
     * pointer FROM this same camera ($57FDBA); reading it back out of the copper was
     * the old bandaid (it gave the renderer a second source of truth to curve-fit
     * against). cam16 is used only by the margin==0 compare path; the real widescreen
     * path keys off view_left (ws_view_left), also camera-derived. */
    int cam16  = cam & ~15;
    int mincol = (ev.level_lo < 0 ? 0 : ev.level_lo) >> 4;
    int maxcol = (ev.level_hi + 320) >> 4;               /* last valid level column */

    /* Wide camera (the margin>0 = real-widescreen path). The view must stay CLAMPED to
     * the level's world bounds (never reveal void past an edge) and, when the level is
     * NARROWER than the view, the level is CENTERED (equal black margins) — both per the
     * user. worldX maps 1:1 to output x (worldX = view_left + x); the tilemap, objects and
     * player are all keyed by absolute worldX so they track automatically, and smooth
     * scroll comes from the signed camera's fine bits (no page coarse/fine hysteresis since
     * we read the tilemap directly). The margin==0 COMPARE path keeps the exact
     * engine-aligned mapping in the loop below so wsdiff stays a valid pixel check. */
    int view_left = ws_view_left(ow);                     /* shared with the object-cull overrides */

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
    /* New frame on the draw list + load this frame's per-scanline palette LUT
     * (Phase 1 seam — see instructions/gpu-renderer-plan.md). Every sprite pass
     * APPENDS index quads to s_scene; the single composite below rasterizes them
     * in insertion order (obj < player < char < static = the old Z-order) into the
     * object layer. */
    scene_reset(&s_scene);
    scene_load_palrows();
    memset(s_objlayer, 0, sizeof s_objlayer);
    native_wstiles_compose(pf_top, pf_bot, rowstride, mincol, maxcol);   /* terrain background */
    native_wswater_compose(pf_top, pf_bot);         /* animated page patches (water surface) */
    native_wsobj_compose(pf_top, pf_bot);
    native_wsplayer_compose(pf_top, pf_bot);          /* player UNDER characters... */
    native_wschar_compose(pf_top, pf_bot);            /* ...so enemies render OVER the player (vanilla Z-order) */
    native_wsstatic_compose(pf_top, pf_bot, cam16);   /* caged Marry Men + static-placement objects */
    native_wsrope_compose(pf_top, pf_bot);          /* chandelier ropes (blitter LINE mode) — on top */
    native_wsbanner_compose(ow, cam, pf_top);       /* GET READY / GAME OVER — screen-fixed UI quads */
    s_scene_ylo = pf_top; s_scene_yhi = pf_bot;     /* publish the scene's row span */
    /* Publish the camera view so a windowed per-sprite consumer (P4) can project
     * world quads itself: screen_x = x - view_left, world clip [mincol,maxcol)*16. */
    s_scene.view_left = view_left;
    s_scene.wclip_x0  = mincol * 16;
    s_scene.wclip_x1  = maxcol * 16 < WS_LAYER_W ? maxcol * 16 : WS_LAYER_W;
    scene_composite_argb(&s_scene, &s_objlayer[0][0], WS_LAYER_W, HW_DISPLAY_H, pf_top, pf_bot);

    /* The whole playfield (terrain + sprites) is now in the draw list, rasterized into
     * s_objlayer in absolute world X. This loop is just the camera projection: map each
     * output pixel to its world X and copy the composited pixel, clipped to the
     * camera-reachable columns [mincol,maxcol) (outside = the void the vanilla camera
     * never scrolls to → BLACK in the wide view; the margin==0 COMPARE path keeps the
     * s_fb baseline it's diffed against). No per-pixel tile decode here anymore. */
    for (int y = pf_top; y < pf_bot; y++) {
        const ScanState *st = &s_scan[y];
        /* must be the 5-plane single-playfield scanline the scroll buffer drives */
        int bpu = (st->bplcon0 >> 12) & 7;
        if (bpu < 5 || ((st->bplcon0 >> 10) & 1)) continue;   /* skip HUD/dpf lines */
        uint32_t *row = out + (size_t)y * ow;

        int x_off   = DDF_TO_X(st->ddfstrt);
        int scroll1 = st->bplcon1 & 0xF;

        for (int x = 0; x < ow; x++) {
            int worldX = (margin > 0)
                ? view_left + x                               /* wide: 1:1, centered/clamped */
                : cam16 + ((x - margin) - x_off - scroll1);   /* compare: engine-aligned */
            int drawn = 0;
            if (worldX >= 0 && worldX < WS_LAYER_W) {
                int col = worldX >> 4;
                if (col >= mincol && col < maxcol) {          /* camera-reachable column */
                    uint32_t o = s_objlayer[y][worldX];
                    if (o & 0xFF000000u) { row[x] = o; drawn = 1; }
                }
            }
            if (!drawn && margin > 0) row[x] = 0xFF000000u;   /* void = black (compare keeps s_fb) */
        }
    }

    /* Banner (GET READY / GAME OVER) — screen-fixed UI quads, on top of everything. */
    scene_composite_screen_argb(&s_scene, out, ow, HW_DISPLAY_H);

    s_scene_ready = 1;     /* this frame's draw list is complete (windowed present may consume it) */
}

/* Re-draw the engine's captured object sprite blits natively into the wide buffer.
 * Each object is 5 plane-blits (dest steps by the plane stride $2A0C); position is
 * recovered from the dest's offset within its double-buffer page vs the scroll
 * coarse offset, mapped into the same screen coordinate the tile bg uses. Opaque
 * blits (con0 $09F0, D=A) copy all pixels; cookie-cut blits (minterm $CA) draw only
 * where the shared mask plane is set (transparency). */
/* WS_PLANE_STRIDE / WS_ROWSTRIDE defined above (near native_wsstatic_compose). */


