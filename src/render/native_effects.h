/* native_effects.h — post-process visual effects for the NATIVE (BenRen) renderer.
 *
 * The vanilla (Amiga-blit) path is byte-faithful to the original and must never
 * be touched. The native renderer composes the whole frame itself, which makes
 * it the place where PC-only effects live. This is the seam those effects hang
 * off: a single SCREEN-SPACE pass that runs AFTER the playfield + sprites are
 * composited into the wide output buffer, but BEFORE screen-fixed UI (the
 * GET READY / GAME OVER banner), so HUD/banner overlays are never dimmed.
 *
 * Effects operate on the composited ARGB buffer over the playfield rows
 * [pf_top, pf_bot). The renderer projects any world-anchored light source (e.g.
 * the player) into output pixels itself and passes it as (light_sx, light_sy) —
 * the effects layer stays pure screen-space and never re-derives the camera
 * projection (which differs between the 4:3 and widescreen paths). light_sx < 0
 * means "no source this frame" (menus/transitions) → effects fall back to the
 * view centre.
 *
 * All effects are OFF by default (the "lighting" cfg knob); when off this is a
 * cheap early-out, so it's safe to call unconditionally. Adding an effect:
 * resolve its knob, then read+write `out` in the row range. */
#pragma once
#include <stdint.h>

void native_effects_apply(uint32_t *out, int ow, int oh, int pf_top, int pf_bot,
                          int light_sx, int light_sy);
