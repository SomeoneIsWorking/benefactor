#version 450
/* effects.frag — the Vulkan-only post-process pass for the Hardware renderer.
 *
 * Samples the composed output surface (B8G8R8A8) and applies up to three
 * independent effects over the playfield rows, driven by push constants the
 * native renderer publishes each frame (see render/effects_frame.h). With
 * flags==0 (or outside the playfield rows) this is an exact passthrough, so the
 * Hardware renderer with all effects off looks identical to the plain blit.
 *
 * Geometry is in CONTENT pixels: v_uv (0..1) * res reproduces the old CPU effect
 * math exactly (ambient vignette + player torch), plus a subtle character glow. */
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PC {
    vec2 res;     /* content_w, content_h */
    vec2 light;   /* projected player centre, content px; <0 = none */
    vec2 pf;      /* pf_top, pf_bot in content px */
    int  flags;   /* bit0 ambient, bit1 torch, bit2 charglow */
} pc;

const int FX_AMBIENT  = 1;
const int FX_TORCH    = 2;
const int FX_CHARGLOW = 4;

float smooth01(float t) { return t * t * (3.0 - 2.0 * t); }

/* Radial brightness: 1.0 inside `core`, falling to `floorB` at `edge` (smoothstep),
 * flat at `floorB` beyond — matches the old CPU apply_radial exactly. */
float radial(vec2 px, vec2 c, float core, float edge, float floorB) {
    float d = distance(px, c);
    if (d <= core) return 1.0;
    if (d >= edge) return floorB;
    return 1.0 - smooth01((d - core) / (edge - core)) * (1.0 - floorB);
}

void main() {
    vec3 c  = texture(u_tex, v_uv).rgb;
    vec2 px = v_uv * pc.res;

    if (px.y >= pc.pf.x && px.y < pc.pf.y) {
        vec2 centre = vec2(pc.res.x * 0.5, (pc.pf.x + pc.pf.y) * 0.5);
        vec2 lit    = (pc.light.x >= 0.0) ? pc.light : centre;
        float bright = 1.0;

        if ((pc.flags & FX_AMBIENT) != 0)
            bright *= radial(px, centre, pc.res.x * 0.20, pc.res.x * 0.62, 0.45);

        if ((pc.flags & FX_TORCH) != 0)
            bright *= radial(px, lit, 84.0, 220.0, 0.30);

        c *= bright;

        /* Character glow: a slight warm additive halo on the player (~26px). */
        if ((pc.flags & FX_CHARGLOW) != 0 && pc.light.x >= 0.0) {
            float g = 1.0 - smooth01(clamp(distance(px, pc.light) / 26.0, 0.0, 1.0));
            c += vec3(0.16, 0.12, 0.05) * g;
        }
    }

    o_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
