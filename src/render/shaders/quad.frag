#version 450
/* quad.frag — sample the atlas (mode 0) or emit a flat colour (mode 1, the
 * per-row void background). Sprite texels are baked opaque (a=1) or transparent
 * (a=0); discard the holes so painter's order (draw order) composites exactly
 * like the CPU rasterizer. No blending needed — edges are hard-cut.
 *
 * This is where a future per-object shader / lighting term hangs off: each draw
 * already carries its own push constants, so a material id + light params can be
 * added per quad without touching the renderer. */
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PC {
    vec4 dst;
    vec4 src;
    vec4 color;
    vec2 screen;
    int  mode;
} pc;

void main() {
    if (pc.mode == 1) { o_color = pc.color; return; }   /* flat colour (void) */
    if (pc.mode == 2) {                                  /* faint additive back-glow */
        /* sprite colour * its alpha * intensity (color.r); ADDITIVE blend. Drawn on
         * an EXPANDED quad behind the sprite, so a soft colour fringe haloes it. */
        vec4 t = texture(u_tex, v_uv);
        o_color = vec4(t.rgb * t.a * pc.color.r, 1.0);
        return;
    }
    vec4 t = texture(u_tex, v_uv);
    if (t.a < 0.5) discard;
    o_color = vec4(t.rgb, 1.0);
}
