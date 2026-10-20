#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>

#include "protekt/game/scene_data.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/observer.hpp"
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

/// THE SUN. Where the light comes FROM, as a world-space direction (x east, y up, z north): high
/// overhead and a little toward -x / -z, so a unit's top face is brightest, the faces turned toward
/// -x and -z come next, the faces turned away are darker, and the underside is darkest. Fixed data,
/// not a function of the camera or the clock: the same scene bytes every frame. Used only to shade
/// the prisms that make units (`emit_turned_prism`); terrain, bolts, radar and reticle keep their
/// flat colours.
inline constexpr math::Vec3 kSunDirection{-0.35F, 0.85F, -0.40F};

/// THE SKY. A dome centred on the camera every frame and fixed in ORIENTATION (north stays north
/// as you turn), built from raw vertex colours: it is the light source, so it never goes through
/// the shaded prism emitter. `kSkyRings` rings of `kSkySegments` vertices climb from the horizon
/// (elevation 0, the lowest the dome goes) to the zenith; the lower bands are quads, the top
/// band a fan of triangles to the zenith point. Then comes the SUN DISC: `kSunDiscSegments`
/// triangles in the plane perpendicular to `kSunDirection`, centred exactly on it, a hair inside
/// the dome so it wins the depth test.
inline constexpr int kSkySegments = 16;
inline constexpr float kSkyRadiusMetres = 1000.0F; ///< well inside the 2000 m far plane
inline constexpr std::size_t kSkyRings = 11;
inline constexpr std::size_t kSkyBandVertexCount = std::size_t{kSkySegments} * 6;
inline constexpr std::size_t kSkyCapVertexCount = std::size_t{kSkySegments} * 3;
inline constexpr std::size_t kSunDiscVertexCount = std::size_t{kSkySegments} * 3;
inline constexpr std::size_t kSkyVertexCount
    = ((kSkyRings - 2) * kSkyBandVertexCount) + kSkyCapVertexCount + kSunDiscVertexCount;
/// Elevation of each ring in degrees, horizon first, and its colour. Rings are packed toward the
/// horizon (never more than 6 degrees apart below 30) so the gradient has no visible banding. The
/// colours blend the original five anchors (0, 12, 30, 60, 90 degrees) linearly by elevation; the
/// renderer's flat clear colour (0.53, 0.81, 0.92) is exactly the 30-degree ring.
inline constexpr std::size_t kSkyThirtyDegreeRing = 5;
inline constexpr std::array<float, kSkyRings> kSkyRingElevationDegrees{
    0.0F, 6.0F, 12.0F, 18.0F, 24.0F, 30.0F, 40.0F, 52.0F, 64.0F, 78.0F, 90.0F};
inline constexpr std::array<math::Vec3, kSkyRings> kSkyRingColours{{{0.8000F, 0.9000F, 0.9500F},
                                                                    {0.7300F, 0.8750F, 0.9450F},
                                                                    {0.6600F, 0.8500F, 0.9400F},
                                                                    {0.6167F, 0.8367F, 0.9333F},
                                                                    {0.5733F, 0.8233F, 0.9267F},
                                                                    {0.5300F, 0.8100F, 0.9200F},
                                                                    {0.4800F, 0.7567F, 0.9067F},
                                                                    {0.4200F, 0.6927F, 0.8907F},
                                                                    {0.3627F, 0.6300F, 0.8720F},
                                                                    {0.3020F, 0.5600F, 0.8440F},
                                                                    {0.2500F, 0.5000F, 0.8200F}}};
inline constexpr math::Vec3 kSunDiscColour{1.0F, 0.97F, 0.80F};

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
/// Presentation only -- no UNIT has a z and none gains one (a bolt does now, since pitch bends it;
/// this is about units). All rates are per SIM TICK (30 a
/// second), never per frame, so the hop does not depend on the display's refresh rate.
///
/// A stationary tank floats this far above the ground, in metres.
inline constexpr float kRideHeightMetres = 0.25F;
/// BOOST. While the boost key is held the spring's target is this much higher, so the hull rises,
/// overshoots once and settles at kRideHeightMetres + this; released, it settles back. 0.9 m is
/// half the 1.8 m hull: the daylight under it grows from 0.25 m to 1.15 m, plainly more than a bump
/// in the ground could do, yet the hull is still less than a hull-and-a-bit off the ground rather
/// than flying. Presentation only: no unit has a z, so boost changes no order, no tick and no
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

/// THE HIT FLASH. How long the red wash takes to fade after the player's tank is hit.
inline constexpr float kHitFlashSeconds = 0.4F;
/// The same, in sim ticks: the flash is clocked by the sim's own counter, as the hover is.
inline constexpr sim::Tick kHitFlashTicks
    = static_cast<sim::Tick>(kHitFlashSeconds * static_cast<float>(sim::kTicksPerSecond));
/// How far toward deep red the frame goes at the instant of a hit: a wash, not an overlay, so the
/// battle stays readable underneath.
inline constexpr float kHitFlashStrength = 0.55F;

/// The red flash's state, owned by the app. `hit` arms it from the top; `level` is 1 on the tick of
/// the hit, falls linearly, and is exactly 0 from `kHitFlashTicks` later on. Driven by hit EVENTS,
/// so two hits in a row flash twice even if hit points never read differently between frames.
class HitFlash {
public:
    void hit(sim::Tick now) noexcept;
    [[nodiscard]] float level(sim::Tick now) const noexcept;

private:
    sim::Tick hit_at_{};
    bool armed_{false};
};

/// True when one of `events` is a `HitTaken` whose victim is `unit` -- a bolt landed on it.
[[nodiscard]] bool unit_was_hit(std::span<const sim::BattleEvent> events,
                                sim::UnitId unit) noexcept;

/// Column-major view-projection for a camera, matching what the vertex shader expects.
[[nodiscard]] std::array<float, 16> view_projection(const Camera& camera, float aspect,
                                                    float fov_radians = math::kFovRadians) noexcept;

/// The aim point, in framebuffer pixels: from the left, from the TOP.
///
/// One point, and everything reads it: the reticle marks it, a left click FIRES at whatever is
/// under it, a right click orders the ground under it, and `Space` selects an own unit there. It
/// is NOT the pointer -- `game::aim_from` says where it is and why -- but it is the point the
/// reticle is drawn at, so the mark can never promise something a click does not do.
struct Aim {
    float x{};
    float y{};
};

/// WHICH WAY A TANK FACES. The sim gives a unit no heading, only a velocity, so the presentation
/// layer works it out and remembers it. CONVENTION: a heading is a yaw in radians, the camera's own
/// -- 0 is +Y on the map (the world's forward, +Z in metres) and a positive heading turns toward
/// +X, so a unit moving along `(vx, vy)` faces `atan2(vx, vy)`. A tank that is moving faces its
/// velocity; a STOPPED tank keeps the heading it last had; a unit never seen moving faces 0.
/// `observe` is called once per frame with the sim: it records every moving unit's heading and
/// drops the entries of units that are gone or dead. Keyed by unit id in an ordered map, so the
/// result is deterministic. Presentation only: nothing here touches the sim.
class Headings {
public:
    /// The heading of a unit's velocity, or ABSENT when it is standing still.
    [[nodiscard]] static std::optional<float> heading_of(const sim::Unit& unit) noexcept;
    void observe(const sim::Sim& sim);
    /// Current velocity heading if moving, else the remembered one, else 0.
    [[nodiscard]] float of(const sim::Unit& unit) const;

private:
    std::map<std::uint32_t, float> last_;
};

/// A reticle is four bars, six vertices each, in a triangle list.
inline constexpr std::size_t kReticleVertexCount = 24;

/// The health bar under the crosshair is three quads, six vertices each: a light border, a dark
/// track and the fill. It sits just before the reticle in the vertex list, so the reticle stays the
/// tail, and it is present exactly when a unit is under the crosshair.
inline constexpr std::size_t kAimedBarVertexCount = 18;

struct SceneOptions {
    std::uint64_t seed{};
    Camera camera{};
    /// The frame this scene will be drawn into: the projection needs its aspect ratio.
    Extent extent{1280, 720};
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
    /// How the viewer's own tank is oriented, so the drawn tank shows the steering model the player
    /// is using: `heading` is which way the body points and `turret` is how far the turret is
    /// turned relative to it, both in radians. ABSENT draws every unit as an axis-aligned box, so a
    /// frame without one is byte-identical to a frame that never heard of this field.
    struct Facing {
        sim::UnitId unit{};
        float heading{};
        float turret{};
    };
    std::optional<Facing> facing{};
    /// Remembered headings, so a stopped tank keeps its facing. ABSENT (null) faces a stopped tank
    /// along +Y; a moving one still faces its velocity. Not owned.
    const Headings* headings{nullptr};
    /// The red hit flash, 0 (none) to 1 (just hit): the app owns it and advances it each frame.
    /// 0 draws nothing at all, so a frame that is not flashing is byte-identical to one that never
    /// heard of it.
    float hit_flash{0.0F};
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
[[nodiscard]] Scene build_scene(const sim::Sim& sim, const SceneOptions& options,
                                std::span<const sim::UnitId> selected = {});

/// The colour calibration view: three full-height vertical bars, left to right pure red, pure
/// green, pure blue, spanning the whole frame width and nothing else (no terrain, units, reticle or
/// radar). Drawn on the same near-plane HUD layer as the radar, through the same vertex format, so
/// what it shows is what the game frame goes through on its way to the screen. Pure in `options`
/// (camera and extent); it reads no sim, so showing it cannot disturb one.
[[nodiscard]] Scene build_calibration_scene(const SceneOptions& options);

} // namespace protekt::game
