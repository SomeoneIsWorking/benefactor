#version 450
/* fx.frag — BenRen VK DIM pass (ambient darkness). Drawn as a fullscreen triangle
 * after the world sprites, in the same render pass, with a MULTIPLY blend
 * (dst *= brightness). Outputs 1.0 (identity) outside the playfield rows or when the
 * flag is off, so it never touches the HUD/border/banner. (The per-sprite back-glow
 * is a separate additive pass in the quad pipeline.)
 *
 * Pure per-pixel (push constants) — no texture read, no descriptor set. Geometry in
 * CONTENT px: v_uv*res matches the scene. */
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(push_constant) uniform PC {
    vec2 res;     /* content_w, content_h */
    vec2 light;   /* unused (kept for push-constant layout parity) */
    vec2 pf;      /* pf_top, pf_bot, content px */
    int  flags;   /* FX_AMBIENT */
    int  mode;    /* unused */
} pc;

const int FX_AMBIENT = 1;

float smooth01(float t) { return t * t * (3.0 - 2.0 * t); }

float radial(vec2 px, vec2 c, float core, float edge, float floorB) {
    float d = distance(px, c);
    if (d <= core) return 1.0;
    if (d >= edge) return floorB;
    return 1.0 - smooth01((d - core) / (edge - core)) * (1.0 - floorB);
}

void main() {
    vec2 px = v_uv * pc.res;
    float b = 1.0;
    if (px.y >= pc.pf.x && px.y < pc.pf.y && (pc.flags & FX_AMBIENT) != 0) {
        vec2 ctr = vec2(pc.res.x * 0.5, (pc.pf.x + pc.pf.y) * 0.5);
        b = radial(px, ctr, pc.res.x * 0.20, pc.res.x * 0.62, 0.45);
    }
    o_color = vec4(vec3(b), 1.0);
}
