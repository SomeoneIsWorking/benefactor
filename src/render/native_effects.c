/* native_effects.c — resolve the three independent effect toggles from cfg.
 *
 * Effects are Vulkan-only (see effects_frame.h + shaders/fx.frag). There is
 * no CPU effect pass: the native renderer projects the light + playfield rows and
 * publishes a FxFrame; the Vulkan present applies the effects in the fragment
 * shader. This file is the single place that turns the user-facing knobs into the
 * FX_* bitmask both the menu and the shader-param path read.
 *
 *   fx_ambient — ambient darkness (vignette toward the view edges), bool
 *   fx_shadow  — drop shadow behind characters (player / Marry Men / enemies), bool
 *
 * Each is OFF by default. They are independent and may combine. */
#include "render/effects_frame.h"
#include "port/config.h"

int native_fx_flags(void)
{
    int f = 0;
    if (pc_cfg_bool("fx_ambient", 0)) f |= FX_AMBIENT;
    if (pc_cfg_bool("fx_shadow",  0)) f |= FX_SHADOW;
    return f;
}
