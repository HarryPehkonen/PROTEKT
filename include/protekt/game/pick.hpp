#pragma once

#include <cstdint>

#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

/// Turning a click into a thing in the world.
///
/// Two questions, and a click answers both: did it land on a unit (a selection), and where on the
/// ground did it land (an order)? The answer is a value, not a side effect, so the whole of it is
/// testable without a window, a cursor or a GPU.
namespace protekt::game {

inline constexpr float kPickMaxDistanceTiles = 200.0F;
inline constexpr float kPickStepTiles = 0.25F;
inline constexpr float kPickUnitRadiusTiles = 0.75F;

struct Pick {
    /// The unit under the cursor, if any. `valid()` is false for a click on the ground or sky.
    sim::UnitId unit{};
    /// The ground tile the ray met, if it met one. -1/-1 when the ray went over the horizon.
    int tile_x{-1};
    int tile_y{-1};
    /// Distance along the ray to what was hit, in tiles. `kPickMaxDistanceTiles` when nothing was.
    float distance_tiles{kPickMaxDistanceTiles};

    [[nodiscard]] bool has_unit() const noexcept { return unit.valid(); }
    [[nodiscard]] bool has_tile() const noexcept { return tile_x >= 0 && tile_y >= 0; }
};

/// The projection the renderer uses, in one place. A world point in METRES relative to the camera.
/// Returns x and y in pixels (y from the top) and `depth` > 0 in front of the camera, or depth <= 0
/// behind it -- callers must check, because a projected "behind" point lands on screen too.
struct Projected {
    float x{};
    float y{};
    float depth{};
};

[[nodiscard]] Projected project(const Camera& camera, Extent extent, float world_x, float world_y,
                                float world_z) noexcept;

/// The world point a pixel looks at, `distance` metres along the ray. The inverse of `project` for
/// that distance: `project(ray_point(camera, e, px, py, d)) == (px, py)`.
struct RayPoint {
    float x{};
    float y{};
    float z{};
};
[[nodiscard]] RayPoint ray_point(const Camera& camera, Extent extent, float screen_x,
                                 float screen_y, float distance_metres) noexcept;

/// What is under the cursor. Units win over ground: a click on a tank is a selection, and the
/// ground behind it is not the player's intent.
///
/// A unit is hit when the click falls inside its projected box -- `kPickUnitRadiusTiles` either
/// side of its centre, the box standing on the tile it occupies -- and it is the nearest such unit
/// in front of the camera. Ties go to the lower unit id, so a click that hits two overlapping boxes
/// always picks the same one.
[[nodiscard]] Pick pick(const sim::Sim& sim, const SceneOptions& options, float screen_x,
                        float screen_y) noexcept;

} // namespace protekt::game
