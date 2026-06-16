#version 450
/* quad.frag — sample the atlas (mode 0) or emit a flat colour (mode 1, the
 * per-row void background). Sprite texels are baked opaque (a=1) or transparent
 * (a=0); discard the holes so painter's order (draw order) composites exactly
 * like the CPU rasterizer. No blending needed — edges are hard-cut.
 *
 * mode 2 is the per-character drop shadow (black silhouette, alpha-blended). This is
 * where a future per-object shader / material hangs off: each draw already carries
 * its own push constants, so a material id can be added per quad. */
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
    if (pc.mode == 2) {                                  /* drop shadow */
        /* The sprite's SILHOUETTE in black, ALPHA-blended at pc.color.a opacity.
         * Drawn OFFSET behind the character so a soft dark shadow sits under it. */
        float a = texture(u_tex, v_uv).a * pc.color.a;
        o_color = vec4(0.0, 0.0, 0.0, a);
        return;
    }
    vec4 t = texture(u_tex, v_uv);
    if (t.a < 0.5) discard;
    o_color = vec4(t.rgb, 1.0);
}
