#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/game/scene_data.hpp"
#include "protekt/game/weapon.hpp"
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
/// the shaded prism emitter. `kSkyRings` rings of `kSkySegments` vertices climb from the SKIRT
/// (elevation -30, all haze) through the horizon to the zenith; the lower bands are quads, the top
/// band a fan of triangles to the zenith point. Then comes the SUN DISC: `kSunDiscSegments`
/// triangles in the plane perpendicular to `kSunDirection`, centred exactly on it, a hair inside
/// the dome so it wins the depth test.
/// The skirt (rings at -30, -20, -10 and 0 degrees, all the haze colour) closes the gap between
/// the horizon and the terrain silhouette, which the renderer's clear colour used to show through.
inline constexpr int kSkySegments = 16;
inline constexpr float kSkyRadiusMetres = 1000.0F; ///< well inside the 2000 m far plane
inline constexpr std::size_t kSkyRings = 14;
inline constexpr std::size_t kSkyBandVertexCount = std::size_t{kSkySegments} * 6;
inline constexpr std::size_t kSkyCapVertexCount = std::size_t{kSkySegments} * 3;
inline constexpr std::size_t kSunDiscVertexCount = std::size_t{kSkySegments} * 3;
inline constexpr std::size_t kSkyVertexCount
    = ((kSkyRings - 2) * kSkyBandVertexCount) + kSkyCapVertexCount + kSunDiscVertexCount;
/// Elevation of each ring in degrees, horizon first, and its colour. Rings are packed toward the
/// horizon (never more than 6 degrees apart below 30) so the gradient has no visible banding. The
/// colours blend the original five anchors (0, 12, 30, 60, 90 degrees) linearly by elevation; the
/// renderer's flat clear colour (0.53, 0.81, 0.92) is exactly the 30-degree ring.
inline constexpr std::size_t kSkyThirtyDegreeRing = 8;
inline constexpr std::size_t kSkyHorizonRing = 3; ///< the first ring at elevation 0
inline constexpr std::array<float, kSkyRings> kSkyRingElevationDegrees{
    -30.0F, -20.0F, -10.0F, 0.0F,  6.0F,  12.0F, 18.0F,
    24.0F,  30.0F,  40.0F,  52.0F, 64.0F, 78.0F, 90.0F};
inline constexpr std::array<math::Vec3, kSkyRings> kSkyRingColours{{{0.8000F, 0.9000F, 0.9500F},
                                                                    {0.8000F, 0.9000F, 0.9500F},
                                                                    {0.8000F, 0.9000F, 0.9500F},
                                                                    {0.8000F, 0.9000F, 0.9500F},
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

/// THE GROUND'S PAINT. A terrain quad samples one whole atlas tile, and WHICH tile is a pure
/// function of the world tile's `Terrain` FAMILY, then of its variant below (D6). The tiles carry
/// their own colour, so the terrain's vertex colour is `kTerrainLight`, white, the multiplicative
/// identity: what shows is the tile.
[[nodiscard]] constexpr std::string_view terrain_tile_name(world::Terrain terrain) noexcept {
    switch (terrain) {
    case world::Terrain::Flat:
        return "terrain_grass";
    case world::Terrain::Rough:
        return "terrain_dirt";
    case world::Terrain::Ridge:
        return "terrain_rock";
    case world::Terrain::Crater:
        return "terrain_crater";
    }
    return "terrain_grass";
}

/// THE GROUND'S VARIANTS (D6). The type picks the FAMILY above; inside it, each world tile paints
/// from one of `kGroundVariantCount` variant tiles -- the family's pattern redrawn from other hash
/// seeds in the same tones -- so a field of one type is not a grid of identical stamps. Which
/// variant is a pure function of (type, tile_x, tile_y): no world seed, no state, so a tile keeps
/// its variant forever and the same scene comes out byte-identical.
inline constexpr int kGroundVariantCount = 3;
inline constexpr std::array<std::array<std::string_view, kGroundVariantCount>, world::kTerrainCount>
    kGroundVariantNames{{{"terrain_grass_a", "terrain_grass_b", "terrain_grass_c"},
                         {"terrain_dirt_a", "terrain_dirt_b", "terrain_dirt_c"},
                         {"terrain_rock_a", "terrain_rock_b", "terrain_rock_c"},
                         {"terrain_crater_a", "terrain_crater_b", "terrain_crater_c"}}};

/// The variant of world tile (tile_x, tile_y) of type `terrain`, in [0, kGroundVariantCount):
/// splitmix64's finaliser over the packed (type, x, y), modulo the count.
[[nodiscard]] constexpr int variant_index(world::Terrain terrain, int tile_x, int tile_y) noexcept {
    std::uint64_t z = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tile_x)) << 32U)
                      | static_cast<std::uint32_t>(tile_y);
    z ^= (static_cast<std::uint64_t>(terrain) + 1U) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<int>(z % static_cast<std::uint64_t>(kGroundVariantCount));
}

/// The manifest name of `terrain`'s variant `variant`; an out-of-range variant gets the first.
[[nodiscard]] constexpr std::string_view terrain_variant_name(world::Terrain terrain,
                                                              int variant) noexcept {
    const auto type = static_cast<std::size_t>(terrain);
    const auto at = variant >= 0 && variant < kGroundVariantCount
                        ? static_cast<std::size_t>(variant)
                        : std::size_t{0};
    return type < kGroundVariantNames.size() ? kGroundVariantNames.at(type).at(at)
                                             : kGroundVariantNames.front().front();
}
inline constexpr math::Vec3 kTerrainLight{1.0F, 1.0F, 1.0F};

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

/// THE HIT MARK. How long the reticle's pulse takes to fade after a shot of the DRIVEN unit
/// damages an enemy: 6 ticks, 0.2 s.
inline constexpr sim::Tick kHitMarkTicks = 6;

/// The reticle pulse's state, owned by the app, the same shape as `HitFlash`: `hit` arms it, and
/// `level` is 1 on the tick of the hit, falls linearly, and is exactly 0 from `kHitMarkTicks`
/// later on. Getting hit never arms it -- that is `HitFlash`'s job.
class HitMark {
public:
    void hit(sim::Tick now) noexcept;
    [[nodiscard]] float level(sim::Tick now) const noexcept;

private:
    sim::Tick hit_at_{};
    bool armed_{false};
};

/// THE LANDING MARKER (M5). Where the player's own lob touched down, drawn for
/// `kLandingMarkHoldTicks` at full strength and then shrinking away over `kLandingMarkFadeTicks`
/// more, so a shell that lands on empty ground still tells the player where. Fed from the sim's own
/// `Landing` record, never re-derived. The app owns it, like `HitMark`.
/// The hold is 2 s (M5c): a lob at the top of the ladder is in the air for 52 ticks, so a player
/// who fires at the sky and looks back down still has a second to read where it came down.
inline constexpr sim::Tick kLandingMarkHoldTicks = 120;
inline constexpr sim::Tick kLandingMarkFadeTicks = 20;

/// Vertices in one marker: a bracketed cross standing upright at the touchdown tile -- the X's two
/// bars and four corner brackets of two bars each, ten quads of six vertices.
inline constexpr std::size_t kLandingMarkVertexCount = 60;
/// It stands 4 m tall -- more than the eye's height over the ground -- so the middle of it is in
/// the frame at any view pitch a shell can be fired from, however close it lands.
inline constexpr float kLandingMarkHeight = 4.0F;

class NavMarks;

class LandingMarks {
public:
    struct Mark {
        std::int32_t tile_x{};
        std::int32_t tile_y{};
        float level{}; ///< 1 while held, falling linearly to 0 across the fade
    };

    void land(sim::Tick landed_at, std::int32_t tile_x, std::int32_t tile_y);
    /// The marks still showing at `now`, oldest first; exactly the ones with `level` > 0.
    [[nodiscard]] std::vector<Mark> active(sim::Tick now) const;

private:
    struct Entry {
        sim::Tick at{};
        std::int32_t tile_x{};
        std::int32_t tile_y{};
    };
    std::vector<Entry> entries_{};
};

/// True when one of `events` is a `HitTaken` whose bolt came from `shooter` and landed on a unit
/// of the other side, looked up in `units` (the table as it stood before the step). A bolt that
/// hurts the shooter's own side, or one fired by someone else, is not a connection.
[[nodiscard]] bool shooter_connected(std::span<const sim::BattleEvent> events, sim::UnitId shooter,
                                     std::span<const sim::Unit> units) noexcept;

/// True when one of `events` is a `HitTaken` whose victim is `unit` -- a bolt landed on it.
[[nodiscard]] bool unit_was_hit(std::span<const sim::BattleEvent> events,
                                sim::UnitId unit) noexcept;

/// WHICH CAMO A TANK WEARS: an index into its team's row of `kTeamHullNames`, always in
/// [0, kHullVariantCount).
/// A pure hash of (`seed`, `unit_id`) -- the standing determinism rule -- so a tank wears the same
/// pattern on every run, and neighbouring ids spread across all three.
[[nodiscard]] int hull_variant(std::uint64_t seed, std::uint32_t unit_id) noexcept;

/// How far a tank's CAMO parts' vertex colour -- hull, turret and bustle -- is mixed from its
/// (marked) side colour toward white. Their tile already carries the team's camo palette; a
/// saturated side colour multiplied over it would crush the pattern back into one hue, so they take
/// only a pale tint of it and the tile's own tones dominate. The roof panel and the barrel keep the
/// full side colour: they are the team-identity accents.
inline constexpr float kHullPaleMix = 0.85F;

/// The hover skirt's colour: a hot pale cyan, RAW -- never lit, never marked -- so the band of lift
/// under a hover tank stays the brightest thing on it from every side and in every mark.
inline constexpr math::Vec3 kHoverSkirtColour{0.70F, 1.0F, 1.0F};

/// A drawn hover tank: the wedge, four skirt quads, the turret, the barrel and the fin.
inline constexpr std::size_t kHoverTankVertices = (4U * 36U) + (4U * 6U);

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

/// WHERE A GUN POINTS. Presentation derived from the sim's OWN persistent target data, as the
/// driven tank's turret is derived from the player's view: nothing is invented and nothing is
/// eased. The source, first that names a LIVING unit wins: the trigger's `focus_target`, then the
/// Attack stance's `target`, then `aim_target` -- whom the gun last engaged, which the sim keeps
/// until it has nothing in range, so an auto-engaging gun aims too. With none of them the gun lies
/// along `heading` (the hull's, or a turret's own). Returns a world yaw in the `Headings`
/// convention: 0 is +Y on the map, positive turns toward +X.
[[nodiscard]] float gun_yaw(const sim::Sim& sim, const sim::Unit& unit, float heading);

/// A drawn defense turret: a pedestal turned by its heading, then a housing, a muzzle collar at
/// the barrel's tip and the barrel, all three turned by `gun_yaw`.
inline constexpr std::size_t kTurretVertices = std::size_t{4} * 36U;

/// A drawn factory: the hall, the portal's two jambs and lintel round the recessed door, the
/// annex, the stack and the band near its top.
inline constexpr std::size_t kFactoryVertices = std::size_t{7} * 36U;

/// A drawn mortar: a low wide base turned by its heading, then a mount, a short stubby tube angled
/// up and the collar at its mouth, all three turned by `gun_yaw`.
inline constexpr std::size_t kMortarVertices = std::size_t{4} * 36U;

/// A drawn rocket: the tank's seven hull prisms (tracks, compartment, engine deck, grille,
/// fenders), then a mount and a rectangular launcher rack turned by `gun_yaw`.
inline constexpr std::size_t kRocketVertices = std::size_t{9} * 36U;

/// A reticle is four ticks (up, down, left, right) and a centre dot, six vertices each, in a
/// triangle list: the ticks first, the dot last. The centre between the ticks is open.
inline constexpr std::size_t kReticleVertexCount = 30;

/// A floating health bar over a tank is three quads, six vertices each: an outline, a dark track
/// and the fill. The bars follow the radar in the vertex list and come before the reticle, one per
/// living tank in view other than the viewer's own, and only when there is a viewer.
inline constexpr std::size_t kUnitBarVertexCount = 18;

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
    /// The reticle's pulse, 0 (rest) to 1 (just connected): the app owns it. 0 draws the reticle
    /// at rest, byte-identical to one that never heard of it.
    float reticle_pulse{0.0F};
    /// The weapon the driven craft has up, which picks the reticle (`reticle_for`, with the
    /// camera's pitch): the gun is E12's four ticks, unchanged, so a frame that never heard of it
    /// is byte-identical. The mortar adds its range ladder.
    Weapon weapon{Weapon::Gun};
    /// Draw the weapon's name by the reticle. Off by default for the same reason.
    bool show_weapon{false};
    /// The driven weapon is reloading: the reticle is drawn spent (a dim ghost of itself) until it
    /// is ready again. False draws it as it always was.
    bool reloading{false};
    /// The player's landing markers, drawn standing at their touchdown tiles. Null draws none.
    /// Not owned.
    const LandingMarks* landing_marks{nullptr};
    /// The player's navigational marks (N1), drawn as a slim beacon at each tile. Null draws none.
    /// Not owned.
    const NavMarks* nav_marks{nullptr};
    /// The roster strip (N2) along the top of the frame: the player's living units with their
    /// keys and hit points, drawn from the sim's own unit list. Off by default, so a frame that
    /// never heard of it is unchanged.
    bool show_roster{false};
    /// One line under the strip, for what a roster key just said ("Unit 4 is destroyed."). Empty
    /// draws none. The app holds it for a few seconds; the scene only draws it.
    std::string notice{};
    /// The world map (N4), drawn last over everything: the whole world as a square, your own
    /// assets and your nav marks. Off by default, so a frame that never heard of it is unchanged.
    bool show_world_map{false};
};

/// Triangles for the terrain mesh (two per tile, painted by `Terrain`) followed by one box per
/// living unit (coloured by side, taller for a factory than for a tank). Pure: same options and
/// same sim state give byte-identical vertices.
/// `selected` units are drawn brighter than the rest of their side: the player needs to see what he
/// has hold of, and a tint is the smallest honest version of that before there is any text on
/// screen.
///
/// With `options.aim` set, the scene ends with a reticle: `kReticleVertexCount` more vertices
/// (four ticks round an open centre, and a dot), marking what a click at that pixel would hit.
/// White on the ground, red on a hostile (a click there fires), brighter on one of yours (a click
/// there selects). It is a WORLD-space mark placed a short distance along the click's own ray,
/// which is why it needs no font and cannot be hidden by terrain.
[[nodiscard]] Scene build_scene(const sim::Sim& sim, const SceneOptions& options,
                                std::span<const sim::UnitId> selected = {});

/// The colour calibration view: three full-height vertical bars, left to right pure red, pure
/// green, pure blue, spanning the whole frame width and nothing else (no terrain, units, reticle or
/// radar). Drawn on the same near-plane HUD layer as the radar, through the same vertex format, so
/// what it shows is what the game frame goes through on its way to the screen. Pure in `options`
/// (camera and extent); it reads no sim, so showing it cannot disturb one.
[[nodiscard]] Scene build_calibration_scene(const SceneOptions& options);

} // namespace protekt::game
