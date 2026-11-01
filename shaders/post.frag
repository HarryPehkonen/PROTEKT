#version 450
#extension GL_EXT_samplerless_texture_functions : require
// FXAA-style edge smoothing over the scene colour, by integer texel fetches only.
//
// No sampler is bound and nothing is filtered: every read is `texelFetch`, so the result is a
// function of the input texels alone and two renders of one request stay byte-identical (the
// determinism contract the scene's NEAREST sampler keeps for the same reason). The classic
// algorithm blends by sampling the bilinear midpoint between two texels; a sample taken on the line
// between two texel centres is a linear mix of those two texels, so `mix` of two fetches is the
// same blend without a filter in the loop.
//
// Per pixel: find the local luma contrast; below the threshold the pixel is returned untouched.
// Otherwise find which way the edge runs, walk along it both ways to where it ends, and blend the
// pixel toward its neighbour across the edge by how far along the edge it sits (the further from an
// end, the less); a sub-pixel term also softens isolated high-contrast pixels.
layout(set = 0, binding = 0) uniform texture2D u_scene;
layout(location = 0) out vec4 out_colour;

// The tuning lives in ONE place, `kFxaaSettings` in src/render/renderer.cpp, and arrives here.
// `enabled` is 0 for --no-fxaa, where this pass is an exact copy.
layout(push_constant) uniform Fxaa {
    float edge_threshold;
    float edge_threshold_min;
    float subpixel_blend;
    uint enabled;
} fxaa;

// How far along an edge the search walks each way, in pixels. A longer edge than this is blended
// as if it ended here, which only makes a very long shallow edge slightly less smooth.
const int kEdgeSearchSteps = 12;

ivec2 g_size;

vec4 fetch(ivec2 p) {
    return texelFetch(u_scene, clamp(p, ivec2(0), g_size - 1), 0);
}

float luma_of(vec4 c) {
    return dot(c.rgb, vec3(0.299, 0.587, 0.114));
}

float luma_at(ivec2 p) {
    return luma_of(fetch(p));
}

void main() {
    g_size = textureSize(u_scene, 0);
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const vec4 centre = fetch(p);
    if (fxaa.enabled == 0u) {
        out_colour = centre;
        return;
    }

    const float m = luma_of(centre);
    const float n = luma_at(p + ivec2(0, -1));
    const float s = luma_at(p + ivec2(0, 1));
    const float e = luma_at(p + ivec2(1, 0));
    const float w = luma_at(p + ivec2(-1, 0));
    const float highest = max(max(max(n, s), max(e, w)), m);
    const float lowest = min(min(min(n, s), min(e, w)), m);
    const float contrast = highest - lowest;
    if (contrast < max(fxaa.edge_threshold_min, fxaa.edge_threshold * highest)) {
        out_colour = centre;
        return;
    }

    const float ne = luma_at(p + ivec2(1, -1));
    const float nw = luma_at(p + ivec2(-1, -1));
    const float se = luma_at(p + ivec2(1, 1));
    const float sw = luma_at(p + ivec2(-1, 1));

    // The sub-pixel term: how far the pixel sits from the average of its neighbourhood.
    float around = 2.0 * (n + s + e + w) + ne + nw + se + sw;
    around = clamp(abs(around / 12.0 - m) / contrast, 0.0, 1.0);
    const float smoothed = smoothstep(0.0, 1.0, around);
    const float pixel_blend = smoothed * smoothed * fxaa.subpixel_blend;

    // Which way the edge runs: compare the second differences across each axis.
    const float horizontal = 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e)
                             + abs(nw + sw - 2.0 * w);
    const float vertical = 2.0 * abs(e + w - 2.0 * m) + abs(ne + nw - 2.0 * n)
                           + abs(se + sw - 2.0 * s);
    const bool is_horizontal = horizontal >= vertical;

    // Across the edge is the axis the edge does NOT run along. Take the steeper side as "opposite".
    const ivec2 across = is_horizontal ? ivec2(0, 1) : ivec2(1, 0);
    const ivec2 along = is_horizontal ? ivec2(1, 0) : ivec2(0, 1);
    const float positive = is_horizontal ? s : e;
    const float negative = is_horizontal ? n : w;
    const bool positive_steeper = abs(positive - m) >= abs(negative - m);
    const ivec2 toward = positive_steeper ? across : -across;
    const float opposite = positive_steeper ? positive : negative;
    const float gradient = abs(opposite - m);
    const float edge_luma = 0.5 * (m + opposite);

    // Walk each way along the edge until the luma across it stops matching the edge's own average.
    float distance_positive = float(kEdgeSearchSteps);
    float delta_positive = 0.0;
    for (int i = 1; i <= kEdgeSearchSteps; ++i) {
        const ivec2 at = p + along * i;
        delta_positive = 0.5 * (luma_at(at) + luma_at(at + toward)) - edge_luma;
        if (abs(delta_positive) >= 0.25 * gradient) {
            distance_positive = float(i);
            break;
        }
    }
    float distance_negative = float(kEdgeSearchSteps);
    float delta_negative = 0.0;
    for (int i = 1; i <= kEdgeSearchSteps; ++i) {
        const ivec2 at = p - along * i;
        delta_negative = 0.5 * (luma_at(at) + luma_at(at + toward)) - edge_luma;
        if (abs(delta_negative) >= 0.25 * gradient) {
            distance_negative = float(i);
            break;
        }
    }

    // Only blend if the near end of the edge bends the right way: the pixel must lie on the side of
    // the edge that the end's luma moved away from, or the blend would pull the wrong colour in.
    const bool near_is_positive = distance_positive <= distance_negative;
    const float shortest = near_is_positive ? distance_positive : distance_negative;
    const float end_delta = near_is_positive ? delta_positive : delta_negative;
    float edge_blend = 0.0;
    if ((end_delta < 0.0) != (m - edge_luma < 0.0)) {
        edge_blend = 0.5 - shortest / (distance_positive + distance_negative);
    }

    out_colour = mix(centre, fetch(p + toward), max(pixel_blend, edge_blend));
}
