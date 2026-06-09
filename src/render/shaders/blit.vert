#version 450
/* Fullscreen triangle (3 vertices, no vertex buffer). uv 0..1 spans the target;
   at 1:1 dst:src with nearest filtering this is an exact texel copy. */
layout(location = 0) out vec2 v_uv;
void main() {
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
