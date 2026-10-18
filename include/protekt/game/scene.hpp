#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "protekt/math/math.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

/// The world as the player sees it: terrain under the camera, and a box per unit.
///
/// SCALE, FIXED HERE AND STATED EVERYWHERE: one tile is `kMetresPerTile` metres, and one height
/// unit is a 64th of a tile. A tank therefore crosses 6 tiles a second -- 12 m/s -- and the world
/// rises and falls by about 16 m. Every number below is in metres; the conversion happens once.
namespace protekt::game {

inline constexpr float kMetresPerTile = 2.0F;
inline constexpr float kMetresPerHeightUnit = kMetresPerTile / 64.0F;

/// Where the player's eye is, in world metres and radians. Yaw 0 looks down +Z.
struct Camera {
    float x{};
    float y{};
    float z{};
    float yaw{};
    float pitch{};
    [[nodiscard]] static float wrap_angle(float radians) noexcept;
};

/// Ground height in metres at a tile -- the ONE conversion from a tile's height units to world
/// metres. The terrain mesh and the picker both go through it, because if they disagreed about
/// where the ground is, a click would land somewhere the player cannot see.
[[nodiscard]] float height_metres(std::uint64_t seed, int tile_x, int tile_y) noexcept;

/// Column-major view-projection for a camera, matching what the vertex shader expects.
[[nodiscard]] std::array<float, 16> view_projection(const Camera& camera, float aspect,
                                                    float fov_radians = math::kFovRadians) noexcept;

/// The aim point, in framebuffer pixels: from the left, from the TOP.
///
/// One point, and everything reads it: the reticle marks it, a left click selects the unit under
/// it, a right click orders the ground under it. It is NOT the pointer -- `game::aim_from` says
/// where it is and why -- but it is the point the reticle is drawn at, so the mark can never
/// promise something a click does not do.
struct Aim {
    float x{};
    float y{};
};

/// A reticle is four bars, six vertices each, in a triangle list.
inline constexpr std::size_t kReticleVertexCount = 24;

struct SceneOptions {
    std::uint64_t seed{};
    Camera camera{};
    /// The frame this scene will be drawn into: the projection needs its aspect ratio.
    render::Extent extent{1280, 720};
    /// Terrain half-extent in tiles: the mesh is (2*radius+1)^2 tiles centred on the camera.
    int radius_tiles{24};
    /// Where the gun is pointed, when there is anything to point it with. ABSENT means no reticle
    /// and no picking at all, which is what a headless or `--autodrive` frame has: those frames
    /// therefore stay byte-identical to what they drew before there was anything to draw here.
    std::optional<Aim> aim{};
    /// Whose eyes this is, which is what colours the reticle: a unit that is not yours turns it
    /// red, because that is the one click that fires.
    sim::Side viewer_side{sim::Side::Player};
};

/// Triangles for the terrain mesh (two per tile, coloured by `Terrain`) followed by one box per
/// living unit (coloured by side, taller for a factory than for a tank). Pure: same options and
/// same sim state give byte-identical vertices.
/// `selected` units are drawn brighter than the rest of their side: the player needs to see what he
/// has hold of, and a tint is the smallest honest version of that before there is any text on
/// screen.
///
/// With `options.aim` set, the scene ends with a reticle: `kReticleVertexCount` more vertices,
/// marking what a click at that pixel would hit. White on the ground, red on a hostile (a click
/// there fires), brighter on one of yours (a click there selects). It is a WORLD-space mark placed
/// a short distance along the click's own ray, which is why it needs no font and cannot be hidden
/// by terrain.
[[nodiscard]] render::Scene build_scene(const sim::Sim& sim, const SceneOptions& options,
                                        std::span<const sim::UnitId> selected = {});

} // namespace protekt::game
