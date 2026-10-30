#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "protekt/game/scene.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/fixed.hpp"
#include "protekt/sim/units.hpp"

/// A VEHICLE'S BODY, LEVEL AGAIN. The scene turns every vehicle so its up follows the ground, and
/// most of the shape tests were written to read a body's design -- which way the barrel points, how
/// high the deck is, what colour the roof panel is -- off terrain that happens to be sloped. They
/// are about the design, not the lean, so they read it through this: the inverse of the scene's
/// turn, about the same point on the ground, by the same `ground_normal`. A body that was never
/// tilted (a structure, or level ground) comes back unchanged, to the bit.
namespace protekt::game::untilt {

/// `body` (one vehicle's vertices, as `build_scene` drew them) turned back to level.
inline void level(std::span<SceneVertex> body, const sim::Unit& unit, std::uint64_t seed) noexcept {
    if (stands_level(unit.kind)) {
        return; // never turned, so nothing to undo
    }
    const math::Vec3 normal = ground_normal(seed, unit.x, unit.y);
    const float sine = std::hypot(normal[0], normal[2]);
    if (sine == 0.0F) {
        return;
    }
    const math::Vec3 axis{normal[2] / sine, 0.0F, -normal[0] / sine};
    const float cosine = normal[1];
    constexpr float kOne = static_cast<float>(sim::Fixed::kOne);
    const math::Vec3 pivot{static_cast<float>(unit.x.raw) / kOne * kMetresPerTile,
                           ground_metres(seed, unit.x, unit.y),
                           static_cast<float>(unit.y.raw) / kOne * kMetresPerTile};
    for (SceneVertex& vertex : body) {
        const math::Vec3 v{vertex.position[0] - pivot[0], vertex.position[1] - pivot[1],
                           vertex.position[2] - pivot[2]};
        const math::Vec3 swung = math::cross(axis, v);
        const float along = math::dot(axis, v) * (1.0F - cosine);
        for (std::size_t c = 0; c < 3; ++c) {
            vertex.position.at(c)
                = pivot.at(c) + (v.at(c) * cosine) - (swung.at(c) * sine) + (axis.at(c) * along);
        }
    }
}

} // namespace protekt::game::untilt
