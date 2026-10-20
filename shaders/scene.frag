#version 450
layout(location = 0) in vec3 v_colour;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

// Texture times vertex light: the per-face shading, selection tints and reticle all live in the
// vertex colour and keep working on top of any texture. Against the all-white texture this is an
// identity (x * 1.0 is exact), which is what keeps the frame unchanged.
void main() {
    out_colour = vec4(texture(u_texture, v_uv).rgb * v_colour, 1.0);
}
