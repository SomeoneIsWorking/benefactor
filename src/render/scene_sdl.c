/* scene_sdl.c — per-sprite SDL consumer of the BenRen draw list. See scene_sdl.h. */
#include "render/scene_sdl.h"
#include <stdlib.h>
#include <string.h>

/* Bake one quad's index bitmap into an ARGB streaming texture, resolving the
 * per-output-row palette on the CPU (SDL2 has no fragment shader). `dy0` is the
 * output row quad row 0 lands on (used to pick each row's palette). */
static SDL_Texture *quad_texture(SDL_Renderer *r, const Scene *s, const SceneQuad *q)
{
    SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, q->w, q->h);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    void *pixels; int pitch;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) {
        SDL_DestroyTexture(tex); return NULL;
    }
    for (int rr = 0; rr < q->h; rr++) {
        int dy = q->y + rr;                 /* output row this texel lands on */
        const uint32_t *pal = (dy >= 0 && dy < SCENE_MAX_ROWS) ? s->pal_rows[dy] : s->pal_rows[0];
        const uint8_t *src = q->idx + (size_t)rr * q->stride;
        uint32_t *dst = (uint32_t *)((uint8_t *)pixels + (size_t)rr * pitch);
        for (int c = 0; c < q->w; c++) {
            uint8_t v = src[c];
            dst[c] = (v == SCENE_TRANSPARENT)
                   ? 0u                                          /* alpha 0 -> not drawn */
                   : (0xFF000000u | (pal[v & (SCENE_PAL - 1)] & 0x00FFFFFFu));
        }
    }
    SDL_UnlockTexture(tex);
    return tex;
}

int scene_draw_sdl(SDL_Renderer *r, const Scene *s, int y_lo, int y_hi)
{
    if (!r || !s) return -1;

    /* Vertical clip to [y_lo, y_hi): SDL clips the dest, and because each quad's
     * texture bakes the per-output-row palette, clipped-away rows never matter. */
    SDL_Rect clip = { 0, y_lo, 1 << 15, y_hi - y_lo };
    SDL_RenderSetClipRect(r, &clip);

    int rc = 0;
    for (int i = 0; i < s->nquads && rc == 0; i++) {
        const SceneQuad *q = &s->quads[i];
        if (q->space != SCENE_SPACE_WORLD) continue;   /* screen-space UI: windowed present only */
        if (q->w <= 0 || q->h <= 0) continue;

        SDL_Texture *tex = quad_texture(r, s, q);
        if (!tex) { rc = -1; break; }
        SDL_Rect d = { q->x, q->y, q->w, q->h };
        if (SDL_RenderCopy(r, tex, NULL, &d) != 0) rc = -1;
        SDL_DestroyTexture(tex);
    }

    SDL_RenderSetClipRect(r, NULL);
    return rc;
}

#define SCENE_SDL_ATLAS_W 1024
#define SCENE_SDL_ATLAS_H 2048

void scene_sdl_cache_free(SceneSdlCache *c)
{
    if (!c) return;
    if (c->atlas) SDL_DestroyTexture(c->atlas);
    if (c->base)  SDL_DestroyTexture(c->base);
    c->atlas = NULL; c->base = NULL; c->r = NULL; c->base_w = c->base_h = 0;
}

/* Ensure the cache's textures exist for renderer `r` and base size ow x oh.
 * If the renderer changed, the old textures died with it — just drop them. */
static int cache_ensure(SceneSdlCache *c, SDL_Renderer *r, int ow, int oh)
{
    if (c->r != r) { c->atlas = NULL; c->base = NULL; c->r = r; c->base_w = c->base_h = 0; }
    if (!c->atlas) {
        c->atlas = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     SCENE_SDL_ATLAS_W, SCENE_SDL_ATLAS_H);
        if (!c->atlas) return -1;
        SDL_SetTextureBlendMode(c->atlas, SDL_BLENDMODE_BLEND);
    }
    if (c->base && (c->base_w != ow || c->base_h != oh)) {
        SDL_DestroyTexture(c->base); c->base = NULL;
    }
    if (!c->base) {
        c->base = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, ow, oh);
        if (!c->base) return -1;
        SDL_SetTextureBlendMode(c->base, SDL_BLENDMODE_NONE);
        c->base_w = ow; c->base_h = oh;
    }
    return 0;
}

/* Shelf-pack every drawable quad of `s` into the atlas and bake its texels
 * (same per-output-row palette resolve as quad_texture) under ONE lock.
 * Packed positions go to pos[] ({x,y} per quad; w/h come from the quad).
 * Returns 0, or -1 when the quads don't fit / SDL fails (caller falls back). */
static int atlas_pack_bake(SceneSdlCache *c, const Scene *s, SDL_Rect *pos)
{
    int cur_x = 0, cur_y = 0, shelf_h = 0, max_y = 0;
    for (int i = 0; i < s->nquads; i++) {
        const SceneQuad *q = &s->quads[i];
        pos[i].w = 0;                                    /* mark not-packed */
        if (q->w <= 0 || q->h <= 0) continue;
        if (q->w > SCENE_SDL_ATLAS_W) return -1;
        if (cur_x + q->w > SCENE_SDL_ATLAS_W) {          /* new shelf */
            cur_x = 0; cur_y += shelf_h; shelf_h = 0;
        }
        if (cur_y + q->h > SCENE_SDL_ATLAS_H) return -1; /* atlas full */
        pos[i] = (SDL_Rect){ cur_x, cur_y, q->w, q->h };
        cur_x += q->w;
        if (q->h > shelf_h) shelf_h = q->h;
        if (cur_y + q->h > max_y) max_y = cur_y + q->h;
    }
    if (max_y == 0) return 0;                            /* nothing to bake */

    SDL_Rect lock = { 0, 0, SCENE_SDL_ATLAS_W, max_y };
    void *pixels; int pitch;
    if (SDL_LockTexture(c->atlas, &lock, &pixels, &pitch) != 0) return -1;
    for (int i = 0; i < s->nquads; i++) {
        const SceneQuad *q = &s->quads[i];
        if (pos[i].w == 0) continue;
        for (int rr = 0; rr < q->h; rr++) {
            int dy = q->y + rr;             /* output row this texel lands on */
            const uint32_t *pal = (dy >= 0 && dy < SCENE_MAX_ROWS) ? s->pal_rows[dy]
                                                                   : s->pal_rows[0];
            const uint8_t *src = q->idx + (size_t)rr * q->stride;
            uint32_t *dst = (uint32_t *)((uint8_t *)pixels
                          + (size_t)(pos[i].y + rr) * pitch) + pos[i].x;
            for (int col = 0; col < q->w; col++) {
                uint8_t v = src[col];
                dst[col] = (v == SCENE_TRANSPARENT)
                         ? 0u
                         : (0xFF000000u | (pal[v & (SCENE_PAL - 1)] & 0x00FFFFFFu));
            }
        }
    }
    SDL_UnlockTexture(c->atlas);
    return 0;
}

int scene_draw_sdl_window(SDL_Renderer *r, const Scene *s, int y_lo, int y_hi,
                          const uint32_t *base, int ow, int oh,
                          SceneSdlCache *cache)
{
    if (!r || !s || !base || !cache) return -1;
    if (cache_ensure(cache, r, ow, oh) != 0) return -1;

    SDL_Rect *pos = (SDL_Rect *)malloc(sizeof(SDL_Rect) * (size_t)(s->nquads ? s->nquads : 1));
    if (!pos) return -1;
    if (atlas_pack_bake(cache, s, pos) != 0) { free(pos); return -1; }

    int rc = 0;

    /* Whole frame starts black, then the scene rows take their per-scanline
     * COLOR00 (pal_rows[y][0]) as the void colour — black in normal play, but
     * the victory fade is a COLOR00 curtain and the CPU compose paints the
     * void with it (byte-identity requires the same here). Fill in runs of
     * equal colour (normally one black run = nothing to draw). */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    {
        int y = y_lo;
        while (y < y_hi) {
            uint32_t c0 = (y >= 0 && y < SCENE_MAX_ROWS) ? s->pal_rows[y][0] : 0;
            int y1 = y + 1;
            while (y1 < y_hi) {
                uint32_t c1 = (y1 >= 0 && y1 < SCENE_MAX_ROWS) ? s->pal_rows[y1][0] : 0;
                if ((c1 & 0xFFFFFF) != (c0 & 0xFFFFFF)) break;
                y1++;
            }
            if (c0 & 0xFFFFFF) {
                SDL_SetRenderDrawColor(r, (c0 >> 16) & 0xFF, (c0 >> 8) & 0xFF, c0 & 0xFF, 255);
                SDL_Rect band = { 0, y, ow, y1 - y };
                if (SDL_RenderFillRect(r, &band) != 0) rc = -1;
            }
            y = y1;
        }
    }

    /* Base layer: the rows the scene does NOT own (top border + HUD) come from
     * the composed output surface unchanged — rect-update just those bands. */
    if (y_lo > 0) {
        SDL_Rect rr = { 0, 0, ow, y_lo };
        if (SDL_UpdateTexture(cache->base, &rr, base, ow * 4) != 0 ||
            SDL_RenderCopy(r, cache->base, &rr, &rr) != 0) rc = -1;
    }
    if (y_hi < oh) {
        SDL_Rect rr = { 0, y_hi, ow, oh - y_hi };
        if (SDL_UpdateTexture(cache->base, &rr, base + (size_t)y_hi * ow, ow * 4) != 0 ||
            SDL_RenderCopy(r, cache->base, &rr, &rr) != 0) rc = -1;
    }

    /* World quads: per-sprite, camera-projected (screen_x = x - view_left),
     * clipped to the camera-reachable world columns AND the playfield rows. */
    {
        int cx0 = s->wclip_x0 - s->view_left, cx1 = s->wclip_x1 - s->view_left;
        if (cx0 < 0)  cx0 = 0;
        if (cx1 > ow) cx1 = ow;
        if (cx1 > cx0) {
            SDL_Rect clip = { cx0, y_lo, cx1 - cx0, y_hi - y_lo };
            SDL_RenderSetClipRect(r, &clip);
            for (int i = 0; i < s->nquads && rc == 0; i++) {
                const SceneQuad *q = &s->quads[i];
                if (q->space != SCENE_SPACE_WORLD || pos[i].w == 0) continue;
                SDL_Rect d = { q->x - s->view_left, q->y, q->w, q->h };
                if (SDL_RenderCopy(r, cache->atlas, &pos[i], &d) != 0) rc = -1;
            }
            SDL_RenderSetClipRect(r, NULL);
        }
    }

    /* Screen quads (the banner): screen-fixed UI on top, no camera, full frame. */
    for (int i = 0; i < s->nquads && rc == 0; i++) {
        const SceneQuad *q = &s->quads[i];
        if (q->space != SCENE_SPACE_SCREEN || pos[i].w == 0) continue;
        SDL_Rect d = { q->x, q->y, q->w, q->h };
        if (SDL_RenderCopy(r, cache->atlas, &pos[i], &d) != 0) rc = -1;
    }
    free(pos);
    return rc;
}

/* CPU reference rasterizer (the lossless ground truth). */
void scene_composite_argb(const Scene *s, uint32_t *dst, int dst_w, int dst_h, int y_lo, int y_hi);

long scene_sdl_selftest(const Scene *s, int dst_w, int dst_h, int y_lo, int y_hi, int *max_chan)
{
    if (max_chan) *max_chan = 0;
    if (!s || dst_w <= 0 || dst_h <= 0) return -1;

    const size_t npx = (size_t)dst_w * dst_h;
    uint32_t *cpu = (uint32_t *)calloc(npx, sizeof(uint32_t));
    if (!cpu) return -1;

    /* CPU reference: composite over a zeroed buffer. */
    scene_composite_argb(s, cpu, dst_w, dst_h, y_lo, y_hi);

    /* SDL per-sprite: draw into a software-rendered surface, then read back. No
     * window / GPU — runs with the display off. */
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, dst_w, dst_h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *r = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
    if (!r) { if (surf) SDL_FreeSurface(surf); free(cpu); return -1; }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);          /* clear to 0x00000000 (matches cpu) */
    SDL_RenderClear(r);
    int drc = scene_draw_sdl(r, s, y_lo, y_hi);
    SDL_RenderPresent(r);

    long ndiff = (drc != 0) ? -1 : 0;
    if (drc == 0) {
        const uint32_t *sdl = (const uint32_t *)surf->pixels;
        int stride = surf->pitch / 4;
        int mc = 0;
        for (int y = 0; y < dst_h; y++)
            for (int x = 0; x < dst_w; x++) {
                uint32_t a = cpu[(size_t)y * dst_w + x];
                uint32_t b = sdl[(size_t)y * stride + x];
                if (a == b) continue;
                ndiff++;
                for (int sh = 0; sh < 32; sh += 8) {
                    int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
                    if (d < 0) d = -d;
                    if (d > mc) mc = d;
                }
            }
        if (max_chan) *max_chan = mc;
    }

    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    free(cpu);
    return ndiff;
}

long scene_sdl_window_selftest(const Scene *s, const uint32_t *base,
                               int ow, int oh, int y_lo, int y_hi, int *max_chan)
{
    if (max_chan) *max_chan = 0;
    if (!s || !base || ow <= 0 || oh <= 0) return -1;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *r = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
    if (!r) { if (surf) SDL_FreeSurface(surf); return -1; }

    SceneSdlCache cache = {0};
    int drc = scene_draw_sdl_window(r, s, y_lo, y_hi, base, ow, oh, &cache);
    SDL_RenderPresent(r);
    scene_sdl_cache_free(&cache);   /* before the renderer goes away */

    long ndiff = (drc != 0) ? -1 : 0;
    if (drc == 0) {
        const uint32_t *sdl = (const uint32_t *)surf->pixels;
        int stride = surf->pitch / 4;
        int mc = 0;
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                uint32_t a = base[(size_t)y * ow + x];      /* CPU-composed reference */
                uint32_t b = sdl[(size_t)y * stride + x];
                if (a == b) continue;
                ndiff++;
                for (int sh = 0; sh < 32; sh += 8) {
                    int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
                    if (d < 0) d = -d;
                    if (d > mc) mc = d;
                }
            }
        if (max_chan) *max_chan = mc;
    }

    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    return ndiff;
}
