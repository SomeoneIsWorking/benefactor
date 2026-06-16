#version 450
/* quad.vert — one textured (or flat-colour) quad from push constants, no vertex
 * buffer. Drawn as a 4-vertex triangle strip; gl_VertexIndex picks the corner.
 * dst is the destination rect in OUTPUT PIXELS (converted to NDC here); src is the
 * source rect in atlas UV (0..1). This is the BenRen VK per-sprite primitive: the
 * renderer issues one draw per sprite/tile/banner with its own push constants. */
layout(push_constant) uniform PC {
    vec4 dst;     /* x, y, w, h in output pixels */
    vec4 src;     /* u0, v0, u1, v1 in atlas UV */
    vec4 color;   /* flat colour (mode 1) */
    vec2 screen;  /* output extent in pixels (for pixel -> NDC) */
    int  mode;    /* 0 = sample atlas, 1 = flat colour */
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 px  = pc.dst.xy + corner * pc.dst.zw;
    vec2 ndc = (px / pc.screen) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = mix(pc.src.xy, pc.src.zw, corner);
}
