/* present_backend.h — the seam between "what to show" and "how to show it".
 *
 * The renderer (native_renderer.c) composes the final ARGB8888 output surface
 * (engine/hw.c: s_out, s_hw_out_w x HW_DISPLAY_H). A PresentBackend owns the
 * window and turns that surface into pixels on screen. Two implementations:
 *   - present_sdl.c    — SDL_Renderer + streaming texture (default, always built)
 *   - present_vulkan.c — Vulkan swapchain + textured fullscreen quad
 *                        (built only when CMake finds Vulkan; the future home of
 *                         the per-character lighting pass)
 *
 * engine/hw.c keeps composing s_out, pumping SDL events, and pacing frames; it
 * just delegates window creation + present to the selected backend. This is the
 * groundwork for Vulkan effects without disturbing the software renderer. */
#ifndef RENDER_PRESENT_BACKEND_H
#define RENDER_PRESENT_BACKEND_H

#include <stdint.h>
#include <SDL2/SDL.h>

typedef struct PresentBackend {
    const char *name;                                   /* "sdl" | "vulkan" */
    /* Create the window for `content_w` x `content_h` logical content (scaled to
     * the actual window). Returns 0 on success, -1 on failure. */
    int  (*init)(const char *title, int content_w, int content_h);
    /* Upload `argb` (w x h, ARGB8888, row stride w*4) and present it to the window. */
    void (*present)(const uint32_t *argb, int w, int h);
    void (*toggle_fullscreen)(void);
    SDL_Window *(*window)(void);                        /* hw.c still pumps SDL events */
    void (*shutdown)(void);
} PresentBackend;

/* Select a backend by name ("sdl" | "vulkan" | NULL=default sdl). Falls back to
 * SDL (with a log line) if the requested backend isn't available in this build.
 * Never returns NULL. */
const PresentBackend *present_backend_select(const char *name);

/* Backend providers (defined in their respective .c files). */
const PresentBackend *present_backend_sdl(void);
#ifdef BENEFACTOR_HAVE_VULKAN
const PresentBackend *present_backend_vulkan(void);     /* NULL if runtime init unavailable */
#endif

#endif /* RENDER_PRESENT_BACKEND_H */
