/* recomp/native_renderer.h — Copper-walking native renderer */
#pragma once

/* Render one frame by walking the copper list in chip RAM and reading
 * bitplane data directly, bypassing hw_copper.c / hw_blitter.c emulation.
 * Writes the result into s_fb[] (the shared ARGB8888 framebuffer).
 * Called from hw_present_frame() in place of hw_render_frame(). */
void native_render_frame(void);
