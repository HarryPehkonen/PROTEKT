#version 450
layout(location = 0) in vec3 v_colour;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_alpha;
layout(location = 3) in float v_fog;
layout(location = 0) out vec4 out_colour;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

// The fog's colour (the sky's horizon ring), pushed by the renderer after the vertex stage's matrix.
layout(push_constant) uniform Fog {
    layout(offset = 64) vec3 u_fog_colour;
};

// Texture times vertex light: the per-face shading, selection tints and reticle all live in the
// vertex colour and keep working on top of any texture. Against the all-white texture this is an
// identity (x * 1.0 is exact), which is what keeps the frame unchanged. The alpha comes straight
// from the vertex, and 1 makes the blend below a no-op -- so world geometry and every HUD plate
// drawn at alpha 1 stay exactly as they were, and only the panel faces that ask for it blend.
// The distance fog is mixed in AFTER the texture, so the texel's darkening cannot crush it. With
// v_fog at 0 mix() returns the surface exactly, so unfogged geometry is the old expression.
void main() {
    vec3 surface = texture(u_texture, v_uv).rgb * v_colour;
    out_colour = vec4(mix(surface, u_fog_colour, v_fog), v_alpha);
}
