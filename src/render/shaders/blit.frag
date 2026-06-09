#version 450
/* Sample the uploaded content texture. Source is the composed output surface
   (B8G8R8A8); we sample straight through to the target. */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(binding = 0) uniform sampler2D u_tex;
void main() {
    o_color = texture(u_tex, v_uv);
}
