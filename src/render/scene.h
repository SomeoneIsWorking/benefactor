/* scene.h — the per-frame gameplay DRAW LIST (the renderer/backend seam).
 *
 * Instead of compositing every sprite into one ARGB surface in software and
 * handing the backend a finished image (the old path — see present_vulkan.c's
 * blit), the gameplay renderer EMITS a list of quads here, and a backend
 * (CPU rasterizer, Vulkan, SDL) draws each quad itself. This is what makes
 * "render each sprite via SDL or Vulkan" possible and gives a per-character
 * lighting pass real per-sprite geometry to attach to.
 *
 * A quad's pixels are a colour-INDEX bitmap (`idx`, one byte per texel):
 *   0..31  = palette colour index for that texel
 *   0xFF   = SCENE_TRANSPARENT (cookie-cut hole — not drawn)
 * The consumer does the palette lookup, exactly as a GPU sampler + discard
 * would. Colours come from `pal_rows` (this engine's palette is per-scanline /
 * copper-driven; the LUT is per output row so colour effects stay exact).
 *
 * See instructions/gpu-renderer-plan.md. */
#ifndef RENDER_SCENE_H
#define RENDER_SCENE_H

#include <stdint.h>

#define SCENE_TRANSPARENT  0xFFu   /* idx value meaning "don't draw this texel" */
#define SCENE_MAX_ROWS     512     /* >= HW_DISPLAY_H */
#define SCENE_PAL          32      /* colours per scanline (5-plane + EHB top half) */
#define SCENE_MAX_QUADS    8192
#define SCENE_ARENA_BYTES  (8u * 1024u * 1024u)   /* index-bitmap arena (per frame) */

/* One sprite/tile to draw. (x,y) is the dest top-left in the TARGET surface's
 * coordinate system; the emitter chooses that system (world X for the object
 * layer, screen X for the final output) and the consumer honours it. `idx`
 * points into the scene arena; row r, col c texel = idx[r*stride + c]. */
typedef struct {
    int            x, y, w, h;
    const uint8_t *idx;
    int            stride;
} SceneQuad;

typedef struct {
    SceneQuad  quads[SCENE_MAX_QUADS];
    int        nquads;
    uint8_t   *arena;          /* SCENE_ARENA_BYTES; index bitmaps live here */
    uint32_t   arena_used;
    uint32_t   pal_rows[SCENE_MAX_ROWS][SCENE_PAL];   /* ARGB8888 per scanline */
} Scene;

/* Begin a new frame: drop all quads + free the arena. Lazily allocates the
 * arena on first use. */
void scene_reset(Scene *s);

/* Copy `nrows` palette rows (each SCENE_PAL ARGB entries) into the scene's
 * per-row LUT. `src` is a flat array of nrows*SCENE_PAL u32. */
void scene_set_pal_rows(Scene *s, const uint32_t *src, int nrows);

/* Reserve `nbytes` of contiguous index storage from the arena. Returns NULL if
 * the arena is exhausted (the quad should then be skipped). */
uint8_t *scene_alloc_idx(Scene *s, uint32_t nbytes);

/* Append a quad (no-op if the quad list is full). `idx` must point into the
 * arena (from scene_alloc_idx). */
void scene_add_quad(Scene *s, int x, int y, int w, int h,
                    const uint8_t *idx, int stride);

/* CPU consumer: overlay every quad onto `dst` (dst_w x dst_h ARGB8888), drawing
 * only rows in [y_lo, y_hi) and only non-transparent texels, resolving colours
 * through pal_rows[screen_y]. Drawn texels are written as 0xFF000000|RGB (opaque
 * alpha) so a downstream pass can test the alpha to detect "a sprite is here".
 * Quad (x,y) are interpreted in `dst`'s coordinate system. Painter's order. */
void scene_composite_argb(const Scene *s, uint32_t *dst, int dst_w, int dst_h,
                          int y_lo, int y_hi);

#endif /* RENDER_SCENE_H */
