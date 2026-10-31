#pragma once

#include <cmath>

#include "protekt/game/scene.hpp"

/// DISTANCE FOG, FROM THE TEST'S SIDE. The scene stores each world vertex's RAW ink in `colour` and
/// the fog as a separate AMOUNT in `fog`; the fragment shader mixes the fog colour in last. A test
/// that names an ink therefore reads the colour exactly as written and checks the amount against
/// the vertex's distance from the eye -- so a scene that wrote the wrong amount fails here, not
/// only in a rendered frame. Untextured geometry samples the all-white texture (x1), so the
/// comparison of ink stays EXACT; there is no tolerance in this file.
namespace protekt::game::haze {

/// The fog amount the scene should have written for `vertex`, from its distance to the eye.
[[nodiscard]] inline float expected_fog(const SceneOptions& options,
                                        const SceneVertex& vertex) noexcept {
    const float dx = vertex.position[0] - options.camera.x;
    const float dy = vertex.position[1] - options.camera.y;
    const float dz = vertex.position[2] - options.camera.z;
    return fog_amount(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)),
                      fog_far_metres(options.radius_tiles));
}

/// Did the scene write the amount its distance asks for?
[[nodiscard]] inline bool fog_is_right(const SceneOptions& options,
                                       const SceneVertex& vertex) noexcept {
    return vertex.fog == expected_fog(options, vertex);
}

/// `ink` as the frame finally draws it at `vertex`: mixed toward the horizon colour by the
/// vertex's OWN `fog` amount (what the scene wrote, which is the thing under test), as the shader
/// does after the texture.
[[nodiscard]] inline math::Vec3 seen(const SceneVertex& vertex, const math::Vec3& ink) noexcept {
    const math::Vec3& fog = kSkyRingColours.at(kSkyHorizonRing);
    if (vertex.fog <= 0.0F) {
        return ink;
    }
    if (vertex.fog >= 1.0F) {
        return fog;
    }
    return math::Vec3{ink[0] + ((fog[0] - ink[0]) * vertex.fog),
                      ink[1] + ((fog[1] - ink[1]) * vertex.fog),
                      ink[2] + ((fog[2] - ink[2]) * vertex.fog)};
}

/// Is `vertex` drawn in `ink`, haze allowed for? The raw colour is the ink exactly, AND the fog
/// amount is the one its distance asks for -- so the drawn colour, `seen(vertex, ink)`, is right.
[[nodiscard]] inline bool is_ink(const SceneOptions& options, const SceneVertex& vertex,
                                 const math::Vec3& ink) noexcept {
    return vertex.colour == ink && fog_is_right(options, vertex);
}

} // namespace protekt::game::haze
