/* native_effects.h — effect-flag resolution for the NATIVE (BenRen) renderer.
 *
 * Effects are Vulkan-only now (see effects_frame.h + shaders/fx.frag): there
 * is no CPU effect pass. This unit just resolves the three independent effect
 * toggles from cfg into the FX_* bitmask the renderer publishes and the Vulkan
 * present consumes. Off by default; in Vanilla/Software the flags are inert
 * (only the Vulkan backend's set_effects path applies them). */
#pragma once

/* FX_AMBIENT | FX_TORCH | FX_CHARGLOW, resolved live from the fx_* cfg knobs. */
int native_fx_flags(void);
