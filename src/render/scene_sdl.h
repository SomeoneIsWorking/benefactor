/* scene_sdl.h — the per-sprite SDL consumer of the BenRen draw list.
 *
 * Instead of compositing every sprite into one surface and blitting that, this
 * draws EACH quad as its own SDL texture via SDL_RenderCopy — a real per-sprite
 * renderer ("functions like a real PC game"). It's the SDL counterpart of the
 * Vulkan per-quad path, and the routine the windowed SDL present will call once
 * the whole frame is on the draw list.
 *
 * SDL2's 2D renderer has no fragment shader, so the engine's per-output-row
 * palette is resolved on the CPU when baking each quad's texture (transparent
 * texels -> alpha 0). Drawn texels land as opaque 0xFF|RGB, exactly matching the
 * CPU rasterizer (scene_composite_argb), so the two can be diffed byte-for-byte.
 *
 * See instructions/gpu-renderer-plan.md (P2). */
#ifndef RENDER_SCENE_SDL_H
#define RENDER_SCENE_SDL_H

#include "render/scene.h"
#include <SDL2/SDL.h>

/* Draw every quad of `s` into the current render target of `r`, compositing over
 * whatever the target already holds (transparent texels leave it untouched).
 * Only output rows in [y_lo, y_hi) are affected (vertical clip, like the CPU
 * rasterizer). Returns 0 on success, -1 on an SDL error. */
int scene_draw_sdl(SDL_Renderer *r, const Scene *s, int y_lo, int y_hi);

/* Headless self-test (display off): rasterize `s` two ways into dst_w x dst_h
 * ARGB buffers — the CPU reference (scene_composite_argb) and this per-sprite SDL
 * consumer (via a software renderer + readback) — and compare. Returns the count
 * of differing pixels (0 = byte-identical); *max_chan (if non-NULL) = the largest
 * per-channel absolute difference. Returns -1 on an SDL/alloc error. This is the
 * P2 correctness gate: it proves the SDL per-sprite path reproduces the draw list
 * losslessly before it becomes a runtime backend. */
long scene_sdl_selftest(const Scene *s, int dst_w, int dst_h, int y_lo, int y_hi, int *max_chan);

/* Persistent SDL resources for the windowed per-sprite present. Creating and
 * destroying a texture PER QUAD PER FRAME (the original P4 code) collapsed on
 * quad-heavy scenes (1600+ quads → visible slowdown; the F3 overlay "fixed" it
 * only because overlays force the plain-blit present). Instead all quads are
 * baked into ONE persistent streaming ATLAS (single lock per frame, shelf
 * packing) and drawn with per-quad RenderCopy from atlas rects; the base
 * surface texture persists too (rect-updated, only the non-scene rows).
 * Textures belong to one SDL_Renderer: free with scene_sdl_cache_free() while
 * that renderer is still alive. Zero-initialize the struct before first use. */
typedef struct SceneSdlCache {
    SDL_Renderer *r;                 /* owning renderer (cache auto-resets if it changes) */
    SDL_Texture  *atlas;             /* SCENE_SDL_ATLAS_W x _H, BLEND, streaming */
    SDL_Texture  *base;              /* composed-output copy for non-scene rows  */
    int base_w, base_h;
} SceneSdlCache;
void scene_sdl_cache_free(SceneSdlCache *c);

/* P4 — the WINDOWED per-sprite frame: clear black, draw the non-scene rows
 * (top border + HUD) from the composed `base` surface, then every WORLD quad
 * camera-projected (screen_x = x - s->view_left, clipped to the scene's world
 * clip range and [y_lo, y_hi)), then the SCREEN quads (banner) on top. This is
 * the routine the SDL present backend calls instead of blitting `base`.
 * `cache` (required) holds the persistent atlas/base textures. */
int scene_draw_sdl_window(SDL_Renderer *r, const Scene *s, int y_lo, int y_hi,
                          const uint32_t *base, int ow, int oh,
                          SceneSdlCache *cache);

/* Headless gate for the windowed path: render scene_draw_sdl_window into an
 * offscreen software renderer and byte-diff the FULL frame against `base`
 * (the CPU-composed output, which the windowed path must reproduce exactly).
 * Returns differing-pixel count (0 = identical), -1 on error. */
long scene_sdl_window_selftest(const Scene *s, const uint32_t *base,
                               int ow, int oh, int y_lo, int y_hi, int *max_chan);

#endif /* RENDER_SCENE_SDL_H */
