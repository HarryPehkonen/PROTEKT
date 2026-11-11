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

#include "protekt/game/atlas.hpp"
#include "protekt/game/faction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/scene_data.hpp"
#include "protekt/game/stats.hpp"
#include "protekt/game/targets.hpp"
#include "protekt/game/terrain_cache.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

/// The world as the player sees it: terrain under the camera, and a box per unit.
///
/// SCALE, FIXED HERE AND STATED EVERYWHERE: one tile is `kMetresPerTile` metres, and one height
/// unit is a 64th of a tile. A tank therefore crosses 6 tiles a second -- 12 m/s -- and the world
/// rises and falls by about 16 m. Every number below is in metres; the conversion happens once.
namespace protekt::game {

/// Harri's ruling: an enemy's bar is always red, so it can never be mistaken for one of my own.
/// The fill length still follows the enemy's health; only the colour is fixed. It is the ramp's own
/// red (`bar_segments`' 0.33 anchor is 1.0, 0.15, 0.10) pulled a touch off it: a friendly bar is
/// red only when its vehicle is nearly dead, an enemy's is red at every health. It lives here, not
/// in scene.cpp, so the test that pins it reads the same constant.
inline constexpr std::array<float, 3> kEnemyBarRed{0.95F, 0.10F, 0.10F};

inline constexpr float kMetresPerTile = 2.0F;
inline constexpr float kMetresPerHeightUnit = kMetresPerTile / 64.0F;

/// THE HALF-WIDTH A HULL IS DRAWN INSIDE, in metres, for every kind: the factory's skyline, the
/// turret, the recycler, the tank's body and the fender. ONE value with six users in `scene.cpp`,
/// so the drawings cannot drift apart -- and so anything that has to match a hull (the contact
/// shadow) has one number to match it to, rather than a guess per kind.
inline constexpr float kHullHalfMetres = kMetresPerTile * 0.3F;

/// THE SUN. Where the light comes FROM, as a world-space direction (x east, y up, z north): high
/// overhead and a little toward -x / -z, so a unit's top face is brightest, the faces turned toward
/// -x and -z come next, the faces turned away are darker, and the underside is darkest. Fixed data,
/// not a function of the camera or the clock: the same scene bytes every frame. Since AP the
/// FRAGMENT STAGE lights from it: every vertex that carries a normal (terrain, hulls, buildings,
/// turrets, wrecks, spheres) is lit by `face_light` of its interpolated normal; the sky, the HUD,
/// bolts and the particles' prisms have no normal and keep their flat colours.
inline constexpr math::Vec3 kSunDirection{-0.35F, 0.85F, -0.40F};

/// THE DARKEST A LIT SURFACE GETS, the light of a face square away from the sun. Never black. The
/// fragment shader reads this and `kSunDirection` from push constants, so the ramp's two numbers
/// have one definition, here.
inline constexpr float kShadeFloor = 0.45F;

/// How lit a surface is: its outward unit normal against the sun, mapped so a normal square to the
/// sun is 1.0 and one square away is `kShadeFloor`. A half-Lambert ramp rather than a clamped dot,
/// so a face turned away from the sun still differs from the underside: top > sun side > shade side
/// > bottom. THE SAME EXPRESSION IS IN `shaders/scene.frag` -- the one place the ramp is written
/// twice, because a CPU-side oracle and a GPU-side light cannot share a line of code. The CPU copy
/// is used only to give flat ground its identity colour (`terrain_base_colour`).
[[nodiscard]] float face_light(const math::Vec3& normal);

/// THE CREASE ANGLE of the smooth shading (AP), in degrees: two faces that meet at a vertex have
/// their normals averaged there only if their normals are within this angle of each other; farther
/// apart, the edge stays sharp. 75 degrees is chosen from the angles the shapes actually have,
/// MEASURED between every pair of faces that share a position over a scene holding one of every
/// unit kind: two crowds, about 3,900 pairs at 40 to 50 degrees (chamfers, eight-sided rings --
/// meant to be round) and about 6,900 at 85 to 90 (boxes, decks against walls -- meant to be
/// corners), with a thin valley of about 230 pairs between 65 and 80. And one hard lower bound: an
/// icosphere's faces that share a corner but no edge are 63.4 degrees apart, and a threshold under
/// that leaves each face of a sphere ignoring two of its five neighbours (measured: a ball 22
/// degrees off radial at 60). 75 sits 11.6 above the sphere and 10 below the nearest box edge.
inline constexpr float kCreaseAngleDegrees = 75.0F;

/// EVERY BARREL IS ROUND (BA): a gun and its collar are a ring of this many corners swept along the
/// gun, with both ends closed (`emit_barrel`, the one helper every barrel site calls), where each
/// was a four-cornered box. TWELVE, because a gun is seen from a long way and from behind and above
/// as well as side-on, and a 12-gon's flats are 30 degrees apart: well inside the crease angle, so
/// the smooth shading blends them into one curved surface at any range the player sees a gun from,
/// where eight (45 degrees) still shows a lit flat edge on a thick collar close up. It is a
/// multiple of four, so the ring touches the box's four extents exactly and a barrel is as wide and
/// tall as the box it replaced.
///
/// THE ONE PLACE WHERE MORE SIDES IS RIGHT. Everywhere else a higher side count destroys detail,
/// because smooth shading averages any panels closer than `kCreaseAngleDegrees` and the facets are
/// lost (the alien shell is faceted on purpose; more sides there only smooths it). A barrel's goal
/// IS the average: a cylinder. Do not "fix" this back to four corners.
inline constexpr int kBarrelSides = 12;

/// The vertices of ONE round barrel (or collar): `kBarrelSides` quads round the side (two triangles
/// each), and a fan of `kBarrelSides - 2` triangles on each end. A box was 36.
inline constexpr std::size_t kBarrelVertices = (static_cast<std::size_t>(kBarrelSides) * 12U) - 12U;

/// SMOOTH SHADING (AP): replaces the normal of each vertex in [first, last) -- a run of triangles,
/// each vertex carrying its FACE's outward unit normal -- with the average of the normals of the
/// faces that share its position, which is what turns a faceted hull into a curved surface.
/// "Share" is exact position equality. Per vertex and per face, only the faces whose normals are
/// within `crease_degrees` of THIS face's normal are averaged in, so a box's edge (90 degrees)
/// stays sharp while the panels of a rounded hull blend; a vertex on a crease thus carries a
/// different normal for each side of it. The average is weighted by each triangle's AREA, so it
/// does not depend on how a quad was cut into two triangles. A vertex whose normal is all zero is
/// not lit and is left alone, and so is a vertex that is the only one at its position.
void smooth_vertex_normals(std::vector<SceneVertex>& vertices, std::size_t first, std::size_t last,
                           float crease_degrees = kCreaseAngleDegrees);

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
/// THE MOON, after the sun in the sky's vertex list: ONE flat disc of `kMoonSegments` triangles,
/// at the fixed world direction `kMoonDirection` (bearing 70 degrees, yaw 0 being +z and positive
/// toward +x, at 28 degrees elevation), 5 degrees of rim radius. Like the sun it is its own
/// emitter: a raw colour, never through `face_light`. It is 90.6 degrees from the sun. A
/// world-fixed body, so it is in view only while the camera faces within the frame of its bearing
/// -- as the sun is. There are no stars: nothing in this game has a night.
inline constexpr math::Vec3 kMoonDirection{0.83F, 0.47F, 0.30F};
inline constexpr math::Vec3 kMoonDiscColour{0.93F, 0.94F, 0.97F};
inline constexpr float kMoonAngularRadius = 0.09F; ///< rim radius as a fraction of the distance
inline constexpr int kMoonSegments = 24;
inline constexpr std::size_t kMoonDiscVertexCount = std::size_t{kMoonSegments} * 3;
inline constexpr float kSkyRadiusMetres = 1000.0F; ///< well inside the 2000 m far plane
inline constexpr std::size_t kSkyRings = 14;
inline constexpr std::size_t kSkyBandVertexCount = std::size_t{kSkySegments} * 6;
inline constexpr std::size_t kSkyCapVertexCount = std::size_t{kSkySegments} * 3;
inline constexpr std::size_t kSunDiscVertexCount = std::size_t{kSkySegments} * 3;
inline constexpr std::size_t kSkyVertexCount = ((kSkyRings - 2) * kSkyBandVertexCount)
                                               + kSkyCapVertexCount + kSunDiscVertexCount
                                               + kMoonDiscVertexCount;
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

/// DISTANCE FOG, for AERIAL PERSPECTIVE: the ground dissolving into the horizon, as Battlezone
/// II's did, so the meshed rim melts into the sky instead of ending in a hard line and distant
/// things lose contrast the way far things do. The fog colour is the sky's own horizon ring,
/// `kSkyRingColours.at(kSkyHorizonRing)` -- reused, never restated. It is applied where the
/// geometry is BUILT (the scene is a CPU vertex list), by `fogged`, to everything the WORLD draws
/// and nothing the HUD draws (see `build_scene`).
/// A vertex nearer than `kFogNearMetres` keeps its exact colour, so something driving toward the
/// camera never thins out; from there to the far distance the colour moves toward the fog along
/// `t^kFogRampExponent`. The far distance is the meshed radius (`fog_far_metres`), so the fog and
/// the cull agree: by the rim a vertex IS the horizon colour.
///
/// THICK ENOUGH TO SEE (Harri: "Can you make the fog thicker?", 2026-10-30). Measured on a
/// 1280x720 capture of the live view BEFORE this change: the fog reached 111 of 720 rows, 18.9 m
/// to 80.0 m, and the whole-frame difference between the fog and no fog at all was 0.011/255 --
/// which is why he could not tell it was there. The ground nearer than 12 m fills most of the
/// frame and was untouched. So the ramp now starts at 5 m and is LINEAR (`t`): at 20 m the ground
/// is a fifth of the way to the horizon, at 40 m a half, and the rim still lands on the horizon
/// colour exactly. A nearer start also cannot thin out what is driving at the camera: at 5 m the
/// tank's own hull and the ground under it are still exact.
inline constexpr float kFogNearMetres = 5.0F;
inline constexpr int kFogRampExponent = 1;

/// Where the fog is total: the terrain's half-extent in metres, the radius the scene culls to.
[[nodiscard]] constexpr float fog_far_metres(int radius_tiles) noexcept {
    return static_cast<float>(radius_tiles) * kMetresPerTile;
}

/// How much of the fog covers something `distance` metres from the eye when the fog is complete at
/// `far`: 0 at or inside `kFogNearMetres`, EXACTLY 1 at or beyond `far`, the ramp in between. The
/// ramp is defined here and nowhere else: `fogged` reads it, and the scene stores it per vertex for
/// the fragment shader, which mixes the fog in AFTER the texture.
[[nodiscard]] constexpr float fog_amount(float distance, float far) noexcept {
    if (distance <= kFogNearMetres) {
        return 0.0F;
    }
    if (distance >= far) {
        return 1.0F;
    }
    const float t = (distance - kFogNearMetres) / (far - kFogNearMetres);
    float amount = 1.0F;
    for (int step = 0; step < kFogRampExponent; ++step) {
        amount *= t;
    }
    return amount;
}

/// `colour` as seen `distance` metres from the camera when the fog is complete at `far`. Pure in
/// its three arguments. Unchanged at or inside `kFogNearMetres`; EXACTLY the horizon colour at or
/// beyond `far` (an assignment, not a blend that rounds to it); strictly nearer the horizon colour
/// the farther the distance in between. The scene no longer calls this -- it stores `fog_amount`
/// and the GPU does the mix -- but it is what that mix computes, and the tests read colours by it.
[[nodiscard]] constexpr math::Vec3 fogged(const math::Vec3& colour, float distance,
                                          float far) noexcept {
    const float amount = fog_amount(distance, far);
    if (amount <= 0.0F) {
        return colour;
    }
    const math::Vec3& fog = kSkyRingColours.at(kSkyHorizonRing);
    if (amount >= 1.0F) {
        return fog;
    }
    return math::Vec3{colour[0] + ((fog[0] - colour[0]) * amount),
                      colour[1] + ((fog[1] - colour[1]) * amount),
                      colour[2] + ((fog[2] - colour[2]) * amount)};
}

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

/// THE GROUND'S VARIANTS (D6, six since D7). The type picks the FAMILY above; inside it, each world
/// tile paints from one of `kGroundVariantCount` variant tiles -- the family's pattern redrawn from
/// other hash seeds in the same tones -- so a field of one type is not a grid of identical stamps.
/// Which variant is a pure function of (type, tile_x, tile_y): no world seed, no state, so a tile
/// keeps its variant forever and the same scene comes out byte-identical.
inline constexpr int kGroundVariantCount = 6;
inline constexpr std::array<std::array<std::string_view, kGroundVariantCount>, world::kTerrainCount>
    kGroundVariantNames{{{"terrain_grass_a", "terrain_grass_b", "terrain_grass_c",
                          "terrain_grass_d", "terrain_grass_e", "terrain_grass_f"},
                         {"terrain_dirt_a", "terrain_dirt_b", "terrain_dirt_c", "terrain_dirt_d",
                          "terrain_dirt_e", "terrain_dirt_f"},
                         {"terrain_rock_a", "terrain_rock_b", "terrain_rock_c", "terrain_rock_d",
                          "terrain_rock_e", "terrain_rock_f"},
                         {"terrain_crater_a", "terrain_crater_b", "terrain_crater_c",
                          "terrain_crater_d", "terrain_crater_e", "terrain_crater_f"}}};

/// The hash of world tile (tile_x, tile_y) of type `terrain`: splitmix64's finaliser over the
/// packed (type, x, y). The variant and the orientation read different bits of it.
[[nodiscard]] constexpr std::uint64_t ground_hash(world::Terrain terrain, int tile_x,
                                                  int tile_y) noexcept {
    std::uint64_t z = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tile_x)) << 32U)
                      | static_cast<std::uint32_t>(tile_y);
    z ^= (static_cast<std::uint64_t>(terrain) + 1U) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return z;
}

/// The variant of world tile (tile_x, tile_y) of type `terrain`, in [0, kGroundVariantCount):
/// the tile's hash modulo the count.
[[nodiscard]] constexpr int variant_index(world::Terrain terrain, int tile_x, int tile_y) noexcept {
    return static_cast<int>(ground_hash(terrain, tile_x, tile_y)
                            % static_cast<std::uint64_t>(kGroundVariantCount));
}

/// THE GROUND'S ORIENTATION (D7). Beside its variant, each world tile gets one of the eight
/// symmetries of the square (four rotations, each plain or mirrored): `orientation / 4` mirrors,
/// `orientation % 4` rotates quarter turns. It is applied to the tile's UV corners when the quad
/// is emitted, so the atlas is untouched, and it is the same kind of pure function as the variant.
inline constexpr int kOrientationCount = 8;

/// The UV of quad corner `corner` (0..3 in the order p00, p10, p11, p01) of `tile` under
/// `orientation`; orientation 0 is the identity.
[[nodiscard]] constexpr std::array<float, 2> oriented_corner_uv(const UvRect& tile, int orientation,
                                                                int corner) noexcept {
    const std::array<std::array<float, 2>, 4> base{
        {{tile.u0, tile.v0}, {tile.u1, tile.v0}, {tile.u1, tile.v1}, {tile.u0, tile.v1}}};
    const bool mirrored = (orientation & 4) != 0;
    const int along = (corner + (orientation & 3)) & 3;
    const int source = mirrored ? (4 - along) & 3 : along;
    return base.at(static_cast<std::size_t>(source));
}

/// The orientation of world tile (tile_x, tile_y) of type `terrain`, in [0, kOrientationCount).
[[nodiscard]] constexpr int orientation_index(world::Terrain terrain, int tile_x,
                                              int tile_y) noexcept {
    return static_cast<int>((ground_hash(terrain, tile_x, tile_y) >> 40U)
                            % static_cast<std::uint64_t>(kOrientationCount));
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
/// The terrain's colour AS LIT on FLAT ground -- what flat ground looks like; the slope's
/// difference from it is the whole effect of the light (`terrain_base_colour`).
inline constexpr math::Vec3 kTerrainLight{1.0F, 1.0F, 1.0F};

/// The terrain's vertex colour: `kTerrainLight` divided by what the ramp gives for straight up
/// (0.958, not 1.0), so that after the fragment stage multiplies the light back in, flat ground is
/// exactly the colour it had before the ground was lit, and a tile tilted toward the sun comes out
/// a little above it. The slope's difference from flat is the whole effect.
[[nodiscard]] math::Vec3 terrain_base_colour();

/// Where the player's eye is, in world metres and radians. Yaw 0 looks down +Z.
struct Camera {
    float x{};
    float y{};
    float z{};
    float yaw{};
    float pitch{};
    /// The frame the yaw and pitch are expressed in: the hull's up when the view rides a vehicle
    /// (`ground_normal` under it), world up otherwise. It changes the camera's roll and nothing
    /// else -- the view direction is the yaw and pitch's, whatever this is.
    math::Vec3 up{0.0F, 1.0F, 0.0F};
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

/// Structures stand level on whatever slope they sit on; only things that DRIVE follow the ground.
/// The turret and the two deployed buildings, the recycler and the scavenger, are structures, as
/// the factory is.
[[nodiscard]] bool stands_level(sim::UnitKind kind) noexcept;

/// THE HOVER. A hover tank does not weld itself to the ground: its ride height follows the terrain
/// through a spring and damper, so a rise under it makes the hull lag, overshoot once, and settle.
/// Presentation only -- no UNIT has a z and none gains one (a bolt does now, since pitch bends it;
/// this is about units). All rates are per SIM TICK (30 a
/// second), never per frame, so the hop does not depend on the display's refresh rate.
///
/// A stationary tank floats this far above the ground, in metres.
inline constexpr float kRideHeightMetres = 0.25F;

/// THE STEEPEST A VEHICLE'S BODY IS DRAWN TILTED, as a slope (rise over run). PROVISIONAL, and
/// Harri's to set. It is derived, not picked: the hull stands `kRideHeightMetres` clear of the
/// ground and is `kHullHalfMetres` from its centre to its side, so a slope steeper than the one
/// that lifts a side by the whole ride height over that half-width (0.25 / 0.6, about 22.6
/// degrees) is a hull leaning past the clearance it was drawn with. Beyond it the body stops
/// following the ground, so a tank on a cliff edge leans, never rolls over.
inline constexpr float kMaxTiltSlope = kRideHeightMetres / kHullHalfMetres;

/// The unit-length normal of the ground under a vehicle at (x, y), for tilting its body. Four
/// samples, one a hull half-width out on each side of the point (east, west, north, south), give
/// the slope along each axis; the gradient is clamped to `kMaxTiltSlope` and the normal is
/// (-d/dx, 1, -d/dz) normalised. Exactly (0, 1, 0) on level ground. Pure in `(seed, x, y)`.
[[nodiscard]] math::Vec3 ground_normal(std::uint64_t seed, sim::Fixed x, sim::Fixed y) noexcept;

/// THE HULL'S LEAN AS A ROTATION, the one definition of how a vehicle's frame is turned off world
/// up: Rodrigues, about the horizontal axis `up x normal`, through `pivot`. The scene turns a
/// vehicle's body with it and the player's eye rides the hull with it, so the two cannot disagree.
/// A level normal (exactly `{0, 1, 0}`, as `ground_normal` returns it) turns nothing at all.
struct GroundTilt {
    math::Vec3 axis{};
    math::Vec3 pivot{};
    float sine{};
    float cosine{1.0F};
    [[nodiscard]] bool level() const noexcept { return sine == 0.0F; }
    /// `point` turned about the pivot; `point` itself when level.
    [[nodiscard]] math::Vec3 apply(const math::Vec3& point) const noexcept;
    /// `direction` (a normal, not a point) turned by the same rotation, about no pivot.
    [[nodiscard]] math::Vec3 turn(const math::Vec3& direction) const noexcept;
};
[[nodiscard]] GroundTilt tilt_about(const math::Vec3& pivot, const math::Vec3& normal) noexcept;

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

/// THE WAKE A WRECK LEAVES (D4). A unit that dies is a `Killed` event: the sim's table keeps the
/// dead unit, so the app says WHERE by naming the victim and the scene reads its own tile back
/// out. Nothing is stored per puff, so a replay stays the whole input.
///
/// First a BURST: `kBlastPuffs` puffs thrown out and up over `kBlastTicks`, shrinking and cooling
/// from `kBlastHotColour` to `kBlastCoolColour` as they go. Then a COLUMN: `kSmokePuffs` grey
/// puffs stacked over the wreck, rising and drifting on a wind that comes off the frame's seed,
/// thinning out until `kSmokeTicks` have passed and it is gone.
///
/// The sizes are set against the hull that died: a tank is `2 * kHullHalfMetres` = 1.2 m across.
/// The fireball opens at about twice the hull (`kBlastHalfMetres` = 1.0 m, a 2 m box) and is big,
/// bright and brief, as BZ2's were. A PUFF a sixteenth of the hull is a speck nobody sees, so the
/// burst's sparks are not puffs: they are `Particle`s of their own (`kBlastSparks`), small but
/// bright and opaque, and sized against the fireball.
inline constexpr std::size_t kBlastPuffs = 20;
inline constexpr sim::Tick kBlastTicks = 30; ///< one second at 30 Hz
inline constexpr float kBlastHalfMetres = 1.0F;
inline constexpr float kBlastLastHalfMetres
    = 0.35F;                                      ///< where it has shrunk to as it cools to smoke
inline constexpr float kBlastSpreadMetres = 2.5F; ///< how far out a puff is thrown
inline constexpr float kBlastRiseMetres = 2.4F;   ///< and how high the burst reaches
inline constexpr float kBlastAlpha = 0.92F;
inline constexpr std::array<float, 3> kBlastHotColour{1.00F, 0.78F, 0.30F}; ///< near white-hot
inline constexpr std::array<float, 3> kBlastCoolColour{0.40F, 0.36F, 0.33F};

/// THE BLAST'S SPARKS (AI). The fireball is a 2 m box, so a spark of `kBlastSparkHalfMetres` (18 cm
/// across, a ninth of it) is small beside it and still a few pixels at the distances the camera
/// keeps. `kBlastSparks` of them leave the fireball's centre at up to `kBlastSparkSpeedMetres` a
/// tick outward and `kBlastSparkLiftMetres` up, fall under `kSparkGravity`, and are gone after
/// `kBlastSparkTicks` -- before the burst itself ends, so the smoke column has none.
inline constexpr std::size_t kBlastSparks = 14;
inline constexpr sim::Tick kBlastSparkTicks = 24;
inline constexpr float kBlastSparkHalfMetres = 0.09F;
inline constexpr float kBlastSparkSpeedMetres = 0.22F;
inline constexpr float kBlastSparkLiftMetres = 0.28F;

/// The column is wider than the hull (`kSmokeHalfMetres` = 1.1 m, a 2.2 m box), nine puffs deep so
/// the boxes overlap into one plume, and rises 6 m over the 390 ticks after the burst. It ends at
/// `kSmokeTicks`: a wreck is visibly alight for fourteen seconds and then just litter.
inline constexpr std::size_t kSmokePuffs = 9;
inline constexpr sim::Tick kSmokeTicks = 420; ///< fourteen seconds, then the wreck is cold
inline constexpr float kSmokeBaseMetres = 0.5F;
inline constexpr float kSmokeSpacingMetres = 0.8F;
inline constexpr float kSmokeRiseMetresPerTick = 0.016F;
inline constexpr float kSmokeDriftMetresPerTick = 0.012F;
inline constexpr float kSmokeHalfMetres = 1.1F;
inline constexpr float kSmokeAlpha = 0.34F;
inline constexpr std::array<float, 3> kSmokeColour{0.44F, 0.44F, 0.46F};

/// A puff (burst or column) is a prism of `kPuffSides` corners, not a box: eight is the fewest that
/// stops a plume reading as stacked dice, and the next step up (twelve) costs 48 more vertices a
/// puff for a silhouette nobody can tell at the puff's distance. Its flats sit where the box's
/// faces did, so it is exactly as wide across as the box it replaced and no wider. The vertex
/// count is two rim-fan caps (`kPuffSides - 2` triangles each) and `kPuffSides` side quads.
inline constexpr std::size_t kPuffSides = 8;
[[nodiscard]] constexpr std::size_t prism_vertices(std::size_t sides) noexcept {
    return (sides * 6) + (2 * (sides - 2) * 3);
}
inline constexpr std::size_t kPuffVertices = prism_vertices(kPuffSides);

/// A particle's SHAPE (AH). A PUFF is an `Icosphere` -- twenty uniform triangles, twelve corners
/// evenly spread, no poles to pinch (Harri, from his own research: a UV sphere pinches, an
/// icosphere does not) -- and each one carries its own roll. A SPARK stays a `Prism` of four
/// corners, a cube: at 2.5 px nothing rounder can be told from it, and it costs a third of a
/// sphere's vertices.
enum class ParticleShape : std::uint8_t { Icosphere, Prism };

/// An icosphere puff's corners stand `kPuffRadiusPerHalf * half` from its centre, so a puff of
/// `half` is about as big as the box it replaced (the sphere's flats are nearer than its corners).
inline constexpr float kPuffRadiusPerHalf = 1.2F;
/// How fast a puff may roll, in radians per tick either way (0.05 = 1.5 rad a second, slow).
inline constexpr float kPuffRollRadiansPerTick = 0.05F;

/// THE PARTICLE (AG): the one spawn the blast's puffs, the wreck's smoke, every spark and (AH)
/// every mote are made by. A particle is an icosphere, or a prism of `sides` corners, whose centre,
/// size, colour and alpha are all FUNCTIONS OF ITS AGE and of nothing stored: it stands at `origin
/// + velocity * age` less `gravity` (metres per tick squared) pulling it down, and `half`, `colour`
/// and `alpha` run from their `_from` value to their `_to` value over `lifetime` ticks. At `age >=
/// lifetime` it is gone and `emit_particle` draws nothing. `floor_per_metre` keeps it at least that
/// many metres of half width per metre between it and the eye (0 = no floor), which is how a spark
/// stays a few pixels wide however far away it is. The caller derives its particles from the sim's
/// tick and the frame's seed, so a replay needs nothing in it. An icosphere turns about the
/// vertical by `roll + roll_rate * age` (and leans over by 0.6 of that), so puffs that overlap are
/// not copies of one another; a prism does not turn.
struct Particle {
    math::Vec3 origin{};
    math::Vec3 velocity{}; ///< metres per tick
    float gravity{0.0F};   ///< metres per tick squared, downward
    sim::Tick lifetime{1};
    float half_from{0.0F};
    float half_to{0.0F};
    math::Vec3 colour_from{};
    math::Vec3 colour_to{};
    float alpha_from{1.0F};
    float alpha_to{1.0F};
    ParticleShape shape{ParticleShape::Icosphere};
    std::size_t sides{kPuffSides}; ///< a prism's corners; an icosphere ignores it
    float floor_per_metre{0.0F};
    float roll{0.0F};      ///< radians at age 0 (an icosphere's)
    float roll_rate{0.0F}; ///< radians per tick (an icosphere's)
};

/// Draw `particle` as it is `age` ticks after it was spawned, seen from `eye`.
void emit_particle(std::vector<SceneVertex>& out, const Particle& particle, sim::Tick age,
                   const math::Vec3& eye);

/// A SPARK (AG, AI). Harri: "leave the sparks small, and just not remove them in the view ...
/// that's probably mostly because the sparks are very bright". So a spark is a four-sided prism (a
/// cube, 36 vertices), OPAQUE (alpha 1: the softness that makes the beam good is what makes a spark
/// invisible) and near white-hot, cooling only to a bright amber -- never dim. The view's FXAA
/// pulls a lone one-pixel speck three quarters toward its neighbours (`renderer.cpp`,
/// `subpixel_blend`), and that is NOT lowered here: a spark is instead kept `kSparkMinPixels` wide
/// on screen, at the reference height `kSparkReferenceRows`, by `kSparkFloorPerMetre` -- half
/// of 2.5 px of a 60 degree view's `2 * tan(30 degrees) / 720` metres-per-metre-of-distance pixel.
inline constexpr std::size_t kSparkSides = 4;
inline constexpr std::size_t kSparkVertices = prism_vertices(kSparkSides);
inline constexpr std::array<float, 3> kSparkHotColour{1.00F, 0.97F, 0.82F};
inline constexpr std::array<float, 3> kSparkCoolColour{1.00F, 0.80F, 0.35F};
inline constexpr float kSparkMinPixels = 2.5F;
inline constexpr int kSparkReferenceRows = 720;
inline constexpr float kSparkFloorPerMetre
    = 0.57735F * kSparkMinPixels / static_cast<float>(kSparkReferenceRows);
inline constexpr float kSparkGravity = 0.008F; ///< metres per tick squared

/// FLOATING MOTES (AH; Harri: "I would eventually like particles floating in the air for effect").
/// `kMoteCount` pale specks drifting near the viewer, VIEW-ONLY: each is a `Particle` icosphere
/// rebuilt every frame (there is no manager and no pool), its place and its roll hashed from the
/// frame's seed and slid along the seed's wind by the sim's tick, so nothing is stored and nothing
/// is hashed. They live in a square `kMoteBoxMetres` across centred on the camera and WRAP at its
/// edge -- a mote leaving one side re-enters the other -- fading out toward the edge so the wrap is
/// never seen; each floats between `kMoteLowMetres` and `kMoteCeilingMetres` over the ground
/// beneath it. A dozen is the budget: this is a camera-local garnish, not weather.
inline constexpr std::size_t kMoteCount = 12;
inline constexpr float kMoteBoxMetres = 32.0F;
inline constexpr float kMoteLowMetres = 0.4F;
inline constexpr float kMoteCeilingMetres = 6.0F;
inline constexpr float kMoteHalfMetres = 0.05F;
inline constexpr float kMoteAlpha = 0.5F;
inline constexpr float kMoteDriftMetresPerTick = 0.008F;
inline constexpr float kMoteBobMetres = 0.3F;
inline constexpr float kMoteBobRadiansPerTick = 0.02F;
inline constexpr std::array<float, 3> kMoteColour{0.95F, 0.92F, 0.80F};

/// A BIOMETAL POOL VENTING (Harri, 2026-11-01, from BattleZone 2, where a pool is "a shallow
/// circular vent with a pale rim and a visible plume of steam"). Until now a pool was a white
/// outline on the radar and NOTHING in the world, so it read as a decal. Each pool in view is a
/// plume: `kPoolPlumePuffs` see-through puffs that rise `kPoolPlumeHeightMetres` over
/// `kPoolPlumeTicks`, each a fraction of a cycle behind the last, widening and fading as they
/// climb, over ALTERED GROUND (`site_ground.hpp`: the pool's own look, painted into the terrain --
/// there is no disc or rim lying on it any more, and a deployed scavenger stands on the ground
/// itself). Every puff's place is a function of the sim's tick, so nothing is stored and a replay
/// needs nothing in it. The plume is taller than a tank on purpose: from the ground it must be
/// obvious that something is coming out of the ground there.
inline constexpr std::array<float, 3> kPoolSteamColour{0.88F, 0.88F, 0.90F};
inline constexpr std::size_t kPoolPlumePuffs = 10;
inline constexpr float kPoolPlumeHeightMetres = 14.0F;
inline constexpr sim::Tick kPoolPlumeTicks = 150; ///< five seconds for a puff to climb the plume
inline constexpr float kPoolPuffHalfBaseMetres = 0.7F;
inline constexpr float kPoolPuffHalfTopMetres = 2.4F;
inline constexpr float kPoolSteamAlpha = 0.34F;

/// THE REPAIR BEAM (Harri, 2026-11-05: "a bright, trembling beam of different colours coming from
/// the service truck to the item being repaired"; 2026-11-06: "the shapes should be fairly
/// transparent and spherical"). One beam per (truck, patient) pair the sim is mending this tick,
/// and none otherwise (`repair_links`): a chain of `kRepairBeamBeads` SPHERES from the truck to the
/// patient, each in the next colour of `kRepairBeamPalette` at `kRepairBeamAlpha`, and the whole
/// run of colours slid along the beam one step every `kRepairBeamFlowTicks` so it pours toward the
/// patient. Multi-coloured on purpose: the one exception to the HUD's flat single-colour look,
/// because it was asked for. It TREMBLES: every bead is thrown off the straight line by up to
/// `kRepairBeamTrembleMetres` on each axis, a fresh throw every sim tick (a function of the tick,
/// the pair and the bead -- nothing stored, nothing hashed), fading to nothing at both ends so the
/// beam always lands on the truck and on the patient. The ends sit `kRepairBeamLiftMetres` above
/// the ground under each unit.
///
/// A sphere is an icosahedron: `kSphereFaces` = 20 triangles, 60 vertices, twelve corners all one
/// radius from the centre. It is the fewest faces that still read round: at the 10 m a player
/// watches a repair from a bead is about 25 px across at 720 rows, and a second subdivision
/// (80 faces, four times the vertices on 28 beads a beam) would add a silhouette nobody can tell.
///
/// At the patient's end the weld throws `kRepairSparks` SPARKS (`Particle`), each living
/// `kRepairSparkTicks` and restarted from the weld on a fresh seeded heading when it dies, so the
/// shower never stops while the beam stands and never needs a spawn event.
inline constexpr std::size_t kSphereFaces = 20;
inline constexpr std::size_t kSphereVertices = kSphereFaces * 3;
inline constexpr std::array<std::array<float, 3>, 6> kRepairBeamPalette{{{1.0F, 0.15F, 0.15F},
                                                                         {1.0F, 0.65F, 0.05F},
                                                                         {1.0F, 1.0F, 0.2F},
                                                                         {0.2F, 1.0F, 0.3F},
                                                                         {0.1F, 0.9F, 1.0F},
                                                                         {0.85F, 0.3F, 1.0F}}};
inline constexpr std::size_t kRepairBeamBeads = 28;
inline constexpr float kRepairBeamBeadRadiusMetres = 0.2F;
inline constexpr float kRepairBeamAlpha = 0.4F;
inline constexpr float kRepairBeamTrembleMetres = 0.3F;
inline constexpr float kRepairBeamLiftMetres = 1.0F;
inline constexpr sim::Tick kRepairBeamFlowTicks = 2;
inline constexpr std::size_t kRepairSparks = 8;
inline constexpr sim::Tick kRepairSparkTicks = 12;
inline constexpr float kRepairSparkHalfMetres = 0.05F;
inline constexpr float kRepairSparkSpeedMetres = 0.12F;
inline constexpr float kRepairSparkLiftMetres = 0.1F;

/// A truck and the unit it is mending this tick.
struct RepairLink {
    sim::UnitId truck{};
    sim::UnitId patient{};
};

/// Every (truck, patient) pair the sim's service repair is advancing right now: a living truck, a
/// living friendly unit of its side that is not itself and is hurt, within
/// `sim::kServiceRepairRadiusTiles`. The sim's own rule (`service_repair_step`) asked of the same
/// state, read-only -- a unit at full health is nobody's patient, so a truck beside one has no
/// link. Ascending patient id, then truck id.
[[nodiscard]] std::vector<RepairLink> repair_links(const sim::Sim& sim);

/// THE KILLS A WAKE IS DRAWN FROM. The scene derives a wreck's burst and its column from
/// `now - event.tick`, so the `Killed` event has to reach the scene on every frame of the wreck's
/// life, not only on the frame that stepped the kill. The app owns this, like `HitMark`: it diffs
/// the frame (`sim::events_between`), keeps the kills, and forgets one when its age on the sim's
/// own tick reaches `kSmokeTicks` -- the scene's cutoff, so there is no second clock. A kill that
/// arrives again (a frame that stepped nothing re-reads the last tick) is not listed twice. Only
/// `Killed` is kept: the hit flash and the hit mark are driven from `HitTaken` by the audio
/// pipeline, per tick.
class WakeLog {
public:
    void after_frame(const sim::UnitSnapshot& before, const sim::Sim& sim, sim::Tick stamp);
    [[nodiscard]] std::span<const sim::BattleEvent> events() const noexcept { return kills_; }

private:
    std::vector<sim::BattleEvent> kills_;
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
/// The hold is 2 s (M5c): a lob at the highest elevation is in the air for 52 ticks, so a player
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
class RadarActivity;
struct RowState;

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

/// The hover skirt's colour: a hot pale cyan at red 0.70, RAW -- never lit, never marked -- so the
/// band of lift under a hover tank stays the brightest thing on it from every side and in every
/// mark.
///
/// 0.70 and no more, BY THE OWNER'S RULING (2026-11-09).  AY raised it to 0.78 when AY's rounding
/// gave the hull a face that turns straight into the sun and the lit pale camo reached 2.75 in
/// channel sum against the old hull's 2.70.  Asked to choose, he kept the colour and let the TEST
/// carry the slack instead: what is worth holding is that the band READS as the brightest thing,
/// not the exact sum that used to express it.  See `kSkirtBrightnessSlack` in tests/test_scene.cpp.
inline constexpr math::Vec3 kHoverSkirtColour{0.70F, 1.0F, 1.0F};

/// A drawn hover tank: the parts of `shape_of(UnitKind::HoverTank)` (three stepped wedge sections,
/// the fin and the turret, each a chamfered extrusion), then the four skirt quads and the barrel,
/// which are not shape parts (the barrel is round since BA: 132 vertices where the box was 36, 864
/// before). The shape is 804 vertices (AY cut the corners of the three wedge
/// sections and the turret; 444 before), 168 before it was derived.
inline constexpr std::size_t kHoverTankVertices = 960;

/// THE HOVER TANK'S SIZES that the shape (`vehicle_shape.cpp`), the mesh builder and the plate all
/// read, here and nowhere else. Metres, along the heading from the centre and up from the ground.
/// The wedge runs from the tail to the nose and rides on `kHoverHullBottom`; under it a bright band
/// of lift, the SKIRT, hangs to `kHoverSkirtBottom` with the air under that. The skirt and the
/// barrel are not shape parts -- the skirt is raw unlit colour and the barrel elevates -- so the
/// mesh and the plate each add them from these numbers.
inline constexpr float kHoverTail = -1.3F;
inline constexpr float kHoverNose = 1.5F;
inline constexpr float kHoverNoseHalf = 0.12F; ///< the skirt's plan narrows to this at the nose
inline constexpr float kHoverHullBottom = 0.4F;
inline constexpr float kHoverSkirtBottom = 0.15F;
inline constexpr float kHoverSkirtInset = 0.9F; ///< of the hull's outline: tucked under the hull
inline constexpr float kHoverTurretBase = 0.6F; ///< sunk into the sloping deck
inline constexpr float kHoverTurretFront = 0.42F;
inline constexpr float kHoverBarrelBottom = 0.1F; ///< above the turret's base
inline constexpr float kHoverBarrelTop = 0.2F;
inline constexpr float kHoverBarrelTip = 1.8F; ///< from the hover tank's centre, past the nose

/// THE TANK'S BARREL, in metres above the hull's own base (the `ground` the tank is built on, which
/// is the hull height once it hovers). Here, and not inside the tank builder, because the driver's
/// eye is derived from the barrel's axis: two copies of the numbers would let the eye and the gun
/// drift apart.
inline constexpr float kTankTurretBase = 0.8F;    ///< sunk into the compartment roof
inline constexpr float kTankBarrelBottom = 0.18F; ///< above the turret's base
inline constexpr float kTankBarrelTop = 0.3F;
/// The turret's height, so its roof is `kTankTurretRoof` over the hull's base. The shape
/// (`vehicle_shape.cpp`) stands its turret on `kTankTurretBase` and tops it at the roof, and the
/// team-colour panel lies on that roof, so the number lives once, here.
inline constexpr float kTankTurretHeight = 0.4F;
inline constexpr float kTankTurretRoof = kTankTurretBase + kTankTurretHeight;
/// Where the barrel leaves the turret, along the aim from the tank's centre: the turret's front
/// face, which is also the shape's.
inline constexpr float kTankTurretFront = 0.35F;
/// The muzzle collar at the barrel's tip: its length, and its bottom and top above the turret's
/// base.
inline constexpr float kTankCollarLength = 0.3F;
inline constexpr float kTankCollarBottom = 0.14F;
inline constexpr float kTankCollarTop = 0.34F;
/// From the tank's centre along the aim, well past the hull front: the muzzle collar's far end.
inline constexpr float kTankBarrelTip = 2.0F;
/// The barrel's axis above the hull's base: the line the shot travels along when level, and the
/// line the driver's eye sits on (`app::kEyeAboveHullMetres`).
inline constexpr float kTankBarrelAxisMetres
    = kTankTurretBase + ((kTankBarrelBottom + kTankBarrelTop) * 0.5F);

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

/// THE HARVESTER'S GAIT. The sim moves a machine by position; its legs are drawing. A machine's
/// WALK PHASE (a fraction of a stride cycle, `[0, 1)`) advances with the DISTANCE IT ACTUALLY MOVED
/// -- `kStrideMetres` of ground to a cycle -- and with nothing else: no clock, so a slow machine
/// does not cycle its legs in place, a paused one does not step, and a capture is reproducible. A
/// machine that did not move keeps its phase EXACTLY, so its legs hold. Keyed by unit id in an
/// ordered map, `Headings`' sibling: called once per frame with the sim's BEFORE-snapshot, drops
/// the dead, and touches nothing in the sim. A unit never seen moving has NO phase, and is drawn
/// at the neutral stance (the feet at their static places).
inline constexpr float kStrideHalfMetres
    = 0.35F; ///< how far a foot reaches fore and aft of its static place
inline constexpr float kStrideMetres = 4.0F * kStrideHalfMetres; ///< ground covered by one cycle
inline constexpr float kStepLiftMetres = 0.18F; ///< how high a swinging foot clears the ground

/// One leg's pose at a walk phase: how far fore (+) of its static place the foot is (planted legs
/// slide AFT at exactly the machine's own speed, so the foot stays put on the ground; swinging legs
/// are carried forward), and how high it is lifted. `leg` counts 0..5 around the body, so the
/// TRIPOD sets are the even and the odd legs (1-3-5 against 2-4-6 in the owner's counting); the
/// two sets are half a cycle apart.
struct LegPose {
    float along{};
    float lift{};
};
[[nodiscard]] LegPose leg_pose(float phase, std::size_t leg) noexcept;

class Strides {
public:
    void after_frame(const sim::UnitSnapshot& before, const sim::Sim& sim);
    /// The unit's phase, or ABSENT when it has never moved.
    [[nodiscard]] std::optional<float> of(const sim::Unit& unit) const;

private:
    std::map<std::uint32_t, float> phase_;
};

/// WHERE A GUN POINTS. Presentation derived from the sim's OWN persistent target data, as the
/// driven tank's turret is derived from the player's view: nothing is invented and nothing is
/// eased. The source, first that names a LIVING unit wins: the trigger's `focus_target`, then the
/// Attack stance's `target`, then `aim_target` -- whom the gun last engaged, which the sim keeps
/// until it has nothing in range, so an auto-engaging gun aims too. With none of them the gun lies
/// along `heading` (the hull's, or a turret's own). Returns a world yaw in the `Headings`
/// convention: 0 is +Y on the map, positive turns toward +X.
[[nodiscard]] float gun_yaw(const sim::Sim& sim, const sim::Unit& unit, float heading);

/// A drawn defense turret, driven or not (ONE builder, `build_defense_turret`): the parts of
/// `shape_of(UnitKind::Turret)` (a flared base and a pedestal, turned by the heading; a dome with
/// its vision slit and a whip aerial, turned by `gun_yaw`), then the muzzle collar and the barrel,
/// which elevate and so are not parts. It was 144, four prisms, undriven and 432, a tank's hull
/// under a squat turret, when a player drove it. 492 before AY cut the base's and the pedestal's
/// corners, and 636 before BA made the collar and the barrel round (132 each, 36 each as boxes).
inline constexpr std::size_t kTurretVertices = 828;

/// THE TURRET'S GUN, in metres: the dome's front face (along the aim from the turret's centre),
/// where the shape ends and the barrel begins, the barrel and its collar over the GROUND (a turret
/// has no hull under it to measure from), and the tip. The barrel elevates when a player drives
/// the turret, so it is not a part: the mesh builds it from these numbers and the plate draws it
/// level from them, and nothing else says how long it is.
inline constexpr float kTurretGunFront = 0.5F;
inline constexpr float kTurretBarrelBottom = 1.2F;
inline constexpr float kTurretBarrelTop = 1.36F;
inline constexpr float kTurretCollarLength = 0.3F;
inline constexpr float kTurretCollarBottom = 1.16F;
inline constexpr float kTurretCollarTop = 1.4F;
inline constexpr float kTurretBarrelTip = 2.0F;

/// A drawn factory (BC): the rounded hall (288: ten plan corners, a crown of three steps, a floor
/// and a roof fan), the portal's two jambs and lintel round the recessed door, which stay flat
/// prisms of 36 (108), the rounded annex (468) and the stack and its band near its top, each a
/// twelve-sided loft of 132. It was seven prisms of 36 (252 vertices, 84 triangles).
inline constexpr std::size_t kFactoryVertices = 288U + (3U * 36U) + 468U + (2U * 132U);

/// HOW BIG THE RECYCLER IS DRAWN, as a multiple of the tank-scale model `build_recycler` describes
/// (1.2 x 2.6 x 1.25 m). Harri's number: "I think the recycler should be bigger. Maybe twice as
/// big. It's pretty big in BattleZone 2." (2026-11-02). Every vertex is scaled by it about the
/// machine's base centre, once, so the shape stays one description and the size one dial: change
/// this and nothing else. Presentation only -- the sim's solid footprint is 4 m square regardless.
inline constexpr float kRecyclerScale = 2.0F;

/// A drawn recycler: a track assembly each side (the track, six road wheels, the skirt), the
/// belly, the side panel and its strip, the body, its spine, two grilles, the rear bumper, the
/// plow and its slit, the cab, its roof hatch and its vision band, the radiator, and the crane
/// arm's four parts. BC rounded the two tracks, the two skirts, the body and the cab - six masses
/// of 180 vertices (eight corners, bevelled) - and set the hatch into the cab's dome as a 28th flat
/// prism: 27 + 1 prisms of 36 in all. It was 33 prisms of 36 (1188 vertices, 396 triangles).
inline constexpr std::size_t kRecyclerVertices = (6U * 180U) + (28U * 36U);

/// A drawn mortar bike: all of it the parts of `shape_of(UnitKind::MortarBike)` (the frame, the
/// seat, two wheels with their rings and hubs, the fork and the handlebars; then, turned by
/// `gun_yaw`, the mount, the stubby tube, the collar and the bore cut in it). Nothing is added to
/// the shape. It was 144, four prisms, before it was derived: a low box where the plate had drawn a
/// bike. 1488 before AY cut the frame's and the mount's corners.
inline constexpr std::size_t kMortarBikeVertices = 1656;

/// A drawn walker: the parts of `shape_of(UnitKind::Walker)` (two bent legs and their feet, the
/// body, its cockpit glass and the roof panel), then the gun arm, which is not a shape part. It
/// was 180, five prisms, before it was derived, and no test pinned it. 744 before AY cut the body's
/// corners, and 792 before BA made the gun arm round.
inline constexpr std::size_t kWalkerVertices = 888;

/// THE WALKER'S GUN ARM, in metres: the body's front face (along the aim from the walker's centre),
/// where the shape ends and the arm begins, the arm's bottom and top over the ground, and its tip.
/// The arm elevates, so it is not a part: the mesh builds it from these numbers and the plate draws
/// it level from them, and nothing else says how long it is.
inline constexpr float kWalkerGunFront = 0.5F;
inline constexpr float kWalkerGunBottom = 1.9F;
inline constexpr float kWalkerGunTop = 2.06F;
inline constexpr float kWalkerGunTip = 1.8F;

/// A drawn APC: all of it the parts of `shape_of(UnitKind::Apc)` (the hull, its firing ports and
/// roof panel, eight wheels with their rings and hubs, then, turned by `gun_yaw`, the turret and
/// its gun). Nothing is added to the shape. It was 144, four prisms, and no test pinned it. 3804
/// before AY cut the hull's and the housing's corners.
inline constexpr std::size_t kApcVertices = 3948;

/// A drawn rocket tank: the parts of `shape_of(UnitKind::RocketTank)` (the tank's chassis, a mount,
/// the launcher rack and the lane cut in it), then the team-colour panel on the rack. The shape is
/// 3000 vertices (2700 of chassis; AY cut the chassis' corners, 2340 before); it was 324, nine
/// prisms, before it was derived.
inline constexpr std::size_t kRocketTankVertices = 3036;

/// THE ROCKET TANK'S RACK, in metres over the hull's base: the top of the rack, which the shape
/// (`vehicle_shape.cpp`) ends at and the panel lies on, so the number lives once, here. The panel
/// stands `kRocketPanelThickness` on it, and the total stays under the health bar's 1.2 m anchor.
inline constexpr float kRocketRackTop = 1.15F;
inline constexpr float kRocketPanelThickness = 0.03F;

/// A drawn assault tank: the parts of `shape_of(UnitKind::AssaultTank)` (the tank's chassis, the
/// turret block and its hatch), then the roof panel, the muzzle collar and the barrel, which are
/// not shape parts (the collar and the barrel are round since BA, 132 each, 3072 in all before).
/// The shape is 2964 vertices (2508 before AY); the whole was 396, eleven prisms, before it was
/// derived.
inline constexpr std::size_t kAssaultTankVertices = 3264;

/// THE ASSAULT TANK'S BLOCK AND BARREL, in metres: the block's roof (over the hull's base) and
/// front face (along the aim from the tank's centre), where the shape ends and the panel and the
/// barrel begin, and the barrel and its collar, which elevate and so are not parts, over the
/// turret's base `kTankTurretBase`.
inline constexpr float kAssaultTurretRoof = 1.5F;
inline constexpr float kAssaultTurretFront = 0.5F;
inline constexpr float kAssaultBarrelBottom = 0.3F;
inline constexpr float kAssaultBarrelTop = 0.46F;
inline constexpr float kAssaultCollarLength = 0.3F;
inline constexpr float kAssaultCollarBottom = 0.26F;
inline constexpr float kAssaultCollarTop = 0.5F;
inline constexpr float kAssaultBarrelTip = 2.1F;

/// A drawn bomber: all of it the parts of `shape_of(UnitKind::Bomber)` (the fuselage and its
/// canopy, the nose, the tail fin, the wing, the bomb with its fins, and the team-colour stripe on
/// the spine). Nothing is added to the shape: it drops the mortar's shell by the sim's lob and
/// carries no barrel. It was 108, three boxes (a flat slab, a nose and a panel); 744 before AY cut
/// the fuselage's and the wing's corners.
inline constexpr std::size_t kBomberVertices = 984;

/// The GUN's reticle is four ticks (up, down, left, right) and a centre dot, six vertices each, in
/// a triangle list: the ticks first, the dot last. The centre between the ticks is open. Every
/// other weapon draws its own shape (`reticle_for`) with its own count, `reticle_vertex_count`; the
/// dot is the last quad of every one.
inline constexpr std::size_t kReticleVertexCount = 30;
/// How many vertices the reticle of `weapon` is, the tail of the vertex list when there is an aim.
[[nodiscard]] std::size_t reticle_vertex_count(Weapon weapon) noexcept;

/// A floating health bar over a tank is three quads, six vertices each: an outline, a dark track
/// and the fill. The bars follow the radar in the vertex list and come before the reticle, one per
/// living tank in view other than the viewer's own, and only when there is a viewer.
inline constexpr std::size_t kUnitBarVertexCount = 18;

/// The HUD's small lettering: 5x5 cell glyphs, each cell this many pixels square, on a plate with
/// this margin. The radar's N S E W are drawn in it, and so are the name over and the distance
/// under a unit the player points at -- one size for all of them, a letter 10 px tall.
inline constexpr float kLetterCellPixels = 2.0F;
inline constexpr float kLetterPlatePadPixels = 2.0F;

/// THE AMMO READING IS A PLACEHOLDER (SHIPGAUGE-1). The driven craft has no magazine in the sim --
/// nothing counts rounds and nothing ever runs dry -- so the ship instrument's blue arc is always
/// full and its number is always the string "100". It is a CONSTANT, never a value the sim
/// computed, and it must stay obviously so: AMMO-2 replaces the constant, the string and this
/// comment with a real reading. The user's ruling, 2026-10-23: "For now, we can keep the ammo
/// capacity always at 100%. When we add capacity, we can change that." It lives here rather than in
/// scene.cpp's anonymous namespace because a test pins it by name.
inline constexpr float kAmmoPlaceholderFraction = 1.0F;
[[nodiscard]] constexpr std::string_view ammo_placeholder_percent() noexcept { return "100"; }

/// A PIECE OF WRECKAGE, DRAWN (C5). C1 scatters four pieces of a dead unit's hull on the ground
/// around it and settles them onto ground a vehicle can stand on -- and until now nothing drew
/// them, so a battle that littered the field looked exactly like one that had not. The mark is a
/// small dark plate lying flat on the terrain, deliberately SMALLER than the footprint of the tank
/// that died (that hull is `kMetresPerTile * 0.3` either side) so a field of them reads as debris
/// and never as units. It is lifted a few centimetres clear of the ground so it does not fight the
/// terrain it lies on.
inline constexpr float kScrapHalfMetres = 0.30F;
inline constexpr float kScrapLiftMetres = 0.05F;
inline constexpr math::Vec3 kScrapColour{0.30F, 0.28F, 0.26F};
/// A scrap mark is one quad, so six vertices, and it emits NONE when the field holds no litter.
inline constexpr std::size_t kScrapVertexCount = 6U;

/// CONTACT SHADOWS: a dark, translucent patch laid flat on the ground under each living unit, so a
/// hull reads as standing on the terrain and not as pasted onto it (BZ2's trick at this fidelity).
/// THIS IS A STAND-IN FOR REAL SHADOWING: the sun exists and faces are shaded by it, but nothing
/// casts, so there is no shadow to draw and this blob takes its place. The ink is near-black with
/// a slight green, and the alpha is low enough that the terrain texture still shows through.
inline constexpr math::Vec3 kShadowInk{0.02F, 0.03F, 0.02F};
inline constexpr float kShadowAlpha = 0.35F;
/// Lifted this far clear of the terrain, as the scrap plates are, so it does not fight the ground.
inline constexpr float kShadowLiftMetres = 0.03F;
/// How much wider than the hull its patch is, so the ink shows as ground around the base instead of
/// vanishing under it. ONE value for every kind, because every kind is drawn inside one hull width.
inline constexpr float kShadowFill = 1.2F;
/// A patch's half-side in metres: a shade wider than the hull standing on it.
/// THE SIM'S `Footprint` IS NOT THIS. A deployed building reserves a `kBuildingFootprintTiles`
/// square that no unit may enter -- four times the ground the structure drawn inside it covers (a
/// factory, a turret, a recycler and a tank are all drawn inside `kHullHalfMetres`). Sizing the
/// patch from that reservation put a black square two and a half times a building's own width
/// around it, which reads as a puddle and not as a shadow. Measure what is DRAWN.
inline constexpr float kShadowHalfMetres = kHullHalfMetres * kShadowFill;
/// One patch is one quad, so six vertices.
inline constexpr std::size_t kShadowVertexCount = 6U;

/// The deploy preview's two colours (BZ2's placement wireframe), and how its lines sit on the
/// ground: a stroke this many metres either side of the tile edge, lifted this far clear of the
/// terrain. GREEN means the sim would accept the deploy there, RED means it would refuse it -- the
/// colour is a claim about what SPACE will do, so it is read from the sim's own verdict
/// (`sim::deploy_site_ok`) and never re-derived here.
inline constexpr math::Vec3 kDeployPreviewColour{0.10F, 1.0F, 0.20F};
inline constexpr math::Vec3 kDeployPreviewBlockedColour{1.0F, 0.22F, 0.14F};
inline constexpr float kDeployPreviewHalfStrokeMetres = 0.08F;
inline constexpr float kDeployPreviewLiftMetres = 0.06F;
/// One stroke per tile edge of the footprint's grid: (side + 1) lines each way, `side` tiles long.
inline constexpr std::size_t kDeployPreviewVertexCount
    = std::size_t{2} * (sim::kBuildingFootprintTiles + 1) * sim::kBuildingFootprintTiles * 6U;

/// WHAT THE ARMED DEPLOY WOULD BUILD, and whether the sim would allow it there. The footprint is
/// where the reticle meets the ground; `valid` is `sim::deploy_site_ok`'s answer for that site,
/// carried rather than recomputed, because a second test of the same ground in the drawing layer is
/// a second thing that can disagree with the sim about what SPACE will do.
struct DeployPreview {
    sim::Footprint site{};
    bool valid{false};
};

/// The ledger's amber (C6), once the `SCRAP` label's ink and now only the command menu's. Public
/// because the command menu's leader line
/// and its dot are drawn in this exact colour -- the HUD's own, chosen by the user over a measured
/// green -- and a test that pins the line to it must be able to name it rather than copy its
/// numbers.
inline constexpr std::array<float, 3> kLedgerInk{1.0F, 0.85F, 0.30F};

/// THE COMMAND MENU'S LAYOUT (MENU-READ), in frame pixels, as a pure function of the rows, so the
/// geometry is tested without a frame. Every row is two bars side by side, a KEY bar hugging its
/// key and a VERB bar hugging its verb. The key column is RIGHT-aligned: every key bar ends at the
/// same x, so a digit sits a gutter away from its own verb whatever the widest key is, and a wide
/// key (`SPACE`, `TAB`) grows to the LEFT. The leader line runs down that gutter.
struct MenuRowLayout {
    float key_left{};   ///< the key bar's OUTER (rounded) end
    float key_right{};  ///< the key bar's INNER (square) edge: one x for every row
    float key_text{};   ///< where the key's first glyph starts
    float verb_left{};  ///< the verb bar's INNER (square) edge
    float verb_right{}; ///< the verb bar's OUTER (rounded) end; `verb_left` when there is no verb
    float verb_text{};  ///< where the verb's first glyph starts
    float right_left{}; ///< where the right column's text (a nav row's metres) starts
    float top{};        ///< the bars' top edge
    float height{};     ///< the bars' height
    float text_top{};   ///< every glyph's top on this row
    bool enabled{true}; ///< `MenuRow::enabled`: false draws the flat, unlit bar
};

/// The rows, and the one straight leader line down the gutter between the key and verb columns.
/// The line starts below the state row (row 0) and ends below the last row; a menu of one row has
/// no gutter to mark and `has_line` is false.
struct MenuLayout {
    std::vector<MenuRowLayout> rows;
    bool has_line{false};
    float line_left{};
    float line_right{};
    float line_top{};
    float line_bottom{};
};

/// `rows` laid out from the menu's top-left corner (`left`, `top`) at a glyph cell of `cell` px.
[[nodiscard]] MenuLayout menu_layout(std::span<const MenuRow> rows, float left, float top,
                                     float cell);

/// THE LEADER LINE'S WHOLE PATH (MENU-LEG), in frame pixels along the line's centre: down the
/// gutter from `line_top`, along the radar's top edge, and down onto the selected unit's blip, with
/// square elbows. At most four corners, so three segments, and the dot is on the last one.
struct MenuLeader {
    std::vector<std::array<float, 2>> points;
    bool dot{false}; ///< the last point is a blip, and a filled dot is drawn on it
};

/// The path for `layout`. With no radar drawn (`radar_top` absent) it is MENU-READ's one segment
/// down the gutter; with a radar and no `blip` it runs down the gutter to the radar's top edge and
/// stops there. A layout with no line has no path. `blip` is the selected unit's radar mark in
/// FRAME pixels, as the radar drew it -- this never projects a unit itself.
[[nodiscard]] MenuLeader menu_leader(const MenuLayout& layout, std::optional<float> radar_top,
                                     std::optional<std::array<float, 2>> blip);

/// ONE LEADER PER SELECTED UNIT (the group rule): `menu_leader` for each of `blips` in order, every
/// one the whole path from the menu, so a group is a fan of lines sharing the gutter and the top
/// edge, each ending in its own dot. With no blips it is the one bare leader, as `menu_leader`
/// makes it with none. `radar_top` is the top edge of whatever panel is drawn -- the radar's, or
/// the map's when the world map has taken its corner -- and the blips are on that panel, in FRAME
/// pixels.
[[nodiscard]] std::vector<MenuLeader> menu_leaders(const MenuLayout& layout,
                                                   std::optional<float> radar_top,
                                                   std::span<const std::array<float, 2>> blips);

/// THE ATTACK OFFER'S RED LINE (`SceneOptions::attack_target`): it leaves the menu where the first
/// row's verb bar ends -- the right side of the background round `SPACE Attack` -- at that bar's
/// vertical middle, the way the amber leader runs from the rows' own edge. Pure in the layout.
[[nodiscard]] std::array<float, 2> attack_line_origin(const MenuLayout& layout) noexcept;

/// WHERE A LIST ROW'S GREEN LEADER LEAVES (the repair list, AC): the right edge of what the row
/// draws -- past its right column's `right_glyphs` glyphs of `cell` px when it has one, else past
/// its verb bar -- a cell clear of the text, at the row's vertical middle. Pure in the layout.
[[nodiscard]] std::array<float, 2> row_line_origin(const MenuLayout& layout, std::size_t index,
                                                   std::size_t right_glyphs, float cell) noexcept;

/// The attack line's ink: red, and deliberately not the reticle's `kReticleHostile` or a unit bar's
/// fill, so the line is its own thing to find in a frame and to test.
inline constexpr std::array<float, 3> kAttackLineInk{1.0F, 0.10F, 0.10F};

/// THE REPAIR OFFER'S GREEN LINE (`SceneOptions::repair_target`): the attack line's mirror, leaving
/// the same place and as thick, ending in a dot on the patient. Green is the colour of what is
/// yours (Harri's rule: "the line says whose the thing is"), and deliberately not the reticle's or
/// a unit bar's green, so the line is its own thing to find in a frame and to test.
inline constexpr std::array<float, 3> kRepairLineInk{0.10F, 1.0F, 0.10F};

/// A menu bar's face is drawn as this many horizontal bands, top to bottom, one quad each.
inline constexpr int kMenuFaceBands = 8;

/// The colour of face band `band` (0 at the top) of a bar on an `enabled` row or not. An enabled
/// face is BZ2's blue highlight, measured off the user's capture: red stays nearly constant and
/// only green and blue fade, so it reads as blue light and not as a lighter card. A disabled face
/// is one flat dark colour with no gradient at all.
[[nodiscard]] std::array<float, 3> menu_face_colour(bool enabled, int band) noexcept;

/// The glyph's 5x5 mask -- the HUD's own, a lowercase letter reading its capital's -- box-filtered
/// onto a `rows` x `cols` grid of target pixels: rows*cols coverage values in 0..1, row-major. A
/// value is the exact share of that pixel's area the mask's lit cells cover, so a stem's interior
/// is 1 and a pixel straddling an edge is strictly between; that IS the menu's antialiasing, with
/// no smoothing pass after it. Empty if the glyph is unmapped (a space, a letter with no mask).
[[nodiscard]] std::vector<float> glyph_coverage(char glyph, int rows, int cols);

/// The pixel grid a menu glyph is drawn on when its capitals are `cap` px each way.
struct MenuGlyphGrid {
    int rows{};
    int cols{};
};

/// A capital, digit or sign fills `cap` x `cap`. A lowercase letter is its capital's mask at 0.71
/// of that on BOTH axes, rounded to whole pixels: small capitals, the user's ruling (MENU-FONT),
/// measured off BZ2's 7 px capitals and 5 px lowercase. It sits on the capitals' baseline.
[[nodiscard]] MenuGlyphGrid menu_glyph_grid(char glyph, int cap) noexcept;

/// The pool the deploy line points at: the tile of the pool `SPACE Deploy` would send a scavenger
/// to. The line ends on the middle of that tile's ground.
struct DeployLineTarget {
    int tile_x{};
    int tile_y{};
};

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
    /// Which faction each side plays, from the scenario. Harri's ruling: a Scavenger and a
    /// Harvester are the SAME machine, differing by FACTION -- humans vs aliens -- never by
    /// player/enemy side. The default, the player human and the enemy alien, is just a default: a
    /// story mission swaps it, so nothing may read `Side::Player` as "human". The sim never sees
    /// this.
    Factions factions{};
    /// The tank the radar is centred on. ABSENT means no radar, which is what a frame with no
    /// player unit in it gets: a radar with no centre would be a lie about where the tank is.
    sim::UnitId viewer_unit{};
    /// The hull the eye belongs to, whether or not the radar is on (`viewer_unit` is absent with
    /// `--no-radar`). The picker never names it: the camera rotates to positions outside it.
    sim::UnitId driven_unit{};
    /// A unit whose hull is drawn at `z` metres instead of on the ground: the hovering tank.
    /// ABSENT draws every unit on the ground, so a frame without one is unchanged.
    struct Hovering {
        sim::UnitId unit{};
        float z{};
    };
    std::optional<Hovering> hover{};
    /// How the viewer's own tank is oriented, so the drawn tank shows the steering model the player
    /// is using: `heading` is which way the body points and `turret` is how far the turret is
    /// turned relative to it, both in radians. `pitch` is the barrel's elevation (up is positive),
    /// the view's own pitch: the barrel is built in the hull's frame at that elevation, so the lean
    /// to the ground carries it with the hull. ABSENT draws every unit as an axis-aligned box, so a
    /// frame without one is byte-identical to a frame that never heard of this field.
    struct Facing {
        sim::UnitId unit{};
        float heading{};
        float turret{};
        float pitch{};
    };
    std::optional<Facing> facing{};
    /// Remembered headings, so a stopped tank keeps its facing. ABSENT (null) faces a stopped tank
    /// along +Y; a moving one still faces its velocity. Not owned.
    const Headings* headings{nullptr};
    /// The walk phases the Harvester's legs read (`Strides`). ABSENT (null) draws every leg at its
    /// static place, byte-identical to a frame that never heard of it. Not owned.
    const Strides* strides{nullptr};
    /// The world's per-tile answers for this seed, remembered (see game/terrain_cache.hpp): with it
    /// the terrain mesh reads a table instead of running the noise three octaves deep five times a
    /// tile, which is 78% of what a frame's mesh costs. ABSENT (null) derives every tile again, so
    /// a frame that never heard of it is unchanged; PRESENT the vertices are the same bytes,
    /// because the cache is a memo of a pure function and nothing else. Not owned, and ignored when
    /// its seed is not this frame's seed.
    const TerrainCache* terrain{nullptr};
    /// The red hit flash, 0 (none) to 1 (just hit): the app owns it and advances it each frame.
    /// 0 draws nothing at all, so a frame that is not flashing is byte-identical to one that never
    /// heard of it.
    float hit_flash{0.0F};
    /// The battle events the scene draws from: the `Killed` ones of the last `kSmokeTicks`, kept by
    /// `WakeLog` (a one-frame event could never drive a fourteen-second column). D4 reads the
    /// `Killed` ones and nothing else: each is a wreck, and a
    /// wreck leaves a burst and then a column of smoke (see `kBlastTicks`/`kSmokeTicks`). EMPTY
    /// emits nothing, so every frame that never heard of this field -- and every frame that lost no
    /// unit -- is byte-identical to what it drew before there was a wake at all.
    std::span<const sim::BattleEvent> events{};
    /// The reticle's pulse, 0 (rest) to 1 (just connected): the app owns it. 0 draws the reticle
    /// at rest, byte-identical to one that never heard of it.
    float reticle_pulse{0.0F};
    /// The weapon the driven craft has up, which picks the reticle's SHAPE (`reticle_for`): the
    /// gun is four ticks round an open centre, so a frame that never heard of it is byte-identical.
    /// The weapon's NAME is never drawn at the reticle (Harri, 2026-11-03: the ship instrument
    /// says it).
    Weapon weapon{Weapon::Gun};
    /// The driven weapon is reloading: the reticle is drawn spent (a dim ghost of itself) until it
    /// is ready again. False draws it as it always was.
    bool reloading{false};
    /// The player's landing markers, drawn standing at their touchdown tiles. Null draws none.
    /// Not owned.
    const LandingMarks* landing_marks{nullptr};
    /// The player's navigational marks (N1), drawn as a slim beacon at each tile. Null draws none.
    /// Not owned.
    const NavMarks* nav_marks{nullptr};
    /// Who moved or fired lately, for the radar's and the map's detection model (the tail). Null
    /// knows only physical motion. Not owned.
    const RadarActivity* radar_activity{nullptr};
    /// DEPLOY's preview: the footprint the armed deploy would build where the reticle meets the
    /// ground, drawn lying on the terrain as its tile grid -- `kDeployPreviewColour` where the sim
    /// would accept it, `kDeployPreviewBlockedColour` where it would not. ABSENT draws nothing, so
    /// a frame that is not deploying is byte-identical to one that never heard of it.
    std::optional<DeployPreview> deploy_preview{};
    /// The command row (ROW-SLOTS) along the top of the frame: `~`, `1`, `2`, `3`, `F1`..`F10`,
    /// each in its own fixed spot, with only the keys that hold something drawn. Null draws none,
    /// so a frame that never heard of it is unchanged -- the same discipline as `nav_marks`. Also
    /// what F1-F10 select from and Shift+F1-F10 rearrange (`Interaction`). NOT owned. Non-const
    /// for that one writer; the scene only reads it.
    RowState* row{nullptr};
    /// One line under the row, for what a function key just said ("Unit 4 is destroyed."). Empty
    /// draws none. The app holds it for a few seconds; the scene only draws it.
    std::string notice{};
    /// The numbered command menu (RB0.5): `menu_rows`'s rows drawn down the left of the frame,
    /// under the strip and the notice line, in the order given -- the gap where 6, 7 and 8 would
    /// be is the data and stays open. Off by default, so a frame that never heard of it is
    /// unchanged.
    bool show_menu{false};
    /// The rows `show_menu` draws. Null or empty draws nothing, so a frame that never heard of it
    /// is byte-identical. NOT owned. The attack list, the repair list, the unit list and the nav
    /// marks are all rows of THIS menu (AW): no list has a drawing of its own.
    const std::vector<MenuRow>* menu{nullptr};
    /// THE ATTACK OFFER (SPACE ATTACK): the hostile asset SPACE would attack right now -- the
    /// crosshair's target (`attack_target`, filled by the app each frame, so the scene stays a pure
    /// function of the sim and these options). While it names a living unit the scene draws a red
    /// line from the menu's `SPACE Attack` row to the unit, a small label of its kind above it
    /// and, under the label, a bar of its health that shrinks from the right and changes colour as
    /// the unit is hurt. An invalid id (the default) draws none of it, so a frame that never heard
    /// of the field is byte-identical.
    sim::UnitId attack_target{};
    /// THE REPAIR OFFER (SPACE REPAIR), the attack offer's mirror: the damaged friendly SPACE would
    /// send a service truck to mend (`repair_target`, filled by the app each frame). While it names
    /// a living unit AND the menu's state row reads `SPACE Repair` the scene draws a green line
    /// from that row to the unit, with a green dot on it. An invalid id (the default) draws none of
    /// it, so a frame that never heard of the field is byte-identical.
    sim::UnitId repair_target{};
    /// THE DEPLOY LINE (see `kDeployLineInk`): the pool `SPACE Deploy` would send the lone selected
    /// scavenger to, filled by the app each frame from the same offer that puts the verb on the
    /// menu. While present AND the menu's state row reads `SPACE Deploy`, the scene draws one thin
    /// white line from that row to the pool's centre. ABSENT (the default) draws none of it, so a
    /// frame that never heard of the field is byte-identical.
    std::optional<DeployLineTarget> deploy_pool{};
    /// THE GO HERE LINE (round 10, 2026-11-05: "a white line from SPACE Go Here to the spot on the
    /// ground"): the tile `SPACE Go Here` would send the selection to, filled by the app each frame
    /// from the same offer that puts the verb on the menu. The same white line as the deploy's
    /// (`kDeployLineInk`), drawn only while the menu's state row reads `SPACE Go Here`. ABSENT (the
    /// default) draws none of it, so a frame that never heard of the field is byte-identical.
    std::optional<DeployLineTarget> go_here_spot{};
    /// THE GET NAV LINE (round 12, AU: "a thin white line going to the spot where the nav beacon
    /// would go"): the ground tile `SPACE GET NAV` would drop a mark on -- the crosshair's own
    /// ground point (`nav_aim`), filled by the app only while the nav chain is placing. The same
    /// white line as the deploy's and Go Here's (`kDeployLineInk`), drawn only while the menu's
    /// state row reads `SPACE GET NAV`. ABSENT (the default) draws none of it, so a frame that
    /// never heard of the field is byte-identical.
    std::optional<DeployLineTarget> nav_spot{};
    /// THE ASSIST (`T`, see game/assist.hpp): the hostile asset the player has latched. While it
    /// names a living, available unit the scene draws a thin red line from it to a small red dot
    /// where the barrel must point for `weapon` to hit it if it holds its course, and gives it the
    /// floating health bar even when it is a structure. An invalid id (the default) draws none of
    /// it, so a frame that never heard of the field is byte-identical. Filled by the app from the
    /// session; the scene stays a pure function of the sim and these options.
    sim::UnitId assist_target{};
    /// THE READOUT LOCK (`T`, see game/attack_aim.hpp `pointed_asset`): an asset of either side
    /// whose name, health bar and distance stay up while the crosshair is elsewhere, until `T`
    /// again or it dies. Pointing at a unit shows the same readout without it. An invalid id (the
    /// default) locks nothing, so a frame that never heard of the field is byte-identical. Filled
    /// by the app from the session.
    sim::UnitId readout_lock{};
    /// The world map (N4), drawn last over everything: the whole world as a square, your own
    /// assets and your nav marks. Off by default, so a frame that never heard of it is unchanged.
    bool show_world_map{false};
    /// The ship instrument (SHIPGAUGE-1) in the bottom right: the hull and ammo arcs, the two
    /// readings in the ring's opening and the four carried weapons beside it. ON by default,
    /// because the game draws it; off lets a test or the calibration view measure another HUD
    /// layer without the instrument's vertices in the frame.
    bool show_ship_gauge{true};
    /// The radar's terrain wireframe (RADAR3D-2): the ground under the marks, as a heightfield
    /// grid on the tilted plane. ON by default, because the game draws it; off lets the plan's
    /// pixel acceptance compare a mesh-on frame against a mesh-off one and demand that the two
    /// differ only inside the oval. A frame with no radar at all (`viewer_unit` absent) draws no
    /// mesh either way.
    bool show_radar_mesh{true};
    /// RADAR3D-2's dial, in tiles: the wireframe's grid pitch. Coarser is cheaper and each step up
    /// costs roughly a quarter of the vertices -- measured in `protekt-meshcost`, which sweeps it
    /// so the frame price of the dial is a number rather than a guess. The default is the shipped
    /// look.
    int radar_mesh_step_tiles{kRadarMeshStepTiles};
    /// The wreckage on the ground (C5): C1's litter, drawn as a small dark plate where each piece
    /// lies, so a player can SEE where the fighting has been. ON by default, because the game draws
    /// it -- but the layer emits NOTHING while `Sim::scraps()` is empty, so a battle that has lost
    /// no unit is byte-identical to one that never heard of this field.
    bool show_scrap{true};
    /// The biometal pools venting (`kPoolVentColour`): drawn in the world for every pool in view,
    /// ON by default because the game draws it, and emitting NOTHING for a sim with no pools.
    bool show_pools{true};
    /// The floating motes (`kMoteCount`): a dozen specks drifting near the camera. OFF by default
    /// so a frame that never heard of them is byte-identical; the game turns them on.
    bool show_motes{false};
    /// THE STORE GAUGE beside the radar (`store_gauge.hpp`), the HUD layer of the scrap economy:
    /// C3's per-side count of banked loads, drawn as a segmented column and the number under it, so
    /// the player can see the currency a queue spends. (It was the ledger's `SCRAP` plate too,
    /// until Harri dropped that, 2026-11-03.) ON by default, because the game draws it; the shared
    /// test rigs turn it off, the same way they already do for the ship instrument, because a HUD
    /// layer that always draws becomes the tail of every vertex list.
    bool show_ledger{true};
    /// The verdict (C8): the one line that says the war is over, drawn only once a recycler has
    /// died (`Sim::war_outcome`) and emitting NOTHING while it is undecided -- so a battle still
    /// being fought is byte-identical to one that never heard of this field.
    bool show_outcome{true};
    /// THE STATISTICS SCREEN (AF): when set, the counts it points at are drawn over the whole frame
    /// -- built and destroyed, scrap, shots, and the time played -- for BOTH sides, on a black
    /// plate in the nearest layer there is. Null (the default) emits NOTHING, so every frame that
    /// is not paused is byte-identical to one that never heard of this field. The game points it at
    /// its tally only while it is paused. Not owned.
    const BattleStats* statistics{nullptr};
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

/// The vehicle-type icon's reserved square under a command-row key's plate, in frame pixels: the
/// ONE place the box is computed, so the layout that reserves it (the ticks start under it) and the
/// drawing that fills it cannot drift apart. `slot_left` is the key's left edge and `plate_top` its
/// plate's top; the square is left-justified on the key, `size` on a side, a gap below the plate.
/// `slot_width` is the key's drawn width, which the square never exceeds: at the design width the
/// icon is 72 px and a frame too narrow for fourteen keys narrows the key and the icon with it.
struct IconBox {
    float left{};
    float top{};
    float size{};
};
[[nodiscard]] IconBox key_icon_box(float slot_left, float plate_top, float px,
                                   float slot_width) noexcept;

/// THE COMMAND STRIP'S DESIGN SIZE, in pixels at 720 (see `append_hud_panels`): a key's plate is
/// `kStripSlotWidthPixels` by `kStripSlotHeightPixels`, the icon square under it
/// `kStripIconPixels` on a side, then the row of ticks; slots `kStripSlotWidthPixels +
/// kStripSlotGapPixels` apart. Public because the panels hung under the strip start below its
/// bottom edge, which `strip_bottom_pixels` gives, and a test holds the strip clear of the radar.
inline constexpr float kStripMarginPixels = 6.0F;
inline constexpr float kStripSlotWidthPixels = 72.0F;
inline constexpr float kStripSlotGapPixels = 6.0F;
inline constexpr float kStripSlotHeightPixels = 34.0F;
inline constexpr float kStripStackGapPixels = 1.0F;
inline constexpr float kStripIconPixels = 72.0F;
inline constexpr float kStripTickHeightPixels = 14.0F;

/// The bottom edge of the tallest thing a key hangs (plate, gap, icon, gap, ticks), in pixels at
/// 720 from the frame's top: 6 + 34 + 1 + 72 + 1 + 14 = 128.
inline constexpr float kStripBottomPixels = kStripMarginPixels + kStripSlotHeightPixels
                                            + kStripStackGapPixels + kStripIconPixels
                                            + kStripStackGapPixels + kStripTickHeightPixels;

/// THE PANELS HUNG UNDER THE STRIP, at 720: the notice's plate pad and glyph cell, where the
/// notice's text starts (`kNoticeTopPixels`: the strip's bottom, a margin, a pad), and where the
/// command menu's first row starts under the notice's reserved row (`kMenuTopPixels`: the notice's
/// text height of five 2 px cells, then two pads). The attack list is laid out the same way.
inline constexpr float kStripPadPixels = 3.0F;
inline constexpr float kStripNoticeCell = 2.0F;
inline constexpr float kNoticeTopPixels = kStripBottomPixels + kStripMarginPixels + kStripPadPixels;
inline constexpr float kMenuTopPixels
    = kNoticeTopPixels + (5.0F * kStripNoticeCell) + (2.0F * kStripPadPixels);

/// The icon's ink: pale and plain whatever the key's side or selection (the TYPE is shown, not
/// whose it is), so a frame reading can tell it from the plate's frame and from the ticks.
inline constexpr std::array<float, 3> kStripIconInk{0.80F, 0.80F, 0.80F};

/// THE BATCH PLATE (BATCH-PLATE). A key held for a shift batch that is still being built draws its
/// icon as a ghost -- the silhouette dimmed -- and fills it with `kStripIconInk` from the bottom up
/// as the unit on the line is built, one cell row at a time (`BatchReading::fill`). Dim enough to
/// read as unfilled, light enough to stand off the outline (`kStripIconOutline`).
inline constexpr std::array<float, 3> kStripIconGhost{0.38F, 0.40F, 0.44F};

/// THE TICK OUTLINE (Harri, round 10, 2026-11-05: "a thin dark grey outline" in place of "the black
/// gaps between them"): every tick of a key is ringed in this dark grey, `kStripTickOutlinePixels`
/// wide (at 720) just OUTSIDE its box -- one quad behind the tick, so a blank tick's hollow middle
/// shows it too. Neighbours stay apart because the gap between ticks is 3 px and two outlines take
/// one each. It replaces the one dark track that ran behind the whole row.
inline constexpr std::array<float, 3> kStripTickOutline{0.30F, 0.30F, 0.30F};
inline constexpr float kStripTickOutlinePixels = 1.0F;

/// THE BUILD GAUGE (Harri, round 10: "the visual effect should be visible in areas that don't have
/// any part of the icon"): while a key holds a batch still being built, its icon's whole reserved
/// square is a gauge on the plate -- `kStripBuildTrack` where the build has yet to reach, and
/// `kStripBuildFill` from the bottom up to the share built (`BatchReading::fill`, one cell row at
/// a time), the glyph drawn over it. Both go with the batch: a key with nothing left to build
/// draws neither.
inline constexpr std::array<float, 3> kStripBuildFill{0.18F, 0.46F, 0.66F};
inline constexpr std::array<float, 3> kStripBuildTrack{0.10F, 0.12F, 0.15F};

/// A tick that stands for a unit still to be built: BLANK, a hollow tick -- the tick's own box
/// drawn as a frame `kStripBlankTickEdgePixels` wide (at 720) in this pale grey, the dark track
/// showing through. Distinct from every colour of the health ramp (`bar_segments`), so a frame
/// reading can tell a blank from a unit.
inline constexpr std::array<float, 3> kStripBlankTickEdge{0.70F, 0.72F, 0.76F};
inline constexpr float kStripBlankTickEdgePixels = 1.0F;
/// THE PICKED TICK (`~` STEPPING): when only SOME of a key's vehicles are selected -- `~` has
/// narrowed the group to one -- each selected vehicle's tick is ringed in the selected yellow
/// (`kStripSelected`), this many pixels wide (at 720) just OUTSIDE the tick's box, so the health
/// colour inside it still reads. The whole group selected rings nothing. Side by side the rings
/// of neighbours stay apart because the gap between ticks is 3 px and a ring takes one of it.
inline constexpr float kStripPickedTickEdgePixels = 1.0F;

/// THE DEPLOY LEADER LINE: while `SPACE Deploy` is the menu's state row, a thin white line runs
/// from the end of that row's bar to the centre of the pool the deploy would go to (Harri,
/// 2026-11-03: "a white thin line pointing from SPACE Deploy to the centre of the pool"). White so
/// it is never the attack line's red or the leader's amber; one pixel wide at 720, thinner than
/// either.
inline constexpr std::array<float, 3> kDeployLineInk{1.0F, 1.0F, 1.0F};
inline constexpr float kDeployLinePixels = 1.0F;

/// The icon's outline: one cell wide round the lit silhouette, the keycap's own near-black
/// (`kStripOutline`), so the pale ink reads over bright sky and sand alike.
inline constexpr std::array<float, 3> kStripIconOutline{0.02F, 0.03F, 0.05F};

} // namespace protekt::game
