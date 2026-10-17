#pragma once

#include <cstdint>

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

/// Column-major view-projection for a camera, matching what the vertex shader expects.
[[nodiscard]] std::array<float, 16> view_projection(const Camera& camera, float aspect,
                                                    float fov_radians = 1.0472F) noexcept; // 60 deg

struct SceneOptions {
    std::uint64_t seed{};
    Camera camera{};
    /// The frame this scene will be drawn into: the projection needs its aspect ratio.
    render::Extent extent{1280, 720};
    /// Terrain half-extent in tiles: the mesh is (2*radius+1)^2 tiles centred on the camera.
    int radius_tiles{24};
};

/// Triangles for the terrain mesh (two per tile, coloured by `Terrain`) followed by one box per
/// living unit (coloured by side, taller for a factory than for a tank). Pure: same options and
/// same sim state give byte-identical vertices.
[[nodiscard]] render::Scene build_scene(const sim::Sim& sim, const SceneOptions& options);

} // namespace protekt::game
