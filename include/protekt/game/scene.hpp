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

/// Ground height in metres at a continuous position given in TILE units (a unit's `x`/`y`), the
/// bilinear blend of the four tile heights around it. The mesh puts tile (tx, ty)'s height at the
/// lattice point (tx, ty), so this is exactly the surface the mesh draws along its tile edges:
/// at an integer position it equals `height_metres` for that tile, and it has no jump anywhere.
/// For things that MOVE (a unit's footing, the eye, the aim ray). Pure in `(seed, x, y)`.
[[nodiscard]] float ground_metres(std::uint64_t seed, sim::Fixed x, sim::Fixed y) noexcept;
[[nodiscard]] float ground_metres(std::uint64_t seed, float tile_x, float tile_y) noexcept;

/// THE HOVER. A hover tank does not weld itself to the ground: its ride height follows the terrain
/// through a spring and damper, so a rise under it makes the hull lag, overshoot once, and settle.
/// Presentation only -- the simulation has no z and gains none. All rates are per SIM TICK (30 a
/// second), never per frame, so the hop does not depend on the display's refresh rate.
///
/// A stationary tank floats this far above the ground, in metres.
inline constexpr float kRideHeightMetres = 0.25F;
/// BOOST. While the boost key is held the spring's target is this much higher, so the hull rises,
/// overshoots once and settles at kRideHeightMetres + this; released, it settles back. 0.9 m is
/// half the 1.8 m hull: the daylight under it grows from 0.25 m to 1.15 m, plainly more than a bump
/// in the ground could do, yet the hull is still less than a hull-and-a-bit off the ground rather
/// than flying. Presentation only: the sim has no z, so boost changes no order, no tick and no
/// hash.
inline constexpr float kBoostLiftMetres = 0.9F;
/// Spring stiffness, in (metres/tick^2) per metre of error. Natural frequency sqrt(k) = 0.2
/// radians per tick, a period of about 31 ticks -- one second -- for the hull to swing once.
inline constexpr float kSpringPerTick = 0.04F;
/// Damping, in (metres/tick) per (metre/tick) of velocity. Damping ratio d / (2 sqrt(k)) = 0.625:
/// UNDERDAMPED, so one overshoot of about 8% of the step, then settled within a second or two.
inline constexpr float kDampPerTick = 0.25F;

/// The hull's height and vertical speed, in metres and metres per tick.
struct Suspension {
    float z{};
    float z_vel{};
};

/// `ticks` steps of the damped spring toward `target_z` (held constant across them), one tick at a
/// time. Pure in its arguments, and two calls of one tick equal one call of two, bit for bit.
[[nodiscard]] Suspension suspend(Suspension state, float target_z, std::uint32_t ticks) noexcept;

/// The suspension, clocked by the sim's tick counter the way the helm is. `advance` is told the
/// ground under the tank and the tick the sim is now on; it steps once per tick that has passed
/// since the last call, so a frame with two ticks lands where two frames of one tick would. The
/// first call puts the hull at rest at ride height rather than dropping it from nowhere.
class Hover {
public:
    /// `boost` raises the target by kBoostLiftMetres for every tick this call steps. Defaulted off,
    /// so a caller that never asks for it gets exactly the hover it always had.
    void advance(sim::Tick tick, float ground_metres, bool boost = false) noexcept;
    /// The hull's base height in metres, or ABSENT before the first `advance`.
    [[nodiscard]] std::optional<float> z() const noexcept;

private:
    Suspension state_{};
    sim::Tick last_tick_{};
    bool clocked_{false};
};

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
    /// The tank the radar is centred on. ABSENT means no radar, which is what a frame with no
    /// player unit in it gets: a radar with no centre would be a lie about where the tank is.
    sim::UnitId viewer_unit{};
    /// A unit whose hull is drawn at `z` metres instead of on the ground: the hovering tank.
    /// ABSENT draws every unit on the ground, so a frame without one is unchanged.
    struct Hovering {
        sim::UnitId unit{};
        float z{};
    };
    std::optional<Hovering> hover{};
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

/// The colour calibration view: three full-height vertical bars, left to right pure red, pure
/// green, pure blue, spanning the whole frame width and nothing else (no terrain, units, reticle or
/// radar). Drawn on the same near-plane HUD layer as the radar, through the same vertex format, so
/// what it shows is what the game frame goes through on its way to the screen. Pure in `options`
/// (camera and extent); it reads no sim, so showing it cannot disturb one.
[[nodiscard]] render::Scene build_calibration_scene(const SceneOptions& options);

} // namespace protekt::game
