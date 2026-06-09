/* recomp/native_renderer.h — Copper-walking native renderer */
#pragma once

#include "render/scene.h"

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
void native_render_scene_dims(int *w, int *h);   /* target object-layer dims (WS_LAYER_W x H) */
