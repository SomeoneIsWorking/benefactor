/* native_renderer.h — Copper-walking native renderer */
#pragma once

#include "render/scene.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Render one frame by walking the copper list in chip RAM and reading
 * bitplane data directly, bypassing hw_copper.c / hw_blitter.c emulation.
 * Writes the result into s_fb[] (the shared ARGB8888 framebuffer).
 * Called from hw_present_frame() in place of hw_render_frame(). */
void native_render_frame(void);

/* The current gameplay draw list (BenRen) + the playfield row span it targets,
 * for the per-sprite backends and headless verification. The scene is valid
 * after a BenRen frame (native_render_wide_bg). See render/scene.h. */
const Scene *native_render_scene(void);
void native_render_scene_yrange(int *lo, int *hi);
void native_render_scene_dims(int *w, int *h); /* target object-layer dims (WS_LAYER_W x H) */

/* REPL diagnostic: per-scanline BPL pointer snapshot from the last vanilla-path
 * render (single-playfield lines only). Returns nplanes (0 = no data). */
int native_render_line_info(int y, uint32_t pt[5], int *xoff, int *scr1, int *width);

/* Scanline queries used by the host overlays to tint against what is behind. */
uint32_t native_scanline_bgcolor(int y);
int native_scanline_palette_luma(int y);

/* The scene is rebuilt once per gameplay frame; the present path must not read
 * a half-built one. Invalidate on anything that changes the world under it. */
void native_render_scene_invalidate(void);
int native_render_scene_ready(void);

/* The guest's own DIWSTRT/DIWSTOP display window, in framebuffer columns. */
void native_std_display_window(int *x0, int *x1);

/* Widescreen viewport: where the wide view sits inside the world, and how a
 * follow camera is clamped to the level's bounds. */
extern int g_ws_view_left, g_ws_view_w;
int ws_view_left(int ow);
int ws_follow_clamp(int ow, int sx);
#ifdef __cplusplus
} /* extern "C" */
#endif
