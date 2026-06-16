/* effects_frame.h — the seam between the native renderer (which knows WHERE the
 * light + playfield are this frame) and the present backend (which APPLIES the
 * effects). Effects are Vulkan-only: the renderer projects the player light and
 * the playfield row span and publishes them here; hw.c forwards the published
 * FxFrame (plus the live effect flags) to the present backend's set_effects()
 * hook. The SDL backend has no set_effects → the software renderer shows no
 * effects (the hard gate). The Vulkan present consumes it as shader push
 * constants (see shaders/fx.frag).
 *
 * Pure screen/content-space: light_sx/sy and pf_top/pf_bot are in CONTENT pixels
 * (the composed output surface, content_w x content_h). The renderer owns the
 * camera projection so the backend never re-derives it. light_sx < 0 means "no
 * light source this frame" (menus/transitions) → effects fall back to view centre. */
#pragma once
#include <stdint.h>

/* Effect flags (bitmask from native_fx_flags(), live from cfg each frame). */
enum {
    FX_AMBIENT    = 1 << 0,  /* ambient darkness — vignette toward the view edges */
    FX_SPRITEGLOW = 1 << 2,  /* sprite glow — faint additive halo behind foreground sprites */
};

typedef struct {
    int valid;                 /* 1 = a gameplay playfield was found this frame */
    int content_w, content_h;  /* composed output surface size */
    int pf_top, pf_bot;        /* playfield row span [pf_top, pf_bot) in content px */
    int light_sx, light_sy;    /* projected player centre, content px; <0 = none */
    int flags;                 /* FX_* bitmask, resolved live from cfg */
    int spriteglow;            /* SPRITE GLOW strength 0..3 (0 = off) */
} FxFrame;

/* Published by the native renderer each frame; consumed by hw.c at present time.
 * Returns a pointer to the latest frame (never NULL); .valid says if it's fresh. */
const FxFrame *native_render_fx_frame(void);

/* Live effect flags from cfg (fx_ambient / fx_spriteglow). */
int native_fx_flags(void);

/* SPRITE GLOW strength 0..3 (the fx_spriteglow knob, clamped). */
int native_fx_spriteglow(void);
