/* present_sdl.c — the default present backend: SDL_Renderer + streaming texture.
 *
 * This is the exact path that lived in engine/hw.c before the backend seam; it is
 * behavior-identical. Also hosts present_backend_select() (always built), which
 * routes to the Vulkan backend when this build has one. */
#include "common/log.h"
#include "render/present_backend.h"
#include "render/scene_sdl.h"
#include <string.h>

static SDL_Window *s_window = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture *s_texture = NULL;
static int s_content_w = 0, s_content_h = 0;
static SceneSdlCache s_scene_cache = {0}; /* persistent atlas/base for present_scene */

/* The content (output-surface) size can change at runtime — the widescreen
 * option in the pause menu / auto mode. Recreate the streaming texture and
 * logical size to match before the next upload. */
static int sdl_ensure_content(int w, int h) {
    if (w == s_content_w && h == s_content_h && s_texture)
        return 0;
    if (s_texture)
        SDL_DestroyTexture(s_texture);
    s_texture =
        SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_texture)
        return -1;
    SDL_RenderSetLogicalSize(s_renderer, w, h);
    s_content_w = w;
    s_content_h = h;
    return 0;
}

static int sdl_init(const char *title, int content_w, int content_h) {
    s_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                content_w * 2, content_h * 2, SDL_WINDOW_RESIZABLE);
    if (!s_window)
        return -1;

    /* No PRESENTVSYNC: vsync would lock the game to the monitor's refresh
     * (e.g. 60 Hz -> 20% too fast). hw_present_frame paces to PAL 50 Hz. */
    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer)
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    if (!s_renderer)
        return -1;

    return sdl_ensure_content(content_w, content_h);
}

static void sdl_present(const uint32_t *argb, int w, int h) {
    if (sdl_ensure_content(w, h) != 0)
        return;
    SDL_UpdateTexture(s_texture, NULL, argb, w * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

/* P4 — windowed PER-SPRITE present: draw the BenRen draw list straight to the
 * window (base rows + camera-projected world quads + banner) instead of
 * blitting the composed surface. Verified byte-identical to the composed
 * surface via scene_sdl_window_selftest (harness `scenewin`). */
static void sdl_present_scene(const Scene *s, int y_lo, int y_hi, const uint32_t *base, int w,
                              int h, const PresentRect *rects, int nrects) {
    if (sdl_ensure_content(w, h) != 0)
        return;
    if (scene_draw_sdl_window(s_renderer, s, y_lo, y_hi, base, w, h, &s_scene_cache) != 0) {
        /* SDL failure mid-frame: fall back to the plain blit so the user
         * still sees the (identical) composed frame. */
        sdl_present(base, w, h);
        return;
    }
    /* PC overlay rects (HUD icons / profiler): re-assert those composed-surface
     * regions on top of the scene. The cache's base texture already holds the
     * full composed frame rows it was updated with; update + copy just the rects. */
    for (int i = 0; i < nrects && s_scene_cache.base; i++) {
        SDL_Rect rc = {rects[i].x, rects[i].y, rects[i].w, rects[i].h};
        SDL_UpdateTexture(s_scene_cache.base, &rc, base + (size_t)rc.y * w + rc.x, w * 4);
        SDL_RenderCopy(s_renderer, s_scene_cache.base, &rc, &rc);
    }
    SDL_RenderPresent(s_renderer);
}

static SDL_Window *sdl_window(void) { return s_window; }

static void sdl_shutdown(void) {
    scene_sdl_cache_free(&s_scene_cache);
    if (s_texture)
        SDL_DestroyTexture(s_texture);
    if (s_renderer)
        SDL_DestroyRenderer(s_renderer);
    if (s_window)
        SDL_DestroyWindow(s_window);
    s_texture = NULL;
    s_renderer = NULL;
    s_window = NULL;
    s_content_w = s_content_h = 0;
}

static const PresentBackend SDL_BACKEND = {
    "sdl",
    sdl_init,
    sdl_present,
    sdl_present_scene,
    NULL /* set_effects: software renders no effects (the hard gate) */,
    sdl_window,
    sdl_shutdown};

const PresentBackend *present_backend_sdl(void) { return &SDL_BACKEND; }

const PresentBackend *present_backend_select(const char *name) {
#ifdef BENEFACTOR_HAVE_VULKAN
    if (name && !strcmp(name, "vulkan")) {
        const PresentBackend *vk = present_backend_vulkan();
        if (vk)
            return vk;
        benefactor_log_write(BENEFACTOR_LOG_WARNING, "render",
                             "Vulkan backend unavailable at runtime; using SDL");
        return present_backend_sdl();
    }
#else
    if (name && !strcmp(name, "vulkan"))
        benefactor_log_write(BENEFACTOR_LOG_WARNING, "render",
                             "built without Vulkan support; using SDL");
#endif
    return present_backend_sdl();
}
