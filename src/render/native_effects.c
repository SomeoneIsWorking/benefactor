/* native_effects.c — post-process effects for the native (BenRen) renderer.
 *
 * See native_effects.h for the seam contract. Today this hosts the "lighting"
 * effect; it is the template for future native-only effects (bloom, colour
 * grade, scanlines, …) — each one resolves its own cfg knob and reads/writes the
 * composited output buffer over the playfield rows. Pure screen-space: the light
 * source position is projected by the renderer and handed in, so nothing here
 * curve-fits to a reference or duplicates the camera projection. */
#include "render/native_effects.h"
#include "port/config.h"        /* pc_cfg_show — the "lighting" knob */
#include <string.h>
#include <math.h>

typedef enum { LIGHT_OFF = 0, LIGHT_AMBIENT, LIGHT_PLAYER } LightMode;

static LightMode lighting_mode(void)
{
    char buf[24] = "";
    if (!pc_cfg_show("lighting", buf, sizeof buf, NULL) || !buf[0]) return LIGHT_OFF;
    if (!strcasecmp(buf, "ambient")) return LIGHT_AMBIENT;
    if (!strcasecmp(buf, "player"))  return LIGHT_PLAYER;
    /* "off"/"0"/"false"/unknown → off (a typo shouldn't silently dim the game). */
    return LIGHT_OFF;
}

/* Multiply a pixel's RGB by `q8` (0..256 fixed-point), preserving alpha. */
static inline uint32_t scale_rgb(uint32_t argb, int q8)
{
    uint32_t a = argb & 0xFF000000u;
    uint32_t r = (((argb >> 16) & 0xFF) * (uint32_t)q8) >> 8;
    uint32_t g = (((argb >>  8) & 0xFF) * (uint32_t)q8) >> 8;
    uint32_t b = (((argb      ) & 0xFF) * (uint32_t)q8) >> 8;
    return a | (r << 16) | (g << 8) | b;
}

/* Radial light: brightness 1.0 inside `core` px of (cx,cy), falling off to the
 * ambient `floor` by `edge` px, then flat at `floor`. Applied per playfield row. */
static void apply_radial(uint32_t *out, int ow, int pf_top, int pf_bot,
                         float cx, float cy, float core, float edge, float floor)
{
    const float span = (edge > core) ? (edge - core) : 1.0f;
    for (int y = pf_top; y < pf_bot; y++) {
        uint32_t *row = out + (size_t)y * ow;
        const float dy = (float)y - cy;
        for (int x = 0; x < ow; x++) {
            const float dx = (float)x - cx;
            const float d  = sqrtf(dx * dx + dy * dy);
            float bright;
            if      (d <= core) bright = 1.0f;
            else if (d >= edge) bright = floor;
            else {
                float t = (d - core) / span;        /* 0..1 across the falloff band */
                t = t * t * (3.0f - 2.0f * t);      /* smoothstep */
                bright = 1.0f - t * (1.0f - floor);
            }
            row[x] = scale_rgb(row[x], (int)(bright * 256.0f + 0.5f));
        }
    }
}

void native_effects_apply(uint32_t *out, int ow, int oh, int pf_top, int pf_bot,
                          int light_sx, int light_sy)
{
    (void)oh;
    LightMode mode = lighting_mode();
    if (mode == LIGHT_OFF || pf_bot <= pf_top || ow <= 0) return;

    const float ph = (float)(pf_bot - pf_top);

    if (mode == LIGHT_AMBIENT) {
        /* Soft vignette around the view centre — no light source, just mood. */
        apply_radial(out, ow, pf_top, pf_bot,
                     ow * 0.5f, pf_top + ph * 0.5f,
                     ow * 0.20f, ow * 0.62f, 0.45f);
        return;
    }

    /* LIGHT_PLAYER: a torch centred on the player, surroundings dimmed to an
     * ambient floor. The renderer hands us the projected screen position; if the
     * player sprite wasn't on screen this frame (light_sx < 0), fall back to the
     * view centre so transitions/menus don't pop. */
    float cx = (light_sx >= 0) ? (float)light_sx : ow * 0.5f;
    float cy = (light_sx >= 0) ? (float)light_sy : pf_top + ph * 0.5f;
    apply_radial(out, ow, pf_top, pf_bot, cx, cy, 84.0f, 220.0f, 0.30f);
}
