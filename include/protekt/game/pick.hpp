#pragma once

#include <array>
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

/// How tall a unit's pick box is, in metres: a factory stands taller than a vehicle. The picker's
/// box and `attack_point`'s middle both read this, so they are the same unit's same body.
[[nodiscard]] float unit_box_height_metres(sim::UnitKind kind) noexcept;

/// THE AVAILABILITY RULE (VIS), the one place it lives. A unit is available to the player -- to
/// the pick, the reticle's mark, Space and the attack list -- only if the world view would DRAW it:
/// inside the camera's drawn radius (`options.radius_tiles`, a box about the camera's tile, the one
/// every draw path culls at) AND not hidden by the viewer's radar reach. The scene's draw paths and
/// the interaction both ask these, so they cannot drift apart.
///
/// A HOSTILE farther from the viewer than `viewer_radar_radius` is hidden; your own side never is,
/// wherever it stands. The boundary is INSIDE (`<=`), in the same float arithmetic as
/// `radar_marks`. With no viewer unit in `options` (an observer frame) there is no radar to be
/// limited by, and nothing is hidden. Pure functions of `(sim, options, unit)`.
[[nodiscard]] bool hidden_by_reach(const sim::Sim& sim, const SceneOptions& options,
                                   const sim::Unit& unit);
/// True when the unit stands outside the meshed box about the camera's tile: no ground under it.
[[nodiscard]] bool outside_drawn_radius(const SceneOptions& options,
                                        const sim::Unit& unit) noexcept;
/// Both conditions: alive is the caller's business, this is only about where the unit stands.
[[nodiscard]] bool available_to_viewer(const sim::Sim& sim, const SceneOptions& options,
                                       const sim::Unit& unit);

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

/// WHERE THE SHOT GOES so that it lands on the mark. The camera's ray through the aim point is what
/// the reticle marks, but the bolt leaves the shooter's MUZZLE, a couple of metres below the eye:
/// fired along the camera's own direction it runs parallel to the mark and passes under it by that
/// drop at any range (the "I have to aim higher" error). So the shot is aimed from the muzzle at
/// the point the mark's ray reaches -- `picked.distance_tiles` along it, which is the unit, the
/// ground or, over the horizon, the far end of the pick -- and crosses the mark exactly there.
/// A unit vector in world metres: x, UP, z. The camera's own forward when that point is not in
/// front of the muzzle. Presentation only; the sim is told the direction and nothing else.
[[nodiscard]] std::array<float, 3> shot_direction(const sim::Unit& shooter,
                                                  const SceneOptions& options, const Pick& picked,
                                                  Aim aim) noexcept;

} // namespace protekt::game
