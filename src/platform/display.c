#include "display.h"
#include "../amiga/custom.h"
#include "../amiga/memory.h"

#include <string.h>
#include <stdio.h>

/* ── SDL2 objects ────────────────────────────────────────────────────────── */

static SDL_Window   *window   = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture  *texture  = NULL;

/* Pixel buffer: DISPLAY_W × DISPLAY_H × RGBA8888 */
static uint32_t pixels[DISPLAY_W * DISPLAY_H];

/* ── Init / fini ─────────────────────────────────────────────────────────── */

int display_init(const char *title)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init(VIDEO): %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DISPLAY_W * 2, DISPLAY_H * 2,   /* 2× scale */
        SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        /* Fall back to software renderer */
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderSetLogicalSize(renderer, DISPLAY_W, DISPLAY_H);

    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        DISPLAY_W, DISPLAY_H);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

void display_fini(void)
{
    if (texture)  { SDL_DestroyTexture(texture);   texture  = NULL; }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = NULL; }
    if (window)   { SDL_DestroyWindow(window);     window   = NULL; }
}

void display_toggle_fullscreen(void)
{
    if (!window) return;
    uint32_t flags = SDL_GetWindowFlags(window);
    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
        SDL_SetWindowFullscreen(window, 0);
    else
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
}

/* ── Amiga OCS → RGBA conversion ─────────────────────────────────────────── */

/*
 * Convert Amiga 12-bit colour (0RGB, 4 bits each) to 32-bit ARGB8888.
 * The standard Amiga expansion is: repeat the 4-bit nibble → 8 bits.
 * e.g. $F → 0xFF,  $7 → 0x77.
 */
static inline uint32_t amiga_color_to_rgba(uint16_t c)
{
    uint8_t r = (uint8_t)(((c >> 8) & 0xF) * 0x11);
    uint8_t g = (uint8_t)(((c >> 4) & 0xF) * 0x11);
    uint8_t b = (uint8_t)(((c     ) & 0xF) * 0x11);
    return (uint32_t)(0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
}

/*
 * Render one scan line from bitplane data into the pixel buffer.
 *
 * bpl_ptrs[0..n_planes-1] point to the start of each bitplane for this line.
 * `palette`  is the 32-entry (or 64-entry EHB) ARGB palette.
 * `y`        is the current output row.
 * `n_planes` is 1..5 (OCS) or 6 (EHB).
 * `ddfstrt`  and `ddfstop` define the fetch window (in low-res pixels).
 */
static void render_line(int y, uint32_t bpl_ptrs[6], const uint32_t *palette,
                         int n_planes, uint16_t /*ddfstrt*/, uint16_t /*ddfstop*/,
                         int scroll_x, int mod_odd, int mod_even)
{
    (void)mod_odd; (void)mod_even;

    uint32_t *out = pixels + y * DISPLAY_W;

    /* We always render DISPLAY_W (320) pixels starting at column 0.
     * Horizontal scroll (BPLCON1 low nibble) is applied in pixel units. */
    int sx = scroll_x & 0xF;

    for (int x = 0; x < DISPLAY_W; x++) {
        int src_x = x + sx;
        int byte_off = src_x >> 3;
        int bit      = 7 - (src_x & 7);

        uint8_t colour_idx = 0;
        for (int p = 0; p < n_planes; p++) {
            uint8_t bp_byte = mem_read8(bpl_ptrs[p] + (uint32_t)byte_off);
            if ((bp_byte >> bit) & 1)
                colour_idx |= (uint8_t)(1 << p);
        }
        out[x] = palette[colour_idx];
    }
}

/* ── Frame renderer ──────────────────────────────────────────────────────── */

void display_render_frame(void)
{
    /* Build palette from custom chip colour registers */
    uint16_t bplcon0  = custom_bplcon0();
    int      n_planes = (bplcon0 >> 12) & 7;
    if (n_planes == 0) n_planes = 1;
    if (n_planes > 6)  n_planes = 6;
    int      ehb      = (bplcon0 & 0x0080) != 0;  /* Extra Half-Brite */

    uint32_t palette[64];
    for (int i = 0; i < 32; i++)
        palette[i] = amiga_color_to_rgba(custom_color(i));
    if (ehb) {
        for (int i = 0; i < 32; i++) {
            /* EHB colours are half-brightness of the base 32 */
            uint32_t c = palette[i];
            palette[32 + i] = (c & 0xFF000000)
                | (((c >> 17) & 0x7F) << 16)
                | (((c >>  9) & 0x7F) <<  8)
                | (((c >>  1) & 0x7F));
        }
    }

    /* Bitplane pointers – advanced per line */
    uint32_t bpl[6];
    for (int p = 0; p < 6; p++)
        bpl[p] = custom_bplptr(p);

    uint16_t diwstrt  = custom_diwstrt();
    uint16_t diwstop  = custom_diwstop();
    uint16_t ddfstrt  = custom_ddfstrt();
    uint16_t ddfstop  = custom_ddfstop();
    uint16_t bpl1mod  = custom_bpl1mod();
    uint16_t bpl2mod  = custom_bpl2mod();
    uint16_t bplcon1  = custom_bplcon1();
    int      scroll_x = bplcon1 & 0xF;

    /* DIW defines active display window */
    int diy_start = (diwstrt >> 8) & 0xFF;
    int diy_stop  = (diwstop >> 8) & 0xFF;
    if (diy_stop <= diy_start) diy_stop = diy_start + DISPLAY_H;

    /* Clamp to our output buffer */
    int out_y_start = 0;
    int out_y_end   = DISPLAY_H;

    memset(pixels, 0, sizeof(pixels));

    for (int y = out_y_start; y < out_y_end; y++) {
        int amiga_y = y + diy_start;
        if (amiga_y < diy_start || amiga_y >= diy_stop) {
            /* Outside display window – output background colour */
            uint32_t *row = pixels + y * DISPLAY_W;
            uint32_t  bg  = palette[0];
            for (int x = 0; x < DISPLAY_W; x++) row[x] = bg;
        } else {
            render_line(y, bpl, palette, n_planes,
                        ddfstrt, ddfstop, scroll_x,
                        (int16_t)bpl1mod, (int16_t)bpl2mod);
        }

        /* Advance bitplane pointers by bytes-per-line + modulo */
        int bytes_per_line = (DISPLAY_W / 8);
        for (int p = 0; p < n_planes; p++) {
            bpl[p] += (uint32_t)bytes_per_line;
            if (p & 1)
                bpl[p] = (uint32_t)((int32_t)bpl[p] + (int16_t)bpl2mod);
            else
                bpl[p] = (uint32_t)((int32_t)bpl[p] + (int16_t)bpl1mod);
        }
    }
}

void display_present(void)
{
    if (!texture || !renderer) return;

    SDL_UpdateTexture(texture, NULL, pixels, DISPLAY_W * (int)sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* ── Per-scanline renderer ────────────────────────────────────────────────── */
/*
 * Called once per Amiga scanline (0..311), immediately after the copper has
 * run for that line.  The current custom-chip register state (bitplane
 * pointers, palette, BPLCON0) is snapshotted here, so copper mid-frame
 * changes are applied correctly.
 *
 * Bitplane pointers in the hardware are auto-incremented *by the DMA engine*
 * after each word is fetched, i.e. they advance by bytes_per_line after each
 * line.  We mirror that here by storing the *start-of-line* pointer and
 * advancing it ourselves.
 */

/* Track the bitplane pointer offset accumulated so far in this frame.
 * Snapshot happens just before the first visible scanline (at diy_start-1). */
static uint32_t bpl_live[6];   /* current auto-advancing pointers */
static int      bpl_snapped = 0;  /* 1 = bpl_live was snapped this frame */

void display_render_scanline(int amiga_line)
{
    /* Determine visible area start from DIWSTRT */
    uint16_t diwstrt = custom_diwstrt();
    int diy_start = (diwstrt >> 8) & 0xFF;
    if (diy_start < 1) diy_start = 44;  /* sensible PAL default */

    /* Reset snap flag at start of new frame */
    if (amiga_line == 0)
        bpl_snapped = 0;

    /* Snapshot bitplane pointers exactly at the scanline where display begins.
     * By this point the copper has had lines 0..diy_start-1 to write BPLxPTH/L. */
    if (amiga_line == diy_start && !bpl_snapped) {
        for (int p = 0; p < 6; p++)
            bpl_live[p] = custom_bplptr(p) & 0x1FFFFF;
        bpl_snapped = 1;
        /* Debug: log a few snaps so we can verify pointers are valid */
        static int dbg_snap_count = 0;
        static int dbg_snap_frame = 0;
        dbg_snap_frame++;
        if (dbg_snap_frame > 950 && dbg_snap_count < 5) {
            fprintf(stderr, "[BplSnap frame~%d] line=%d bpl0=%06x bpl1=%06x n_planes=%d diwstrt=%04x\n",
                    dbg_snap_frame,
                    amiga_line,
                    bpl_live[0], bpl_live[1],
                    ((custom_bplcon0() >> 12) & 7),
                    custom_diwstrt());
            dbg_snap_count++;
        }
    }

    if (!bpl_snapped)
        return;  /* don't render before we've snapped */

    uint16_t bplcon0 = custom_bplcon0();
    int n_planes = (bplcon0 >> 12) & 7;
    if (n_planes > 6) n_planes = 6;
    int ehb = (bplcon0 & 0x0080) != 0;

    /* Build palette from current custom chip colour registers */
    uint32_t palette[64];
    for (int i = 0; i < 32; i++)
        palette[i] = amiga_color_to_rgba(custom_color(i));
    if (ehb) {
        for (int i = 0; i < 32; i++) {
            uint32_t c = palette[i];
            palette[32 + i] = (c & 0xFF000000)
                | (((c >> 17) & 0x7F) << 16)
                | (((c >>  9) & 0x7F) <<  8)
                | (((c >>  1) & 0x7F));
        }
    }

    int out_y = amiga_line - diy_start;

    if (out_y >= 0 && out_y < DISPLAY_H && n_planes > 0) {
        uint16_t bplcon1 = custom_bplcon1();
        int scroll_x = bplcon1 & 0xF;
        render_line(out_y, bpl_live, palette, n_planes, 0, 0, scroll_x, 0, 0);
    }

    /* Advance bitplane pointers by one line (40 bytes for 320 pixels) */
    if (out_y >= 0) {
        int bytes_per_line = DISPLAY_W / 8;   /* 40 bytes */
        uint16_t bpl1mod = custom_bpl1mod();
        uint16_t bpl2mod = custom_bpl2mod();
        for (int p = 0; p < n_planes; p++) {
            bpl_live[p] = (bpl_live[p] + (uint32_t)bytes_per_line) & 0x1FFFFF;
            if (p & 1)
                bpl_live[p] = (uint32_t)((int32_t)bpl_live[p] + (int16_t)bpl2mod) & 0x1FFFFF;
            else
                bpl_live[p] = (uint32_t)((int32_t)bpl_live[p] + (int16_t)bpl1mod) & 0x1FFFFF;
        }
    }
}
