/* scene_sdl.c — per-sprite SDL consumer of the BenRen draw list. See scene_sdl.h. */
#include "render/scene_sdl.h"
#include <stdlib.h>

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

        SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, q->w, q->h);
        if (!tex) { rc = -1; break; }
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

        void *pixels; int pitch;
        if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) {
            SDL_DestroyTexture(tex); rc = -1; break;
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

        SDL_Rect d = { q->x, q->y, q->w, q->h };
        if (SDL_RenderCopy(r, tex, NULL, &d) != 0) rc = -1;
        SDL_DestroyTexture(tex);
    }

    SDL_RenderSetClipRect(r, NULL);
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
