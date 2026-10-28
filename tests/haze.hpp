#pragma once

#include <cmath>

#include "protekt/game/scene.hpp"

/// DISTANCE FOG, FROM THE TEST'S SIDE. Everything the world draws goes through `fogged` by its
/// distance from the eye, so a test that names an ink and finds or checks vertices by it must ask
/// what that ink looks like THROUGH THE HAZE at the vertex's own place. This is the same function
/// the scene uses, not a copy of its rule: the colour is still asserted exactly.
namespace protekt::game::haze {

/// `ink` as the scene draws it at `vertex`'s position, for a frame built from `options`.
[[nodiscard]] inline math::Vec3 seen(const SceneOptions& options, const SceneVertex& vertex,
                                     const math::Vec3& ink) noexcept {
    const float dx = vertex.position[0] - options.camera.x;
    const float dy = vertex.position[1] - options.camera.y;
    const float dz = vertex.position[2] - options.camera.z;
    return fogged(ink, std::sqrt((dx * dx) + (dy * dy) + (dz * dz)),
                  fog_far_metres(options.radius_tiles));
}

/// Is `vertex` drawn in `ink`, once the haze is allowed for?
[[nodiscard]] inline bool is_ink(const SceneOptions& options, const SceneVertex& vertex,
                                 const math::Vec3& ink) noexcept {
    return vertex.colour == seen(options, vertex, ink);
}

} // namespace protekt::game::haze
