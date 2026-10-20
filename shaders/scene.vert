#version 450
// One vertex format for the whole frame: world position and a flat colour. The depth buffer does
// the work, not the submission order -- the unit's triangles are submitted FIRST on purpose, so a
// frame in which it is visible proves the depth test ran.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_colour;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec3 v_colour;
layout(location = 1) out vec2 v_uv;

layout(push_constant) uniform Push {
    mat4 view_proj;
} push;

void main() {
    v_colour = in_colour;
    v_uv = in_uv;
    gl_Position = push.view_proj * vec4(in_position, 1.0);
}
