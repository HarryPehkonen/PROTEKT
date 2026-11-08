#version 450
layout(location = 0) in vec3 v_colour;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_alpha;
layout(location = 3) in float v_fog;
layout(location = 4) in vec3 v_normal;
layout(location = 0) out vec4 out_colour;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

// The fog's colour (the sky's horizon ring) and the sun's light, pushed by the renderer after the
// vertex stage's matrix. The sun direction is a unit vector toward the sun; the floor is the light
// of a normal square away from it. Both are the game's own constants (kSunDirection, kShadeFloor).
layout(push_constant) uniform Light {
    layout(offset = 64) vec3 u_fog_colour;
    layout(offset = 76) float u_shade_floor;
    layout(offset = 80) vec3 u_sun_direction;
};

// Texture times vertex light: the per-face shading, selection tints and reticle all live in the
// vertex colour and keep working on top of any texture. Against the all-white texture this is an
// identity (x * 1.0 is exact), which is what keeps the frame unchanged. The alpha comes straight
// from the vertex, and 1 makes the blend below a no-op -- so world geometry and every HUD plate
// drawn at alpha 1 stay exactly as they were, and only the panel faces that ask for it blend.
// The distance fog is mixed in AFTER the texture, so the texel's darkening cannot crush it. With
// v_fog at 0 mix() returns the surface exactly, so unfogged geometry is the old expression.
// THE LIGHT, from the normal: the half-Lambert ramp of `face_light` in scene.cpp, which is the
// same expression -- a normal square to the sun is 1, one square away is the floor. The normal is
// the smoothed vertex normal, interpolated across the triangle and renormalised, which is what
// makes a faceted hull read as a curved surface. A vertex with no normal (all zero: the sky, the
// HUD, the particles) is not lit and keeps its colour exactly, so nothing but the lit world changes.
float surface_light() {
    float length_squared = dot(v_normal, v_normal);
    if (length_squared <= 0.0) {
        return 1.0;
    }
    float facing = clamp(dot(v_normal * inversesqrt(length_squared), u_sun_direction), -1.0, 1.0);
    return u_shade_floor + ((1.0 - u_shade_floor) * (0.5 + (0.5 * facing)));
}

void main() {
    vec3 surface = texture(u_texture, v_uv).rgb * v_colour * surface_light();
    out_colour = vec4(mix(surface, u_fog_colour, v_fog), v_alpha);
}
