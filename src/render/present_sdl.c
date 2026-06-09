/* present_sdl.c — the default present backend: SDL_Renderer + streaming texture.
 *
 * This is the exact path that lived in engine/hw.c before the backend seam; it is
 * behavior-identical. Also hosts present_backend_select() (always built), which
 * routes to the Vulkan backend when this build has one. */
#include "render/present_backend.h"
#include <string.h>
#include <stdio.h>

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;

static int sdl_init(const char *title, int content_w, int content_h)
{
    s_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        content_w * 2, content_h * 2,
        SDL_WINDOW_RESIZABLE);
    if (!s_window) return -1;

    /* No PRESENTVSYNC: vsync would lock the game to the monitor's refresh
     * (e.g. 60 Hz -> 20% too fast). hw_present_frame paces to PAL 50 Hz. */
    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer)
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    if (!s_renderer) return -1;

    SDL_RenderSetLogicalSize(s_renderer, content_w, content_h);

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        content_w, content_h);
    if (!s_texture) return -1;
    return 0;
}

static void sdl_present(const uint32_t *argb, int w, int h)
{
    (void)h;
    SDL_UpdateTexture(s_texture, NULL, argb, w * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

static void sdl_toggle_fullscreen(void)
{
    uint32_t flags = SDL_GetWindowFlags(s_window);
    SDL_SetWindowFullscreen(s_window,
        (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

static SDL_Window *sdl_window(void) { return s_window; }

static void sdl_shutdown(void)
{
    if (s_texture)  SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window)   SDL_DestroyWindow(s_window);
    s_texture = NULL; s_renderer = NULL; s_window = NULL;
}

static const PresentBackend SDL_BACKEND = {
    "sdl", sdl_init, sdl_present, sdl_toggle_fullscreen, sdl_window, sdl_shutdown
};

const PresentBackend *present_backend_sdl(void) { return &SDL_BACKEND; }

const PresentBackend *present_backend_select(const char *name)
{
#ifdef BENEFACTOR_HAVE_VULKAN
    if (name && !strcmp(name, "vulkan")) {
        const PresentBackend *vk = present_backend_vulkan();
        if (vk) return vk;
        fprintf(stderr, "[render] vulkan backend unavailable at runtime; using sdl\n");
        return present_backend_sdl();
    }
#else
    if (name && !strcmp(name, "vulkan"))
        fprintf(stderr, "[render] built without Vulkan support; using sdl\n");
#endif
    return present_backend_sdl();
}
