#version 450
/* fx.frag — BenRen VK lighting pass. Drawn as a fullscreen triangle AFTER the
 * scene quads, in the SAME render pass, blended into the framebuffer:
 *   mode 0 (dim)  — output brightness; pipeline uses MULTIPLY blend (dst *= b).
 *                   Combines ambient darkness + torch glow. Identity (1.0) outside
 *                   the playfield rows / when those flags are off.
 *   mode 1 (glow) — output a warm colour; pipeline uses ADDITIVE blend (dst += g).
 *                   Character glow around the player. Identity (0.0) elsewhere.
 *
 * Pure per-pixel + the projected light position (push constants) — no texture read,
 * so it needs no descriptor set. Geometry in CONTENT px: v_uv*res, matching the
 * scene's coordinate system (content fills the window). */
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(push_constant) uniform PC {
    vec2 res;     /* content_w, content_h */
    vec2 light;   /* player centre, content px; <0 = none */
    vec2 pf;      /* pf_top, pf_bot, content px */
    int  flags;   /* FX_AMBIENT|FX_TORCH|FX_CHARGLOW */
    int  mode;    /* 0 = dim (multiply), 1 = glow (add) */
} pc;

const int FX_AMBIENT  = 1;
const int FX_TORCH    = 2;
const int FX_CHARGLOW = 4;

float smooth01(float t) { return t * t * (3.0 - 2.0 * t); }

float radial(vec2 px, vec2 c, float core, float edge, float floorB) {
    float d = distance(px, c);
    if (d <= core) return 1.0;
    if (d >= edge) return floorB;
    return 1.0 - smooth01((d - core) / (edge - core)) * (1.0 - floorB);
}

void main() {
    vec2 px   = v_uv * pc.res;
    bool inpf = (px.y >= pc.pf.x && px.y < pc.pf.y);

    if (pc.mode == 0) {                       /* dim: identity = white (dst*1) */
        float b = 1.0;
        if (inpf) {
            vec2 ctr = vec2(pc.res.x * 0.5, (pc.pf.x + pc.pf.y) * 0.5);
            vec2 lit = (pc.light.x >= 0.0) ? pc.light : ctr;
            if ((pc.flags & FX_AMBIENT) != 0)
                b *= radial(px, ctr, pc.res.x * 0.20, pc.res.x * 0.62, 0.45);
            if ((pc.flags & FX_TORCH) != 0)
                b *= radial(px, lit, 84.0, 220.0, 0.30);
        }
        o_color = vec4(vec3(b), 1.0);
    } else {                                  /* glow: identity = black (dst+0) */
        vec3 g = vec3(0.0);
        if (inpf && (pc.flags & FX_CHARGLOW) != 0 && pc.light.x >= 0.0) {
            float f = 1.0 - smooth01(clamp(distance(px, pc.light) / 26.0, 0.0, 1.0));
            g = vec3(0.16, 0.12, 0.05) * f;
        }
        o_color = vec4(g, 1.0);
    }
}
