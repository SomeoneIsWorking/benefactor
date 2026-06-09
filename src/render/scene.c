/* scene.c — draw-list storage + the CPU rasterizer (the reference consumer).
 *
 * The CPU rasterizer reproduces the old software compositing exactly, which is
 * how Phase 1 proves the draw list is lossless: same pixels as before, just
 * routed through the seam. The Vulkan/SDL consumers draw the SAME list.
 * See instructions/gpu-renderer-plan.md. */
#include "render/scene.h"
#include <stdlib.h>
#include <string.h>

void scene_reset(Scene *s)
{
    s->nquads = 0;
    s->arena_used = 0;
    if (!s->arena) s->arena = malloc(SCENE_ARENA_BYTES);
}

void scene_set_pal_rows(Scene *s, const uint32_t *src, int nrows)
{
    if (nrows > SCENE_MAX_ROWS) nrows = SCENE_MAX_ROWS;
    memcpy(s->pal_rows, src, (size_t)nrows * SCENE_PAL * sizeof(uint32_t));
}

uint8_t *scene_alloc_idx(Scene *s, uint32_t nbytes)
{
    if (!s->arena) return NULL;
    /* keep allocations word-aligned (R8 texture rows upload fine either way, but
     * alignment avoids surprises if a consumer reinterprets the arena) */
    nbytes = (nbytes + 3u) & ~3u;
    if (s->arena_used + nbytes > SCENE_ARENA_BYTES) return NULL;
    uint8_t *p = s->arena + s->arena_used;
    s->arena_used += nbytes;
    return p;
}

static void scene_add_quad_space(Scene *s, int x, int y, int w, int h,
                                 const uint8_t *idx, int stride, uint8_t space)
{
    if (s->nquads >= SCENE_MAX_QUADS || !idx) return;
    SceneQuad *q = &s->quads[s->nquads++];
    q->x = x; q->y = y; q->w = w; q->h = h;
    q->idx = idx; q->stride = stride; q->space = space;
}

void scene_add_quad(Scene *s, int x, int y, int w, int h,
                    const uint8_t *idx, int stride)
{ scene_add_quad_space(s, x, y, w, h, idx, stride, SCENE_SPACE_WORLD); }

void scene_add_quad_screen(Scene *s, int x, int y, int w, int h,
                           const uint8_t *idx, int stride)
{ scene_add_quad_space(s, x, y, w, h, idx, stride, SCENE_SPACE_SCREEN); }

void scene_composite_argb(const Scene *s, uint32_t *dst, int dst_w, int dst_h,
                          int y_lo, int y_hi)
{
    if (y_lo < 0) y_lo = 0;
    if (y_hi > dst_h) y_hi = dst_h;
    for (int i = 0; i < s->nquads; i++) {
        const SceneQuad *q = &s->quads[i];
        if (q->space != SCENE_SPACE_WORLD) continue;
        for (int r = 0; r < q->h; r++) {
            int dy = q->y + r;
            if (dy < y_lo || dy >= y_hi) continue;
            const uint8_t *row = q->idx + (size_t)r * q->stride;
            const uint32_t *pal = s->pal_rows[dy];
            uint32_t *out = dst + (size_t)dy * dst_w;
            for (int c = 0; c < q->w; c++) {
                uint8_t v = row[c];
                if (v == SCENE_TRANSPARENT) continue;
                int dx = q->x + c;
                if (dx < 0 || dx >= dst_w) continue;
                out[dx] = 0xFF000000u | (pal[v & (SCENE_PAL - 1)] & 0x00FFFFFFu);
            }
        }
    }
}

void scene_composite_screen_argb(const Scene *s, uint32_t *dst,
                                 int dst_w, int dst_h)
{
    for (int i = 0; i < s->nquads; i++) {
        const SceneQuad *q = &s->quads[i];
        if (q->space != SCENE_SPACE_SCREEN) continue;
        for (int r = 0; r < q->h; r++) {
            int dy = q->y + r;
            if (dy < 0 || dy >= dst_h) continue;
            const uint8_t *row = q->idx + (size_t)r * q->stride;
            const uint32_t *pal = s->pal_rows[dy];
            uint32_t *out = dst + (size_t)dy * dst_w;
            for (int c = 0; c < q->w; c++) {
                uint8_t v = row[c];
                if (v == SCENE_TRANSPARENT) continue;
                int dx = q->x + c;
                if (dx < 0 || dx >= dst_w) continue;
                out[dx] = 0xFF000000u | (pal[v & (SCENE_PAL - 1)] & 0x00FFFFFFu);
            }
        }
    }
}
