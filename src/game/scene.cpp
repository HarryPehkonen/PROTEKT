#include "protekt/game/scene.hpp"

#include "protekt/game/assist.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/attack_aim.hpp"
#include "protekt/game/kind_icon.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/site_ground.hpp"
#include "protekt/game/store_gauge.hpp"
#include "protekt/game/vehicle_shape.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/game/world_map.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/spec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace protekt::game {

namespace {

using math::add;
using math::cross;
using math::dot;
using math::kPi;
using math::look_at;
using math::Mat4;
using math::multiply;
using math::normalise;
using math::perspective;
using math::Vec3;

Vec3 side_colour(sim::Side side) {
    return side == sim::Side::Player ? Vec3{0.15F, 0.35F, 0.85F} : Vec3{0.85F, 0.15F, 0.15F};
}

/// `alpha` defaults to 1, which makes the renderer's blend a no-op, so every caller that does not
/// ask for transparency emits exactly the vertices it always did.
void emit_triangle(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                   const Vec3& colour, float alpha = 1.0F, const Vec3& normal = Vec3{}) {
    for (const Vec3* corner : {&a, &b, &c}) {
        out.push_back(SceneVertex{*corner, colour, alpha, 0.0F, 0.0F, 0.0F, normal});
    }
}

void emit_quad(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
               const Vec3& d, const Vec3& colour, float alpha = 1.0F) {
    emit_triangle(out, a, b, c, colour, alpha);
    emit_triangle(out, a, c, d, colour, alpha);
}

/// A quad that samples one whole atlas tile: its corners a, b, c, d take the tile's corners
/// top-left, top-right, bottom-right, bottom-left. The colour still multiplies the texel. `normal`
/// is the quad's outward unit normal for the fragment stage's light (AP), carried by all four
/// corners; the zero default is "not lit", the colour as it is.
void emit_tiled_quad(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                     const Vec3& d, const Vec3& colour, const UvRect& tile, int orientation = 0,
                     const Vec3& normal = Vec3{}) {
    const auto corner = [&](const Vec3& p, int which) {
        const auto uv = oriented_corner_uv(tile, orientation, which);
        return SceneVertex{p, colour, 1.0F, uv.at(0), uv.at(1), 0.0F, normal};
    };
    const SceneVertex va = corner(a, 0);
    const SceneVertex vb = corner(b, 1);
    const SceneVertex vc = corner(c, 2);
    const SceneVertex vd = corner(d, 3);
    for (const SceneVertex& v : {va, vb, vc, va, vc, vd}) {
        out.push_back(v);
    }
}

/// The tile's terrain, from the cache when there is one for THIS seed and from the world otherwise.
[[nodiscard]] world::Terrain cached_terrain(const TerrainCache* cache, std::uint64_t seed,
                                            int tile_x, int tile_y) noexcept {
    if (cache != nullptr && cache->seed() == seed) {
        return cache->terrain(tile_x, tile_y);
    }
    return world::sample(seed, tile_x, tile_y).terrain;
}

/// A tile corner's height in metres, from the cache when there is one for this seed.
/// `height_metres` is the only other way to ask and the two must agree: the cache's answer IS that
/// function's answer, remembered (TerrainCache.MatchesTheWorld pins it), which is why this is a
/// memo and not a second source of truth about the ground.
[[nodiscard]] float cached_height_metres(const TerrainCache* cache, std::uint64_t seed, int tile_x,
                                         int tile_y) noexcept {
    if (cache != nullptr && cache->seed() == seed) {
        return static_cast<float>(cache->height_units(tile_x, tile_y)) * kMetresPerHeightUnit;
    }
    return height_metres(seed, tile_x, tile_y);
}

/// The outward unit normal of a face from a normal vector `raw` (a cross product, of any length),
/// turned to point away from the solid's centre: `away` is a vector from that centre to a point on
/// the face. The zero vector for a degenerate face, which has no area to light and so reads as
/// "not lit".
Vec3 outward_unit(const Vec3& raw, const Vec3& away) {
    const float length = std::sqrt(dot(raw, raw));
    if (!(length > 1.0e-12F)) {
        return Vec3{};
    }
    const float sign = dot(raw, away) < 0.0F ? -1.0F : 1.0F;
    return Vec3{sign * raw[0] / length, sign * raw[1] / length, sign * raw[2] / length};
}

/// The normal of one terrain quad, for the fragment stage's light.
///
/// THE NORMAL is the cross product of the quad's two diagonals, (p01 - p10) x (p11 - p00). That is
/// the area-weighted mean of the two triangles' normals for EITHER split of the quad (so it does
/// not care that the mesh cuts along p00-p11), and for a flat quad it is exactly straight up. One
/// normal for the quad's six vertices; `smooth_vertex_normals` then blends it with the neighbours'.
Vec3 terrain_normal(const Vec3& p00, const Vec3& p10, const Vec3& p11, const Vec3& p01) {
    const Vec3 across{p01[0] - p10[0], p01[1] - p10[1], p01[2] - p10[2]};
    const Vec3 along{p11[0] - p00[0], p11[1] - p00[1], p11[2] - p00[2]};
    return normalise(cross(across, along));
}

void build_terrain(std::vector<SceneVertex>& out, const TerrainCache* cache, std::uint64_t seed,
                   int centre_x, int centre_y, int radius, const SiteGroundMap& sites) {
    // Each tile's corners are sampled one tile beyond it on the far edge (tx+1, ty+1); widening
    // the low edge by one tile too keeps the sampled HEIGHT field symmetric around the camera,
    // even though the tolerance on the mesh's visual extent absorbs the extra row.
    // Each quad takes its tile with x along u and z along v, the same way round everywhere, so
    // neighbours of one type, variant and orientation continue each other's (wrapping) paint, and
    // neighbours of two variants or orientations meet in the same tones (D7 turns and mirrors the
    // UV corners per tile, never the tile).
    const AtlasManifest& atlas = atlas_manifest();
    std::array<std::array<UvRect, kGroundVariantCount>, world::kTerrainCount> tiles{};
    for (int t = 0; t < world::kTerrainCount; ++t) {
        for (int v = 0; v < kGroundVariantCount; ++v) {
            tiles.at(static_cast<std::size_t>(t)).at(static_cast<std::size_t>(v))
                = atlas.rect(terrain_variant_name(static_cast<world::Terrain>(t), v));
        }
    }
    // THE ALTERED GROUND (`site_ground.hpp`): a tile a deployed thing reaches is painted from its
    // kind's own atlas tile, upright (orientation 0) so the plating's seams run on from tile to
    // tile. Its HEIGHT is the ground's, untouched: only the paint changes.
    std::array<UvRect, kSiteKindCount> site_tiles{};
    for (std::size_t kind = 0; kind < site_tiles.size(); ++kind) {
        site_tiles.at(kind) = atlas.rect(site_ground(static_cast<SiteKind>(kind)).tile);
    }
    for (int ty = centre_y - radius - 1; ty <= centre_y + radius; ++ty) {
        for (int tx = centre_x - radius - 1; tx <= centre_x + radius; ++tx) {
            const world::Terrain terrain = cached_terrain(cache, seed, tx, ty);
            const std::optional<SiteKind> site = sites.at(tx, ty);
            const UvRect& tile
                = site.has_value()
                      ? site_tiles.at(static_cast<std::size_t>(*site))
                      : tiles.at(static_cast<std::size_t>(terrain))
                            .at(static_cast<std::size_t>(variant_index(terrain, tx, ty)));
            const float x0 = static_cast<float>(tx) * kMetresPerTile;
            const float x1 = static_cast<float>(tx + 1) * kMetresPerTile;
            const float z0 = static_cast<float>(ty) * kMetresPerTile;
            const float z1 = static_cast<float>(ty + 1) * kMetresPerTile;
            const Vec3 p00{x0, cached_height_metres(cache, seed, tx, ty), z0};
            const Vec3 p10{x1, cached_height_metres(cache, seed, tx + 1, ty), z0};
            const Vec3 p11{x1, cached_height_metres(cache, seed, tx + 1, ty + 1), z1};
            const Vec3 p01{x0, cached_height_metres(cache, seed, tx, ty + 1), z1};
            emit_tiled_quad(out, p00, p10, p11, p01, terrain_base_colour(), tile,
                            site.has_value() ? 0 : orientation_index(terrain, tx, ty),
                            terrain_normal(p00, p10, p11, p01));
        }
    }
}

/// The sky dome, centred on the camera. Raw colours, never shaded. See `kSkySegments`.
void append_sky(std::vector<SceneVertex>& out, const Camera& camera) {
    const Vec3 eye{camera.x, camera.y, camera.z};
    const auto at = [&](const Vec3& direction, float radius) {
        return Vec3{eye[0] + (direction[0] * radius), eye[1] + (direction[1] * radius),
                    eye[2] + (direction[2] * radius)};
    };
    const auto ring_point = [&](std::size_t ring, int segment) {
        const float elevation = kSkyRingElevationDegrees.at(ring) * kPi / 180.0F;
        const float azimuth = 2.0F * kPi * static_cast<float>(segment) / kSkySegments;
        const float flat = std::cos(elevation);
        return at(Vec3{std::sin(azimuth) * flat, std::sin(elevation), std::cos(azimuth) * flat},
                  kSkyRadiusMetres);
    };
    for (std::size_t ring = 0; ring + 1 < kSkyRings; ++ring) {
        const Vec3& low = kSkyRingColours.at(ring);
        const Vec3& high = kSkyRingColours.at(ring + 1);
        const bool cap = ring + 2 == kSkyRings;
        for (int segment = 0; segment < kSkySegments; ++segment) {
            const Vec3 a = ring_point(ring, segment);
            const Vec3 b = ring_point(ring, segment + 1);
            const Vec3 c = ring_point(ring + 1, segment + 1);
            const Vec3 d = ring_point(ring + 1, segment);
            out.push_back(SceneVertex{a, low});
            out.push_back(SceneVertex{b, low});
            out.push_back(SceneVertex{c, high});
            if (!cap) {
                out.push_back(SceneVertex{a, low});
                out.push_back(SceneVertex{c, high});
                out.push_back(SceneVertex{d, high});
            }
        }
    }

    // A disc is a fan on the plane perpendicular to its direction, a hair inside the dome so it
    // wins the depth test. The sun sits on exactly the direction the prisms are lit from.
    constexpr float kDiscRadius = 0.98F * kSkyRadiusMetres;
    const auto disc
        = [&](const Vec3& toward, float angular_radius, int segments, const Vec3& colour) {
              const Vec3 direction = normalise(toward);
              const Vec3 across = normalise(cross(Vec3{0.0F, 1.0F, 0.0F}, direction));
              const Vec3 up = cross(direction, across);
              const Vec3 centre = at(direction, kDiscRadius);
              const auto rim = [&](int segment) {
                  const float angle
                      = 2.0F * kPi * static_cast<float>(segment) / static_cast<float>(segments);
                  const float scale = kDiscRadius * angular_radius;
                  const float u = std::cos(angle) * scale;
                  const float v = std::sin(angle) * scale;
                  return Vec3{centre[0] + (across[0] * u) + (up[0] * v),
                              centre[1] + (across[1] * u) + (up[1] * v),
                              centre[2] + (across[2] * u) + (up[2] * v)};
              };
              for (int segment = 0; segment < segments; ++segment) {
                  emit_triangle(out, centre, rim(segment), rim(segment + 1), colour);
              }
          };
    constexpr float kSunAngularSize = 0.06F; ///< rim radius as a fraction of the distance
    disc(kSunDirection, kSunAngularSize, kSkySegments, kSunDiscColour);
    disc(kMoonDirection, kMoonAngularRadius, kMoonSegments, kMoonDiscColour);
}

/// Fog the vertices in [first, last) by their distance from the eye: store each one's fog AMOUNT
/// and leave its colour the raw lighting colour, because the fragment shader mixes the fog in after
/// the texture (mixing it into the colour let the texture's multiply crush it). THE ONE PLACE the
/// fog is applied: `build_scene` calls it, through `fog_world`, on the world's runs of the vertex
/// list less the sparks' (AH) and on nothing else.
void fog_range(std::vector<SceneVertex>& vertices, std::size_t first, std::size_t last,
               const Camera& camera, int radius_tiles) {
    const float far = fog_far_metres(radius_tiles);
    for (std::size_t i = first; i < last; ++i) {
        SceneVertex& v = vertices.at(i);
        const float dx = v.position[0] - camera.x;
        const float dy = v.position[1] - camera.y;
        const float dz = v.position[2] - camera.z;
        v.fog = fog_amount(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)), far);
    }
}

/// A run [first, last) of the vertex list that the fog must leave alone: EMISSIVE geometry, which
/// is to say a spark (AH). A hot thing does not fade into the horizon, and Harri asked for sparks
/// he can see from farther away -- the distance fog is what was taking them, not their size.
struct VertexRun {
    std::size_t first{};
    std::size_t last{};
};

/// `fog_range` over [first, last) EXCEPT the `emissive` runs, which are in list order. Their
/// vertices keep the fog amount they were built with, which is none.
void fog_world(std::vector<SceneVertex>& vertices, std::size_t first, std::size_t last,
               const std::vector<VertexRun>& emissive, const Camera& camera, int radius_tiles) {
    std::size_t at = first;
    for (const VertexRun& run : emissive) {
        fog_range(vertices, at, run.first, camera, radius_tiles);
        at = run.last;
    }
    fog_range(vertices, at, last, camera, radius_tiles);
}

/// A selected unit keeps its side's colour and is mixed towards white, so a glance tells you both
/// whose it is and that it is yours to move.
Vec3 highlight(const Vec3& colour) {
    constexpr float kMix = 0.55F;
    return Vec3{colour[0] + ((1.0F - colour[0]) * kMix), colour[1] + ((1.0F - colour[1]) * kMix),
                colour[2] + ((1.0F - colour[2]) * kMix)};
}

/// The unit under the crosshair is mixed toward a warning colour, so the player can see what a
/// click there would act on -- the reticle says it in colour, this says it on the unit itself.
/// Own and hostile are told apart: amber for yours ("do not shoot this"), a hot salmon-red for a
/// hostile (the reticle's red family, lifted in green and blue because the enemy's base colour is
/// already a dark red and a plain red mix would barely move it). Neither is a white mix, so neither
/// can be mistaken for `highlight`; the mix is strong (80%) so each reads apart from the unmarked
/// side colour at a glance.
Vec3 aim_tint(const Vec3& colour, bool own) {
    constexpr float kMix = 0.8F;
    constexpr Vec3 kOwnWarning{1.0F, 0.70F, 0.10F};
    constexpr Vec3 kHostileWarning{1.0F, 0.45F, 0.35F};
    const Vec3& warning = own ? kOwnWarning : kHostileWarning;
    return Vec3{colour[0] + ((warning[0] - colour[0]) * kMix),
                colour[1] + ((warning[1] - colour[1]) * kMix),
                colour[2] + ((warning[2] - colour[2]) * kMix)};
}

/// How a unit is marked this frame. Precedence: SELECTED wins over AIMED, because selection is the
/// state the player set on purpose and must never be masked; the aim cue only decorates a unit
/// that is not selected.
enum class Mark : std::uint8_t { None, Selected, AimedOwn, AimedHostile };

/// A local offset turned by `angle` and added to a world point on the map plane. Local +z is
/// FORWARD and local +x is across it; the angle is a camera yaw, so forward lands on
/// `(sin angle, cos angle)` -- exactly the `math::basis_from` forward the view is built from, in
/// which yaw 0 is +Z and a positive yaw turns toward +X (the screen's RIGHT). The sign is pinned by
/// `SceneFacing.TheBarrelPointsWhereTheTurretLooks`.
std::array<float, 2> rotate_offset(float x, float z, float dx, float dz, float angle) {
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    return {x + (dx * cosine) + (dz * sine), z - (dx * sine) + (dz * cosine)};
}

/// A prism in a frame turned by `angle` about the point (x, z): it spans `left` to `right` across,
/// and its side profile is a quadrilateral -- ground-level ends `back_bottom` and `front_bottom`
/// along the heading, top ends `back_top` and `front_top`, heights `y0` at the bottom and
/// `back_y1` / `front_y1` at the top. A box is the case where the top ends equal the bottom ends;
/// a slanted front plate (a glacis) is `front_top` short of `front_bottom`. Its faces are the six
/// `emit_shaded_solid` emits, in that order and winding; a turn about the vertical axis keeps the
/// winding.
struct Prism {
    float left;
    float right;
    float back_bottom;
    float back_top;
    float front_bottom;
    float front_top;
    float y0;
    float back_y1;
    float front_y1;
};

/// One face of a prism, with its WORLD normal. The normal comes from the face's own corners, so the
/// turn by `angle` and any slant are already in it; it is flipped to point away from the prism's
/// centre, so it does not depend on the winding. The colour is left UNLIT: the fragment stage
/// multiplies the light in from the (smoothed) normal.
void emit_shaded_face(std::vector<SceneVertex>& out, const std::array<Vec3, 8>& p,
                      const Vec3& centre, std::size_t a, std::size_t b, std::size_t c,
                      std::size_t d, const Vec3& colour, const UvRect& tile) {
    const Vec3 ab{p.at(b)[0] - p.at(a)[0], p.at(b)[1] - p.at(a)[1], p.at(b)[2] - p.at(a)[2]};
    const Vec3 ac{p.at(c)[0] - p.at(a)[0], p.at(c)[1] - p.at(a)[1], p.at(c)[2] - p.at(a)[2]};
    const Vec3 outward{(p.at(a)[0] + p.at(c)[0]) * 0.5F - centre[0],
                       (p.at(a)[1] + p.at(c)[1]) * 0.5F - centre[1],
                       (p.at(a)[2] + p.at(c)[2]) * 0.5F - centre[2]};
    emit_tiled_quad(out, p.at(a), p.at(b), p.at(c), p.at(d), colour, tile, 0,
                    outward_unit(cross(ab, ac), outward));
}

/// The eight corners of a prism in a frame turned by `angle` about (x, z), in the order the faces
/// below index them: back-bottom left/right, back-top right/left, then the same at the front.
std::array<Vec3, 8> prism_corners(float x, float z, float angle, const Prism& s) {
    const auto corner = [&](float dx, float dz, float y) {
        const std::array<float, 2> at = rotate_offset(x, z, dx, dz, angle);
        return Vec3{at.at(0), y, at.at(1)};
    };
    return {{corner(s.left, s.back_bottom, s.y0), corner(s.right, s.back_bottom, s.y0),
             corner(s.right, s.back_top, s.back_y1), corner(s.left, s.back_top, s.back_y1),
             corner(s.left, s.front_bottom, s.y0), corner(s.right, s.front_bottom, s.y0),
             corner(s.right, s.front_top, s.front_y1), corner(s.left, s.front_top, s.front_y1)}};
}

/// SHADING ORDER: `colour` is the FINAL unit colour -- the side colour after `highlight` or
/// `aim_tint` has picked the mark -- and the face light multiplies it last, so a selection or aim
/// cue is tinted by the light like everything else but is never replaced by it.
/// `top_tile` is the top face's tile; the other five faces take `tile`.
/// The six faces of a solid with prism-ordered corners `p`, shaded by their world normals.
void emit_shaded_solid(std::vector<SceneVertex>& out, const std::array<Vec3, 8>& p,
                       const Vec3& colour, const UvRect& tile, const UvRect& top_tile) {
    Vec3 centre{0.0F, 0.0F, 0.0F};
    for (const Vec3& corner_point : p) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            centre.at(axis) += corner_point.at(axis) * 0.125F;
        }
    }
    emit_shaded_face(out, p, centre, 0, 1, 2, 3, colour, tile);     // near
    emit_shaded_face(out, p, centre, 5, 4, 7, 6, colour, tile);     // far
    emit_shaded_face(out, p, centre, 4, 0, 3, 7, colour, tile);     // left
    emit_shaded_face(out, p, centre, 1, 5, 6, 2, colour, tile);     // right
    emit_shaded_face(out, p, centre, 3, 2, 6, 7, colour, top_tile); // top
    emit_shaded_face(out, p, centre, 4, 5, 1, 0, colour, tile);     // bottom
}

void emit_turned_prism(std::vector<SceneVertex>& out, float x, float z, float angle, const Prism& s,
                       const Vec3& colour, const UvRect& tile, const UvRect& top_tile) {
    emit_shaded_solid(out, prism_corners(x, z, angle, s), colour, tile, top_tile);
}

void emit_turned_prism(std::vector<SceneVertex>& out, float x, float z, float angle, const Prism& s,
                       const Vec3& colour, const UvRect& tile) {
    emit_turned_prism(out, x, z, angle, s, colour, tile, tile);
}

/// A box in a frame turned by `angle` about the point (x, z): `across` is its half-width, and it
/// runs from `back` to `front` along the heading, from `y0` to `y1`.
void emit_turned_box(std::vector<SceneVertex>& out, float x, float z, float angle, float across,
                     float back, float front, float y0, float y1, const Vec3& colour,
                     const UvRect& tile) {
    emit_turned_prism(out, x, z, angle,
                      Prism{-across, across, back, back, front, front, y0, y1, y1}, colour, tile);
}

/// One ring of a `emit_loft`: where it is in the unit's frame (`x` across, `y` over the ground, `z`
/// along the heading) and its two half-sizes, `across` on the horizontal and `up` on the other axis
/// of the plane the sweep's direction leaves it.
struct LoftStation {
    float x;
    float y;
    float z;
    float across;
    float up;
};

/// A smooth-looking solid swept through `stations`: each ring is `sides` corners of an ellipse, set
/// square to the sweep's direction there (the line from the station before to the station after),
/// and neighbouring rings are joined by quads -- the renderer's own way of approximating a curve, a
/// fan of facets. The two end rings are closed by a fan, so the solid is watertight. Built level,
/// in the unit's frame, then turned by `heading` about (x, z) over `ground`; every quad carries
/// its own world normal, flipped to point away from its ring's centre, so a sweep with a bend in
/// it still lights as one body. A ring with a zero size is a point: that is how a sweep tapers to a
/// tip.
void emit_loft(std::vector<SceneVertex>& out, float x, float z, float ground, float heading,
               std::span<const LoftStation> sweep, std::size_t sides, const Vec3& colour,
               const UvRect& tile) {
    const std::vector<LoftStation> stations(sweep.begin(), sweep.end()); // `.at()`, no subscript
    const std::size_t count = stations.size();
    constexpr float kTau = 6.2831853F;
    constexpr float kParallel = 1.0e-4F; ///< a sweep this close to straight up has no "across"
    const auto world = [&](const Vec3& local) {
        const std::array<float, 2> at = rotate_offset(x, z, local[0], local[2], heading);
        return Vec3{at.at(0), ground + local[1], at.at(1)};
    };
    std::vector<std::vector<Vec3>> rings;
    rings.reserve(stations.size());
    for (std::size_t i = 0; i < stations.size(); ++i) {
        const LoftStation& here = stations.at(i);
        const LoftStation& before = stations.at(i == 0 ? 0 : i - 1);
        const LoftStation& after = stations.at(std::min(i + 1, count - 1));
        const Vec3 along
            = normalise(Vec3{after.x - before.x, after.y - before.y, after.z - before.z});
        Vec3 right = cross(Vec3{0.0F, 1.0F, 0.0F}, along);
        right = std::hypot(right[0], right[1], right[2]) < kParallel ? Vec3{1.0F, 0.0F, 0.0F}
                                                                     : normalise(right);
        const Vec3 over = cross(along, right);
        std::vector<Vec3>& ring = rings.emplace_back();
        ring.reserve(sides);
        for (std::size_t corner = 0; corner < sides; ++corner) {
            const float angle = kTau * static_cast<float>(corner) / static_cast<float>(sides);
            const float a = std::cos(angle) * here.across;
            const float u = std::sin(angle) * here.up;
            ring.push_back(world(Vec3{here.x + (right[0] * a) + (over[0] * u),
                                      here.y + (right[1] * a) + (over[1] * u),
                                      here.z + (right[2] * a) + (over[2] * u)}));
        }
    }
    const auto facing = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& centre) {
        const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        return outward_unit(cross(ab, ac),
                            Vec3{a[0] - centre[0], a[1] - centre[1], a[2] - centre[2]});
    };
    const auto centre_of = [&](const std::vector<Vec3>& ring) {
        Vec3 sum{0.0F, 0.0F, 0.0F};
        for (const Vec3& p : ring) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                sum.at(axis) += p.at(axis) / static_cast<float>(ring.size());
            }
        }
        return sum;
    };
    for (std::size_t i = 0; i + 1 < rings.size(); ++i) {
        const Vec3 centre = centre_of(rings.at(i));
        for (std::size_t corner = 0; corner < sides; ++corner) {
            const std::size_t next = (corner + 1) % sides;
            const Vec3& a = rings.at(i).at(corner);
            const Vec3& b = rings.at(i).at(next);
            const Vec3& c = rings.at(i + 1).at(next);
            const Vec3& d = rings.at(i + 1).at(corner);
            // A ring pinched to a point leaves (a, b, c) with no area; the quad's other half has.
            const Vec3 first = facing(a, b, c, centre);
            emit_tiled_quad(out, a, b, c, d, colour, tile, 0,
                            first == Vec3{} ? facing(a, c, d, centre) : first);
        }
    }
    // The caps are flat fans from a ring's first corner, sampling the tile's middle texel.
    const float mid_u = (tile.u0 + tile.u1) * 0.5F;
    const float mid_v = (tile.v0 + tile.v1) * 0.5F;
    for (const std::size_t end : {std::size_t{0}, rings.size() - 1}) {
        const std::vector<Vec3>& ring = rings.at(end);
        const Vec3 neighbour = centre_of(rings.at(end == 0 ? 1 : end - 1));
        for (std::size_t corner = 1; corner + 1 < sides; ++corner) {
            const Vec3 normal = facing(ring.at(0), ring.at(corner), ring.at(corner + 1), neighbour);
            for (const Vec3* p : {&ring.at(0), &ring.at(corner), &ring.at(corner + 1)}) {
                out.push_back(SceneVertex{*p, colour, 1.0F, mid_u, mid_v, 0.0F, normal});
            }
        }
    }
}

/// THE ALIEN RIB (AZ): a sweep's smooth spine cut into overlapping PLATES, as a sawtooth of rings.
/// Smooth shading (`smooth_vertex_normals`) averages every pair of faces closer than
/// `kCreaseAngleDegrees` (75), so a segmentation made of gentle angles -- more sides, a finer
/// loft -- is blended into a dome. A plate edge has to be a STEP: each of the `plates_per_span`
/// plates of a span starts `tuck` (a fraction) smaller than the spine, swells to the spine's full
/// size by its rear edge, and the next plate starts again at the small size just `gap` behind it.
/// The step between them is a wall nearly square to the sweep (its slant is `kRibSlant` at most, so
/// its normal is 83 degrees or more from the surface it overlaps) and the swell leans the other
/// way, so the two meet at 90 degrees and over; the swell itself, being gentle, is averaged into a
/// smooth plate. The result is a ROUND mass carrying CRISP ridges, which is what a normal average
/// cannot flatten. The spine's own two ends are kept exactly (a nose, a tail), so a pinched tip
/// stays a tip and the machine's extent is the spine's: no plate is ever bigger than it.
/// `gap` is derived from the step's height, never chosen, so the slant holds at every size.
constexpr float kRibSlant = 0.12F;

std::vector<LoftStation> plated_stations(std::span<const LoftStation> spine,
                                         std::size_t plates_per_span, float tuck) {
    std::vector<LoftStation> out;
    out.reserve(spine.size() * plates_per_span * 2U);
    const auto mix = [](float a, float b, float t) { return a + ((b - a) * t); };
    const std::size_t spans = spine.size() - 1U;
    for (std::size_t span = 0; span < spans; ++span) {
        const LoftStation& from = *(spine.begin() + static_cast<std::ptrdiff_t>(span));
        const LoftStation& to = *(spine.begin() + static_cast<std::ptrdiff_t>(span + 1U));
        const Vec3 run = normalise(Vec3{to.x - from.x, to.y - from.y, to.z - from.z});
        for (std::size_t plate = 0; plate < plates_per_span; ++plate) {
            const float t0 = static_cast<float>(plate) / static_cast<float>(plates_per_span);
            const float t1 = static_cast<float>(plate + 1U) / static_cast<float>(plates_per_span);
            const auto at = [&](float t, float scale) {
                return LoftStation{mix(from.x, to.x, t), mix(from.y, to.y, t), mix(from.z, to.z, t),
                                   mix(from.across, to.across, t) * scale,
                                   mix(from.up, to.up, t) * scale};
            };
            out.push_back(at(t0, 1.0F - tuck));
            const bool last = span + 1U == spans && plate + 1U == plates_per_span;
            LoftStation rear = at(t1, 1.0F);
            if (!last) {
                const float gap = kRibSlant * tuck * std::min(rear.across, rear.up);
                rear.x -= run[0] * gap;
                rear.y -= run[1] * gap;
                rear.z -= run[2] * gap;
            }
            out.push_back(rear);
        }
    }
    out.front() = spine.front();
    return out;
}

/// A plated sweep: `plated_stations`, lofted.
void emit_plated(std::vector<SceneVertex>& out, float x, float z, float ground, float heading,
                 std::span<const LoftStation> spine, std::size_t plates_per_span, float tuck,
                 std::size_t sides, const Vec3& colour, const UvRect& tile) {
    const std::vector<LoftStation> stations = plated_stations(spine, plates_per_span, tuck);
    emit_loft(out, x, z, ground, heading, stations, sides, colour, tile);
}

/// ONE ROUND BARREL (BA), for every gun and every collar: a ring of `kBarrelSides` corners swept
/// from `back` to `front` along the heading and closed at both ends, as wide (`across` each side)
/// and as tall (`y0` to `y1`) as the box it replaces, so a gun is no longer, shorter or fatter than
/// it was. The corners are at angle 0, 90, 180 and 270 degrees among others, so the ring touches
/// the box's four faces' extents exactly.
///
/// MORE SIDES IS RIGHT HERE, AND ONLY HERE. Everywhere else a higher side count destroys a read,
/// because smooth shading (`smooth_vertex_normals`, `kCreaseAngleDegrees`) averages the panels of a
/// pair closer than the crease and the facets are lost: the alien shell must stay faceted, and
/// raising its count only smooths it. A barrel's goal IS that average -- a cylinder -- so the
/// sides meet 30 degrees apart, well inside the crease, and blend into one curved surface, while
/// each cap is 90 degrees from the side and keeps its edge. Do not "fix" this back to four
/// corners, and do not mistake it for the alien-shell mistake.
void emit_barrel(std::vector<SceneVertex>& out, float x, float z, float angle, float across,
                 float back, float front, float y0, float y1, const Vec3& colour,
                 const UvRect& tile) {
    const float centre = (y0 + y1) * 0.5F;
    const float half = (y1 - y0) * 0.5F;
    emit_loft(out, x, z, 0.0F, angle,
              std::array<LoftStation, 2>{
                  {{0.0F, centre, back, across, half}, {0.0F, centre, front, across, half}}},
              static_cast<std::size_t>(kBarrelSides), colour, tile);
}

/// The part of a wall tile from texel column `c0` to `c1` and texel row `r0` (top) to `r1`, stood
/// UPRIGHT: a side face hands its ground edge the rect's v0 (see `emit_shaded_solid`), so v0 here
/// is the part's BOTTOM row and the tile's eaves land at the top of the wall. Edges on the tile's
/// own border stay at its half-texel inset.
UvRect upright_part(const UvRect& tile, int c0, int r0, int c1, int r1) {
    constexpr float kTexel = 2.0F * kHalfTexel;
    const float u_origin = tile.u0 - kHalfTexel;
    const float v_origin = tile.v0 - kHalfTexel;
    const auto u = [&](int column) {
        return std::clamp(u_origin + (static_cast<float>(column) * kTexel), tile.u0, tile.u1);
    };
    const auto v = [&](int row) {
        return std::clamp(v_origin + (static_cast<float>(row) * kTexel), tile.v0, tile.v1);
    };
    return UvRect{u(c0), v(r1), u(c1), v(r0)};
}

/// The miter inset of a counter-clockwise convex `profile`: every edge moved `by` metres inward,
/// the corners meeting where the moved edges do. It is the face of a chamfered part, smaller than
/// the section behind it.
std::vector<std::array<float, 2>> inset_profile(const std::vector<std::array<float, 2>>& profile,
                                                float by) {
    const std::size_t n = profile.size();
    std::vector<std::array<float, 2>> moved;
    moved.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& before = profile.at((i + n - 1) % n);
        const auto& here = profile.at(i);
        const auto& after = profile.at((i + 1) % n);
        const auto direction
            = [](const std::array<float, 2>& from, const std::array<float, 2>& to) {
                  const float dx = to[0] - from[0];
                  const float dy = to[1] - from[1];
                  const float length = std::hypot(dx, dy);
                  return std::array<float, 2>{dx / length, dy / length};
              };
        const std::array<float, 2> in = direction(before, here);
        const std::array<float, 2> out_of = direction(here, after);
        // Inward is to the left of a counter-clockwise edge.
        const std::array<float, 2> n1{-in[1], in[0]};
        const std::array<float, 2> n2{-out_of[1], out_of[0]};
        const float scale = by / (1.0F + (n1[0] * n2[0]) + (n1[1] * n2[1]));
        moved.push_back({here[0] + ((n1[0] + n2[0]) * scale), here[1] + ((n1[1] + n2[1]) * scale)});
    }
    return moved;
}

/// One `ShapePart` swept across `[lo, hi]`, chamfered: the profile at full size from `lo + bevel`
/// to `hi - bevel`, drawn in to its inset at the two outer faces, so the flanks and the top and
/// bottom edges slope instead of meeting at a square corner. Each quad carries its own world
/// normal, flipped to point away from the part's centre (the part is convex), and the two faces are
/// flat fans. A part with no bevel is a plain extrusion. Built in the unit's frame -- `across` over
/// the heading, `up` over the ground -- then turned by `heading` about (x, z).
void emit_shape_prism(std::vector<SceneVertex>& out, float x, float z, float ground, float heading,
                      const ShapePart& part, float lo, float hi, const Vec3& colour,
                      const UvRect& tile) {
    struct Section {
        std::vector<std::array<float, 2>> profile;
        float across;
    };
    std::vector<Section> sections;
    if (part.bevel > 0.0F) {
        const std::vector<std::array<float, 2>> face = inset_profile(part.profile, part.bevel);
        sections = {{face, lo},
                    {part.profile, lo + part.bevel},
                    {part.profile, hi - part.bevel},
                    {face, hi}};
    } else {
        sections = {{part.profile, lo}, {part.profile, hi}};
    }
    std::vector<std::vector<Vec3>> rings;
    Vec3 centre{0.0F, 0.0F, 0.0F};
    std::size_t points = 0;
    for (const Section& section : sections) {
        std::vector<Vec3>& ring = rings.emplace_back();
        for (const auto& corner : section.profile) {
            const std::array<float, 2> at = rotate_offset(x, z, section.across, corner[0], heading);
            ring.push_back(Vec3{at.at(0), ground + corner[1], at.at(1)});
            for (std::size_t axis = 0; axis < 3; ++axis) {
                centre.at(axis) += ring.back().at(axis);
            }
            ++points;
        }
    }
    for (float& axis : centre) {
        axis /= static_cast<float>(points);
    }
    const auto facing = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        return outward_unit(cross(ab, ac),
                            Vec3{a[0] - centre[0], a[1] - centre[1], a[2] - centre[2]});
    };
    for (std::size_t i = 0; i + 1 < rings.size(); ++i) {
        const std::size_t n = rings.at(i).size();
        for (std::size_t corner = 0; corner < n; ++corner) {
            const std::size_t next = (corner + 1) % n;
            const Vec3& a = rings.at(i).at(corner);
            const Vec3& b = rings.at(i).at(next);
            const Vec3& c = rings.at(i + 1).at(next);
            const Vec3& d = rings.at(i + 1).at(corner);
            // A ring pinched to a point leaves (a, b, c) with no area; the quad's other half has.
            const Vec3 first = facing(a, b, c);
            emit_tiled_quad(out, a, b, c, d, colour, tile, 0,
                            first == Vec3{} ? facing(a, c, d) : first);
        }
    }
    const float mid_u = (tile.u0 + tile.u1) * 0.5F;
    const float mid_v = (tile.v0 + tile.v1) * 0.5F;
    for (const std::vector<Vec3>& ring : {rings.front(), rings.back()}) {
        for (std::size_t corner = 1; corner + 1 < ring.size(); ++corner) {
            const Vec3 normal = facing(ring.at(0), ring.at(corner), ring.at(corner + 1));
            for (const Vec3* p : {&ring.at(0), &ring.at(corner), &ring.at(corner + 1)}) {
                out.push_back(SceneVertex{*p, colour, 1.0F, mid_u, mid_v, 0.0F, normal});
            }
        }
    }
}

/// One corner of a plan (across, along) with whether the edge from it to the NEXT corner is a flat
/// run of wall (`flat`) or one facet of a rounded corner.
struct PlanPoint {
    std::array<float, 2> at;
    bool flat;
};

/// The radius of each vertical corner of a rounded plan, in the order the plan runs.
struct PlanRadii {
    float back_left;
    float back_right;
    float front_right;
    float front_left;
};

/// How many facets a rounded plan corner is cut into: 30 degrees each, so smooth shading (crease
/// 75 degrees) blends them into one curve while a corner stays a curve and not a bevel.
constexpr int kCornerFacets = 3;

/// A rectangle's plan, counter-clockwise from its back-left corner -- back edge toward +across --
/// with each corner replaced by a quarter circle of its radius in `kCornerFacets` facets (a zero
/// radius keeps the corner square). Every arc stays inside the rectangle, so the plan never grows.
std::vector<PlanPoint> rounded_plan(float left, float right, float back, float front,
                                    const PlanRadii& radii) {
    constexpr float kQuarter = 1.5707963F;
    struct Corner {
        float centre_across;
        float centre_along;
        float radius;
        float start; ///< the arc's first angle, in quarter turns
    };
    const std::array<Corner, 4> corners{{
        {left + radii.back_left, back + radii.back_left, radii.back_left, 2.0F},
        {right - radii.back_right, back + radii.back_right, radii.back_right, 3.0F},
        {right - radii.front_right, front - radii.front_right, radii.front_right, 0.0F},
        {left + radii.front_left, front - radii.front_left, radii.front_left, 1.0F},
    }};
    const std::array<std::array<float, 2>, 4> square{
        {{left, back}, {right, back}, {right, front}, {left, front}}};
    std::vector<PlanPoint> plan;
    plan.reserve(corners.size() * (static_cast<std::size_t>(kCornerFacets) + 1U));
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const Corner& c = corners.at(i);
        if (!(c.radius > 0.0F)) {
            plan.push_back({square.at(i), true});
            continue;
        }
        for (int facet = 0; facet <= kCornerFacets; ++facet) {
            const float angle
                = (c.start + (static_cast<float>(facet) / static_cast<float>(kCornerFacets)))
                  * kQuarter;
            plan.push_back({{c.centre_across + (c.radius * std::cos(angle)),
                             c.centre_along + (c.radius * std::sin(angle))},
                            facet == kCornerFacets});
        }
    }
    return plan;
}

/// A texel sub-rect of an UPRIGHT tile (`upright_part`: its v0 is the bottom row's v and its v1 the
/// top's) covering heights `lo` to `hi` of a tile that stands `tile_height` metres tall from
/// `base`.
UvRect upright_slice(const UvRect& tile, float base, float tile_height, float lo, float hi) {
    const auto v = [&](float height) {
        return tile.v0 + ((tile.v1 - tile.v0) * ((height - base) / tile_height));
    };
    return UvRect{tile.u0, v(lo), tile.u1, v(hi)};
}

/// A tall block with a ROUNDED PLAN and a ROUNDED CROWN, built from `plan` standing on `y0`, its
/// walls straight up to `y_crown` and then rolled in over `crown_height` and `crown_inset` in
/// `kCrownFacets` steps along a quarter ellipse to a flat roof. This is the factory's and the
/// annex's mass, and it is lofted by hand and not as a `ShapePart` because a part's flanks are flat
/// end caps sampling one texel, and these walls carry the PAINTED art.
///
/// THE PAINTED FACES: a flat run of wall wears `wall_tile` whole (a straight run is a whole face of
/// the old prism), standing upright over `tile_height` metres from `y0` so the art keeps its scale
/// whatever the crown does to the height. A rounded corner's facets are far too narrow to carry a
/// door, so they wear `corner_tile`, a plain corrugated strip of the same wall -- the tile
/// "wraps" the corner as ribbed sheet, not as a door crushed into a facet. The crown wears
/// `crown_tile`, and the roof its whole `roof_tile` by the plan's bounding box.
void emit_rounded_tower(std::vector<SceneVertex>& out, float x, float z, float angle,
                        const std::vector<PlanPoint>& plan, float y0, float y_crown,
                        float crown_height, float crown_inset, float tile_height,
                        const Vec3& colour, const UvRect& wall_tile, const UvRect& corner_tile,
                        const UvRect& crown_tile, const UvRect& roof_tile) {
    constexpr int kCrownFacets = 3;
    constexpr float kQuarter = 1.5707963F;
    std::vector<std::array<float, 2>> base;
    base.reserve(plan.size());
    for (const PlanPoint& point : plan) {
        base.push_back(point.at);
    }
    struct Ring {
        std::vector<std::array<float, 2>> corners;
        float y;
    };
    std::vector<Ring> rings;
    rings.reserve(static_cast<std::size_t>(kCrownFacets) + 2U);
    rings.push_back({base, y0});
    rings.push_back({base, y_crown});
    for (int step = 1; step <= kCrownFacets; ++step) {
        const float angle_up
            = kQuarter * static_cast<float>(step) / static_cast<float>(kCrownFacets);
        rings.push_back({inset_profile(base, crown_inset * (1.0F - std::cos(angle_up))),
                         y_crown + (crown_height * std::sin(angle_up))});
    }
    const auto world = [&](const std::array<float, 2>& at, float y) {
        const std::array<float, 2> p = rotate_offset(x, z, at.at(0), at.at(1), angle);
        return Vec3{p.at(0), y, p.at(1)};
    };
    Vec3 centre{0.0F, 0.0F, 0.0F};
    std::size_t points = 0;
    for (const Ring& ring : rings) {
        for (const auto& at : ring.corners) {
            const Vec3 p = world(at, ring.y);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                centre.at(axis) += p.at(axis);
            }
            ++points;
        }
    }
    for (float& axis : centre) {
        axis /= static_cast<float>(points);
    }
    for (std::size_t i = 0; i + 1 < rings.size(); ++i) {
        const Ring& low = rings.at(i);
        const Ring& high = rings.at(i + 1);
        for (std::size_t corner = 0; corner < plan.size(); ++corner) {
            const std::size_t next = (corner + 1) % plan.size();
            const Vec3 a = world(low.corners.at(corner), low.y);
            const Vec3 b = world(low.corners.at(next), low.y);
            const Vec3 c = world(high.corners.at(next), high.y);
            const Vec3 d = world(high.corners.at(corner), high.y);
            const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const Vec3 ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const Vec3 normal = outward_unit(
                cross(ab, ac), Vec3{a[0] - centre[0], a[1] - centre[1], a[2] - centre[2]});
            UvRect tile = crown_tile;
            if (i == 0) {
                tile = upright_slice(plan.at(corner).flat ? wall_tile : corner_tile, y0,
                                     tile_height, low.y, high.y);
            }
            emit_tiled_quad(out, a, b, c, d, colour, tile, 0, normal);
        }
    }
    // The roof and the floor: flat fans, the roof's tile laid over the plan's bounding box.
    const Ring& top = rings.back();
    const Ring& floor = rings.front();
    float across_lo = 1.0e9F;
    float across_hi = -1.0e9F;
    float along_lo = 1.0e9F;
    float along_hi = -1.0e9F;
    for (const auto& at : top.corners) {
        across_lo = std::min(across_lo, at.at(0));
        across_hi = std::max(across_hi, at.at(0));
        along_lo = std::min(along_lo, at.at(1));
        along_hi = std::max(along_hi, at.at(1));
    }
    for (const Ring* ring : {&floor, &top}) {
        const Vec3 normal{0.0F, ring == &top ? 1.0F : -1.0F, 0.0F};
        const bool roof = ring == &top;
        for (std::size_t corner = 1; corner + 1 < ring->corners.size(); ++corner) {
            for (const std::size_t index : {std::size_t{0}, corner, corner + 1}) {
                const auto& at = ring->corners.at(index);
                const float u = (at.at(0) - across_lo) / (across_hi - across_lo);
                const float v = (at.at(1) - along_lo) / (along_hi - along_lo);
                const float mid_u = roof ? roof_tile.u0 + ((roof_tile.u1 - roof_tile.u0) * u)
                                         : (roof_tile.u0 + roof_tile.u1) * 0.5F;
                const float mid_v = roof ? roof_tile.v0 + ((roof_tile.v1 - roof_tile.v0) * v)
                                         : (roof_tile.v0 + roof_tile.v1) * 0.5F;
                out.push_back(
                    SceneVertex{world(at, ring->y), colour, 1.0F, mid_u, mid_v, 0.0F, normal});
            }
        }
    }
}

/// THE FACTORY, a skyline rather than a crate, inside the old box's footprint (0.3 tile either side
/// of the unit) with its hall roof at the old box's 4.5 m, which is what the picker tests against.
/// BC ROUNDED IT, by ROUTE (b): the painted faces stay flat panels SET INTO a rounded mass, because
/// the door is one tile 0.8 m wide with an opening four texels off one edge and twelve off the
/// other, and a door crushed onto the facets of a lofted ring is a smear:
/// - the HALL, a rounded tower: its rear corners are quarter circles, its roof is rolled in over
///   the last 0.85 m into a crown, and the walls tile (corrugation, the painted door) is on its
///   flat runs of wall, the plain corrugated strip of it round the corners;
/// - a PORTAL, left as it was and flat -- two jambs and a lintel framing the painted door, each
///   sampling exactly the strip of the wall tile it covers -- so the DOOR is a door, square to the
///   viewer, in the rounded wall behind it (the lintel stands as a parapet over the crown, and the
///   hall's front is the one face left flat on purpose);
/// - a lower ANNEX beside the hall, rounded at all four corners with its own small crown;
/// - a STACK at the annex's rear corner, now a round chimney (twelve sides, 30 degrees apart,
///   which smooth shading blends into a cylinder) rising well over the hall roof, with a
///   team-colour round band near its top (the fleet's saturated accent, as a barrel's).
/// Local +z is the door's side; a player's factory faces +Y and an enemy's -Y, toward the field
/// the other side comes from. Every structure part keeps the pale tint; only the band does not.
void build_factory(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                   Mark mark, float lift_z, bool lifted) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    // float + bool rather than optional<float>: gcc -O2 wrongly warns may-be-uninitialized on the
    // optional's storage at the call site (the same false alarm hover_.z() once raised).
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const float half_width = kHullHalfMetres; ///< the footprint, every part inside it
    const float angle = unit.side == sim::Side::Player ? 0.0F : kPi;
    constexpr float kHallTop = 4.5F;   ///< the picker's housing height
    constexpr float kHallRight = 0.2F; ///< the annex takes the rest of the width
    constexpr float kRecess = 0.15F;   ///< how far the door's face is set back
    // The door's painted frame, in the walls tile's texels: its outer edge is the opening.
    constexpr int kTileTexels = 64;
    constexpr int kOpeningLeft = 4;
    constexpr int kOpeningRight = 52;
    constexpr int kOpeningTop = 13;
    constexpr float kHallRear = 0.25F;   ///< the radius of the hall's two rear corners
    constexpr float kHallCrown = 0.85F;  ///< how much of the hall's height is the crown ...
    constexpr float kHallCrownIn = 0.2F; ///< ... and how far it rolls in, under `kHallRear`
    constexpr float kAnnexTop = 2.6F;
    constexpr float kAnnexBack = -0.5F;
    constexpr float kAnnexFront = 0.3F; ///< set back from the portal's face
    constexpr float kAnnexRound = 0.19F;
    constexpr float kAnnexCrown = 0.4F;
    constexpr float kAnnexCrownIn = 0.12F;
    constexpr float kStackAxisAcross = 0.42F; ///< the old box's middle, 0.3 to 0.54
    constexpr float kStackAxisAlong = -0.44F; ///< and -0.56 to -0.32
    constexpr float kStackRadius = 0.12F;     ///< half the old box's 0.24
    constexpr float kStackTop = 6.2F;
    constexpr float kBandRadius = 0.15F; ///< a collar just proud of the stack
    constexpr float kBandBottom = 5.7F;
    constexpr float kBandTop = 5.95F;
    constexpr int kStackSides = 12;

    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    // Pale-mixed like the hull: the deep side colour would multiply the tile's detail down to a
    // tenth of a luma step; the pale tint lets the metal carry and the team read through as a tint.
    Vec3 pale = colour;
    for (float& channel : pale) {
        channel += (1.0F - channel) * kHullPaleMix;
    }
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect walls_tile = atlas.rect("factory_walls");
    const UvRect walls = upright_part(walls_tile, 0, 0, kTileTexels, kTileTexels);
    const UvRect roof = atlas.rect("factory_roof");
    // Plain sheet for the rounded things: the wall's leftmost four columns, which are corrugation
    // and seam and no door, and the roof's ribs between its seams.
    constexpr int kPlainColumns = 4;
    const UvRect plain_wall = upright_part(walls_tile, 0, 0, kPlainColumns, kTileTexels);
    const UvRect ribs = upright_part(roof, 0, 44, kTileTexels, 60);

    const float left = -half_width;
    const float front = half_width;
    const float face = front - kRecess;
    const float width = kHallRight - left;
    // The front face's u runs from the hall's right (+x) edge to its left, and its height spans
    // the whole tile, so a texel column or row is a fixed place on the wall.
    const auto across = [&](int column) {
        return kHallRight - (width * static_cast<float>(column) / static_cast<float>(kTileTexels));
    };
    const float lintel_bottom = kHallTop * static_cast<float>(kTileTexels - kOpeningTop)
                                / static_cast<float>(kTileTexels);
    const auto box = [&](float l, float r, float back, float fwd, float y0, float y1) {
        return Prism{l, r, back, back, fwd, fwd, ground + y0, ground + y1, ground + y1};
    };

    emit_rounded_tower(out, x, z, angle,
                       rounded_plan(left, kHallRight, -half_width, face,
                                    PlanRadii{kHallRear, kHallRear, 0.0F, 0.0F}),
                       ground, ground + kHallTop - kHallCrown, kHallCrown, kHallCrownIn, kHallTop,
                       pale, walls, plain_wall, ribs, roof);
    emit_turned_prism(out, x, z, angle,
                      box(across(kOpeningLeft), kHallRight, face, front, 0.0F, lintel_bottom), pale,
                      upright_part(walls_tile, 0, kOpeningTop, kOpeningLeft, kTileTexels));
    emit_turned_prism(
        out, x, z, angle, box(left, across(kOpeningRight), face, front, 0.0F, lintel_bottom), pale,
        upright_part(walls_tile, kOpeningRight, kOpeningTop, kTileTexels, kTileTexels));
    emit_turned_prism(out, x, z, angle, box(left, kHallRight, face, front, lintel_bottom, kHallTop),
                      pale, upright_part(walls_tile, 0, 0, kTileTexels, kOpeningTop), roof);
    emit_rounded_tower(out, x, z, angle,
                       rounded_plan(kHallRight, half_width, kAnnexBack, kAnnexFront,
                                    PlanRadii{kAnnexRound, kAnnexRound, kAnnexRound, kAnnexRound}),
                       ground, ground + kAnnexTop - kAnnexCrown, kAnnexCrown, kAnnexCrownIn,
                       kAnnexTop, pale, walls, plain_wall, ribs, roof);
    emit_loft(out, x, z, ground, angle,
              std::array<LoftStation, 2>{
                  {{kStackAxisAcross, 0.0F, kStackAxisAlong, kStackRadius, kStackRadius},
                   {kStackAxisAcross, kStackTop, kStackAxisAlong, kStackRadius, kStackRadius}}},
              static_cast<std::size_t>(kStackSides), pale, ribs);
    emit_loft(out, x, z, ground, angle,
              std::array<LoftStation, 2>{
                  {{kStackAxisAcross, kBandBottom, kStackAxisAlong, kBandRadius, kBandRadius},
                   {kStackAxisAcross, kBandTop, kStackAxisAlong, kBandRadius, kBandRadius}}},
              static_cast<std::size_t>(kStackSides), colour, ribs);
}

/// The ink and tile a `ShapeRole` asks for. A role is a tone and a tile, no more: Hull wears the
/// team's camo in its pale tint as a tank's armour does, Track the side colour on the dark link
/// tile, Metal the pale tint on the plain turret metal, Load a darkened camo for rough scrap, and
/// Glass is the dark inlay of a CUT -- proud of the flank by a centimetre, which is how a window or
/// a wheel's ring shows on a solid. One mapping for every kind that has a shape.
struct ShapeLook {
    Vec3 colour{};      ///< the marked side colour
    Vec3 camo_colour{}; ///< its pale tint
    UvRect hull_tile{};
    UvRect tracks_tile{};
    UvRect metal_tile{};
};

/// Every part of `parts` in its paint order, in the ink and tile its role asks for, a mirrored part
/// built twice, once each side of the centreline. Hull parts are turned by `heading` about (x, z),
/// Turret parts by `turret_angle`.
void emit_shape_parts(std::vector<SceneVertex>& out, float x, float z, float ground, float heading,
                      float turret_angle, const std::vector<ShapePart>& parts,
                      const ShapeLook& look) {
    constexpr float kLoadTone = 0.55F;
    constexpr float kGlassTone = 0.25F;
    const auto scaled
        = [](const Vec3& c, float by) { return Vec3{c[0] * by, c[1] * by, c[2] * by}; };
    for (const ShapePart& part : parts) {
        Vec3 ink = look.camo_colour;
        UvRect tile = look.hull_tile;
        switch (part.role) {
        case ShapeRole::Hull:
            break;
        case ShapeRole::Track:
            ink = look.colour;
            tile = look.tracks_tile;
            break;
        case ShapeRole::Metal:
            tile = look.metal_tile;
            break;
        case ShapeRole::Load:
            ink = scaled(look.camo_colour, kLoadTone);
            break;
        case ShapeRole::Glass:
            ink = scaled(look.colour, kGlassTone);
            tile = look.tracks_tile;
            break;
        }
        const float turned = part.group == ShapeGroup::Turret ? turret_angle : heading;
        emit_shape_prism(out, x, z, ground, turned, part, part.across_lo, part.across_hi, ink,
                         tile);
        if (part.mirrored) {
            emit_shape_prism(out, x, z, ground, turned, part, -part.across_hi, -part.across_lo, ink,
                             tile);
        }
    }
}

/// How far a drawn barrel may point below the hull's horizontal before it would run into the
/// hull. A barrel pivots at the turret's front face (0.35 m ahead of the tank's centre, its lower
/// edge 0.98 m up) and the compartment roof's front edge is 0.2 m further on at 0.85 m: the barrel
/// clears that corner while tan(depression) < 0.13 / 0.2, which is 33 degrees. 0.5 rad is 28.6
/// degrees, a margin under it. PROVISIONAL and a design call (it caps what is DRAWN, never the
/// shot, which flies the view's own pitch); the other hulls borrow it unchecked.
constexpr float kMaxBarrelDepression = 0.5F;

/// Elevates the vertices from `first` on (a barrel just built level, along `yaw`) by `pitch`
/// radians, up positive, about the horizontal axis across the barrel through `pivot_forward` metres
/// along `yaw` from (x, z) at height `pivot_y`. Built in the hull's frame, before the lean to the
/// ground, so the lean carries the barrel with the hull. Zero rotates nothing, to the bit.
void elevate_barrel(std::vector<SceneVertex>& out, std::size_t first, float x, float z, float yaw,
                    float pivot_forward, float pivot_y, float pitch) {
    if (pitch == 0.0F) {
        return;
    }
    const float angle = std::max(pitch, -kMaxBarrelDepression);
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    const float forward_x = std::sin(yaw);
    const float forward_z = std::cos(yaw);
    const float pivot_x = x + (forward_x * pivot_forward);
    const float pivot_z = z + (forward_z * pivot_forward);
    for (std::size_t i = first; i < out.size(); ++i) {
        Vec3& p = out.at(i).position;
        const float dx = p[0] - pivot_x;
        const float dz = p[2] - pivot_z;
        const float up = p[1] - pivot_y;
        const float along = (dx * forward_x) + (dz * forward_z);
        const float lift = (along * sine) + (up * cosine);
        const float ahead = (along * cosine) - (up * sine);
        p[0] = pivot_x + (dx - (along * forward_x)) + (ahead * forward_x);
        p[2] = pivot_z + (dz - (along * forward_z)) + (ahead * forward_z);
        p[1] = pivot_y + lift;
        // The normal turns with the barrel (a direction: no pivot, no translation).
        Vec3& n = out.at(i).normal;
        const float n_along = (n[0] * forward_x) + (n[2] * forward_z);
        const float n_lift = (n_along * sine) + (n[1] * cosine);
        const float n_ahead = (n_along * cosine) - (n[1] * sine);
        n[0] = (n[0] - (n_along * forward_x)) + (n_ahead * forward_x);
        n[2] = (n[2] - (n_along * forward_z)) + (n_ahead * forward_z);
        n[1] = n_lift;
    }
}

/// A tank, built of flat-shaded prisms. The TANK ITSELF is built from `shape_of(UnitKind::Tank)`,
/// the one description its plate is also projected from: tracks with road wheels, hull, fenders,
/// turret, bustle and hatch, Hull parts turned by `heading` and Turret parts by `heading + turret`.
/// What is not the body's shape is added here: a flat team-colour panel on the turret roof, and the
/// barrel with its muzzle collar, which elevates and which the weapon code owns, so it is always
/// the last prism. The SCOUT and the MISSILE SCOUT come through here too and are built from their
/// own shapes the same way, Hull parts by `heading` and Turret parts (the missile scout's rack) by
/// `heading + turret`; they carry no gun, so they get neither panel nor barrel. These three are the
/// only kinds that do: every other kind has a builder of its own, so there is no body left here
/// that is not a shape.
void build_facing_tank(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                       Mark mark, float lift_z, bool lifted, float heading, float turret,
                       float pitch) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    // The tracks and grille are dark, with their links, because the TRACK TILE is, and the hull,
    // engine deck, fenders and turret wear the camo because their tile does: the vertex keeps the
    // side colour, the selection tint and the face light, and the texel multiplies them. Which of
    // its team's three camo tiles is this tank's -- one pattern for the whole tank. The roof panel
    // wears the plain turret metal.
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect panel_tile = atlas.rect("tank_turret");
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    constexpr float kTurretCentre = -0.15F;   ///< set back from the hull's centre
    constexpr float kPanelHalfAcross = 0.22F; ///< of the turret's 0.36: camo shows either side
    constexpr float kPanelBack = -0.4F;       ///< along the turret from its centre
    constexpr float kPanelFront = 0.25F;
    constexpr float kPanelThickness = 0.03F; ///< standing just proud of the roof
    constexpr float kBarrelHalfAcross = 0.06F;
    constexpr float kCollarHalfAcross = 0.1F;

    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    // The camo parts' colour is a PALE tint of the marked side colour, so the camo tile's tones
    // dominate.
    Vec3 camo_colour = colour;
    for (float& channel : camo_colour) {
        channel += (1.0F - channel) * kHullPaleMix;
    }

    const float turret_angle = heading + turret;
    const float turret_y0 = ground + kTankTurretBase;
    const std::array<float, 2> turret_at = rotate_offset(x, z, 0.0F, kTurretCentre, turret_angle);
    // Built from the kind's shape: Hull parts turned by the heading, Turret parts by the turret's
    // bearing.
    emit_shape_parts(
        out, x, z, ground, heading, turret_angle, shape_of(unit.kind),
        ShapeLook{colour, camo_colour, hull_tile, tracks_tile, atlas.rect("tank_turret")});
    if (unit.kind != sim::UnitKind::Tank) {
        return; // the scout and the missile scout carry no gun: no panel, no barrel
    }
    const float roof = turret_y0 + kTankTurretHeight;
    emit_turned_box(out, turret_at.at(0), turret_at.at(1), turret_angle, kPanelHalfAcross,
                    kPanelBack, kPanelFront, roof, roof + kPanelThickness, colour, panel_tile);
    const float turret_front = kTankTurretFront;
    const std::size_t barrel_first = out.size();
    emit_barrel(out, x, z, turret_angle, kCollarHalfAcross, kTankBarrelTip - kTankCollarLength,
                kTankBarrelTip, turret_y0 + kTankCollarBottom, turret_y0 + kTankCollarTop, colour,
                barrel_tile);
    emit_barrel(out, x, z, turret_angle, kBarrelHalfAcross, turret_front,
                kTankBarrelTip - kTankCollarLength, turret_y0 + kTankBarrelBottom,
                turret_y0 + kTankBarrelTop, colour, barrel_tile);
    // The barrel is the last thing built, in the hull's frame: raised to the aim's elevation about
    // its root at the turret's front, and then the lean to the ground takes it along with the rest.
    elevate_barrel(out, barrel_first, x, z, turret_angle, turret_front,
                   turret_y0 + ((kTankBarrelBottom + kTankBarrelTop) * 0.5F), pitch);
}

/// THE SCAVENGER (C2), the HUMAN hauler (the alien one is `build_harvester`). Built from
/// `shape_of(UnitKind::Scavenger)` and from nothing else: this function holds no number about what
/// the machine looks like, and the 72 px plate under the command row is the same parts seen
/// side-on, so the two cannot disagree. Turned by `heading` alone; nothing on it aims.
void build_scavenger(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                     Mark mark, float lift_z, bool lifted, float heading) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));

    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    Vec3 camo_colour = colour;
    for (float& channel : camo_colour) {
        channel += (1.0F - channel) * kHullPaleMix;
    }
    emit_shape_parts(out, x, z, ground, heading, heading, shape_of(sim::UnitKind::Scavenger),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});
}

/// The marked side colour, and its pale camo tint, that the shape-built kinds and the alien kit
/// wear.
struct KindColours {
    Vec3 colour;
    Vec3 camo;
};
KindColours kind_colours(const sim::Unit& unit, Mark mark) {
    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    Vec3 camo = colour;
    for (float& channel : camo) {
        channel += (1.0F - channel) * kHullPaleMix;
    }
    return KindColours{colour, camo};
}

/// THE HARVESTER, the alien Scavenger: the SAME kind as the human hauler, drawn when the unit's
/// FACTION is Alien (`build_scene` decides, once). Harri's ruling: the alien kit differs in SHAPE,
/// not only colour, and is more organic -- no flat walls or square corners, ribbed and tapered
/// rather than boxed, legs rather than tracks or a hover skirt, a curving spine rather than a
/// mirror, a carapace rather than a hull. It is a grazing animal: a low domed carapace that rises
/// to its high point just behind the head and tapers to the tail, a tapered PROBOSCIS trailing
/// behind it and dragging its tip along the ground, six splayed legs, and a crest of side colour
/// down the spine so the side still reads from the air.
///
/// AZ made it MORE of a bug, and the how is the lesson (`plated_stations`): the carapace, the
/// proboscis and the legs were smooth lofts whose panels were 36 and 60 degrees apart, inside the
/// crease angle, so the smooth pass averaged every one and no segmentation survived as shading.
/// Now each is cut into overlapping PLATES -- a swell and a step -- whose edges meet at 90 degrees
/// and over, so the machine is still a round mass but wears a trilobite's / an isopod's armour:
/// twelve shingled plates on the back, a serrated dorsal comb (four-sided, so its blade edges are
/// 90 degrees too), a segmented tail, jointed legs of overlapping cones, a pair of curved MANDIBLES
/// (four-sided blades) and two eyes of side colour on the head. Every part is inside the envelope
/// the smooth loft had, so the machine is the size it was. Everything is an `emit_loft` sweep, so
/// it is built from one small primitive and turned by `heading` alone -- nothing on it aims. The
/// carapace wears the team's camo in the pale tint, as a tank's armour does; the rest is the side
/// colour on the dark track tile (the eyes and the comb on the turret tile, lit).
/// Large on purpose: 5.5 m long and 2.3 m across against the hauler's 2.6 by 1.2, because a shape
/// that reads as a small blob across the map is worse than a box.
void build_harvester(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                     Mark mark, float lift_z, bool lifted, float heading,
                     std::optional<float> stride) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect panel_tile = atlas.rect("tank_turret");
    // Ten corners round is where a ring stops reading as a polygon at the range a harvester is seen
    // from; the legs are thin enough that six do; the blades (mandibles, comb) are four, because
    // their EDGES are the point.
    constexpr std::size_t kCarapaceSides = 10;
    constexpr std::size_t kTailSides = 8;
    constexpr std::size_t kLegSides = 6;
    constexpr std::size_t kBladeSides = 4;
    constexpr std::size_t kEyeSides = 8;
    // The carapace, nose to tail. Its high point (1.2 m) sits just behind the head, a metre over
    // the hauler's roof, and it narrows to the tail. A rounded nose and a rounded tail are rings
    // that shrink to a point. Two shingled plates to a span, each tucked by a fifth of its size.
    constexpr std::array<LoftStation, 7> kCarapace{{
        {0.0F, 0.42F, 1.30F, 0.02F, 0.02F}, // the nose's tip
        {0.0F, 0.50F, 1.05F, 0.30F, 0.22F}, // the head, low and narrow
        {0.0F, 0.68F, 0.55F, 0.60F, 0.38F}, // the shoulders, climbing
        {0.0F, 0.78F, 0.05F, 0.74F, 0.46F}, // the high point
        {0.0F, 0.68F, -0.55F, 0.66F, 0.38F},
        {0.0F, 0.52F, -1.10F, 0.42F, 0.26F}, // tapering to the tail
        {0.0F, 0.42F, -1.40F, 0.02F, 0.02F},
    }};
    constexpr std::size_t kCarapacePlates = 2;
    constexpr float kCarapaceTuck = 0.22F;
    // The proboscis hangs off the tail and tapers to a tip that rests on the ground, so it reads
    // as dragged. The tip stays 3 cm off the ground, where the terrain's own roughness cannot cut
    // it away. Segmented like a tail: two plates to a span, tucked by a third.
    constexpr std::array<LoftStation, 6> kProboscis{{
        {0.0F, 0.42F, -1.10F, 0.24F, 0.20F},
        {0.0F, 0.32F, -1.85F, 0.21F, 0.17F},
        {0.0F, 0.20F, -2.65F, 0.16F, 0.13F},
        {0.0F, 0.10F, -3.40F, 0.10F, 0.08F},
        {0.0F, 0.07F, -3.90F, 0.05F, 0.04F},
        {0.0F, 0.06F, -4.15F, 0.01F, 0.01F},
    }};
    constexpr std::size_t kTailPlates = 2;
    constexpr float kTailTuck = 0.3F;
    // The dorsal comb of side colour along the spine, four-sided so it is a BLADE, and cut into
    // teeth: three to a span, tucked by half.
    constexpr std::array<LoftStation, 5> kCrest{{
        {0.0F, 0.99F, 0.62F, 0.03F, 0.05F},
        {0.0F, 1.24F, 0.05F, 0.09F, 0.11F},
        {0.0F, 1.17F, -0.55F, 0.08F, 0.10F},
        {0.0F, 0.90F, -1.05F, 0.05F, 0.07F},
        {0.0F, 0.79F, -1.28F, 0.01F, 0.01F},
    }};
    constexpr std::size_t kCrestTeeth = 3;
    constexpr float kCrestTuck = 0.5F;
    // Three legs a side, from a hip inside the carapace out to a high knee and down to a foot 1.1 m
    // from the centre. The hips follow the carapace's width; the feet are splayed wider than the
    // dome so the machine stands like a crab, not on a skirt. Each is two jointed segments of
    // overlapping cones.
    constexpr std::array<float, 3> kLegAlong{0.70F, 0.05F, -0.65F};
    constexpr float kHipAcross = 0.50F;
    constexpr float kHipHeight = 0.60F;
    constexpr float kKneeAcross = 0.95F;
    constexpr float kKneeHeight = 0.85F;
    constexpr float kFootAcross = 1.10F;
    constexpr float kFootHeight = 0.03F; ///< the sole just off the ground
    constexpr float kHipThick = 0.13F;
    constexpr float kKneeThick = 0.10F;
    constexpr float kFootThick = 0.04F;
    constexpr std::size_t kLegPlates = 2;
    constexpr float kLegTuck = 0.35F;
    // The mandibles: curved blades off the head's flanks, closing toward the nose's tip (1.3 m),
    // which they never pass.
    constexpr std::array<LoftStation, 4> kMandible{{
        {0.24F, 0.46F, 0.95F, 0.07F, 0.09F},
        {0.34F, 0.40F, 1.08F, 0.06F, 0.075F},
        {0.26F, 0.36F, 1.22F, 0.04F, 0.05F},
        {0.10F, 0.34F, 1.29F, 0.008F, 0.008F},
    }};
    constexpr float kEyeAcross = 0.19F;
    constexpr float kEyeHeight = 0.69F;
    constexpr float kEyeAlong = 0.88F;
    constexpr float kEyeRadius = 0.075F;

    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    Vec3 camo_colour = colour;
    for (float& channel : camo_colour) {
        channel += (1.0F - channel) * kHullPaleMix;
    }

    emit_plated(out, x, z, ground, heading, kCarapace, kCarapacePlates, kCarapaceTuck,
                kCarapaceSides, camo_colour, hull_tile);
    emit_plated(out, x, z, ground, heading, kProboscis, kTailPlates, kTailTuck, kTailSides, colour,
                tracks_tile);
    emit_plated(out, x, z, ground, heading, kCrest, kCrestTeeth, kCrestTuck, kBladeSides, colour,
                panel_tile);
    for (const float side : {-1.0F, 1.0F}) {
        for (std::size_t index = 0; index < kLegAlong.size(); ++index) {
            const float along = kLegAlong.at(index);
            // Counted around the body, front-left first: the right side is 1 2 3 front to back
            // and the left side comes back 0 5 4. The even legs are one tripod, the odd the other.
            const std::size_t around = side > 0.0F ? 1U + index : (index == 0U ? 0U : 6U - index);
            // A machine with no stride stands at its static stance, the pose it was drawn in
            // before there was a gait: the feet at `along`, the knees at their static height.
            const LegPose pose = stride.has_value() ? leg_pose(*stride, around) : LegPose{};
            const std::array<LoftStation, 3> leg{{
                {side * kHipAcross, kHipHeight, along, kHipThick, kHipThick},
                {side * kKneeAcross, kKneeHeight + pose.lift, along + 0.5F * pose.along, kKneeThick,
                 kKneeThick},
                {side * kFootAcross, kFootHeight + pose.lift, along + pose.along, kFootThick,
                 kFootThick},
            }};
            emit_plated(out, x, z, ground, heading, leg, kLegPlates, kLegTuck, kLegSides, colour,
                        tracks_tile);
        }
        std::array<LoftStation, 4> mandible = kMandible;
        for (LoftStation& station : mandible) {
            station.x *= side;
        }
        emit_plated(out, x, z, ground, heading, mandible, 1, 0.3F, kBladeSides, colour,
                    tracks_tile);
        const std::array<LoftStation, 3> eye{{
            {side * kEyeAcross, kEyeHeight, kEyeAlong + kEyeRadius, 0.01F, 0.01F},
            {side * kEyeAcross, kEyeHeight, kEyeAlong, kEyeRadius, kEyeRadius},
            {side * kEyeAcross, kEyeHeight, kEyeAlong - kEyeRadius, 0.01F, 0.01F},
        }};
        emit_loft(out, x, z, ground, heading, eye, kEyeSides, colour, panel_tile);
    }
}

/// THE HAULER, the alien Tug: the SAME kind as the human tug, drawn when the unit's FACTION is
/// Alien (`build_scene` decides, once). A BEAST OF BURDEN, in the Harvester's idiom: a squat ox-
/// beetle 2.55 m from its horns to its tow-spike, the tug's width and no taller, on four pillar
/// legs. Its body is a ten-sided loft, nose to tail, cut into shingled plates (`plated_stations`:
/// the step between two plates is a wall square to the body, so the smooth pass cannot average it
/// away), and across its back sits a PACK -- a plated ridge of side colour, the load it carries and
/// the colour the side reads by from above. A pair of curved four-sided HORNS rise off the head and
/// a four-sided TOW-SPIKE trails behind it where the tug's hitch is. Its extent is the tug's: 2.55
/// m long (nose -1.45), 1.16 m across at the feet, 1.1 m to the top of the horns.
void build_hauler(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                  Mark mark, float lift_z, bool lifted, float heading) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect panel_tile = atlas.rect("tank_turret");
    constexpr std::size_t kBodySides = 10;
    constexpr std::size_t kLimbSides = 6;
    constexpr std::size_t kBladeSides = 4;
    constexpr std::array<LoftStation, 6> kBody{{
        {0.0F, 0.60F, 1.10F, 0.02F, 0.02F},  // the muzzle's tip
        {0.0F, 0.64F, 0.88F, 0.30F, 0.22F},  // the head, broad and low
        {0.0F, 0.70F, 0.42F, 0.44F, 0.32F},  // the shoulders
        {0.0F, 0.72F, -0.30F, 0.45F, 0.34F}, // the barrel of the body
        {0.0F, 0.66F, -0.95F, 0.38F, 0.28F},
        {0.0F, 0.58F, -1.30F, 0.02F, 0.02F}, // the rump
    }};
    constexpr std::size_t kBodyPlates = 2;
    constexpr float kBodyTuck = 0.2F;
    // The pack across the back: it stands a few centimetres over the body's top (1.06 m).
    constexpr std::array<LoftStation, 4> kPack{{
        {0.0F, 0.98F, 0.28F, 0.02F, 0.02F},
        {0.0F, 0.98F, 0.10F, 0.28F, 0.12F},
        {0.0F, 0.98F, -0.50F, 0.26F, 0.12F},
        {0.0F, 0.96F, -0.72F, 0.02F, 0.02F},
    }};
    constexpr std::size_t kPackPlates = 2;
    constexpr float kPackTuck = 0.3F;
    // Four legs, stout: a hip inside the body, a knee, a foot flat on the ground.
    constexpr std::array<float, 2> kLegAlong{0.62F, -0.62F};
    constexpr float kHipAcross = 0.32F;
    constexpr float kHipHeight = 0.60F;
    constexpr float kKneeAcross = 0.46F;
    constexpr float kKneeHeight = 0.34F;
    constexpr float kFootAcross = 0.50F;
    constexpr float kFootHeight = 0.03F;
    constexpr float kHipThick = 0.15F;
    constexpr float kKneeThick = 0.11F;
    constexpr float kFootThick = 0.07F;
    constexpr std::size_t kLegPlates = 2;
    constexpr float kLegTuck = 0.3F;
    // The horns curve up and forward off the head, inside the muzzle's (1.1 m) and the height.
    constexpr std::array<LoftStation, 4> kHorn{{
        {0.14F, 0.76F, 0.80F, 0.06F, 0.07F},
        {0.21F, 0.86F, 0.88F, 0.045F, 0.055F},
        {0.19F, 0.97F, 0.98F, 0.025F, 0.03F},
        {0.13F, 1.06F, 1.07F, 0.008F, 0.008F},
    }};
    // The tow-spike where the tug's hitch is, reaching 1.45 m behind the centre.
    constexpr std::array<LoftStation, 3> kSpike{{
        {0.0F, 0.62F, -1.05F, 0.09F, 0.10F},
        {0.0F, 0.64F, -1.28F, 0.06F, 0.07F},
        {0.0F, 0.72F, -1.45F, 0.008F, 0.008F},
    }};

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    emit_plated(out, x, z, ground, heading, kBody, kBodyPlates, kBodyTuck, kBodySides, camo_colour,
                hull_tile);
    emit_plated(out, x, z, ground, heading, kPack, kPackPlates, kPackTuck, kBodySides, colour,
                panel_tile);
    emit_plated(out, x, z, ground, heading, kSpike, 1, 0.3F, kBladeSides, colour, tracks_tile);
    for (const float side : {-1.0F, 1.0F}) {
        for (const float along : kLegAlong) {
            const std::array<LoftStation, 3> leg{{
                {side * kHipAcross, kHipHeight, along, kHipThick, kHipThick},
                {side * kKneeAcross, kKneeHeight, along, kKneeThick, kKneeThick},
                {side * kFootAcross, kFootHeight, along, kFootThick, kFootThick},
            }};
            emit_plated(out, x, z, ground, heading, leg, kLegPlates, kLegTuck, kLimbSides, colour,
                        tracks_tile);
        }
        std::array<LoftStation, 4> horn = kHorn;
        for (LoftStation& station : horn) {
            station.x *= side;
        }
        emit_plated(out, x, z, ground, heading, horn, 1, 0.3F, kBladeSides, colour, tracks_tile);
    }
}

/// THE GUARDIAN, the alien Turret: the SAME kind (and the DeployedTurret it becomes) as the human
/// turret, drawn when the unit's faction is Alien. A GUN EMPLACEMENT THAT IS ALSO AN ANIMAL: it
/// crouches on a stack of four flaring TIERS (a bracket fungus, a pagoda -- `plated_stations` swept
/// UP, so each tier is a cone that widens upward and ends in a ledge facing the sky) on four
/// hooked claws, and its head is a beetle's carapace -- plated like the Harvester's -- that turns
/// with the aim, with two eyes of side colour, a pair of curved MANDIBLES closing round the
/// barrel's root, and a thorn that leans back where the human turret has its aerial. The gun is
/// the same gun: the collar and the barrel are `emit_barrel`, from the same numbers, round, and the
/// last thing built so the player's elevation still reaches them. The extent is the turret's:
/// 1.2 m across, 1.4 m of base (the head 1 m), 1.8 m to the thorn's tip, the barrel to 2.0.
void build_guardian(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                    Mark mark, float lift_z, bool lifted, float heading, float aim, float pitch) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect panel_tile = atlas.rect("tank_turret");
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    constexpr float kBarrelHalfAcross = 0.08F;
    constexpr float kCollarHalfAcross = 0.13F;
    constexpr std::size_t kTierSides = 12;
    constexpr std::size_t kHeadSides = 10;
    constexpr std::size_t kClawSides = 6;
    constexpr std::size_t kBladeSides = 4;
    constexpr std::size_t kEyeSides = 8;
    // The tiers, swept UP from the ground: each is `across` over the aim and `up` along the heading
    // (the ring's second axis is the unit's z). Four tiers, tucked by a quarter; the ledge between
    // two faces the sky.
    constexpr std::array<LoftStation, 5> kTiers{{
        {0.0F, 0.00F, 0.0F, 0.60F, 0.70F},
        {0.0F, 0.32F, 0.0F, 0.52F, 0.60F},
        {0.0F, 0.58F, 0.0F, 0.46F, 0.50F},
        {0.0F, 0.84F, 0.0F, 0.40F, 0.42F},
        {0.0F, 1.10F, 0.0F, 0.32F, 0.32F},
    }};
    constexpr float kTierTuck = 0.25F;
    // The head, nose forward along the aim, 1.1 m to 1.7 m over the ground and 0.9 m across, drawn
    // out to a point at its muzzle through which the barrel comes.
    constexpr std::array<LoftStation, 6> kHead{{
        {0.0F, 1.36F, -0.50F, 0.02F, 0.02F},
        {0.0F, 1.38F, -0.38F, 0.30F, 0.26F},
        {0.0F, 1.40F, -0.05F, 0.44F, 0.30F},
        {0.0F, 1.38F, 0.30F, 0.36F, 0.26F},
        {0.0F, 1.33F, 0.52F, 0.16F, 0.14F},
        {0.0F, 1.30F, 0.60F, 0.02F, 0.02F},
    }};
    constexpr std::size_t kHeadPlates = 2;
    constexpr float kHeadTuck = 0.2F;
    // The thorn on the crown, leaning back; its tip is 1.8 m up, the aerial's height.
    constexpr std::array<LoftStation, 3> kThorn{{
        {0.0F, 1.58F, -0.08F, 0.05F, 0.07F},
        {0.0F, 1.70F, -0.26F, 0.035F, 0.05F},
        {0.0F, 1.79F, -0.46F, 0.008F, 0.008F},
    }};
    // The mandibles, round the barrel's root, which is at 1.2 to 1.36 m up and 0.08 across.
    constexpr std::array<LoftStation, 4> kMandible{{
        {0.30F, 1.18F, 0.10F, 0.08F, 0.10F},
        {0.36F, 1.17F, 0.40F, 0.07F, 0.085F},
        {0.26F, 1.22F, 0.78F, 0.045F, 0.055F},
        {0.14F, 1.28F, 0.98F, 0.008F, 0.008F},
    }};
    constexpr float kEyeAcross = 0.20F;
    constexpr float kEyeHeight = 1.60F;
    constexpr float kEyeAlong = 0.16F;
    constexpr float kEyeRadius = 0.08F;
    // Four hooked claws on the ground at the base's corners, from the first tier's flank.
    constexpr float kClawAcross = 0.50F; ///< the claw's foot; its sole is 3 cm across
    constexpr float kClawAlong = 0.62F;

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    emit_plated(out, x, z, ground, heading, kTiers, 1, kTierTuck, kTierSides, camo_colour,
                hull_tile);
    for (const float side : {-1.0F, 1.0F}) {
        for (const float end : {-1.0F, 1.0F}) {
            const std::array<LoftStation, 3> claw{{
                {side * 0.34F, 0.46F, end * 0.40F, 0.11F, 0.11F},
                {side * 0.54F, 0.26F, end * 0.58F, 0.06F, 0.06F},
                {side * kClawAcross, 0.03F, end * kClawAlong, 0.03F, 0.03F},
            }};
            emit_plated(out, x, z, ground, heading, claw, 2, 0.3F, kClawSides, colour, tracks_tile);
        }
    }
    emit_plated(out, x, z, ground, aim, kHead, kHeadPlates, kHeadTuck, kHeadSides, camo_colour,
                hull_tile);
    emit_plated(out, x, z, ground, aim, kThorn, 1, 0.3F, kBladeSides, colour, tracks_tile);
    for (const float side : {-1.0F, 1.0F}) {
        std::array<LoftStation, 4> mandible = kMandible;
        for (LoftStation& station : mandible) {
            station.x *= side;
        }
        emit_plated(out, x, z, ground, aim, mandible, 1, 0.3F, kBladeSides, colour, tracks_tile);
        const std::array<LoftStation, 3> eye{{
            {side * kEyeAcross, kEyeHeight, kEyeAlong + kEyeRadius, 0.01F, 0.01F},
            {side * kEyeAcross, kEyeHeight, kEyeAlong, kEyeRadius, kEyeRadius},
            {side * kEyeAcross, kEyeHeight, kEyeAlong - kEyeRadius, 0.01F, 0.01F},
        }};
        emit_loft(out, x, z, ground, aim, eye, kEyeSides, colour, panel_tile);
    }

    const std::size_t barrel_first = out.size();
    emit_barrel(out, x, z, aim, kCollarHalfAcross, kTurretBarrelTip - kTurretCollarLength,
                kTurretBarrelTip, ground + kTurretCollarBottom, ground + kTurretCollarTop, colour,
                barrel_tile);
    emit_barrel(out, x, z, aim, kBarrelHalfAcross, kTurretGunFront,
                kTurretBarrelTip - kTurretCollarLength, ground + kTurretBarrelBottom,
                ground + kTurretBarrelTop, colour, barrel_tile);
    elevate_barrel(out, barrel_first, x, z, aim, kTurretGunFront,
                   ground + ((kTurretBarrelBottom + kTurretBarrelTop) * 0.5F), pitch);
}

/// THE RECYCLER, after Battlezone II's ISDF recycler: a heavy tracked factory on the move -- broad,
/// low, blocky and faceted, its cab the highest thing on it -- held inside the tank's footprint
/// (0.3 tile either side of the unit, 1.3 m fore and aft). BZ2's is about 1.7 times as long as it
/// is wide; at the fleet's half-width this one is slenderer, about 2.2, rather than overhanging.
/// Nothing on it aims, so every part is turned by `heading` alone. From the ground up, in emission
/// order:
/// - a TRACK ASSEMBLY each side, left then right: the track, its run longer on top than on the
///   ground so its ends rake; six silver ROAD WHEELS standing off its outer face; and a SKIRT over
///   its upper half, ends sloped, whose top is the fender line the body sits on;
/// - a dark BELLY between the tracks, and on the right flank a dark SIDE PANEL on the skirt with a
///   thin team-colour STRIP along it, where BZ2's is lit red;
/// - the BODY, the rear two-thirds: chamfered sides, a rear face leaning out into an overhanging
///   deck, a raised silver SPINE down its middle, a louvred GRILLE either side of the spine and a
///   checkered BUMPER under the overhang;
/// - a silver PLOW wedge low at the front with a team-colour SLIT across its face;
/// - the CAB: a tall chamfered block toward the front, its face raked back, a dark VISION BAND
///   wrapped round it and a hatch on its roof;
/// - on the right flank, on the fender line behind the cab, a ribbed RADIATOR and the folded crane
///   ARM stowed flush: the lower boom, a hose, the elbow and the upper boom folded back over them.
/// The armour wears the team's camo in the pale tint, as the tank's does; the silver is that tint
/// on the metal tiles, the dark parts the track tile; the strip, the slit and the grilles' lit
/// slots are the full side colour, the fleet's saturated accent.
/// The parts are described at the tank's scale and drawn `kRecyclerScale` times that, about the
/// base centre; a DEPLOYED one is centred on its footprint and square to the tiles.
void build_recycler(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                    Mark mark, float lift_z, bool lifted, float drive_heading) {
    // A DEPLOYED one stands square on its footprint, at its centre -- the pad's own square and axes
    // (`sites_in_view`) -- whatever heading it is handed; a vehicle keeps its position and heading.
    const bool deployed = unit.kind == sim::UnitKind::DeployedRecycler;
    const sim::Footprint footprint = sim::footprint_around(unit.x, unit.y);
    const float x = deployed ? static_cast<float>(footprint.centre_x()) * kMetresPerTile
                             : static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne)
                                   * kMetresPerTile;
    const float z = deployed ? static_cast<float>(footprint.centre_y()) * kMetresPerTile
                             : static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne)
                                   * kMetresPerTile;
    const float heading = deployed ? 0.0F : drive_heading;
    // The parts are described at the tank's scale into `parts`; `kRecyclerScale` is applied once,
    // at the end, about the machine's base centre.
    std::vector<SceneVertex> parts;
    parts.reserve(kRecyclerVertices);
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect silver_tile = atlas.rect("tank_turret");
    const UvRect steel_tile = atlas.rect("tank_barrel");
    const UvRect roof_tile = atlas.rect("factory_roof");
    const UvRect hatch_tile = atlas.rect("turret_body");
    // Strips of tiles, in their texels: the roof tile's ribs and one of its louvred vents, the
    // turret tile's checker band and the round hatch above it.
    const UvRect ribs = upright_part(roof_tile, 0, 18, 64, 21);
    const UvRect vent = upright_part(roof_tile, 6, 4, 24, 18);
    const UvRect checker = upright_part(hatch_tile, 0, 52, 64, 62);
    const UvRect hatch = upright_part(hatch_tile, 0, 12, 64, 44);

    // Across is from the centre line, along from the unit's centre (+ is the front), up from the
    // ground.
    const float half_width = kHullHalfMetres; ///< the tank's footprint, every part inside it
    constexpr float kHalfLength = 1.3F;       ///< the tank's, fore and aft
    constexpr float kTrackInner = 0.36F;
    constexpr float kTrackOuter = 0.55F;
    constexpr float kTrackGround = 0.98F; ///< half the run on the ground
    constexpr float kTrackRun = 1.12F;    ///< half the run on top: the ends rake
    constexpr float kTrackTop = 0.5F;
    constexpr int kWheels = 6; ///< road wheels a side
    constexpr float kWheelPitch = 0.32F;
    constexpr float kWheelHalfLength = 0.075F;
    constexpr float kWheelOuter = 0.565F; ///< hubs standing off the track's outer face
    constexpr float kWheelBottom = 0.1F;
    constexpr float kWheelTop = 0.24F; ///< under the skirt
    constexpr float kSkirtInner = 0.33F;
    constexpr float kSkirtOuter = 0.58F;
    constexpr float kSkirtFoot = 1.22F;     ///< half its length at the bottom, past the track's run
    constexpr float kSkirtShoulder = 1.04F; ///< and at the top: the ends slope
    constexpr float kSkirtBottom = 0.28F;
    constexpr float kFender = 0.72F; ///< the skirts' top
    constexpr float kBellyHalfAcross = 0.37F;
    constexpr float kBellyBack = -1.05F;
    constexpr float kBellyFront = 0.98F; ///< behind the plow, under the cab's face
    constexpr float kBellyBottom = 0.14F;
    constexpr float kPanelInner = 0.56F;
    constexpr float kPanelOuter = 0.59F; ///< just proud of the skirt
    constexpr float kPanelBack = -0.98F;
    constexpr float kPanelFront = 0.12F;
    constexpr float kPanelBottom = 0.36F;
    constexpr float kPanelTop = 0.66F;
    constexpr float kStripInner = 0.585F;
    constexpr float kStripInset = 0.04F; ///< in from the panel's ends
    constexpr float kStripBottom = 0.56F;
    constexpr float kStripTop = 0.6F;
    constexpr float kBodyHalfAcross = 0.5F;
    constexpr float kBodyChamfer = 0.04F; ///< the flanks' bevel, as it was the old body's lean
    constexpr float kPodBevel = 0.03F;    ///< a track's and a skirt's flank edges
    constexpr float kBodyBack
        = -1.15F; ///< at its foot; the deck overhangs it to the footprint's end
    constexpr float kBodyFront = 0.25F; ///< tucked into the cab
    constexpr float kBodyBottom = 0.5F;
    constexpr float kDeck = 0.92F;
    constexpr float kSpineHalfAcross = 0.2F;
    constexpr float kSpineBack = -1.1F;
    constexpr float kSpineRake = 0.1F;
    constexpr float kSpineFront = 0.24F;
    constexpr float kSpineTop = 1.04F;
    constexpr float kGrilleInner = 0.25F;
    constexpr float kGrilleOuter = 0.42F;
    constexpr float kGrilleBack = -1.0F;
    constexpr float kGrilleFront = -0.3F;
    constexpr float kGrilleThickness = 0.02F; ///< standing just proud of the deck
    constexpr float kBumperHalfAcross = 0.5F;
    constexpr float kBumperFront = -1.0F; ///< into the belly and the skirts
    constexpr float kBumperBottom = 0.28F;
    constexpr float kBumperTop = 0.46F;
    constexpr float kPlowHalfAcross = 0.5F;
    constexpr float kPlowBack = 1.0F;
    constexpr float kPlowFoot = kHalfLength; ///< its face leans back from here to its brow
    constexpr float kPlowBrow = 1.18F;
    constexpr float kPlowBottom = 0.12F;
    constexpr float kPlowTop = 0.42F;
    constexpr float kSlitHalfAcross = 0.36F;
    constexpr float kSlitBottom = 0.24F;
    constexpr float kSlitTop = 0.29F;
    constexpr float kProud = 0.012F;        ///< how far a strip on a raked face stands off it
    constexpr float kCabHalfAcross = 0.52F; ///< at its foot, most of the width
    constexpr float kCabChamfer = 0.07F;    ///< the flanks' bevel
    constexpr float kHatchHalfAcross = 0.2F;
    constexpr float kHatchBack = 0.3F;
    constexpr float kHatchFront = 0.58F;
    constexpr float kHatchSink = 0.1F;    ///< buried in the dome so its sides never float
    constexpr float kHatchProud = 0.004F; ///< a hair over the roof's flat, so it never z-fights it
    constexpr float kCabBack = 0.15F;
    constexpr float kCabBackTop = 0.22F;
    constexpr float kCabFoot = 1.02F; ///< its face leans back from here to the roof
    constexpr float kCabBrow = 0.66F;
    constexpr float kCabBottom = 0.5F;
    constexpr float kCabRoof = 1.25F;        ///< the model's highest point
    constexpr float kBandHalfAcross = 0.49F; ///< just proud of the cab's sides: it wraps round
    constexpr float kBandBack = 0.4F;
    constexpr float kBandBottom = 0.95F;
    constexpr float kBandTop = 1.07F;
    constexpr float kRadiatorInner = 0.44F;
    constexpr float kRadiatorOuter = 0.59F; ///< projecting a little past the skirt
    constexpr float kRadiatorBack = -1.08F;
    constexpr float kRadiatorFront = -0.7F;
    constexpr float kRadiatorTop = 0.98F;
    constexpr float kRadiatorSink
        = 0.17F; ///< into the skirt: its rear end overhangs the rounded shoulder
    constexpr float kBoomInner = 0.5F;
    constexpr float kBoomOuter = 0.58F;
    constexpr float kBoomBack = -0.64F;
    constexpr float kBoomFront = 0.04F;
    constexpr float kBoomTop = 0.8F;
    constexpr float kHoseInner = 0.52F;
    constexpr float kHoseOuter = 0.565F;
    constexpr float kHoseBack = -0.5F;
    constexpr float kHoseFront = -0.06F;
    constexpr float kHoseTop = 0.86F;
    constexpr float kElbowInner = 0.49F;
    constexpr float kElbowOuter = 0.595F;
    constexpr float kElbowBack = 0.0F;
    constexpr float kElbowFront = 0.12F; ///< short of the cab's back
    constexpr float kElbowTop = 0.95F;
    constexpr float kUpperInner = 0.51F;
    constexpr float kUpperOuter = 0.57F;
    constexpr float kUpperTip = -0.52F; ///< its tool end, raked to `kUpperTipTop`
    constexpr float kUpperTipTop = -0.44F;
    constexpr float kUpperBottom = 0.86F;
    constexpr float kUpperTop = 0.93F;

    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    Vec3 pale = colour;
    for (float& channel : pale) {
        channel += (1.0F - channel) * kHullPaleMix;
    }

    const auto plate = [&](float l, float r, float back_bottom, float back_top, float front_bottom,
                           float front_top, float y0, float y1) {
        return Prism{l,         r,           back_bottom, back_top,   front_bottom,
                     front_top, ground + y0, ground + y1, ground + y1};
    };
    const auto box = [&](float l, float r, float back, float front, float y0, float y1) {
        return plate(l, r, back, back, front, front, y0, y1);
    };
    const auto part = [&](const Prism& s, const Vec3& tint, const UvRect& tile) {
        emit_turned_prism(parts, x, z, heading, s, tint, tile);
    };
    // A ROUNDED mass of the hull: the side profile `profile` ((along, up) corners, any winding)
    // cut by the SAME `rounded_profile` every vehicle's hull is, swept across `[lo, hi]` with its
    // flank edges bevelled. The profile is the old prism's side view, so its extent is unchanged
    // and a cut only ever takes material away.
    const auto mass = [&](std::vector<std::array<float, 2>> profile, float lo, float hi,
                          float bevel, const Vec3& tint, const UvRect& tile) {
        emit_shape_prism(parts, x, z, ground, heading,
                         ShapePart{rounded_profile(std::move(profile)), lo, hi, false, bevel,
                                   ShapeRole::Hull, true},
                         lo, hi, tint, tile);
    };
    // Where a raked face from (`foot`, `bottom`) back to (`brow`, `top`) is at height `y`.
    const auto raked = [](float foot, float brow, float bottom, float top, float y) {
        return foot - ((foot - brow) * (y - bottom) / (top - bottom));
    };

    // `inner` to `outer` from the centre line on the `side` (-1 left, +1 right) flank, as a span.
    const auto flank = [](float side, float inner, float outer) {
        return std::array<float, 2>{std::min(side * inner, side * outer),
                                    std::max(side * inner, side * outer)};
    };

    for (const float side : {-1.0F, 1.0F}) {
        const auto track = flank(side, kTrackInner, kTrackOuter);
        mass({{-kTrackGround, 0.0F},
              {kTrackGround, 0.0F},
              {kTrackRun, kTrackTop},
              {-kTrackRun, kTrackTop}},
             track[0], track[1], kPodBevel, colour, tracks_tile);
        const auto hub = flank(side, kTrackOuter, kWheelOuter);
        for (int wheel = 0; wheel < kWheels; ++wheel) {
            const float centre
                = (static_cast<float>(wheel) - (static_cast<float>(kWheels - 1) * 0.5F))
                  * kWheelPitch;
            part(box(hub[0], hub[1], centre - kWheelHalfLength, centre + kWheelHalfLength,
                     kWheelBottom, kWheelTop),
                 pale, steel_tile);
        }
        const auto skirt = flank(side, kSkirtInner, kSkirtOuter);
        mass({{-kSkirtFoot, kSkirtBottom},
              {kSkirtFoot, kSkirtBottom},
              {kSkirtShoulder, kFender},
              {-kSkirtShoulder, kFender}},
             skirt[0], skirt[1], kPodBevel, pale, hull_tile);
    }
    part(box(-kBellyHalfAcross, kBellyHalfAcross, kBellyBack, kBellyFront, kBellyBottom, kTrackTop),
         colour, tracks_tile);
    part(box(kPanelInner, kPanelOuter, kPanelBack, kPanelFront, kPanelBottom, kPanelTop), colour,
         tracks_tile);
    part(box(kStripInner, half_width, kPanelBack + kStripInset, kPanelFront - kStripInset,
             kStripBottom, kStripTop),
         colour, silver_tile);

    // The body: its rear face leans out from its foot to the deck's overhang.
    mass({{kBodyBack, kBodyBottom},
          {kBodyFront, kBodyBottom},
          {kBodyFront, kDeck},
          {-kHalfLength, kDeck}},
         -kBodyHalfAcross, kBodyHalfAcross, kBodyChamfer, pale, hull_tile);
    part(plate(-kSpineHalfAcross, kSpineHalfAcross, kSpineBack, kSpineBack + kSpineRake,
               kSpineFront, kSpineFront, kDeck, kSpineTop),
         pale, silver_tile);
    for (const float side : {-1.0F, 1.0F}) {
        const auto grille = flank(side, kGrilleInner, kGrilleOuter);
        part(box(grille[0], grille[1], kGrilleBack, kGrilleFront, kDeck, kDeck + kGrilleThickness),
             colour, vent);
    }
    part(box(-kBumperHalfAcross, kBumperHalfAcross, -kHalfLength, kBumperFront, kBumperBottom,
             kBumperTop),
         pale, checker);

    part(plate(-kPlowHalfAcross, kPlowHalfAcross, kPlowBack, kPlowBack, kPlowFoot, kPlowBrow,
               kPlowBottom, kPlowTop),
         pale, silver_tile);
    part(plate(-kSlitHalfAcross, kSlitHalfAcross, kPlowBack, kPlowBack,
               raked(kPlowFoot, kPlowBrow, kPlowBottom, kPlowTop, kSlitBottom) + kProud,
               raked(kPlowFoot, kPlowBrow, kPlowBottom, kPlowTop, kSlitTop) + kProud, kSlitBottom,
               kSlitTop),
         colour, silver_tile);

    mass({{kCabBack, kCabBottom},
          {kCabFoot, kCabBottom},
          {kCabBrow, kCabRoof},
          {kCabBackTop, kCabRoof}},
         -kCabHalfAcross, kCabHalfAcross, kCabChamfer, pale, hull_tile);
    // The roof hatch is a flat painted panel SET INTO the rounded roof (route b): the crown leaves
    // only a sliver of true flat, so the panel is sunk into the dome and stands a hair proud.
    part(box(-kHatchHalfAcross, kHatchHalfAcross, kHatchBack, kHatchFront, kCabRoof - kHatchSink,
             kCabRoof + kHatchProud),
         pale, hatch);
    part(plate(-kBandHalfAcross, kBandHalfAcross, kBandBack, kBandBack,
               raked(kCabFoot, kCabBrow, kCabBottom, kCabRoof, kBandBottom) + kProud,
               raked(kCabFoot, kCabBrow, kCabBottom, kCabRoof, kBandTop) + kProud, kBandBottom,
               kBandTop),
         colour, tracks_tile);

    part(box(kRadiatorInner, kRadiatorOuter, kRadiatorBack, kRadiatorFront, kFender - kRadiatorSink,
             kRadiatorTop),
         pale, ribs);
    part(box(kBoomInner, kBoomOuter, kBoomBack, kBoomFront, kFender, kBoomTop), pale, silver_tile);
    part(box(kHoseInner, kHoseOuter, kHoseBack, kHoseFront, kBoomTop, kHoseTop), colour,
         tracks_tile);
    part(box(kElbowInner, kElbowOuter, kElbowBack, kElbowFront, kFender, kElbowTop), pale,
         steel_tile);
    part(plate(kUpperInner, kUpperOuter, kUpperTip, kUpperTipTop, kElbowBack, kElbowBack,
               kUpperBottom, kUpperTop),
         pale, silver_tile);
    for (SceneVertex& vertex : parts) {
        vertex.position[0] = x + ((vertex.position[0] - x) * kRecyclerScale);
        vertex.position[1] = ground + ((vertex.position[1] - ground) * kRecyclerScale);
        vertex.position[2] = z + ((vertex.position[2] - z) * kRecyclerScale);
    }
    out.insert(out.end(), parts.begin(), parts.end());
}

/// THE HOVER TANK, built to read as NOT a tank at a glance: no track skirts at all, a low wedge
/// that steps in to a narrow nose and slopes down to it, riding on a bright band of lift -- the
/// skirt -- with a gap of air under it. The body is `shape_of(UnitKind::HoverTank)`, the one
/// description its plate is also projected from: the wedge, the dorsal fin and the small turret on
/// the deck, Hull parts turned by `heading` and the Turret part by `heading + turret`. What is not
/// the body's shape is added here, from the numbers in scene.hpp: the four SKIRT quads, which are
/// RAW colour -- not lit, not marked, the glow of the lift -- and the barrel over the nose, a
/// saturated side-colour accent as the tank's is, which elevates and so is always the last prism.
void build_hover_tank(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                      Mark mark, float lift_z, bool lifted, float heading, float turret,
                      float pitch) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect skirt_tile = atlas.rect("hover_skirt");
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    constexpr float kBarrelHalfAcross = 0.05F;

    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    Vec3 camo_colour = colour;
    for (float& channel : camo_colour) {
        channel += (1.0F - channel) * kHullPaleMix;
    }

    const float turret_angle = heading + turret;
    emit_shape_parts(out, x, z, ground, heading, turret_angle, shape_of(sim::UnitKind::HoverTank),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});

    // The skirt: one band quad on each of the wedge's four sides, tucked in under the hull's floor.
    const auto corner = [&](float dx, float dz, float y) {
        const std::array<float, 2> at = rotate_offset(x, z, dx, dz, heading);
        return Vec3{at.at(0), y, at.at(1)};
    };
    const float skirt_tail_half = kHullHalfMetres * kHoverSkirtInset;
    const float skirt_nose_half = kHoverNoseHalf * kHoverSkirtInset;
    const float skirt_tail = kHoverTail * kHoverSkirtInset;
    const float skirt_nose = kHoverNose * kHoverSkirtInset;
    const float top = ground + kHoverHullBottom;
    const float bottom = ground + kHoverSkirtBottom;
    const std::array<std::array<float, 2>, 4> ring{{{-skirt_tail_half, skirt_tail},
                                                    {skirt_tail_half, skirt_tail},
                                                    {skirt_nose_half, skirt_nose},
                                                    {-skirt_nose_half, skirt_nose}}};
    for (std::size_t side = 0; side < ring.size(); ++side) {
        const auto& a = ring.at(side);
        const auto& b = ring.at((side + 1) % ring.size());
        emit_tiled_quad(out, corner(a.at(0), a.at(1), top), corner(b.at(0), b.at(1), top),
                        corner(b.at(0), b.at(1), bottom), corner(a.at(0), a.at(1), bottom),
                        kHoverSkirtColour, skirt_tile);
    }

    // The barrel, last: raised to the aim's elevation about its root at the turret's front.
    const float turret_y0 = ground + kHoverTurretBase;
    const std::size_t barrel_first = out.size();
    emit_barrel(out, x, z, turret_angle, kBarrelHalfAcross, kHoverTurretFront, kHoverBarrelTip,
                turret_y0 + kHoverBarrelBottom, turret_y0 + kHoverBarrelTop, colour, barrel_tile);
    elevate_barrel(out, barrel_first, x, z, turret_angle, kHoverTurretFront,
                   turret_y0 + ((kHoverBarrelBottom + kHoverBarrelTop) * 0.5F), pitch);
}

/// THE MORTAR BIKE, built from `shape_of(UnitKind::MortarBike)`, the one description its plate is
/// also projected from: two wheels in line, a frame, a seat, a fork and handlebars, turned by
/// `heading`, and the mortar -- a mount, a stubby tube angled up 45 degrees (it lobs), the collar
/// at its mouth and the bore cut in it -- turned by `aim`. Nothing is added to the shape. The frame
/// wears the team's camo and the tube and collar the side colour, as a tank's barrel does.
void build_mortar_bike(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                       Mark mark, float lift_z, bool lifted, float heading, float aim) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));

    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }
    Vec3 camo_colour = colour;
    for (float& channel : camo_colour) {
        channel += (1.0F - channel) * kHullPaleMix;
    }

    emit_shape_parts(out, x, z, ground, heading, aim, shape_of(sim::UnitKind::MortarBike),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});
}

/// THE ASSAULT TANK, built from `shape_of(UnitKind::AssaultTank)`, the one description its plate is
/// also projected from: the tank's chassis under a taller, wider turret block, Hull parts turned by
/// `heading` and the Turret parts by `heading + turret`. What is not the body's shape is added
/// here, from the numbers in scene.hpp: the team-colour panel on the block's roof, and the muzzle
/// collar and the barrel, which elevate, so the barrel is always the last prism.
void build_assault_tank(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                        Mark mark, float lift_z, bool lifted, float heading, float turret,
                        float pitch) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    constexpr float kPanelHalfAcross = 0.3F;
    constexpr float kPanelBack = -0.7F; ///< the block's rear: the panel runs its full length
    constexpr float kPanelThickness = 0.03F;
    constexpr float kBarrelHalfAcross = 0.08F; ///< a little stouter than the tank's 0.06
    constexpr float kCollarHalfAcross = 0.12F;

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    const float turret_angle = heading + turret;
    emit_shape_parts(out, x, z, ground, heading, turret_angle, shape_of(sim::UnitKind::AssaultTank),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});

    const float roof = ground + kAssaultTurretRoof;
    emit_turned_box(out, x, z, turret_angle, kPanelHalfAcross, kPanelBack, kAssaultTurretFront,
                    roof, roof + kPanelThickness, colour, atlas.rect("tank_turret"));
    const float base = ground + kTankTurretBase;
    const std::size_t barrel_first = out.size();
    emit_barrel(out, x, z, turret_angle, kCollarHalfAcross,
                kAssaultBarrelTip - kAssaultCollarLength, kAssaultBarrelTip,
                base + kAssaultCollarBottom, base + kAssaultCollarTop, colour, barrel_tile);
    emit_barrel(out, x, z, turret_angle, kBarrelHalfAcross, kAssaultTurretFront,
                kAssaultBarrelTip - kAssaultCollarLength, base + kAssaultBarrelBottom,
                base + kAssaultBarrelTop, colour, barrel_tile);
    elevate_barrel(out, barrel_first, x, z, turret_angle, kAssaultTurretFront,
                   base + ((kAssaultBarrelBottom + kAssaultBarrelTop) * 0.5F), pitch);
}

/// THE ROCKET TANK, built from `shape_of(UnitKind::RocketTank)`, the one description its plate is
/// also projected from: the tank's chassis, a mount and a broad launcher rack, Hull parts turned by
/// `heading` and the Turret parts (mount and rack) by `heading + turret`. What is not the body's
/// shape is added here: the team-colour panel lying on the rack's top. No gun, so no barrel.
void build_rocket_tank(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                       Mark mark, float lift_z, bool lifted, float heading, float turret) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    constexpr float kPanelHalfAcross = 0.22F; ///< of the rack's 0.42: camo shows either side
    constexpr float kPanelBack = -0.25F;      ///< along the aim from the centre
    constexpr float kPanelFront = 0.5F;

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    const float rack_angle = heading + turret;
    emit_shape_parts(out, x, z, ground, heading, rack_angle, shape_of(sim::UnitKind::RocketTank),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});
    emit_turned_box(out, x, z, rack_angle, kPanelHalfAcross, kPanelBack, kPanelFront,
                    ground + kRocketRackTop, ground + kRocketRackTop + kRocketPanelThickness,
                    colour, atlas.rect("tank_turret"));
}

/// THE WALKER, built from `shape_of(UnitKind::Walker)`, the one description its plate is also
/// projected from: two bent legs and their feet, a body with a cockpit glass and a team-colour roof
/// panel, all turned by `heading`. What is not the body's shape is added here, from the numbers in
/// scene.hpp: the gun arm held out forward, turned by `heading + turret`, which elevates, so it is
/// always the last prism.
void build_walker(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                  Mark mark, float lift_z, bool lifted, float heading, float turret, float pitch,
                  std::optional<float> stride) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    constexpr float kGunHalfAcross = 0.07F;

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    const ShapeLook look{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                         atlas.rect("tank_turret")};
    // THE LEGS FIRST, as they have always been painted: a biped's gait, the right (front) leg and
    // the left (rear) one half a cycle apart. A machine that has never moved has no phase and
    // stands at phase 0, the authored stance.
    const float phase = stride.value_or(0.0F);
    emit_shape_parts(out, x, z, ground, heading, heading + turret,
                     walker_legs(leg_pose(phase, 0, kWalkerStrideHalfMetres),
                                 leg_pose(phase, 1, kWalkerStrideHalfMetres)),
                     look);
    emit_shape_parts(out, x, z, ground, heading, heading + turret, shape_of(sim::UnitKind::Walker),
                     look);

    const std::size_t gun_first = out.size();
    emit_barrel(out, x, z, heading + turret, kGunHalfAcross, kWalkerGunFront, kWalkerGunTip,
                ground + kWalkerGunBottom, ground + kWalkerGunTop, colour,
                atlas.rect("tank_barrel"));
    elevate_barrel(out, gun_first, x, z, heading + turret, kWalkerGunFront,
                   ground + ((kWalkerGunBottom + kWalkerGunTop) * 0.5F), pitch);
}

/// THE APC, built from `shape_of(UnitKind::Apc)`, the one description its plate is also projected
/// from: a long slab-sided hull with a sloped nose on four wheels a side, its firing ports and a
/// team-colour roof panel, turned by `heading`, and a small turret with a light gun, turned by
/// `heading + turret`. Nothing is added to the shape.
void build_apc(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit, Mark mark,
               float lift_z, bool lifted, float heading, float turret) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    emit_shape_parts(out, x, z, ground, heading, heading + turret, shape_of(sim::UnitKind::Apc),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});
}

/// THE BOMBER, built from `shape_of(UnitKind::Bomber)` and from nothing else: this function holds
/// no number about what the machine looks like, and the 72 px plate under the command row is the
/// same parts seen side-on, so the two cannot disagree. Turned by `heading` alone; the bomb leaves
/// it by the sim's lob, with no barrel.
void build_bomber(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                  Mark mark, float lift_z, bool lifted, float heading) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    emit_shape_parts(out, x, z, ground, heading, heading, shape_of(sim::UnitKind::Bomber),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});
}

/// THE FOUR SUPPORT VEHICLES (the tug, the constructor, the service truck and the service pod),
/// ONE builder for all of them: each is built from `shape_of(kind)` and from nothing else, the one
/// description its plate is also projected from, so this function holds no number about what any
/// of them looks like. Turned by `heading` alone; nothing on them aims.
void build_support_vehicle(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                           Mark mark, float lift_z, bool lifted, float heading) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    emit_shape_parts(out, x, z, ground, heading, heading, shape_of(unit.kind),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});
}

/// A DEFENCE TURRET, driven or not -- ONE builder for both paths, so its mesh cannot depend on
/// whether a player is at the controls. It is built from `shape_of(UnitKind::Turret)` (the
/// DeployedTurret's too), the one description its plate is also projected from: the base and the
/// pedestal are Hull parts, turned by `heading` (the turret crawls and faces its course as a tank
/// does), and the dome, its slit and its aerial the Turret group, turned by `aim`. What is not the
/// body's shape is added here, from the numbers in scene.hpp: the muzzle collar and the barrel,
/// which elevate (only a driven turret's, by `pitch`), so the barrel is always the last prism. The
/// base wears the camo and the dome the plain turret metal, as a tank's turret does; the collar and
/// barrel carry the full side colour, as a tank's do.
void build_defense_turret(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                          Mark mark, float lift_z, bool lifted, float heading, float aim,
                          float pitch) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    constexpr float kBarrelHalfAcross = 0.08F;
    constexpr float kCollarHalfAcross = 0.13F;

    const auto [colour, camo_colour] = kind_colours(unit, mark);
    emit_shape_parts(out, x, z, ground, heading, aim, shape_of(sim::UnitKind::Turret),
                     ShapeLook{colour, camo_colour, hull_tile, atlas.rect("tank_tracks"),
                               atlas.rect("tank_turret")});

    const std::size_t barrel_first = out.size();
    emit_barrel(out, x, z, aim, kCollarHalfAcross, kTurretBarrelTip - kTurretCollarLength,
                kTurretBarrelTip, ground + kTurretCollarBottom, ground + kTurretCollarTop, colour,
                barrel_tile);
    emit_barrel(out, x, z, aim, kBarrelHalfAcross, kTurretGunFront,
                kTurretBarrelTip - kTurretCollarLength, ground + kTurretBarrelBottom,
                ground + kTurretBarrelTop, colour, barrel_tile);
    elevate_barrel(out, barrel_first, x, z, aim, kTurretGunFront,
                   ground + ((kTurretBarrelBottom + kTurretBarrelTop) * 0.5F), pitch);
}

/// A tracer is brighter than anything it flies over: a unit colour is a thing standing on the
/// ground, but a bolt is a few decimetres wide and must not be lost against grass, rock or a
/// side's own tanks. Two hues, so incoming fire and outgoing fire read apart at a glance --
/// a bright cyan for the viewer's side, a bright orange-yellow for the other.
constexpr Vec3 kOwnBoltColour{0.30F, 1.0F, 1.0F};
constexpr Vec3 kHostileBoltColour{1.0F, 0.85F, 0.10F};
/// A tracer's half-width (not a tank-sized cube). Its height is the bolt's own: the sim says how
/// high it is, and it is drawn there, not at some height re-derived from the ground it is over.
constexpr float kBoltHalfWidthMetres = 0.12F;

/// One small box per projectile the sim has in the air, at the units' tile-to-metre conversion and
/// with the six faces, in the winding, that `emit_shaded_solid` gives a unit part. A shell with
/// gravity (the mortar's) also trails three dimmer, smaller boxes at the places it was one, two and
/// three ticks ago, so its arc reads as a streak against the sky.
void emit_bolt_box(std::vector<SceneVertex>& out, float x, float centre, float z, float half,
                   const Vec3& colour, float alpha = 1.0F) {
    const float x0 = x - half;
    const float x1 = x + half;
    const float z0 = z - half;
    const float z1 = z + half;
    const float y0 = centre - half;
    const float y1 = centre + half;
    const std::array<Vec3, 8> p{{{x0, y0, z0},
                                 {x1, y0, z0},
                                 {x1, y1, z0},
                                 {x0, y1, z0},
                                 {x0, y0, z1},
                                 {x1, y0, z1},
                                 {x1, y1, z1},
                                 {x0, y1, z1}}};
    emit_quad(out, p[0], p[1], p[2], p[3], colour, alpha); // near
    emit_quad(out, p[5], p[4], p[7], p[6], colour, alpha); // far
    emit_quad(out, p[4], p[0], p[3], p[7], colour, alpha); // left
    emit_quad(out, p[1], p[5], p[6], p[2], colour, alpha); // right
    emit_quad(out, p[3], p[2], p[6], p[7], colour, alpha); // top
    emit_quad(out, p[4], p[5], p[1], p[0], colour, alpha); // bottom
}

constexpr std::size_t kShellTrailPuffs = 3;
constexpr float kShellTrailShade = 0.55F;

/// A resting bouncer (M3): a fat core with four spokes turning as it spins, and eight shrapnel
/// chips flying out to the pulse radius over every pulse interval, so the damage reach is on
/// screen.
constexpr Vec3 kShrapnelColour{1.0F, 0.45F, 0.05F};
constexpr float kSpinPerTick = 0.35F;

void emit_resting_bouncer(std::vector<SceneVertex>& out, const sim::Projectile& bolt, float x,
                          float centre, float z, const Vec3& colour) {
    constexpr float kTau = 6.2831853F;
    emit_bolt_box(out, x, centre, z, kBoltHalfWidthMetres * 2.0F, colour);
    const float spin = static_cast<float>(bolt.rested) * kSpinPerTick;
    for (int spoke = 0; spoke < 4; ++spoke) {
        const float a = spin + (static_cast<float>(spoke) * kTau / 4.0F);
        emit_bolt_box(out, x + (std::cos(a) * 0.45F), centre, z + (std::sin(a) * 0.45F),
                      kBoltHalfWidthMetres, colour);
    }
    const float phase = static_cast<float>(bolt.rested % sim::kBouncer.pulse_interval)
                        / static_cast<float>(sim::kBouncer.pulse_interval);
    const float reach = static_cast<float>(sim::kBouncer.pulse_radius.raw)
                        / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile * phase;
    for (int chip = 0; chip < 8; ++chip) {
        const float a = static_cast<float>(chip) * kTau / 8.0F;
        emit_bolt_box(out, x + (std::cos(a) * reach), centre, z + (std::sin(a) * reach),
                      kBoltHalfWidthMetres * 1.3F, kShrapnelColour);
    }
}

void append_projectiles(std::vector<SceneVertex>& out, const SceneOptions& options,
                        const sim::Sim& sim) {
    constexpr float kOne = static_cast<float>(sim::Fixed::kOne);
    for (const sim::Projectile& bolt : sim.projectiles()) {
        const float x = static_cast<float>(bolt.x.raw) / kOne * kMetresPerTile;
        const float z = static_cast<float>(bolt.y.raw) / kOne * kMetresPerTile;
        const float centre = static_cast<float>(bolt.z.raw) / kOne * kMetresPerHeightUnit;
        const Vec3 colour = bolt.side == options.viewer_side ? kOwnBoltColour : kHostileBoltColour;
        if (bolt.resting) {
            emit_resting_bouncer(out, bolt, x, centre, z, colour);
            continue;
        }
        emit_bolt_box(out, x, centre, z, kBoltHalfWidthMetres, colour);
        if (bolt.gravity.raw <= 0) {
            continue;
        }
        const Vec3 dim{colour[0] * kShellTrailShade, colour[1] * kShellTrailShade,
                       colour[2] * kShellTrailShade};
        for (std::size_t puff = 1; puff <= kShellTrailPuffs; ++puff) {
            const float back = static_cast<float>(puff);
            const float shrink = 1.0F - (0.2F * back);
            emit_bolt_box(
                out, x - (static_cast<float>(bolt.vx.raw) / kOne * kMetresPerTile * back),
                centre - (static_cast<float>(bolt.vz.raw) / kOne * kMetresPerHeightUnit * back),
                z - (static_cast<float>(bolt.vy.raw) / kOne * kMetresPerTile * back),
                kBoltHalfWidthMetres * shrink, dim);
        }
    }
}

/// THE LANDING MARKER: a bracketed cross standing upright on the touchdown tile, turned to face the
/// camera, 4 m tall and up to 3.2 m across in hot magenta (nothing in the terrain or sky is that
/// colour), so a shell that comes down on empty ground at a low view pitch -- where flat paint on
/// the ground would be edge-on and invisible -- still shows exactly where. The X is two diagonal
/// bars; the four corner brackets are an L of two bars each. It shrinks away across the fade.
/// Always `kLandingMarkVertexCount` vertices per mark.
constexpr Vec3 kLandingMarkColour{1.0F, 0.1F, 0.85F};
/// The mark is 4 m tall whatever the range (the eye is higher than a shell's close landings, so the
/// height is what keeps it in the frame), but its width follows the range -- 0.10 of it, from 0.3
/// to 1.6 m either side -- so a shell that lands at your feet is a tall narrow cross, not a wall.
/// Measured (M5c): the old 0.3-per-metre with a 0.7 m floor gave a 1.9 m-wide cross at 1.6 tiles,
/// which covered a third of the frame's width and eighty percent of its height -- a magenta wall in
/// the player's face at exactly the range the mark exists for.
constexpr float kLandingMarkWidthPerMetre = 0.10F;
constexpr float kLandingMarkMinHalfWidth = 0.3F;
constexpr float kLandingMarkMaxHalfWidth = 1.6F;
constexpr float kLandingMarkBarFraction = 0.125F; ///< half-thickness of a bar, of the half-width
/// A floor on a bar's half-thickness, so the narrowest mark still draws strokes instead of threads.
constexpr float kLandingMarkMinBarMetres = 0.06F;
constexpr float kLandingMarkArmFraction = 0.55F; ///< how far a bracket reaches, of the half-width

void append_landing_marks(std::vector<SceneVertex>& out, const SceneOptions& options,
                          sim::Tick now) {
    if (options.landing_marks == nullptr) {
        return;
    }
    for (const LandingMarks::Mark& mark : options.landing_marks->active(now)) {
        const float tile_x = static_cast<float>(mark.tile_x) + 0.5F;
        const float tile_y = static_cast<float>(mark.tile_y) + 0.5F;
        const float cx = tile_x * kMetresPerTile;
        const float cz = tile_y * kMetresPerTile;
        const float ground = ground_metres(options.seed, tile_x, tile_y);
        // Face the camera: `right` is the horizontal direction across the line from it to the mark.
        float to_x = options.camera.x - cx;
        float to_z = options.camera.z - cz;
        const float length = std::sqrt((to_x * to_x) + (to_z * to_z));
        if (length < 1e-4F) {
            continue; // standing on it: nothing sensible to face
        }
        to_x /= length;
        to_z /= length;
        const float right_x = to_z;
        const float right_z = -to_x;
        const float full = std::clamp(length * kLandingMarkWidthPerMetre, kLandingMarkMinHalfWidth,
                                      kLandingMarkMaxHalfWidth);
        const float half = full * mark.level;
        const float height = kLandingMarkHeight * mark.level;
        const float bar
            = std::max(half * kLandingMarkBarFraction, kLandingMarkMinBarMetres * mark.level);
        const float arm = half * kLandingMarkArmFraction;
        const auto at = [&](float u, float v) {
            return Vec3{cx + (right_x * u), ground + v, cz + (right_z * u)};
        };
        // A bar of half-thickness `bar` from (u0, v0) to (u1, v1), in the mark's own plane.
        const auto stroke = [&](float u0, float v0, float u1, float v1) {
            const float du = u1 - u0;
            const float dv = v1 - v0;
            const float run = std::sqrt((du * du) + (dv * dv));
            const float nu = -dv / run * bar;
            const float nv = du / run * bar;
            emit_quad(out, at(u0 + nu, v0 + nv), at(u1 + nu, v1 + nv), at(u1 - nu, v1 - nv),
                      at(u0 - nu, v0 - nv), kLandingMarkColour);
        };
        stroke(-half, 0.0F, half, height);
        stroke(-half, height, half, 0.0F);
        for (const float side : {-1.0F, 1.0F}) {
            stroke(side * half, 0.0F, side * (half - arm), 0.0F);
            stroke(side * half, 0.0F, side * half, arm);
            stroke(side * half, height, side * (half - arm), height);
            stroke(side * half, height, side * half, height - arm);
        }
    }
}

/// THE WRECKAGE ON THE GROUND (C5). C1 scatters four pieces of a dead unit's hull around it and
/// settles each onto ground a vehicle can stand on; until this layer existed nothing drew them, so
/// the litter the sim has been keeping was invisible to the player. Each piece is a small dark
/// plate lying flat on the terrain (`kScrapHalfMetres`, smaller than the footprint of the tank that
/// died), lifted `kScrapLiftMetres` clear so it does not fight the ground beneath it.
///
/// EMITS NOTHING while the field holds no litter, which is what keeps this layer free for every
/// frame that has never lost a unit: the vertex list -- and so the tail that other tests read -- is
/// exactly what it was. Culled to the meshed radius like the units and the beacons, so a piece
/// never floats over bare void.
void append_scrap(std::vector<SceneVertex>& out, const SceneOptions& options, const sim::Sim& sim) {
    if (!options.show_scrap) {
        return;
    }
    for (const sim::Scrap& piece : sim.scraps()) {
        const int dx
            = piece.x.floor_int() - static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
        const int dy
            = piece.y.floor_int() - static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
        if (std::abs(dx) > options.radius_tiles || std::abs(dy) > options.radius_tiles) {
            continue;
        }
        const float tile_x = static_cast<float>(piece.x.raw) / static_cast<float>(sim::Fixed::kOne);
        const float tile_y = static_cast<float>(piece.y.raw) / static_cast<float>(sim::Fixed::kOne);
        const float cx = tile_x * kMetresPerTile;
        const float cz = tile_y * kMetresPerTile;
        const float ground = ground_metres(options.seed, tile_x, tile_y) + kScrapLiftMetres;
        // The same corner order the terrain mesh uses, for consistency rather than necessity:
        // the pipeline culls nothing.
        emit_quad(out, Vec3{cx - kScrapHalfMetres, ground, cz - kScrapHalfMetres},
                  Vec3{cx + kScrapHalfMetres, ground, cz - kScrapHalfMetres},
                  Vec3{cx + kScrapHalfMetres, ground, cz + kScrapHalfMetres},
                  Vec3{cx - kScrapHalfMetres, ground, cz + kScrapHalfMetres}, kScrapColour);
    }
}

/// CONTACT SHADOWS (see `kShadowInk`): one flat translucent quad per LIVING unit, at the unit's own
/// x/y and the terrain height there, sized by `kShadowHalfMetres` (the hull drawn above it). The
/// viewer's own hull gets one too: it is not drawn in first person, and the patch is the cue left.
/// Culled to the meshed radius exactly as the unit emission is. Drawn BEFORE the hulls and the
/// bars, so it reads as ground under them.
void append_unit_shadows(std::vector<SceneVertex>& out, const SceneOptions& options,
                         const sim::Sim& sim) {
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || !available_to_viewer(sim, options, unit)) {
            continue;
        }
        const float tile_x = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne);
        const float tile_y = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne);
        const float cx = tile_x * kMetresPerTile;
        const float cz = tile_y * kMetresPerTile;
        const float half = kShadowHalfMetres;
        const float ground = ground_metres(options.seed, tile_x, tile_y) + kShadowLiftMetres;
        emit_quad(out, Vec3{cx - half, ground, cz - half}, Vec3{cx + half, ground, cz - half},
                  Vec3{cx + half, ground, cz + half}, Vec3{cx - half, ground, cz + half},
                  kShadowInk, kShadowAlpha);
    }
}

/// D4: a wake's own numbers. splitmix64 over the pair, as `hull_variant` does, so two wrecks do not
/// get the same picture and one wreck's puffs are not a regular ring: sequential ids and sequential
/// puffs both land far apart before the fraction is taken.
float wake_fraction(std::uint64_t seed, std::uint32_t wreck, std::uint32_t salt) noexcept {
    std::uint64_t z = seed + (static_cast<std::uint64_t>(wreck) * 0x9E3779B97F4A7C15ULL)
                      + (static_cast<std::uint64_t>(salt) * 0xD1B54A32D192ED03ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<float>(z & 0xFFFFFFU) / static_cast<float>(0x1000000U);
}

/// How fast a puff rolls, radians per tick, either way and never faster than
/// `kPuffRollRadiansPerTick`: a seeded fraction of the range, as the puff's other numbers are.
float puff_roll_rate(std::uint64_t seed, std::uint32_t owner, std::uint32_t salt) noexcept {
    return ((wake_fraction(seed, owner, salt) * 2.0F) - 1.0F) * kPuffRollRadiansPerTick;
}

/// One puff: a `kPuffSides`-cornered prism centred on (x, centre, z), `half` from the centre to
/// each flat and to each cap, so it is as wide and as tall as the box it replaced. Corners sit
/// half a step round from the axes, which puts the flats on the box's own faces. Winding is the
/// box's: the sides run the way the box's near face does, the top counter-clockwise seen from
/// above, the bottom the reverse. Each cap is a fan from its first corner, so the prism costs
/// `prism_vertices(sides)` and every edge is shared by exactly two triangles. `sides` is at most
/// `kPuffSides`: a spark is the same prism with four.
void emit_puff(std::vector<SceneVertex>& out, float x, float centre, float z, float half,
               const Vec3& colour, float alpha, std::size_t sides = kPuffSides) {
    constexpr float kTau = 6.2831853F;
    const float step = kTau / static_cast<float>(sides);
    // The flat is `half` away, so the corner is `half / cos(step / 2)`.
    const float radius = half / std::cos(step * 0.5F);
    std::array<std::array<float, 2>, kPuffSides> rim{}; // a particle has at most a puff's corners
    for (std::size_t corner = 0; corner < sides; ++corner) {
        const float angle = (static_cast<float>(corner) + 0.5F) * step;
        rim.at(corner) = {x + (std::cos(angle) * radius), z + (std::sin(angle) * radius)};
    }
    const float y0 = centre - half;
    const float y1 = centre + half;
    for (std::size_t corner = 0; corner < sides; ++corner) {
        const auto& a = rim.at(corner);
        const auto& b = rim.at((corner + 1) % sides);
        emit_quad(out, Vec3{a.at(0), y0, a.at(1)}, Vec3{b.at(0), y0, b.at(1)},
                  Vec3{b.at(0), y1, b.at(1)}, Vec3{a.at(0), y1, a.at(1)}, colour, alpha);
    }
    for (std::size_t corner = 1; corner + 1 < sides; ++corner) {
        const auto& b = rim.at(corner);
        const auto& c = rim.at(corner + 1);
        emit_triangle(out, Vec3{rim.at(0).at(0), y1, rim.at(0).at(1)}, Vec3{b.at(0), y1, b.at(1)},
                      Vec3{c.at(0), y1, c.at(1)}, colour, alpha);
        emit_triangle(out, Vec3{rim.at(0).at(0), y0, rim.at(0).at(1)}, Vec3{c.at(0), y0, c.at(1)},
                      Vec3{b.at(0), y0, b.at(1)}, colour, alpha);
    }
}

/// A SPHERE of the repair beam: an icosahedron (`kSphereFaces` = 20 triangles) whose twelve corners
/// all stand `radius` from (x, y, z). Corners are the three golden rectangles' (0, +-1, +-phi)
/// and its two rotations, pushed out to the radius; the faces are the classic twenty, wound
/// outward. The renderer does not cull; each face carries its outward normal (AP), and the
/// smoothing pass blends the twelve corners' into a ball's, so the sphere is lit as a round thing.
/// `roll` (radians, 0 = the corners as listed) turns it about the vertical and leans it over by
/// 0.6 of the same angle, so two spheres with different rolls are not the same picture (AH).
void emit_sphere(std::vector<SceneVertex>& out, float x, float y, float z, float radius,
                 const Vec3& colour, float alpha, float roll = 0.0F) {
    constexpr float kPhi = 1.6180340F;
    constexpr std::array<std::array<float, 3>, 12> kCorners{{{-1.0F, kPhi, 0.0F},
                                                             {1.0F, kPhi, 0.0F},
                                                             {-1.0F, -kPhi, 0.0F},
                                                             {1.0F, -kPhi, 0.0F},
                                                             {0.0F, -1.0F, kPhi},
                                                             {0.0F, 1.0F, kPhi},
                                                             {0.0F, -1.0F, -kPhi},
                                                             {0.0F, 1.0F, -kPhi},
                                                             {kPhi, 0.0F, -1.0F},
                                                             {kPhi, 0.0F, 1.0F},
                                                             {-kPhi, 0.0F, -1.0F},
                                                             {-kPhi, 0.0F, 1.0F}}};
    constexpr std::array<std::array<std::size_t, 3>, kSphereFaces> kFaces{
        {{0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
         {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
         {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}}};
    const float scale = radius / std::sqrt(1.0F + (kPhi * kPhi));
    const float yaw_cos = std::cos(roll);
    const float yaw_sin = std::sin(roll);
    const float lean_cos = std::cos(roll * 0.6F);
    const float lean_sin = std::sin(roll * 0.6F);
    const auto corner = [&](std::size_t index) {
        const auto& c = kCorners.at(index);
        const float turned_x = (c.at(0) * yaw_cos) + (c.at(2) * yaw_sin);
        const float turned_z = (c.at(2) * yaw_cos) - (c.at(0) * yaw_sin);
        const float leaned_y = (c.at(1) * lean_cos) - (turned_z * lean_sin);
        const float leaned_z = (c.at(1) * lean_sin) + (turned_z * lean_cos);
        return Vec3{x + (turned_x * scale), y + (leaned_y * scale), z + (leaned_z * scale)};
    };
    const Vec3 centre{x, y, z};
    for (const auto& face : kFaces) {
        const Vec3 a = corner(face.at(0));
        const Vec3 b = corner(face.at(1));
        const Vec3 c = corner(face.at(2));
        // Outward by construction (the corners stand `radius` from the centre), whatever the
        // winding: the face's normal is turned to point away from the centre.
        const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        emit_triangle(out, a, b, c, colour, alpha,
                      outward_unit(cross(ab, ac),
                                   Vec3{a[0] - centre[0], a[1] - centre[1], a[2] - centre[2]}));
    }
}

/// A SPARK as a `Particle` (AG, AI): `half` wide, near white-hot cooling to amber over its
/// `lifetime`, OPAQUE for all of it, falling under `kSparkGravity`, and kept
/// `kSparkFloorPerMetre` wide per metre of distance so it never drops under a few pixels. The
/// blast's sparks and the weld's are both this, from different origins and velocities.
Particle spark_spawn(const Vec3& origin, const Vec3& velocity, sim::Tick lifetime, float half) {
    Particle spark;
    spark.origin = origin;
    spark.velocity = velocity;
    spark.gravity = kSparkGravity;
    spark.lifetime = lifetime;
    spark.half_from = half;
    spark.half_to = half;
    spark.colour_from = kSparkHotColour;
    spark.colour_to = kSparkCoolColour;
    spark.shape = ParticleShape::Prism;
    spark.sides = kSparkSides;
    spark.floor_per_metre = kSparkFloorPerMetre;
    return spark;
}

/// THE WAKE A WRECK LEAVES (D4): a burnt-out unit throws a burst of hot puffs for `kBlastTicks`,
/// and then smokes for `kSmokeTicks`. Both are DERIVED from what the sim already knows -- the death
/// is a `Killed` event, the tile is the dead unit's own (the table keeps the dead), and every
/// puff's place comes out of `wake_fraction` on the wreck's id and the frame's seed. Nothing is
/// stored per puff, so the order stream stays the whole input and a replay needs no effects in it.
///
/// EMITS NOTHING without a `Killed` event: the layer is free for every frame that lost no unit, and
/// a wreck beyond the meshed radius is skipped rather than floated over bare void.
void append_effects(std::vector<SceneVertex>& out, std::vector<VertexRun>& emissive,
                    const SceneOptions& options, const sim::Sim& sim) {
    if (options.events.empty()) {
        return;
    }
    emissive.reserve(emissive.size() + options.events.size());
    constexpr float kOne = static_cast<float>(sim::Fixed::kOne);
    constexpr float kTau = 6.2831853F;
    const sim::Tick now = sim.tick();
    const Vec3 eye{options.camera.x, options.camera.y, options.camera.z};
    for (const sim::BattleEvent& event : options.events) {
        if (event.kind != sim::EventKind::Killed || event.tick > now) {
            continue;
        }
        const sim::Unit* wreck = sim.find(event.subject);
        // The sim's own word on it: a wreck that is not in the table, or that is still alive, has
        // no wake -- an event from somewhere else cannot put smoke on a living tank.
        if (wreck == nullptr || wreck->alive) {
            continue;
        }
        const int dx = wreck->x.floor_int()
                       - static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
        const int dy = wreck->y.floor_int()
                       - static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
        if (std::abs(dx) > options.radius_tiles || std::abs(dy) > options.radius_tiles) {
            continue;
        }
        const sim::Tick age = now - event.tick;
        if (age >= kSmokeTicks) {
            continue; // cold: what is left on the ground is litter, and litter has its own layer
        }
        const float tile_x = static_cast<float>(wreck->x.raw) / kOne;
        const float tile_y = static_cast<float>(wreck->y.raw) / kOne;
        const float cx = tile_x * kMetresPerTile;
        const float cz = tile_y * kMetresPerTile;
        const float ground = ground_metres(options.seed, tile_x, tile_y);
        const std::uint32_t id = wreck->id.value;
        if (age < kBlastTicks) {
            // The burst: thrown out and up, shrinking and cooling as it goes.
            for (std::uint32_t puff = 0; puff < kBlastPuffs; ++puff) {
                const float heading = wake_fraction(options.seed, id, puff) * kTau;
                const float out_at = 0.35F + (0.65F * wake_fraction(options.seed, id, puff + 64U));
                const float up_at = 0.2F + (0.8F * wake_fraction(options.seed, id, puff + 128U));
                // Wide at the instant of the kill and carried outward as it burns: the puffs start
                // 0.4 of the way to the burst's reach and cover the other 0.6 over the burst. The
                // centre rides the shrinking box (it rests on the ground, so it sinks by the
                // change in `half`) and climbs `kBlastRiseMetres * up_at`: both linear in age.
                const float reach = kBlastSpreadMetres * out_at;
                const float ticks = static_cast<float>(kBlastTicks);
                Particle puff_spawn;
                puff_spawn.origin
                    = {cx + (std::cos(heading) * reach * 0.4F), ground + kBlastHalfMetres,
                       cz + (std::sin(heading) * reach * 0.4F)};
                puff_spawn.velocity
                    = {std::cos(heading) * reach * 0.6F / ticks,
                       ((kBlastRiseMetres * up_at) + (kBlastLastHalfMetres - kBlastHalfMetres))
                           / ticks,
                       std::sin(heading) * reach * 0.6F / ticks};
                puff_spawn.lifetime = kBlastTicks;
                puff_spawn.half_from = kBlastHalfMetres;
                puff_spawn.half_to = kBlastLastHalfMetres;
                puff_spawn.colour_from = kBlastHotColour;
                puff_spawn.colour_to = kBlastCoolColour;
                puff_spawn.alpha_from = kBlastAlpha;
                puff_spawn.alpha_to = 0.0F;
                puff_spawn.roll = wake_fraction(options.seed, id, puff + 192U) * kTau;
                puff_spawn.roll_rate = puff_roll_rate(options.seed, id, puff + 224U);
                emit_particle(out, puff_spawn, age, eye);
            }
            // The sparks, after the puffs: small, bright and OPAQUE, thrown out of the fireball's
            // centre on their own seeded headings and falling as they go (AI). They are one run
            // of the list, and the fog leaves it alone.
            const std::size_t sparks_first = out.size();
            for (std::uint32_t spark = 0; spark < kBlastSparks; ++spark) {
                const float heading = wake_fraction(options.seed, id, spark + 400U) * kTau;
                const float speed
                    = kBlastSparkSpeedMetres
                      * (0.4F + (0.6F * wake_fraction(options.seed, id, spark + 440U)));
                const float lift
                    = kBlastSparkLiftMetres
                      * (0.3F + (0.7F * wake_fraction(options.seed, id, spark + 480U)));
                emit_particle(
                    out,
                    spark_spawn({cx, ground + kBlastHalfMetres, cz},
                                {std::cos(heading) * speed, lift, std::sin(heading) * speed},
                                kBlastSparkTicks, kBlastSparkHalfMetres),
                    age, eye);
            }
            if (out.size() > sparks_first) {
                emissive.push_back({sparks_first, out.size()});
            }
            continue;
        }
        // The column: a slow grey stack, drifting on a wind that comes off the frame's own seed, so
        // wrecks on one map lean the same way -- weather, not seven private breezes.
        const sim::Tick since = age - kBlastTicks;
        const float wind = wake_fraction(options.seed, 0U, 1U) * kTau;
        for (std::uint32_t puff = 0; puff < kSmokePuffs; ++puff) {
            const float jitter = (wake_fraction(options.seed, id, puff + 256U) - 0.5F) * 0.5F;
            const float sway = (wake_fraction(options.seed, id, puff + 320U) - 0.5F) * 0.5F;
            Particle smoke;
            smoke.origin
                = {cx + jitter,
                   ground + kSmokeBaseMetres + (static_cast<float>(puff) * kSmokeSpacingMetres),
                   cz + sway};
            smoke.velocity = {std::cos(wind) * kSmokeDriftMetresPerTick, kSmokeRiseMetresPerTick,
                              std::sin(wind) * kSmokeDriftMetresPerTick};
            smoke.lifetime = kSmokeTicks - kBlastTicks;
            smoke.half_from = kSmokeHalfMetres;
            smoke.half_to = kSmokeHalfMetres;
            smoke.colour_from = kSmokeColour;
            smoke.colour_to = kSmokeColour;
            smoke.alpha_from = kSmokeAlpha;
            smoke.alpha_to = 0.0F;
            smoke.roll = wake_fraction(options.seed, id, puff + 352U) * kTau;
            smoke.roll_rate = puff_roll_rate(options.seed, id, puff + 384U);
            emit_particle(out, smoke, since, eye);
        }
    }
}

/// THE REPAIR BEAMS (see `kRepairBeamPalette`): per `repair_links` pair whose two ends the viewer
/// could see, `kRepairBeamBeads` cubes strung from the truck to the patient. Bead `n` stands
/// `n / (beads - 1)` of the way along, takes palette colour `n + tick / kRepairBeamFlowTicks`
/// (so the colours pour along the beam), and is thrown off the line on each axis by a hash of
/// (seed, tick, pair, bead, axis), times `sin(pi * fraction)` so the ends do not move. A pure
/// function of the sim: it EMITS NOTHING when no repair is advancing.
void append_repair_beams(std::vector<SceneVertex>& out, std::vector<VertexRun>& emissive,
                         const SceneOptions& options, const sim::Sim& sim) {
    constexpr float kPi = 3.14159265F;
    const std::vector<RepairLink> links = repair_links(sim);
    if (links.empty()) {
        return;
    }
    out.reserve(out.size()
                + (links.size()
                   * ((kRepairBeamBeads * kSphereVertices) + (kRepairSparks * kSparkVertices))));
    emissive.reserve(emissive.size() + links.size());
    const auto tile = [](sim::Fixed v) { return static_cast<float>(v.raw) / sim::Fixed::kOne; };
    const std::size_t flow = static_cast<std::size_t>(sim.tick() / kRepairBeamFlowTicks);
    const Vec3 eye{options.camera.x, options.camera.y, options.camera.z};
    for (const RepairLink& link : links) {
        const sim::Unit* truck = sim.find(link.truck);
        const sim::Unit* patient = sim.find(link.patient);
        if (truck == nullptr || patient == nullptr || !available_to_viewer(sim, options, *truck)
            || !available_to_viewer(sim, options, *patient)) {
            continue;
        }
        const std::array<float, 3> from{tile(truck->x) * kMetresPerTile,
                                        ground_metres(options.seed, truck->x, truck->y)
                                            + kRepairBeamLiftMetres,
                                        tile(truck->y) * kMetresPerTile};
        const std::array<float, 3> to{tile(patient->x) * kMetresPerTile,
                                      ground_metres(options.seed, patient->x, patient->y)
                                          + kRepairBeamLiftMetres,
                                      tile(patient->y) * kMetresPerTile};
        const std::uint32_t pair = (truck->id.value * 977U) + patient->id.value;
        for (std::size_t bead = 0; bead < kRepairBeamBeads; ++bead) {
            const float along = static_cast<float>(bead) / static_cast<float>(kRepairBeamBeads - 1);
            const float shake = kRepairBeamTrembleMetres * std::sin(kPi * along);
            std::array<float, 3> at{};
            for (std::size_t axis = 0; axis < at.size(); ++axis) {
                const std::uint32_t salt = (static_cast<std::uint32_t>(sim.tick()) * 256U)
                                           + (static_cast<std::uint32_t>(bead) * 4U)
                                           + static_cast<std::uint32_t>(axis);
                const float throw_at = (wake_fraction(options.seed, pair, salt) * 2.0F) - 1.0F;
                at.at(axis)
                    = from.at(axis) + ((to.at(axis) - from.at(axis)) * along) + (throw_at * shake);
            }
            const std::array<float, 3>& ink
                = kRepairBeamPalette.at((bead + flow) % kRepairBeamPalette.size());
            emit_sphere(out, at.at(0), at.at(1), at.at(2), kRepairBeamBeadRadiusMetres,
                        Vec3{ink.at(0), ink.at(1), ink.at(2)}, kRepairBeamAlpha);
        }
        // The weld's sparks, after the translucent beads: opaque, thrown from the patient's end.
        // Spark `n` is `n / kRepairSparks` of its life ahead of spark 0, so one is always being
        // born; each life gets its own seeded heading from the cycle it is in. They are one run of
        // the list, and the fog leaves it alone.
        const std::size_t sparks_first = out.size();
        for (std::size_t spark = 0; spark < kRepairSparks; ++spark) {
            const std::uint32_t shifted
                = static_cast<std::uint32_t>(sim.tick())
                  + static_cast<std::uint32_t>(spark * kRepairSparkTicks / kRepairSparks);
            const sim::Tick age = shifted % kRepairSparkTicks;
            const std::uint32_t salt = 0x8000U + ((shifted / kRepairSparkTicks) * 64U)
                                       + (static_cast<std::uint32_t>(spark) * 4U);
            const float heading = wake_fraction(options.seed, pair, salt) * 2.0F * kPi;
            const float speed = kRepairSparkSpeedMetres
                                * (0.4F + (0.6F * wake_fraction(options.seed, pair, salt + 1U)));
            const float lift = kRepairSparkLiftMetres
                               * (0.3F + (0.7F * wake_fraction(options.seed, pair, salt + 2U)));
            emit_particle(out,
                          spark_spawn({to.at(0), to.at(1), to.at(2)},
                                      {std::cos(heading) * speed, lift, std::sin(heading) * speed},
                                      kRepairSparkTicks, kRepairSparkHalfMetres),
                          age, eye);
        }
        if (out.size() > sparks_first) {
            emissive.push_back({sparks_first, out.size()});
        }
    }
}

/// THE BIOMETAL POOLS, VENTING (see `kPoolSteamColour`): per pool in view a plume of see-through
/// puffs climbing from it. The pool's GROUND is not drawn here: it is altered ground like a
/// deployed building's (`site_ground.hpp`, painted by `build_terrain`), and there is no disc, rim
/// or any other solid shape lying on it -- a disc under a deployed scavenger read as a decal.
/// Culled to the meshed radius like litter. EMITS NOTHING for a sim without pools. The plume is a
/// pure function of the sim's tick: puff `n` of `kPoolPlumePuffs` is `n / kPoolPlumePuffs` of a
/// cycle ahead of puff 0, climbs `kPoolPlumeHeightMetres` over `kPoolPlumeTicks`, widens as it
/// goes and fades toward the top; each pool is phased by its own tile so the plumes do not breathe
/// in step, and each puff sways a little on its own hash (`wake_fraction`, as the wrecks' smoke
/// does).
void append_pools(std::vector<SceneVertex>& out, const SceneOptions& options, const sim::Sim& sim) {
    if (!options.show_pools) {
        return;
    }
    constexpr float kTau = 6.2831853F;
    for (const sim::Pool& pool : sim.pools()) {
        const int dx
            = pool.tile_x - static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
        const int dy
            = pool.tile_y - static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
        if (std::abs(dx) > options.radius_tiles || std::abs(dy) > options.radius_tiles) {
            continue;
        }
        const float tile_x = static_cast<float>(pool.tile_x) + 0.5F;
        const float tile_y = static_cast<float>(pool.tile_y) + 0.5F;
        const float cx = tile_x * kMetresPerTile;
        const float cz = tile_y * kMetresPerTile;
        const float ground = ground_metres(options.seed, tile_x, tile_y);
        const std::uint32_t phase = static_cast<std::uint32_t>(pool.tile_x * 31 + pool.tile_y * 17);
        const Vec3 steam{kPoolSteamColour.at(0), kPoolSteamColour.at(1), kPoolSteamColour.at(2)};
        const Vec3 eye{options.camera.x, options.camera.y, options.camera.z}; // no floor: unused
        for (std::uint32_t puff = 0; puff < kPoolPlumePuffs; ++puff) {
            const sim::Tick offset
                = (kPoolPlumeTicks * puff) / static_cast<sim::Tick>(kPoolPlumePuffs);
            const float t = static_cast<float>((sim.tick() + offset + phase) % kPoolPlumeTicks)
                            / static_cast<float>(kPoolPlumeTicks);
            const float half = kPoolPuffHalfBaseMetres
                               + ((kPoolPuffHalfTopMetres - kPoolPuffHalfBaseMetres) * t);
            const float sway = (wake_fraction(options.seed, phase, puff) - 0.5F) * 1.2F * t;
            const float sway_z = (wake_fraction(options.seed, phase, puff + 64U) - 0.5F) * 1.2F * t;
            // The one puff: an icosphere `Particle` frozen at age 0 (its place is already the
            // plume's function of the tick), rolling at the blast's rate.
            Particle vapour;
            vapour.origin = {cx + sway, ground + half + (kPoolPlumeHeightMetres * t), cz + sway_z};
            vapour.half_from = half;
            vapour.half_to = half;
            vapour.colour_from = steam;
            vapour.colour_to = steam;
            vapour.alpha_from = kPoolSteamAlpha * (1.0F - (0.7F * t));
            vapour.alpha_to = vapour.alpha_from;
            // Rolling with the plume's CYCLE, not the raw tick: the plume repeats every
            // `kPoolPlumeTicks`, and a roll that did not would break the picture's period.
            vapour.roll = (wake_fraction(options.seed, phase, puff + 128U) * kTau)
                          + (puff_roll_rate(options.seed, phase, puff + 192U)
                             * static_cast<float>(kPoolPlumeTicks) * t);
            emit_particle(out, vapour, 0, eye);
        }
    }
}

/// THE FLOATING MOTES (see `kMoteCount`): `kMoteCount` icosphere `Particle`s hanging in the air
/// round the camera. Mote `n` has a place in a square `kMoteBoxMetres` across, a height, a speed, a
/// bob and a roll, all hashed from the frame's seed (`wake_fraction`, as the smoke's wind is), and
/// is slid along that same wind by the sim's tick. The square follows the camera by WRAPPING: a
/// mote's place relative to the eye is its world place modulo the box, so it stays where it hangs
/// while the camera moves and re-enters on the far side when it leaves the near one, fading out
/// toward the edge so the wrap is not seen. Nothing is stored and nothing reaches the sim: the
/// tick is read, never written. EMITS NOTHING unless `show_motes`.
void append_motes(std::vector<SceneVertex>& out, const SceneOptions& options, const sim::Sim& sim) {
    if (!options.show_motes) {
        return;
    }
    constexpr float kTau = 6.2831853F;
    constexpr std::uint32_t kOwner = 0x4D4F5445U; // "MOTE": its own hash stream, apart from wrecks'
    constexpr float kHalfBox = kMoteBoxMetres * 0.5F;
    const float ticks = static_cast<float>(sim.tick());
    const float wind = wake_fraction(options.seed, 0U, 1U) * kTau; // the smoke's wind
    const Vec3 eye{options.camera.x, options.camera.y, options.camera.z};
    const auto wrapped = [](float distance) {
        const float inside = std::fmod(distance, kMoteBoxMetres);
        return (inside < 0.0F ? inside + kMoteBoxMetres : inside) - kHalfBox;
    };
    out.reserve(out.size() + (kMoteCount * kSphereVertices));
    for (std::uint32_t mote = 0; mote < kMoteCount; ++mote) {
        const std::uint32_t salt = mote * 8U;
        const auto draw = [&](std::uint32_t which) {
            return wake_fraction(options.seed, kOwner, salt + which);
        };
        const float drift = kMoteDriftMetresPerTick * (0.5F + draw(3U)) * ticks;
        const float across_x
            = wrapped((draw(0U) * kMoteBoxMetres) + (std::cos(wind) * drift) - options.camera.x);
        const float across_z
            = wrapped((draw(1U) * kMoteBoxMetres) + (std::sin(wind) * drift) - options.camera.z);
        const float x = options.camera.x + across_x;
        const float z = options.camera.z + across_z;
        const float bob = std::sin((draw(4U) * kTau) + (ticks * kMoteBobRadiansPerTick));
        Particle speck;
        speck.origin
            = {x,
               ground_metres(options.seed, x / kMetresPerTile, z / kMetresPerTile) + kMoteLowMetres
                   + (draw(2U) * (kMoteCeilingMetres - kMoteLowMetres)) + (bob * kMoteBobMetres),
               z};
        speck.half_from = kMoteHalfMetres;
        speck.half_to = kMoteHalfMetres;
        speck.colour_from = kMoteColour;
        speck.colour_to = kMoteColour;
        const float edge = std::max(std::abs(across_x), std::abs(across_z)) / kHalfBox;
        speck.alpha_from = kMoteAlpha * std::min(1.0F, (1.0F - edge) * 4.0F);
        speck.alpha_to = speck.alpha_from;
        const float rate = puff_roll_rate(options.seed, kOwner, salt + 6U);
        speck.roll = (draw(5U) * kTau) + (rate * ticks);
        speck.roll_rate = 0.0F;
        emit_particle(out, speck, 0, eye);
    }
}

/// THE DEPLOY PREVIEW (DEPLOY): the footprint the armed build would stand on, as its tile grid
/// lying on the terrain -- the outline and the lines between its tiles, so it reads as the 2x2 it
/// is. Each stroke runs one tile edge, from corner height to corner height, so it follows the
/// ground. There is no rotation to draw: the footprint is a square, and a square turned a quarter
/// is itself.
void append_deploy_preview(std::vector<SceneVertex>& out, const SceneOptions& options) {
    if (!options.deploy_preview.has_value()) {
        return;
    }
    const DeployPreview preview = *options.deploy_preview;
    const sim::Footprint site = preview.site;
    // The colour is the sim's answer to "would SPACE deploy here?", carried on the preview: green
    // where it would, red where it would refuse. Asked once, in the sim, never re-derived here.
    const math::Vec3 colour = preview.valid ? kDeployPreviewColour : kDeployPreviewBlockedColour;
    const auto corner = [&](int tile_x, int tile_y) {
        const auto x = static_cast<float>(tile_x);
        const auto y = static_cast<float>(tile_y);
        return Vec3{x * kMetresPerTile,
                    ground_metres(options.seed, x, y) + kDeployPreviewLiftMetres,
                    y * kMetresPerTile};
    };
    // A flat stroke from one tile corner to the next, `across` being the horizontal offset to its
    // edges: x for a stroke that runs along z, z for one that runs along x.
    const auto stroke = [&](const Vec3& a, const Vec3& b, float across_x, float across_z) {
        emit_quad(out, Vec3{a.at(0) - across_x, a.at(1), a.at(2) - across_z},
                  Vec3{b.at(0) - across_x, b.at(1), b.at(2) - across_z},
                  Vec3{b.at(0) + across_x, b.at(1), b.at(2) + across_z},
                  Vec3{a.at(0) + across_x, a.at(1), a.at(2) + across_z}, colour);
    };
    constexpr int kSide = sim::kBuildingFootprintTiles;
    for (int line = 0; line <= kSide; ++line) {
        for (int step = 0; step < kSide; ++step) {
            // Along x at y = line, then along z at x = line.
            stroke(corner(site.x + step, site.y + line), corner(site.x + step + 1, site.y + line),
                   0.0F, kDeployPreviewHalfStrokeMetres);
            stroke(corner(site.x + line, site.y + step), corner(site.x + line, site.y + step + 1),
                   kDeployPreviewHalfStrokeMetres, 0.0F);
        }
    }
}

/// THE NAV BEACON (N1): a slim vertical bar in the mark's own colour (`nav_colour`: green, yellow
/// while highlighted) standing on the mark's tile, turned to face the camera, tall enough to see
/// over the terrain from a distance and thin enough not to fill the view up close. No number: that
/// belongs to the nav menu. It is drawn only where the terrain is (the same radius the units use),
/// so it never floats over bare void.
constexpr float kNavBeaconHeight = 8.0F;
constexpr float kNavBeaconWidthPerMetre = 0.012F;
constexpr float kNavBeaconMinHalfWidth = 0.12F;
constexpr float kNavBeaconMaxHalfWidth = 0.6F;

void append_nav_beacons(std::vector<SceneVertex>& out, const SceneOptions& options) {
    if (options.nav_marks == nullptr) {
        return;
    }
    const int camera_tile_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int camera_tile_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    for (const NavMarks::Mark& mark : options.nav_marks->marks()) {
        const math::Vec3& rgb = nav_colour(mark.highlighted);
        if (std::abs(mark.tile_x - camera_tile_x) > options.radius_tiles
            || std::abs(mark.tile_y - camera_tile_y) > options.radius_tiles) {
            continue;
        }
        const float tile_x = static_cast<float>(mark.tile_x) + 0.5F;
        const float tile_y = static_cast<float>(mark.tile_y) + 0.5F;
        const float cx = tile_x * kMetresPerTile;
        const float cz = tile_y * kMetresPerTile;
        const float ground = ground_metres(options.seed, tile_x, tile_y);
        float to_x = options.camera.x - cx;
        float to_z = options.camera.z - cz;
        const float length = std::sqrt((to_x * to_x) + (to_z * to_z));
        if (length < 1e-4F) {
            continue;
        }
        to_x /= length;
        to_z /= length;
        const float half = std::clamp(length * kNavBeaconWidthPerMetre, kNavBeaconMinHalfWidth,
                                      kNavBeaconMaxHalfWidth);
        const float right_x = to_z * half;
        const float right_z = -to_x * half;
        emit_quad(out, Vec3{cx - right_x, ground, cz - right_z},
                  Vec3{cx + right_x, ground, cz + right_z},
                  Vec3{cx + right_x, ground + kNavBeaconHeight, cz + right_z},
                  Vec3{cx - right_x, ground + kNavBeaconHeight, cz - right_z}, rgb);
    }
}

/// The reticle's distance from the eye, in metres. This close, nothing can stand between the eye
/// and the mark: it is a screen mark, and a crater rim must not be able to hide it.
constexpr float kReticleDistanceMetres = 1.5F;
/// Its half-size as a fraction of the half-screen at that distance, so it looks the same at any
/// extent.
constexpr float kReticleHalfFraction = 0.055F;
/// How thick each line is, as a fraction of the half-size: 0.07 of a half-size that is 19.8 px at
/// the 720 reference is a 1.4 px line. Harri, 2026-11-03: "the lines are very bold, and it looks
/// like a kids game" -- they were 0.22 (4.4 px), the rocket's 0.4 (7.9 px).
constexpr float kTriangleBaseFraction = 0.85F;
constexpr float kTriangleTurn = 2.35619449F; ///< 135 degrees: where a triangle's base corners sit
constexpr float kReticleBarFraction = 0.07F;
/// The crosshair's shape, as fractions of the half-size: where the ticks start (the open centre is
/// everything inside), the dot's half-width, and how far a full pulse pushes the ticks out and how
/// much it swells the dot.
constexpr float kReticleGapFraction = 0.35F;
constexpr float kReticleDotFraction = 0.08F;
/// THE FIVE SHAPES (Harri, 2026-11-03: "the crosshairs should actually change to indicate what
/// weapon I have"). Each is thin lines round an open centre and the centre dot, which is always
/// the last quad. GUN: four ticks (above). ROCKET: four corner brackets, a lock-on frame, at
/// `kBracketReachFraction` out with arms `kBracketArmFraction` long. MORTAR: a ring (its splash)
/// of `kRingSegments` segments at `kRingRadiusFraction`. BOUNCER: four ticks on the diagonals
/// (the ricochet's X) from `kDiagonalFromFraction` to `kDiagonalToFraction` out along each.
/// SHOTGUN: the spread, four dots at `kScatterFraction` on each diagonal axis of the open centre,
/// each `kScatterDotFraction` of the half-size, round the centre dot.
constexpr float kBracketReachFraction = 0.8F;
constexpr float kBracketArmFraction = 0.4F;
constexpr float kRingRadiusFraction = 0.8F;
constexpr int kRingSegments = 24;
constexpr float kDiagonalFromFraction = 0.35F;
constexpr float kDiagonalToFraction = 0.9F;
constexpr float kScatterFraction = 0.55F;
constexpr float kScatterDotFraction = 0.11F;
constexpr float kReticlePulseSpread = 0.45F;
constexpr float kReticlePulseDotGrowth = 1.5F;

/// The reticle's colour is the ANSWER to "what would a click here do": red fires, green selects,
/// white is ground. Text is not drawn anywhere in this project yet, so colour is the only channel
/// there is.
/// How much of its colour the reticle keeps while the driven weapon reloads: 30%, so the bright
/// white of a ready reticle drops to a dark grey that no ground or sky is mistaken for it.
constexpr float kReticleSpentShade = 0.3F;
constexpr std::array<float, 3> kReticleGround{0.92F, 0.92F, 0.92F};
constexpr std::array<float, 3> kReticleHostile{1.0F, 0.25F, 0.25F};
constexpr std::array<float, 3> kReticleOwn{0.45F, 1.0F, 0.45F};

std::array<float, 3> reticle_colour(const sim::Sim& sim, const Pick& picked, sim::Side viewer) {
    if (!picked.has_unit()) {
        return kReticleGround;
    }
    const sim::Unit* unit = sim.find(picked.unit);
    if (unit == nullptr) {
        return kReticleGround;
    }
    return unit->side == viewer ? kReticleOwn : kReticleHostile;
}

/// How far the radar is drawn from the eye, PER LAYER, derived from the frame rather than fixed.
///
/// Two measured traps sit behind these numbers, and both were found by looking at frames:
///
/// 1. `ray_point` puts a mark `d` metres FORWARD only for the pixel the ray runs straight through.
///    An off-centre pixel sits further along its own ray, so its forward distance is `d / |dir|`,
///    and |dir| reaches about 1.55 at a 1280x720 frame corner and grows with the aspect ratio. A
///    panel at 0.105 m was therefore only 0.07 m in front of the eye -- inside the 0.1 m near
///    plane, where it is silently clipped and simply is not there. The base distance scales with
///    the frame's own corner ray, so no resolution can push the panel into the near plane.
///
/// 2. The depth test decides between the layers per PIXEL, and depth is `1 - near/forward`, so a
///    LARGE quad interpolates its depth across its own corners: |dir| varies by about a third
///    across a corner panel, and a face 3% further out than the marks still reads NEARER than them
///    in the middle of the panel -- the marks then lose every pixel and the panel is an empty dark
///    square. The ratios below are therefore 1 : 1.6 : 2.2, wider than that variation, so the layer
///    that should be on top is on top everywhere.
///
/// The panel is still only tens of centimetres from the eye -- nearer than any ground the tank can
/// be standing on -- so terrain cannot hide it either.
constexpr float kHudClearance = 1.25F;  ///< how far past the near plane the nearest layer sits
constexpr float kHudFaceRatio = 1.6F;   ///< the panel's face, relative to the marks
constexpr float kHudBorderRatio = 2.2F; ///< and its border, relative to the marks

/// The length of the ray the frame's own corner looks along, unnormalised, exactly as `ray_through`
/// builds it: 1 at the middle of the frame, up to ~1.55 at a corner.
[[nodiscard]] float corner_ray_length(Extent extent) noexcept {
    const float half_tan = std::tan(math::kFovRadians * 0.5F);
    const float height = extent.height > 0 ? static_cast<float>(extent.height) : 1.0F;
    const float rx = (static_cast<float>(extent.width) / height) * half_tan;
    return std::sqrt(1.0F + (rx * rx) + (half_tan * half_tan));
}

std::array<float, 3> radar_colour(RadarTone tone) {
    switch (tone) {
    case RadarTone::You:
        return {0.55F, 1.0F, 0.55F};
    case RadarTone::Yours:
        return {0.30F, 0.85F, 0.35F};
    case RadarTone::Hostile:
        return {1.0F, 0.30F, 0.25F};
    case RadarTone::Litter:
        return {1.0F, 1.0F, 1.0F}; // whatever a scavenger can collect: one white, wreck or pool
    }
    return {1.0F, 1.0F, 1.0F};
}

constexpr int kRadarSegments = 48; ///< slices in the disc and the ring

/// Green from half health up, amber from a fifth, red below -- in integers, so the thresholds are
/// exact. Green and red are the radar's own "yours" and "hostile" colours. Used by the ship
/// instrument's hull reading; the radar itself no longer states health at all (see below).
std::array<float, 3> health_colour(std::uint32_t hp, std::uint32_t max_hp) {
    if (hp * 2 >= max_hp) {
        return radar_colour(RadarTone::Yours);
    }
    if (hp * 5 >= max_hp) {
        return {1.0F, 0.75F, 0.20F};
    }
    return radar_colour(RadarTone::Hostile);
}

/// The compass letters: 5x5 cell glyphs, each cell a 2 px square, so a letter is 10x10 px, in white
/// on a near-black plate with a pixel of margin. They go on the ring, over the dark face, which is
/// what makes them readable on every mark and terrain colour. Rows are top to bottom, bit 4 left.
constexpr float kHudCompassRatio
    = 0.9F; ///< nearer than the marks, so the plate hides what it covers

const std::array<std::uint8_t, 5>& letter_rows(char glyph) {
    static constexpr std::array<std::uint8_t, 5> kN{0b10001, 0b11001, 0b10101, 0b10011, 0b10001};
    static constexpr std::array<std::uint8_t, 5> kE{0b11110, 0b10000, 0b11100, 0b10000, 0b11110};
    static constexpr std::array<std::uint8_t, 5> kS{0b01111, 0b10000, 0b01110, 0b00001, 0b11110};
    static constexpr std::array<std::uint8_t, 5> kW{0b10001, 0b10001, 0b10101, 0b10101, 0b01010};
    switch (glyph) {
    case 'N':
        return kN;
    case 'E':
        return kE;
    case 'S':
        return kS;
    default:
        return kW;
    }
}

/// A NAV MARK as the radar and the world map both draw it (MENU-NAV; his ruling, 2026-10-26:
/// "EXACTLY the same as in the radar ... I don't see any difference in the appearance"): a plus of
/// two crossed bars a pixel thick each way, green; highlighted, the same plus in yellow with a thin
/// yellow diamond round it. His BZ2 capture measured that diamond at about 15x12 px -- a thin
/// outline round the plus, not a bigger marker -- so its corners sit `kNavDiamondReach` plus-halves
/// out, just clear of the plus's tips. ONE function for both surfaces, so the two cannot drift
/// apart. `distance` is the HUD layer it is drawn on.
constexpr float kNavBarHalfPixels = 1.0F;
constexpr float kNavDiamondReach = 1.5F;
constexpr float kNavDiamondHalfPixels = 0.6F; ///< half the diamond line's width

void append_nav_glyph(std::vector<SceneVertex>& out, const SceneOptions& options, float distance,
                      float cx, float cy, float half, bool highlighted) {
    const math::Vec3& rgb = nav_colour(highlighted);
    const auto at = [&](float x, float y) {
        const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
        return Vec3{point.x, point.y, point.z};
    };
    const auto rect = [&](float ax, float ay, float bx, float by) {
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };
    rect(cx - half, cy - kNavBarHalfPixels, cx + half, cy + kNavBarHalfPixels);
    rect(cx - kNavBarHalfPixels, cy - half, cx + kNavBarHalfPixels, cy + half);
    if (!highlighted) {
        return;
    }
    const float reach = half * kNavDiamondReach;
    const std::array<std::array<float, 2>, 4> corners{
        {{cx, cy - reach}, {cx + reach, cy}, {cx, cy + reach}, {cx - reach, cy}}};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const std::array<float, 2>& a = corners.at(i);
        const std::array<float, 2>& b = corners.at((i + 1) % corners.size());
        const float dx = b.at(0) - a.at(0);
        const float dy = b.at(1) - a.at(1);
        const float length = std::hypot(dx, dy);
        // The edge's normal, a line's half-width long: each side is one thin quad along it.
        const float nx = -dy / length * kNavDiamondHalfPixels;
        const float ny = dx / length * kNavDiamondHalfPixels;
        emit_quad(out, at(a.at(0) + nx, a.at(1) + ny), at(b.at(0) + nx, b.at(1) + ny),
                  at(b.at(0) - nx, b.at(1) - ny), at(a.at(0) - nx, a.at(1) - ny), rgb);
    }
}

/// The target diamond's depth on the radar: nearer than the blips (1.0) and than the nav marks
/// (0.87's neighbours are far from it), so the ring over a blip is not lost to the depth test.
constexpr float kRadarTargetRatio = 0.93F;

/// A diamond ring centred at (cx, cy) in FRAME pixels: four bars, one per side, `half` the distance
/// from the centre to a corner and `bar` the thickness. One shape for the radar and the map; each
/// side is a quad whose outer edge runs corner to corner and whose inner edge is the same line
/// pulled toward the centre. Pixels in, through `ray_point` like every HUD layer.
void append_target_diamond(std::vector<SceneVertex>& out, const SceneOptions& options,
                           float distance, float cx, float cy, float half, float bar) {
    constexpr std::array<float, 3> kInk{1.0F, 0.15F, 0.12F};
    const auto at = [&](float x, float y) {
        const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
        return Vec3{point.x, point.y, point.z};
    };
    // Pulling an edge in by `bar` along the diagonal normal moves each corner by bar * sqrt(2).
    const float inner = std::max(0.0F, half - (bar * 1.41421356F));
    const std::array<std::array<float, 2>, 4> dirs{
        {{0.0F, -1.0F}, {1.0F, 0.0F}, {0.0F, 1.0F}, {-1.0F, 0.0F}}}; // top, right, bottom, left
    for (std::size_t side = 0; side < dirs.size(); ++side) {
        const std::array<float, 2>& a = dirs.at(side);
        const std::array<float, 2>& b = dirs.at((side + 1) % dirs.size());
        emit_quad(out, at(cx + (a.at(0) * half), cy + (a.at(1) * half)),
                  at(cx + (b.at(0) * half), cy + (b.at(1) * half)),
                  at(cx + (b.at(0) * inner), cy + (b.at(1) * inner)),
                  at(cx + (a.at(0) * inner), cy + (a.at(1) * inner)), kInk);
    }
}

/// The radar's marks for this frame, in panel pixels: the ONE call both the radar and the command
/// menu's leader line make, so the line's dot lands on the blip the radar drew and not on a second
/// opinion of where the unit is.
[[nodiscard]] std::vector<RadarMark> frame_radar_marks(const SceneOptions& options,
                                                       const sim::Sim& sim) {
    // Through the detection model: a stationary hostile is on the panel only as a sweep passes.
    return radar_detect(radar_marks(sim.units(), options.viewer_unit, options.camera.yaw,
                                    radar_panel(options.extent),
                                    viewer_radar_radius(sim, options.viewer_unit)),
                        sim.units(), sim.tick(), options.radar_activity);
}

/// The radar, drawn in PANEL PIXELS: every corner goes through `ray_point`, the same pixel-to-world
/// function the picker uses, so the panel is exactly where its pixels are and a mark can never sit
/// somewhere other than its own place on the panel. Three metres of nothing, a dark face so the
/// marks read over any terrain, and then the marks: dots for units, an outline for your base, a
/// triangle for you, an arrow at the rim for a base you cannot see.
void append_radar(std::vector<SceneVertex>& out, const SceneOptions& options, const sim::Sim& sim) {
    if (!options.viewer_unit.valid()) {
        return;
    }
    const RadarPanel panel = radar_panel(options.extent);
    // The reach is the driven unit's (a scout sees twice as far), and every layer below -- marks,
    // mesh and nav marks -- is drawn at that ONE radius, or they would disagree about the scale.
    const int reach = viewer_radar_radius(sim, options.viewer_unit);
    const std::vector<RadarMark> marks = frame_radar_marks(options, sim);
    if (marks.empty()) {
        return; // no such unit: there is nothing for the radar to be centred on
    }
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const float face_at = marks_at * kHudFaceRatio;
    const float border_at = marks_at * kHudBorderRatio;
    const auto at = [&](float distance, float px, float py) {
        const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
        return Vec3{point.x, point.y, point.z};
    };
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb, float alpha = 1.0F) {
        emit_quad(out, at(distance, ax, ay), at(distance, bx, ay), at(distance, bx, by),
                  at(distance, ax, by), rgb, alpha);
    };
    constexpr std::array<float, 3> kBorder{0.80F, 0.82F, 0.85F};
    constexpr std::array<float, 3> kFace{0.04F, 0.05F, 0.06F};

    // The panel is an OVAL: the ground disc seen from 21 degrees up, an ellipse inscribed in its
    // rectangle, ringed by the border. The corners of the rectangle are not painted. Both are fans
    // and strips of `kRadarSegments` slices, walked clockwise on the screen like `rect`, and every
    // point goes through `at` like everything else.
    const float centre_x = panel.x + (panel.width * 0.5F);
    const float centre_y = panel.y + (panel.height * 0.5F);
    const float inner_x = panel.width * 0.5F;
    const float inner_y = panel.height * 0.5F;
    const auto rim = [&](float grow, int slice) {
        const float angle = (2.0F * kPi * static_cast<float>(slice)) / kRadarSegments;
        return std::array<float, 2>{centre_x + ((inner_x + grow) * std::cos(angle)),
                                    centre_y + ((inner_y + grow) * std::sin(angle))};
    };
    for (int slice = 0; slice < kRadarSegments; ++slice) {
        const std::array<float, 2> in_a = rim(0.0F, slice);
        const std::array<float, 2> in_b = rim(0.0F, slice + 1);
        const std::array<float, 2> out_a = rim(kRadarBorderPixels, slice);
        const std::array<float, 2> out_b = rim(kRadarBorderPixels, slice + 1);
        emit_quad(out, at(border_at, out_a.at(0), out_a.at(1)),
                  at(border_at, out_b.at(0), out_b.at(1)), at(border_at, in_b.at(0), in_b.at(1)),
                  at(border_at, in_a.at(0), in_a.at(1)), kBorder);
    }
    for (int slice = 0; slice < kRadarSegments; ++slice) {
        const std::array<float, 2> a = rim(0.0F, slice);
        const std::array<float, 2> b = rim(0.0F, slice + 1);
        emit_triangle(out, at(face_at, centre_x, centre_y), at(face_at, a.at(0), a.at(1)),
                      at(face_at, b.at(0), b.at(1)), kFace);
    }

    // THE GROUND UNDER THE MARKS (RADAR3D-2): the heightfield as a wireframe on the same tilted
    // plane the marks sit on, so hills read as raised humps rather than a flat disc. It goes on its
    // own layer just NEARER than the face (so it cannot z-fight with it) and before the letters and
    // the marks, so a blip is never buried under terrain. Each segment is a thin
    // quad: `radar_mesh` already clipped both ends to the oval, so nothing here can leave it.
    const sim::Unit* viewer = sim.find(options.viewer_unit);
    if (options.show_radar_mesh && viewer != nullptr) {
        constexpr std::array<float, 3> kMeshInk{0.30F, 0.46F, 0.38F};
        constexpr float kMeshAlpha = 0.85F;
        constexpr float kMeshNearerThanFace = 0.98F;
        const float mesh_at = face_at * kMeshNearerThanFace;
        const float half = std::max(
            0.5F, (kRadarMeshLinePixels * static_cast<float>(options.extent.height)) / 1440.0F);
        const float kOne = static_cast<float>(sim::Fixed::kOne);
        const float viewer_x = static_cast<float>(viewer->x.raw) / kOne;
        const float viewer_z = static_cast<float>(viewer->y.raw) / kOne;
        for (const RadarSegment& segment :
             radar_mesh(options.seed, viewer_x, viewer_z, options.camera.yaw, panel, reach,
                        options.radar_mesh_step_tiles, options.terrain)) {
            const float ax = panel.x + segment.a.px;
            const float ay = panel.y + segment.a.py;
            const float bx = panel.x + segment.b.px;
            const float by = panel.y + segment.b.py;
            const float dx = bx - ax;
            const float dy = by - ay;
            const float length = std::hypot(dx, dy);
            if (length <= 0.0F) {
                continue;
            }
            const float nx = ((-dy / length) * half);
            const float ny = ((dx / length) * half);
            emit_quad(out, at(mesh_at, ax - nx, ay - ny), at(mesh_at, bx - nx, by - ny),
                      at(mesh_at, bx + nx, by + ny), at(mesh_at, ax + nx, ay + ny), kMeshInk,
                      kMeshAlpha);
        }
    }

    // THE HULL BAR IS RETIRED (user, 2026-10-23): "Retire radar's hull bar. It's replaced by the
    // health bar at the bottom right." The ship instrument states the hull twice already -- as an
    // arc and as a number -- so a third readout floating above the panel was one too many. Nothing
    // takes its slot: the panel's top edge is the border, and the compass letters are what the
    // ring's top carries.

    constexpr std::array<float, 3> kLetterPlate{0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 3> kLetterInk{1.0F, 1.0F, 1.0F};
    for (const RadarLetter& l : radar_compass(options.camera.yaw, panel)) {
        const float half_w = (2.5F * kLetterCellPixels) + kLetterPlatePadPixels;
        const float half_h = (2.5F * kLetterCellPixels) + kLetterPlatePadPixels;
        const float cx = panel.x + l.px;
        const float cy = panel.y + l.py;
        const float plate_at = marks_at * kHudCompassRatio * 1.05F;
        const float ink_at = marks_at * kHudCompassRatio;
        rect(plate_at, cx - half_w, cy - half_h, cx + half_w, cy + half_h, kLetterPlate);
        const std::array<std::uint8_t, 5>& rows = letter_rows(l.glyph);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (int col = 0; col < 5; ++col) {
                if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                    continue;
                }
                const float x = cx - (2.5F * kLetterCellPixels)
                                + (static_cast<float>(col) * kLetterCellPixels);
                const float y = cy - (2.5F * kLetterCellPixels)
                                + (static_cast<float>(row) * kLetterCellPixels);
                rect(ink_at, x, y, x + kLetterCellPixels, y + kLetterCellPixels, kLetterInk);
            }
        }
    }

    // Scrap LAST: the marks all sit at one depth, where the first quad drawn wins (the depth test
    // is LESS), so the litter goes after the blips and can never hide one.
    std::vector<RadarMark> drawn = marks;
    const std::vector<RadarMark> litter = radar_litter(
        sim.scraps(), sim.units(), options.viewer_unit, options.camera.yaw, panel, reach);
    drawn.insert(drawn.end(), litter.begin(), litter.end());
    // ...and the biometal pools, from the same reach: the places a scavenger can deploy.
    const std::vector<RadarMark> pools = radar_pools(sim.pools(), sim.units(), options.viewer_unit,
                                                     options.camera.yaw, panel, reach);
    drawn.insert(drawn.end(), pools.begin(), pools.end());
    // A triangle pointing along its angle: the apex, and two corners 135 degrees either side. The
    // blips' arrows and the nav marks' arrow heads are this one shape.
    const auto triangle = [&](float distance, float cx, float cy, float half, float angle,
                              const std::array<float, 3>& rgb) {
        const auto corner = [&](float radius, float turn) {
            return std::array<float, 2>{cx + (radius * std::sin(angle + turn)),
                                        cy - (radius * std::cos(angle + turn))};
        };
        const std::array<float, 2> apex = corner(half, 0.0F);
        const std::array<float, 2> side_a = corner(half * kTriangleBaseFraction, kTriangleTurn);
        const std::array<float, 2> side_b = corner(half * kTriangleBaseFraction, -kTriangleTurn);
        out.push_back(SceneVertex{at(distance, apex.at(0), apex.at(1)), rgb});
        out.push_back(SceneVertex{at(distance, side_a.at(0), side_a.at(1)), rgb});
        out.push_back(SceneVertex{at(distance, side_b.at(0), side_b.at(1)), rgb});
    };
    for (const RadarMark& mark : drawn) {
        const std::array<float, 3> rgb = radar_colour(mark.tone);
        // The mark's own coordinates are PANEL pixels; the frame is what gets drawn into, so the
        // panel's corner is added once, here, and nowhere else. Forgetting it puts every unit at
        // the top-left corner of the screen.
        const float centre_x = panel.x + mark.px;
        const float centre_y = panel.y + mark.py;
        const float left = centre_x - mark.half;
        const float right = centre_x + mark.half;
        const float top = centre_y - mark.half;
        const float bottom = centre_y + mark.half;
        if (mark.shape == RadarShape::Square) {
            rect(marks_at, left, top, right, bottom, rgb, mark.alpha);
            continue;
        }
        if (mark.shape == RadarShape::Hollow) {
            const float bar = radar_outline_bar(mark);
            rect(marks_at, left, top, right, top + bar, rgb, mark.alpha);
            rect(marks_at, left, bottom - bar, right, bottom, rgb, mark.alpha);
            rect(marks_at, left, top + bar, left + bar, bottom - bar, rgb, mark.alpha);
            rect(marks_at, right - bar, top + bar, right, bottom - bar, rgb, mark.alpha);
            continue;
        }
        triangle(marks_at, centre_x, centre_y, mark.half, mark.angle, rgb);
    }

    // THE TARGET DIAMOND (AQ): the ring round the hostile `t` holds, from `radar_target` -- the
    // unit's place, not its blip -- so it is drawn on every frame the mark lasts, sweep or no
    // sweep.
    if (const std::optional<RadarTarget> target
        = radar_target(sim.units(), options.viewer_unit, options.camera.yaw, panel, reach,
                       options.readout_lock);
        target.has_value()) {
        append_target_diamond(out, options, marks_at * kRadarTargetRatio, panel.x + target->px,
                              panel.y + target->py, target->half, kTargetDiamondBar * target->half);
    }

    // THE NAV MARKS: `append_nav_glyph`'s plus, nearer than the compass so a mark pinned to the rim
    // is not hidden by a letter. BZ2 shows a placed nav point as a tiny plus in its radar, no
    // bigger than the unit blips, and no number: the numbers are the nav menu's (`~`).
    if (options.nav_marks != nullptr) {
        constexpr float kNavRatio = 0.87F;
        for (const RadarNav& nav :
             radar_nav_marks(sim.units(), options.viewer_unit, options.nav_marks->marks(),
                             options.camera.yaw, panel, reach)) {
            if (nav.at_rim) {
                // Out of range and selected: a tiny arrow head at the edge, in the mark's yellow.
                triangle(marks_at * kNavRatio, panel.x + nav.px, panel.y + nav.py, nav.half,
                         nav.angle, nav_colour(true));
                continue;
            }
            append_nav_glyph(out, options, marks_at * kNavRatio, panel.x + nav.px, panel.y + nav.py,
                             nav.half, nav.highlighted);
        }
    }
}

/// The floating health bars: 20x4 px at 800x450 (so fractions of the frame, and the same size at
/// any range), a few pixels above each tank's projected top. The 20x4 is the track; a 1 px outline
/// stands outside it, so the whole mark is 22x6.
constexpr float kUnitBarWidthFraction = 20.0F / 800.0F;
constexpr float kUnitBarHeightFraction = 4.0F / 450.0F;
constexpr float kUnitBarOutlineFraction = 1.0F / 450.0F;
constexpr float kUnitBarGapFraction = 3.0F / 450.0F; ///< from the tank's top to the outline
constexpr float kTankTopMetres = 1.2F;               ///< the turret roof, above the tank's ground

/// WHERE THE WORLD'S UP LIES ON THE SCREEN, a unit vector in pixels (x right, y down): the camera's
/// own basis, taken against the world's (0, 1, 0). It is `(0, -1)`, straight up the screen, for a
/// view with no roll whatever its pitch and yaw; a view that leans with the hull (`Camera::up`)
/// sees the world's up leaning on its screen by exactly that roll, as the horizon does. The health
/// bars are laid out in THIS frame and not the screen's, so they stay level with gravity while the
/// camera banks.
std::array<float, 2> world_up_on_screen(const Camera& camera) noexcept {
    const math::Basis basis = math::basis_from(camera.yaw, camera.pitch, camera.up);
    const float across = basis.right.at(1);
    const float rise = basis.up.at(1);
    const float length = std::hypot(across, rise);
    if (length < 1.0e-6F) {
        return {0.0F, -1.0F}; // looking straight up or down: there is no horizon to keep level with
    }
    return {across / length, -rise / length};
}

std::array<std::uint8_t, 5> mount_glyph(char glyph);

/// One small health bar over each living unit in view that the player has a reason to read, on the
/// radar's HUD layers: a near-black outline, a dark track and a fill in the reticle's colours
/// (green yours, red hostile), centred over the unit and sitting above its projected top. Fixed
/// size, whatever the range. The viewer's own tank gets none -- its hull is in the HUD panel -- and
/// with no viewer there are no bars at all, which keeps a frame with no player in it as it was.
///
/// ON DEMAND (Harri, 2026-11-01: "should not be there until I either select something on my side,
/// or point approximately at it, or have it targetted with the t key"): a unit wears its bar when
/// it is in `selected` (his side's), or it is the one the crosshair is on or within
/// `kAssistConeDegrees` of (`pointed`, from `asset_nearest_ray`: the assist's own cone), or it is
/// the assist's latched target (`SceneOptions::assist_target`). Any asset: a tank, a turret or the
/// factory alike, which sit higher (`unit_box_height_metres`) than a tank's roof.
///
/// LEVEL WITH GRAVITY: the bar's width runs along the world's horizontal and its height up the
/// world's up as the screen sees them (`world_up_on_screen`), so a banked view shows the bars
/// banked with the world. Only the BASIS turns: the anchor stays the unit's projected top and the
/// size stays the same number of pixels, and the camera is not touched.
///
/// POINT AT A THING, READ WHAT IT IS (Harri, 2026-11-03): a unit that is `pointed`, or held by
/// `SceneOptions::readout_lock` (`T`), also wears its NAME (`kind_label`, in the HUD's small
/// capitals, `kLetterCellPixels` cells) right over its bars and its DISTANCE in metres UNDER its
/// middle -- the plate's top as far below the middle as the bars' outer bottom edge is above it, so
/// the pair above and the distance below sit symmetric about the unit. Both ride the projected top
/// (higher the nearer he gets) and are the same pixels at any range. The ink is the bar's: green
/// his, red theirs. The unit SPACE would attack keeps its own label from `append_attack_readout`,
/// so it is not named twice.
///
/// Always `kUnitBarVertexCount` vertices per bar, in sim order; a bar with a zero-width fill stays
/// whole. A unit counting a deploy (`Unit::deploy_ticks` non-zero) wears a deploy bar of the same
/// make whatever the reason above -- the wait is progress the player is owed, not health -- its
/// fill `deploy_ticks / kDeployTicks`, right above the health bar when there is one and in the
/// health bar's own place when there is not; it goes through the same cull, and a unit not counting
/// adds nothing.
void append_unit_bars(std::vector<SceneVertex>& out, const SceneOptions& options,
                      const sim::Sim& sim, std::span<const sim::UnitId> selected,
                      sim::UnitId pointed) {
    if (!options.viewer_unit.valid()) {
        return;
    }
    const float frame_w = static_cast<float>(options.extent.width);
    const float frame_h = static_cast<float>(options.extent.height);
    const float width = frame_w * kUnitBarWidthFraction;
    const float height = frame_h * kUnitBarHeightFraction;
    const float outline = frame_h * kUnitBarOutlineFraction;
    const float gap = frame_h * kUnitBarGapFraction;

    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    // The bar's two pixel axes: `rise` up the world's up as the screen sees it, `across` square to
    // it. For an unbanked view these are the screen's own, and every number below is the one the
    // screen-aligned bar had.
    const std::array<float, 2> rise = world_up_on_screen(options.camera);
    const std::array<float, 2> across{-rise.at(1), rise.at(0)};
    constexpr std::array<float, 3> kOutline{0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 3> kPlate{0.10F, 0.10F, 0.12F};

    for (const sim::Unit& unit : sim.units()) {
        const bool structure = unit.kind == sim::UnitKind::Factory || sim::is_turret(unit.kind);
        const bool wanted
            = (unit.side == options.viewer_side
               && std::find(selected.begin(), selected.end(), unit.id) != selected.end())
              || unit.id == pointed || unit.id == options.assist_target
              || unit.id == options.readout_lock;
        if (!unit.alive || unit.id == options.viewer_unit || (!wanted && unit.deploy_ticks == 0)
            || hidden_by_reach(sim, options, unit)) {
            continue;
        }
        // Only a unit that is drawn gets a bar: the same terrain window the units use.
        if (outside_drawn_radius(options, unit)) {
            continue;
        }
        const bool hovering = options.hover.has_value() && options.hover->unit == unit.id;
        const float ground
            = hovering ? options.hover->z : ground_metres(options.seed, unit.x, unit.y);
        const Projected top = project(
            options.camera, options.extent,
            static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile,
            ground + (structure ? unit_box_height_metres(unit.kind) : kTankTopMetres),
            static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile);
        if (top.depth <= math::kNearPlane || top.x < 0.0F || top.x > frame_w || top.y < 0.0F
            || top.y > frame_h) {
            continue; // behind the eye, or off the frame: not visible
        }
        const std::array<float, 3>& side_rgb
            = unit.side == options.viewer_side ? kReticleOwn : kReticleHostile;
        // A point `along` the bar's width from its centre and `up` above the unit's top, in pixels.
        const auto corner = [&](float distance, float along, float up) {
            const RayPoint point = ray_point(
                options.camera, options.extent, top.x + (across.at(0) * along) + (rise.at(0) * up),
                top.y + (across.at(1) * along) + (rise.at(1) * up), distance);
            return Vec3{point.x, point.y, point.z};
        };
        const auto rect = [&](float distance, float left, float right, float lower, float upper,
                              const std::array<float, 3>& rgb) {
            emit_quad(out, corner(distance, left, upper), corner(distance, right, upper),
                      corner(distance, right, lower), corner(distance, left, lower), rgb);
        };
        const auto bar = [&](float lower, float share) {
            const float left = -width * 0.5F;
            rect(marks_at * kHudBorderRatio, left - outline, left + width + outline,
                 lower - outline, lower + height + outline, kOutline);
            rect(marks_at * kHudFaceRatio, left, left + width, lower, lower + height, kPlate);
            rect(marks_at, left, left + (width * share), lower, lower + height, side_rgb);
        };
        const float lower = gap + outline;
        float next = lower;
        if (wanted) {
            const std::uint32_t max_hp = sim::spec_of(unit.kind).hp;
            const std::uint32_t hp = std::min<std::uint32_t>(unit.hp, max_hp);
            bar(lower, max_hp > 0 ? static_cast<float>(hp) / static_cast<float>(max_hp) : 0.0F);
            next = lower + height + (3.0F * outline);
        }
        // THE DEPLOY BAR: the wait, in the sim's own count, one strip above the health bar's
        // outline (a 1 px spacing and its own outline between). No count, no bar.
        if (unit.deploy_ticks != 0) {
            bar(next, std::min(1.0F, static_cast<float>(unit.deploy_ticks)
                                         / static_cast<float>(sim::kDeployTicks)));
            next += height + (3.0F * outline);
        }
        if (unit.id != pointed && unit.id != options.readout_lock) {
            continue;
        }
        // A plate of `text` centred `along` the bar's width, its bottom edge `bottom` above the
        // unit's top: black plate behind, the letters in the unit's ink.
        const auto label = [&](std::string_view text, float along, float bottom) {
            const float letter = kLetterCellPixels;
            const float text_w = ((static_cast<float>(text.size()) * 6.0F) - 1.0F) * letter;
            const float plate_h = (5.0F * letter) + (2.0F * kLetterPlatePadPixels);
            std::size_t lit = 1; // the plate
            for (const char glyph : text) {
                for (const std::uint8_t row : mount_glyph(glyph)) {
                    lit += static_cast<std::size_t>(std::popcount(row));
                }
            }
            out.reserve(out.size() + (lit * 6U));
            rect(marks_at * kHudFaceRatio, along - (text_w * 0.5F) - kLetterPlatePadPixels,
                 along + (text_w * 0.5F) + kLetterPlatePadPixels, bottom, bottom + plate_h,
                 kOutline);
            for (std::size_t index = 0; index < text.size(); ++index) {
                const std::array<std::uint8_t, 5> rows = mount_glyph(text.at(index));
                for (std::size_t row = 0; row < rows.size(); ++row) {
                    for (int col = 0; col < 5; ++col) {
                        if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                            continue;
                        }
                        const float left
                            = along - (text_w * 0.5F)
                              + ((static_cast<float>(index) * 6.0F + static_cast<float>(col))
                                 * letter);
                        const float upper = bottom + kLetterPlatePadPixels
                                            + (static_cast<float>(5 - row) * letter);
                        rect(marks_at, left, left + letter, upper - letter, upper, side_rgb);
                    }
                }
            }
        };
        if (unit.id != options.attack_target) {
            label(kind_label(unit.kind, faction_of(options.factions, unit.side)), 0.0F, next);
        }
        const Projected middle = attack_point(options, unit);
        if (middle.depth > math::kNearPlane) {
            const float mid_up
                = ((middle.x - top.x) * rise.at(0)) + ((middle.y - top.y) * rise.at(1));
            const float mid_along
                = ((middle.x - top.x) * across.at(0)) + ((middle.y - top.y) * across.at(1));
            const float plate_h = (5.0F * kLetterCellPixels) + (2.0F * kLetterPlatePadPixels);
            // The plate's TOP edge is the mirror of the bars' outer bottom edge (`gap` over the
            // top) in the unit's middle.
            const std::string metres
                = std::to_string(std::llround(distance_metres(options, unit))) + " M";
            label(metres, mid_along, (2.0F * mid_up) - gap - plate_h);
        }
    }
}

/// The DEPLOYING plate over a turret of yours that is not yet mounted (moving, or still settling):
/// the word in black 5x5-cell capitals, cell 3 px at 450 px of frame height (so each letter is
/// 15 px tall), on a yellow plate inside a 1.5 px black outline -- about 170x26 px at 800x450, the
/// same size at any range. Anchored like the E5 bars: centred over the turret's projected top,
/// a few pixels above it. A mounted turret, a hostile one and a tank get nothing.
constexpr std::string_view kMountWord = "DEPLOYING";
constexpr std::array<float, 3> kMountPlate{1.0F, 0.85F, 0.10F};
constexpr float kMountCellFraction = 3.0F / 450.0F;
constexpr float kMountPadFraction = 3.0F / 450.0F;
constexpr float kMountOutlineFraction = 1.5F / 450.0F;
constexpr float kMountGapFraction = 4.0F / 450.0F; ///< from the turret's top to the outline

/// The HUD's 5x5 capitals and digits: the DEPLOYING plate's letters, the weapon names.
/// Rows top to bottom, bit 4 left; anything else is blank.
std::array<std::uint8_t, 5> mount_glyph(char glyph) {
    switch (glyph) {
    case 'A':
        return {0b01110, 0b10001, 0b11111, 0b10001, 0b10001};
    case 'B':
        return {0b11110, 0b10001, 0b11110, 0b10001, 0b11110};
    case 'C':
        return {0b01111, 0b10000, 0b10000, 0b10000, 0b01111};
    case 'D':
        return {0b11110, 0b10001, 0b10001, 0b10001, 0b11110};
    case 'E':
        return {0b11111, 0b10000, 0b11110, 0b10000, 0b11111};
    case 'F':
        return {0b11111, 0b10000, 0b11110, 0b10000, 0b10000};
    case 'G':
        return {0b01111, 0b10000, 0b10011, 0b10001, 0b01111};
    case 'H':
        return {0b10001, 0b10001, 0b11111, 0b10001, 0b10001};
    case 'I':
        return {0b11111, 0b00100, 0b00100, 0b00100, 0b11111};
    case 'K':
        return {0b10001, 0b10010, 0b11100, 0b10010, 0b10001};
    case 'L':
        return {0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
    case 'M':
        return {0b10001, 0b11011, 0b10101, 0b10001, 0b10001};
    case 'N':
        return {0b10001, 0b11001, 0b10101, 0b10011, 0b10001};
    case 'O':
        return {0b01110, 0b10001, 0b10001, 0b10001, 0b01110};
    case 'P':
        return {0b11110, 0b10001, 0b11110, 0b10000, 0b10000};
    case 'R':
        return {0b11110, 0b10001, 0b11110, 0b10010, 0b10001};
    case 'T':
        return {0b11111, 0b00100, 0b00100, 0b00100, 0b00100};
    case 'S':
        return {0b01111, 0b10000, 0b01110, 0b00001, 0b11110};
    case 'U':
        return {0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
    case 'V':
        return {0b10001, 0b10001, 0b10001, 0b01010, 0b00100};
    case 'W':
        return {0b10001, 0b10001, 0b10101, 0b10101, 0b01010};
    case 'Y':
        return {0b10001, 0b01010, 0b00100, 0b00100, 0b00100};
    case '?':
        return {0b01110, 0b10001, 0b00110, 0b00000, 0b00100};
    case '-':
        return {0b00000, 0b00000, 0b01110, 0b00000, 0b00000};
    case '0':
        return {0b01110, 0b10011, 0b10101, 0b11001, 0b01110};
    case '1':
        return {0b00100, 0b01100, 0b00100, 0b00100, 0b01110};
    case '2':
        return {0b11110, 0b00001, 0b01110, 0b10000, 0b11111};
    case '3':
        return {0b11110, 0b00001, 0b00110, 0b00001, 0b11110};
    case '4':
        return {0b10010, 0b10010, 0b11111, 0b00010, 0b00010};
    case '5':
        return {0b11111, 0b10000, 0b11110, 0b00001, 0b11110};
    case '6':
        return {0b01110, 0b10000, 0b11110, 0b10001, 0b01110};
    case '7':
        return {0b11111, 0b00001, 0b00010, 0b00100, 0b00100};
    case '8':
        return {0b01110, 0b10001, 0b01110, 0b10001, 0b01110};
    case '9':
        return {0b01110, 0b10001, 0b01111, 0b00001, 0b01110};
    case '.':
        return {0b00000, 0b00000, 0b00000, 0b01100, 0b01100};
    case '+':
        return {0b00100, 0b00100, 0b11111, 0b00100, 0b00100};
    case '~': // the command menu's tilde key (RB0.5); no other drawn string carries one
        return {0b00000, 0b01000, 0b10101, 0b00010, 0b00000};
    default:
        return {};
    }
}

} // namespace

void emit_particle(std::vector<SceneVertex>& out, const Particle& particle, sim::Tick age,
                   const math::Vec3& eye) {
    if (age >= particle.lifetime) {
        return;
    }
    const float t = static_cast<float>(age) / static_cast<float>(particle.lifetime);
    const float a = static_cast<float>(age);
    const float x = particle.origin.at(0) + (particle.velocity.at(0) * a);
    const float y
        = particle.origin.at(1) + (particle.velocity.at(1) * a) - (0.5F * particle.gravity * a * a);
    const float z = particle.origin.at(2) + (particle.velocity.at(2) * a);
    const float dx = x - eye.at(0);
    const float dy = y - eye.at(1);
    const float dz = z - eye.at(2);
    const float half
        = std::max(particle.half_from + ((particle.half_to - particle.half_from) * t),
                   particle.floor_per_metre * std::sqrt((dx * dx) + (dy * dy) + (dz * dz)));
    Vec3 colour{};
    for (std::size_t channel = 0; channel < colour.size(); ++channel) {
        colour.at(channel)
            = particle.colour_from.at(channel)
              + ((particle.colour_to.at(channel) - particle.colour_from.at(channel)) * t);
    }
    const float alpha = particle.alpha_from + ((particle.alpha_to - particle.alpha_from) * t);
    if (particle.shape == ParticleShape::Icosphere) {
        emit_sphere(out, x, y, z, half * kPuffRadiusPerHalf, colour, alpha,
                    particle.roll + (particle.roll_rate * a));
        return;
    }
    emit_puff(out, x, y, z, half, colour, alpha, particle.sides);
}

// The overlaps are taken in integers: across, the mask is scaled by `cols` and the pixels by 5, so
// mask cell c spans [c*cols, (c+1)*cols) and pixel j spans [5j, 5j+5); down, likewise with `rows`.
// Every edge is then a whole number, a pixel's area is exactly 25, and a fully covered pixel is
// exactly 1 rather than a float sum that lands a hair short of it.
std::vector<float> glyph_coverage(char glyph, int rows, int cols) {
    const std::array<std::uint8_t, 5> mask
        = mount_glyph(static_cast<char>(std::toupper(static_cast<unsigned char>(glyph))));
    if (rows <= 0 || cols <= 0
        || std::all_of(mask.begin(), mask.end(), [](std::uint8_t bits) { return bits == 0U; })) {
        return {};
    }
    constexpr int kMask = 5;
    std::vector<float> coverage(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            int area = 0;
            for (int mr = 0; mr < kMask; ++mr) {
                const int dy
                    = std::min((i + 1) * kMask, (mr + 1) * rows) - std::max(i * kMask, mr * rows);
                if (dy <= 0) {
                    continue;
                }
                for (int mc = 0; mc < kMask; ++mc) {
                    const int dx = std::min((j + 1) * kMask, (mc + 1) * cols)
                                   - std::max(j * kMask, mc * cols);
                    if (dx > 0
                        && ((mask.at(static_cast<std::size_t>(mr)) >> (4 - mc)) & 1U) != 0U) {
                        area += dx * dy;
                    }
                }
            }
            coverage.at((static_cast<std::size_t>(i) * static_cast<std::size_t>(cols))
                        + static_cast<std::size_t>(j))
                = static_cast<float>(area) / static_cast<float>(kMask * kMask);
        }
    }
    return coverage;
}

MenuGlyphGrid menu_glyph_grid(char glyph, int cap) noexcept {
    if (std::islower(static_cast<unsigned char>(glyph)) == 0) {
        return {cap, cap};
    }
    constexpr float kLowercase = 0.71F; // BZ2's 5 px lowercase over its 7 px capitals
    const int side
        = std::max(1, static_cast<int>(std::lround(kLowercase * static_cast<float>(cap))));
    return {side, side};
}

namespace {

void append_mount_plates(std::vector<SceneVertex>& out, const SceneOptions& options,
                         const sim::Sim& sim) {
    if (!options.viewer_unit.valid()) {
        return;
    }
    const float frame_w = static_cast<float>(options.extent.width);
    const float frame_h = static_cast<float>(options.extent.height);
    const float cell = frame_h * kMountCellFraction;
    const float pad = frame_h * kMountPadFraction;
    const float outline = frame_h * kMountOutlineFraction;
    const float gap = frame_h * kMountGapFraction;
    const float letters = static_cast<float>(kMountWord.size());
    const float text_w = ((letters * 6.0F) - 1.0F) * cell;
    const float text_h = 5.0F * cell;

    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto at = [&](float px, float py) {
            const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };
    constexpr std::array<float, 3> kInk{0.0F, 0.0F, 0.0F};

    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.kind != sim::UnitKind::Turret || unit.side != options.viewer_side
            || sim::is_mounted(unit)) {
            continue;
        }
        if (outside_drawn_radius(options, unit)) {
            continue;
        }
        const Projected top = project(
            options.camera, options.extent,
            static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile,
            ground_metres(options.seed, unit.x, unit.y) + kTankTopMetres,
            static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile);
        if (top.depth <= math::kNearPlane || top.x < 0.0F || top.x > frame_w || top.y < 0.0F
            || top.y > frame_h) {
            continue;
        }
        const float left = top.x - (text_w * 0.5F);
        const float plate_bottom = top.y - gap - outline;
        const float plate_top = plate_bottom - text_h - (2.0F * pad);
        const float plate_left = left - pad;
        const float plate_right = left + text_w + pad;
        rect(marks_at * kHudBorderRatio, plate_left - outline, plate_top - outline,
             plate_right + outline, plate_bottom + outline, kInk);
        rect(marks_at * kHudFaceRatio, plate_left, plate_top, plate_right, plate_bottom,
             kMountPlate);
        for (std::size_t index = 0; index < kMountWord.size(); ++index) {
            const std::array<std::uint8_t, 5> rows = mount_glyph(kMountWord.at(index));
            const float glyph_left = left + (static_cast<float>(index) * 6.0F * cell);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                        continue;
                    }
                    const float x = glyph_left + (static_cast<float>(col) * cell);
                    const float y = plate_top + pad + (static_cast<float>(row) * cell);
                    rect(marks_at, x, y, x + cell, y + cell, kInk);
                }
            }
        }
    }
}

constexpr float kHudPerFrameHeight = 1.0F / 720.0F;

/// THE SHIP INSTRUMENT (SHIPGAUGE-1), bottom right: BZ2's own gauge -- a segmented C-shaped ring
/// standing in for the hull and the magazine, the two readings in the ring's opening, and the four
/// carried weapons beside it with the one he has up lit. Everything below is a fraction of the
/// frame's HEIGHT, the unit every other constant on this layer uses, and every corner goes through
/// `ray_point` like the radar's pixels do, so the instrument keeps its shape at any resolution and
/// cannot sit anywhere but where its own pixels say.
///
/// THE RING is a C open to the right: its segments span 45 to 315 degrees, leaving a 90 degree gap
/// centred on the frame's right. Angles are degrees with 0 = screen right and positive DOWNWARD,
/// because that is the way screen y grows -- so 90 is the bottom of the ring, 180 its left and 270
/// its top. The upper arc (180 to 315) is the HULL in green and the lower one (45 to 180) the AMMO
/// in blue, and each is `kShipGaugeSegmentsPerHalf` trapezoids between the inner and outer radius,
/// every one spanning its own slice less a `kShipGaugeSegmentGapDegrees` gap.
constexpr float kShipGaugeCentreFromRight = 0.300F;   ///< the ring's centre, in from the right edge
constexpr float kShipGaugeCentreFromBottom = 0.100F;  ///< and up from the bottom one
constexpr float kShipGaugeOuterFraction = 0.060F;     ///< the ring's outer radius
constexpr float kShipGaugeThicknessFraction = 0.016F; ///< its band, so inner = outer - this
constexpr int kShipGaugeSegmentsPerHalf = 12;
constexpr float kShipGaugeSegmentGapDegrees = 2.5F;
constexpr float kShipGaugeOpenDegrees = 45.0F;   ///< where the walk starts, below the right gap
constexpr float kShipGaugeCloseDegrees = 315.0F; ///< and where it ends, above it
constexpr float kShipGaugeSwapDegrees = 180.0F;  ///< blue below this, green above it
/// The blue half ends and the green one begins at the swap angle: 45 plus half of the 270 degrees
/// the ring spans is 180, the left of the ring, so the halves are 135 degrees each and neither can
/// be widened without moving this.
static_assert(kShipGaugeSwapDegrees
              == kShipGaugeOpenDegrees + ((kShipGaugeCloseDegrees - kShipGaugeOpenDegrees) * 0.5F));
/// An UNLIT segment keeps its hue at 35%, so an empty half still reads as part of the instrument --
/// and the eye can see HOW empty it is -- instead of vanishing.
constexpr std::array<float, 3> kShipGaugeHullLit{0.30F, 0.95F, 0.40F};
constexpr std::array<float, 3> kShipGaugeHullDim{0.12F, 0.32F, 0.14F};
constexpr std::array<float, 3> kShipGaugeAmmoLit{0.30F, 0.55F, 1.00F};
constexpr std::array<float, 3> kShipGaugeAmmoDim{0.12F, 0.20F, 0.35F};

/// THE TWO READINGS in the ring's opening, in the same 5x5 cell capitals the reticle's label and
/// the mount plates use: 3 px a cell at 720 px of frame height, so a digit is 15 px tall. Both
/// stand on ONE opaque near-black plate, because a number in this corner looks out at terrain and
/// the plate is its contrast -- the vertex format has no alpha worth leaning on for a reading that
/// has to survive grass, rock and sky alike. The arcs are NOT plated; the ring's own segments are
/// the shape.
constexpr float kShipGaugeCellFraction = 3.0F / 720.0F;
constexpr std::array<float, 3> kShipGaugePlateColour{0.03F, 0.04F, 0.03F};
constexpr float kShipGaugeHullNumberAbove = 0.020F; ///< the hull number, above the centre
constexpr float kShipGaugeAmmoNumberBelow = 0.012F; ///< the ammo number, below it
/// Both readings are right-aligned to centre + this, so a 2-digit and a 3-digit value line up on
/// the right. THE ONE PLACE THE BRIEF'S 0.018 MEASURED WRONG: a 3-digit block is 17 cells wide
/// (0.0708 H) and the opening's left edge is the inner radius, 0.044 H from the centre, so 0.018
/// put the block's left edge at -0.0528 H -- 6.4 px of the leading digit under the ring's own
/// lower-left band at 720 px. The hull digit survived (its ink is a darker green than the arc) but
/// the AMMO digit was the arc's own blue, drawn on blue: "100" rendered as "00". 0.030 clears the
/// band by 2.3 px (its plate still tucks 0.6 px under the arc, where the arc covers it).
constexpr float kShipGaugeNumberRight = 0.030F; ///< both right-aligned to centre + this

/// THE WEAPON LIST: the carried weapons beside the ring, top to bottom in `kDrivenWeapons`
/// order, one row each, the active one on a green bar. row_y(i) = cy + (i - 1.5) * pitch, so the
/// four rows straddle the ring's own centre line, two above it and two below.
constexpr float kShipGaugeRowPitchCells = 8.0F;
constexpr float kShipGaugeIconRadiusFraction = 0.006F;
constexpr float kShipGaugeIconGapFraction = 0.014F; ///< ring's outer edge to the icon's centre
constexpr float kShipGaugeNameGapFraction = 0.014F; ///< the icon's centre to the row's text
constexpr int kShipGaugeIconSegments = 8;           ///< the icon disc: a fan of eight triangles
constexpr float kShipGaugeActiveBarHeightCells = 6.0F;
constexpr float kShipGaugeActiveBarPadFraction = 0.004F; ///< the bar's left edge, past the icon
/// HARRI'S HIGH-CONTRAST REQUIREMENT: the weapon he has up must be obvious at a glance, so the
/// active row is bright green ink standing on a green bar and every other row is dim grey with no
/// bar at all. Never subtle.
constexpr std::array<float, 3> kShipGaugeActiveInk{0.55F, 1.0F, 0.55F};
constexpr std::array<float, 3> kShipGaugeIdleInk{0.55F, 0.55F, 0.55F};
constexpr std::array<float, 3> kShipGaugeActiveBar{0.16F, 0.42F, 0.18F};
/// The icon disc's colour per weapon, in `kDrivenWeapons` order. The colours are OURS: BZ2's own
/// icons are weapon art we do not have, and `ReticleShape` carries no colour to take one from
/// (weapon.hpp:75), so the reticle's SHAPE stays the shape cue and this disc is only a second,
/// coarser cue beside the name. Indexed by the row's position in `kDrivenWeapons`, never by the
/// enum's own value, so the table and the list can never fall out of step.
constexpr std::array<std::array<float, 3>, 5> kShipGaugeIconColour{
    {{{0.55F, 0.90F, 0.55F}},   // gun
     {{0.95F, 0.55F, 0.25F}},   // rocket
     {{0.95F, 0.85F, 0.30F}},   // mortar
     {{0.45F, 0.70F, 0.95F}},   // bouncer
     {{0.90F, 0.45F, 0.80F}}}}; // shotgun

void append_ship_gauge(std::vector<SceneVertex>& out, const SceneOptions& options,
                       const sim::Sim& sim) {
    const float frame_w = static_cast<float>(options.extent.width);
    const float frame_h = static_cast<float>(options.extent.height);
    const float distance = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto at = [&](float px, float py) {
        const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
        return Vec3{point.x, point.y, point.z};
    };
    const auto rect = [&](float layer, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto to = [&](float px, float py) {
            const RayPoint point = ray_point(options.camera, options.extent, px, py, layer);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, to(ax, ay), to(bx, ay), to(bx, by), to(ax, by), rgb);
    };
    // THREE LAYERS, in the ratios the HUD clearance comment above lays out (1 : 1.6 : 2.2, wider
    // than the depth variation across any one of these quads): the reading and the weapon rows
    // nearest, then the ring, then the two plates. The ring and the reading are not decorative
    // neighbours -- a three digit number right-aligned in the ring's opening runs about six pixels
    // of its leading digit UNDER the ring's own left band -- so on ONE layer the two chew holes in
    // each other wherever they overlap, and the digit that loses reads as a different number.
    const float ring_at = distance * kHudFaceRatio;
    const float plate_at = distance * kHudBorderRatio;
    const auto ring_point = [&](float px, float py) {
        const RayPoint point = ray_point(options.camera, options.extent, px, py, ring_at);
        return Vec3{point.x, point.y, point.z};
    };

    const float cx = frame_w - (kShipGaugeCentreFromRight * frame_h);
    const float cy = frame_h - (kShipGaugeCentreFromBottom * frame_h);
    const float outer = kShipGaugeOuterFraction * frame_h;
    const float inner = outer - (kShipGaugeThicknessFraction * frame_h);
    const float cell = frame_h * kShipGaugeCellFraction;
    const float pad = cell;

    // THE HULL FRACTION, and the number that follows it. The sim owns both: the driven unit's own
    // hp over its spec's. With NO driven unit there is no hull to report at all, so every green
    // segment is lit rather than the arc reading empty -- an empty arc would say "dead" about a
    // tank that is not even there -- and no hull number is drawn either, because a number the sim
    // never produced is the lie the ammo placeholder exists to avoid.
    const sim::Unit* viewer = sim.find(options.viewer_unit);
    const std::uint32_t max_hp = viewer != nullptr ? sim::spec_of(viewer->kind).hp : 0U;
    const std::uint32_t hp = viewer != nullptr ? std::min<std::uint32_t>(viewer->hp, max_hp) : 0U;
    const float hull_fraction = viewer == nullptr ? 1.0F
                                : max_hp > 0 ? static_cast<float>(hp) / static_cast<float>(max_hp)
                                             : 0.0F;
    // WHOLE SEGMENTS LIGHT UP, never a sliver of one: the gauge is read at a glance in the middle
    // of a fight, and part of a segment is not a reading.
    const auto lit_segments = [](float fraction) {
        return std::clamp(
            static_cast<int>(std::lround(fraction * static_cast<float>(kShipGaugeSegmentsPerHalf))),
            0, kShipGaugeSegmentsPerHalf);
    };
    const int hull_lit = lit_segments(hull_fraction);
    const int ammo_lit = lit_segments(kAmmoPlaceholderFraction);

    // A point `degrees` round (0 right, positive downward) and `radius` out, in frame pixels.
    const auto on_ring = [&](float radius, float degrees) {
        const float radians = degrees * (kPi / 180.0F);
        return std::array<float, 2>{cx + (radius * std::cos(radians)),
                                    cy + (radius * std::sin(radians))};
    };
    const auto segment = [&](float from, float to, const std::array<float, 3>& rgb) {
        const std::array<float, 2> a = on_ring(inner, from);
        const std::array<float, 2> b = on_ring(outer, from);
        const std::array<float, 2> c = on_ring(outer, to);
        const std::array<float, 2> d = on_ring(inner, to);
        emit_quad(out, ring_point(a.at(0), a.at(1)), ring_point(b.at(0), b.at(1)),
                  ring_point(c.at(0), c.at(1)), ring_point(d.at(0), d.at(1)), rgb);
    };

    // The two halves are ONE walk from `kShipGaugeOpenDegrees` to `kShipGaugeCloseDegrees`: slices
    // 0..11 are the blue half and 12..23 the green one. BZ2's arcs DRAIN TOWARD THE MIDDLE, so blue
    // fills from the 45 end and green from the 315 end and the two never swap ends as they empty --
    // filling either one from the middle outward would swap them, and the instrument would be
    // unreadable at exactly the moment it matters.
    const float slice = (kShipGaugeCloseDegrees - kShipGaugeOpenDegrees)
                        / (2.0F * static_cast<float>(kShipGaugeSegmentsPerHalf));
    const float half_gap = kShipGaugeSegmentGapDegrees * 0.5F;
    for (int index = 0; index < 2 * kShipGaugeSegmentsPerHalf; ++index) {
        const bool upper = index >= kShipGaugeSegmentsPerHalf;
        // The position within its OWN half, counted from that half's starting angle: for the blue
        // half that is the 45 end, where its fill starts, and for the green half the 180 end, which
        // is why green's lit run is the run at the END of its half.
        const int step = index % kShipGaugeSegmentsPerHalf;
        const int lit_count = upper ? hull_lit : ammo_lit;
        const bool lit = upper ? step >= (kShipGaugeSegmentsPerHalf - lit_count) : step < lit_count;
        // Half a gap comes off each open end of the ring too, so the notch on the right is the same
        // size as the gaps between the segments rather than half again as wide.
        const float start = kShipGaugeOpenDegrees + (static_cast<float>(index) * slice) + half_gap;
        const std::array<float, 3>& rgb = upper ? (lit ? kShipGaugeHullLit : kShipGaugeHullDim)
                                                : (lit ? kShipGaugeAmmoLit : kShipGaugeAmmoDim);
        segment(start, start + slice - kShipGaugeSegmentGapDegrees, rgb);
    }

    // THE TWO NUMBERS, right-aligned to one edge so a two digit value and a three digit one line up
    // on it, and on ONE plate that covers both lines and the gap between them.
    const std::string_view ammo_reading = ammo_placeholder_percent();
    std::string hull_reading;
    if (viewer != nullptr) {
        hull_reading = std::to_string(hp);
    }
    const float right_edge = cx + (kShipGaugeNumberRight * frame_h);
    const float hull_middle = cy - (kShipGaugeHullNumberAbove * frame_h);
    const float ammo_middle = cy + (kShipGaugeAmmoNumberBelow * frame_h);
    const float half_text = 2.5F * cell;
    // A run of glyphs is six cells of pitch each, the last one five wide: two glyphs are 11 cells.
    const auto text_width
        = [&](std::size_t glyphs) { return ((static_cast<float>(glyphs) * 6.0F) - 1.0F) * cell; };
    const float hull_width = hull_reading.empty() ? 0.0F : text_width(hull_reading.size());
    const float widest = std::max(hull_width, text_width(ammo_reading.size()));
    rect(plate_at, right_edge - widest - pad,
         (hull_reading.empty() ? ammo_middle : hull_middle) - half_text - pad, right_edge + pad,
         ammo_middle + half_text + pad, kShipGaugePlateColour);
    // `word` with its RIGHT edge at `right` and its middle at `middle`, in `ink`: the weapon HUD's
    // own glyph walk, over the same 5x5 cell font.
    const auto text
        = [&](std::string_view word, float right, float middle, const std::array<float, 3>& ink) {
              const float left = right - text_width(word.size());
              const float top = middle - half_text;
              for (std::size_t index = 0; index < word.size(); ++index) {
                  const std::array<std::uint8_t, 5> rows = mount_glyph(word.at(index));
                  const float glyph_left = left + (static_cast<float>(index) * 6.0F * cell);
                  for (std::size_t row = 0; row < rows.size(); ++row) {
                      for (int col = 0; col < 5; ++col) {
                          if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                              continue;
                          }
                          const float x = glyph_left + (static_cast<float>(col) * cell);
                          const float y = top + (static_cast<float>(row) * cell);
                          rect(distance, x, y, x + cell, y + cell, ink);
                      }
                  }
              }
          };
    if (!hull_reading.empty()) {
        // The number wears the same colour its arc does: green from half health, amber from a
        // fifth, red below, so the reading and the ring can never disagree.
        text(hull_reading, right_edge, hull_middle, health_colour(hp, max_hp));
    }
    text(ammo_reading, right_edge, ammo_middle, kShipGaugeAmmoLit);

    // THE WEAPON LIST: one row per carried weapon, in `kDrivenWeapons` order, the active one on its
    // green bar. The name is the weapon HUD's own (`weapon_name`), so the list and the label by the
    // reticle can never disagree about what is up.
    const float pitch = kShipGaugeRowPitchCells * cell;
    const float icon_x = cx + outer + (kShipGaugeIconGapFraction * frame_h);
    const float name_x = icon_x + (kShipGaugeNameGapFraction * frame_h);
    const float icon_radius = kShipGaugeIconRadiusFraction * frame_h;
    const float bar_half = kShipGaugeActiveBarHeightCells * 0.5F * cell;
    for (std::size_t row = 0; row < kDrivenWeapons.size(); ++row) {
        // B: a hull blocked from a weapon never offers it. The row keeps its slot in the pitch
        // ladder rather than the list re-centring, so a block leaves a gap instead of moving every
        // row the player has learned. Nothing is blocked today; the check is the seam.
        if (viewer != nullptr && !weapon_allowed(viewer->kind, kDrivenWeapons.at(row))) {
            continue;
        }
        // Centred on the ring: the list's middle row sits at the ring's own centre. With an even
        // count the centre falls between two rows; with an odd one it lands on a row.
        const float row_y = cy
                            + ((static_cast<float>(row)
                                - (static_cast<float>(kDrivenWeapons.size()) - 1.0F) * 0.5F)
                               * pitch);
        const bool active = kDrivenWeapons.at(row) == options.weapon;
        const std::string_view name = weapon_name(kDrivenWeapons.at(row));
        const float name_width = text_width(name.size());
        // The bar goes down FIRST, on the ring's own layer, so the name stands ON it rather than
        // under it.
        if (active) {
            rect(ring_at, icon_x - (kShipGaugeActiveBarPadFraction * frame_h), row_y - bar_half,
                 name_x + name_width + pad, row_y + bar_half, kShipGaugeActiveBar);
        }
        // The disc: a fan of `kShipGaugeIconSegments` triangles round the icon's centre, cheap and
        // round enough at fourteen pixels across.
        const std::array<float, 3>& icon_rgb = kShipGaugeIconColour.at(row);
        for (int slice_index = 0; slice_index < kShipGaugeIconSegments; ++slice_index) {
            const float from = (2.0F * kPi * static_cast<float>(slice_index))
                               / static_cast<float>(kShipGaugeIconSegments);
            const float to = (2.0F * kPi * static_cast<float>(slice_index + 1))
                             / static_cast<float>(kShipGaugeIconSegments);
            emit_triangle(
                out, at(icon_x, row_y),
                at(icon_x + (icon_radius * std::cos(from)), row_y + (icon_radius * std::sin(from))),
                at(icon_x + (icon_radius * std::cos(to)), row_y + (icon_radius * std::sin(to))),
                icon_rgb);
        }
        text(name, name_x + name_width, row_y, active ? kShipGaugeActiveInk : kShipGaugeIdleInk);
    }
}

/// THE HUD PANELS along the top left of the frame, in frame pixels on the HUD layer, scaled by the
/// frame's height (the px below are at 720).
///
/// THE COMMAND ROW (ROW-SLOTS): `~`, `1`, `2`, `3`, `F1`..`F10`, fourteen plates each in its own
/// fixed spot, left to right. A key that holds nothing is not drawn and the row does not close up:
/// its space stays blank for what is built next. A plate is at most 88 px wide (it narrows so the
/// fourteen fit a narrow frame) and 34 tall on a black plate with a 2 px frame (yellow when any
/// unit of the group is selected, white otherwise), 6 px from the top and 6 px apart. Every glyph
/// is 10 px tall (cell 2.0), which is BZ2's own HUD text height, counted rather than guessed: its
/// text is 10 px in the 800x600 originals and still ~10 px in a 1280x730 screenshot of the
/// remaster, so it is a fixed pixel font that ignores the resolution. The plate carries ONE thing:
/// the key's label, centred on the plate both ways (Harri, 2026-10-30: "Centre the label. Stop
/// drawing the kind text. Stop drawing the count on the plate. Only the f key label, the icon under
/// it, and the vertical health ticks under the icon."). No kind name and no head count: the count
/// was the only numeric reading of a group's size, and the ticks stop at ten, which he accepted.
/// Under the plate hangs the ICON in its reserved square (`key_icon_box`: a pale side-view
/// silhouette, `kind_icon` for a vehicle or a building, `armory_icon` for the key `3`,
/// left-justified like the ticks, no colour for whose it is; `~` has none), and under that a ROW of
/// upright TICKS, ONE PER LIVING VEHICLE, left to right in the group's spawn order from the key's
/// left edge, up to ten (BAR-LOOK, HIS RULING 2026-10-30, from a BZ2 crop: "Vertical, and always
/// the same size. They are left-justified, up to ten. ... it only changes colour, never the size").
/// Every tick is the same size and never shrinks with damage; each is coloured by its own vehicle's
/// hit points on `bar_segments`'s ramp, bright green through yellow and red to a near-black that
/// still reads against its dark track. A vehicle that dies takes its tick with it and the ones
/// after it close up to the left, no blanks. `~` is its label alone and has no ticks; a building
/// key has one tick. Under the row, `options.notice` in 10 px yellow capitals.
///
/// THE ATTACK LIST (BZ2A): the nearest enemies `3 ATTACK` offered, by the same plate and the same
/// type, one row per enemy. It sits in the row BELOW the notice, which is reserved whether or not
/// there is a notice, so the list never moves and never overlaps: the player is choosing under time
/// pressure and a panel that jumps is a panel that gets misread.
constexpr float kStripFramePixels = 2.0F;
constexpr float kStripKeyCell = 2.0F;
/// (The gap between the things stacked under a plate, the icon's reserved square and the strip's
/// bottom edge are in scene.hpp: `kStripStackGapPixels`, `kStripIconPixels`, `kStripBottomPixels`.)
///
/// The vehicle-type icon's reserved square under the plate (Harri: "an image of the type of
/// vehicle", turrets in his crop, then "four to five times the current lengths ... re-draw the
/// icons"). 72 px at 720 is 4.5 times the 16 px of the first cut and is the width of the key
/// itself, so the icon hangs under its key as BZ2's do (a ~65 px keycap, 65-75 px icons). It is
/// `kIconCells` mask cells of one pixel each, so a mask fills it exactly.
static_assert(kStripIconPixels == static_cast<float>(kIconCells),
              "an icon mask is one pixel a cell, so the box is exactly the mask");
static_assert(kStripIconPixels <= kStripSlotWidthPixels,
              "the icon hangs inside its key's width; it is never wider than the plate");
/// One tick: 4 px wide and 14 tall (`kStripTickHeightPixels`), so it is 3.5 times taller than wide
/// and cannot be read as a square, 3 px apart so neighbours stay distinct. Ten of them span
/// 10 * 4 + 9 * 3 = 67 px, inside the 72 px slot; a slot narrowed by a narrow frame scales the
/// whole row down to fit, since a row wider than its key would run into the next key's.
constexpr float kStripTickWidthPixels = 4.0F;
constexpr float kStripTickGapPixels = 3.0F;
/// How many ticks a key has room for: Harri's own count, "up to ten" (BZ2). An eleventh vehicle is
/// not drawn: the ticks never shrink or wrap to make room.
constexpr std::size_t kStripTicks = 10;
/// How far behind the glyphs and ticks (the marks layer) the strip's under-layers sit: the ticks'
/// outlines and the build gauge's track a hair in front of a plate's face, and the gauge's fill a
/// step nearer than that, so ink > fill > track > plate face at any range.
constexpr float kStripUnderInkRatio = kHudFaceRatio * 0.99F;
constexpr float kStripGaugeFillRatio = kHudFaceRatio * 0.98F;
/// The corner radius of a HUD panel, at 720p. Panels are rounded like BZ2's rather than square, and
/// the frame ring clamps the radius to at least its own width so the face inside it can never be
/// handed a negative radius.
constexpr float kHudCornerPixels = 5.0F;
/// How much of the world shows through a HUD panel's face. The frame and every glyph stay opaque,
/// so the ink keeps its full contrast against the panel and only the panel itself is translucent --
/// which is how BZ2's panels read.
constexpr float kHudPanelAlpha = 0.65F;
constexpr std::array<float, 3> kStripFrame{0.95F, 0.95F, 0.95F};
/// THE KEYCAP (Harri: "I would also like the function key displayed in the same way as here (F3)"):
/// a command-row key reads as a cap, not a flat panel, by paint alone -- the plate's box, its text
/// and its frame colour are what they were. From the outside in: a 1 px near-black OUTLINE that
/// takes the frame's outer pixel (the frame's second pixel keeps the white or the yellow, which is
/// what says "selected"), then a 1 px BEVEL in the margin the glyphs leave inside the frame (the
/// label starts 1 px inside it), lighter along the top and left and darker along the bottom and
/// right as if lit from the upper left, then a slate face. Flat bands: the renderer has no gradient
/// here. The face is dark enough that the white text keeps its contrast through the panel's
/// translucency.
constexpr float kStripOutlinePixels = 1.0F;
constexpr float kStripBevelPixels = 1.0F;
constexpr std::array<float, 3> kStripOutline{0.02F, 0.03F, 0.05F};
constexpr std::array<float, 3> kStripBevelLight{0.50F, 0.57F, 0.67F};
constexpr std::array<float, 3> kStripBevelDark{0.08F, 0.10F, 0.14F};
constexpr std::array<float, 3> kStripKeycapFace{0.16F, 0.21F, 0.29F};
/// How a plate is painted: the glass the notice wears (black, one frame ring),
/// or the keycap a command-row key wears. One drawing each; every key is a keycap.
enum class PlateStyle : std::uint8_t { Glass, Keycap };
constexpr std::array<float, 3> kStripSelected{1.0F, 0.85F, 0.10F};
/// THE COMMAND MENU (RB0.5, the look MENU-READ): `menu_rows`'s rows, key then verb, down the left
/// margin in the row under the notice. Every list is rows of it (AW): `~ Select Unit`, `3 Repair`,
/// `3 Attack` and the nav marks, so no list has a look of its own. Each row is two of BZ2's
/// translucent blue bars, laid out by `menu_layout`: a key bar right-aligned on one column edge and
/// a verb bar a gutter past it, rounded on their OUTER ends and square where they face the gutter,
/// so the amber leader line down it reads as cutting a slice out of the background (the user's
/// correction, 2026-10-27). A live row is the strip's white on a blue face that fades top to
/// bottom; a dimmed row is grey on a flat dark face whatever the reason, because `MenuRow::reason`
/// is for the code and the look is the same for both. The glyphs gain weight from a 1 px black
/// outline, never from a bigger cell.
///
/// `kMenuCell` is the LAYOUT cell: every bar is measured in it, the HUD's 2 px. The TYPE is drawn
/// at `kMenuTypeCell`, smaller, inside the same bars (MENU-FONT): the user's "only the font would
/// be smaller while the menu item background shape would remain the same size", which makes the
/// menu's type scale its own and the one deliberate exception to the HUD's single one. Its glyphs
/// are the 5x5 masks box-filtered onto whole pixels (`glyph_coverage`), drawn one quad per covered
/// pixel at its coverage, so their edges are antialiased; lowercase is small capitals.
constexpr float kMenuCell = 2.0F;
constexpr float kMenuTypeCell = 1.5F;
constexpr std::array<float, 3> kMenuDimInk{0.67F, 0.67F, 0.67F};
/// The geometry, in layout cells (2 px at the 720 reference): a 13-cell bar every 15.5 cells, the
/// key padded 3.5 cells either side and the verb 4, a 4-cell gutter between the two columns with a
/// 1-cell line down its middle. Measured off the user's capture in pixels; see `menu_layout`.
constexpr float kMenuBarCells = 13.0F;
constexpr float kMenuPitchCells = 15.5F;
constexpr float kMenuKeyPadCells = 3.5F;
constexpr float kMenuVerbPadCells = 4.0F;
constexpr float kMenuGutterCells = 4.0F;
constexpr float kMenuLineCells = 1.0F;
/// The filled dot the line ends on, over the selected unit's radar blip (MENU-LEG): its radius in
/// pixels at the 720 reference, and the slices of its fan.
constexpr float kMenuDotPixels = 2.0F;
constexpr int kMenuDotSlices = 8;
/// The leader line and its dot are drawn this much NEARER than the HUD's marks layer, on the same
/// rays. `ray_point` puts a pixel on the same spot of the screen at any distance, so this moves
/// nothing sideways; but at the marks' own distance the dot lost the depth test to the very blip
/// it marks and was not drawn at all (MENU-LEG's first capture). Farther than the radar's nav
/// marks (0.87), so a nav plus still reads over a line that crosses it.
constexpr float kMenuLeaderRatio = 0.95F;
/// The right column (a nav row's metres) clears the widest verb bar by this much.
constexpr float kMenuRightGapCells = 2.0F;
/// The face, measured down a key bar in the user's capture -- (65,92,145) at the top to (63,66,85)
/// at the bottom over a world of ~(72,67,60) -- and solved for the bar's own colour at 0.68 alpha.
/// RED HOLDS and only green and blue fade: a blue light, never a lighter card. The first band is
/// the brighter top edge the capture shows over its top tenth.
constexpr std::array<float, 3> kMenuFaceHighlight{0.255F, 0.471F, 0.843F};
constexpr std::array<float, 3> kMenuFaceTop{0.243F, 0.404F, 0.710F};
constexpr std::array<float, 3> kMenuFaceBottom{0.231F, 0.251F, 0.376F};
constexpr float kMenuFaceAlpha = 0.68F;
/// A dimmed row's face: flat, dark, a little more opaque so the grey ink still reads on it.
constexpr std::array<float, 3> kMenuUnlitFace{0.055F, 0.063F, 0.086F};
constexpr float kMenuUnlitAlpha = 0.74F;
/// The pale ring round a live bar's rounded end, half a cell (1 px) wide. A dimmed bar has none.
constexpr std::array<float, 3> kMenuRing{0.82F, 0.90F, 1.0F};
/// THE LIST ROWS' TWO THIN LINES (AW), under a unit's name on the verb bar: health, then ammunition
/// capacity. Each is a dark track the width of the name with the fill laid on it,
/// `kMenuBarLineCells` thick, at these heights down the 13-cell bar -- clear of the type, which is
/// centred.
constexpr float kMenuBarLineCells = 0.5F;
constexpr float kMenuHealthLineCells = 9.0F;
constexpr float kMenuAmmoLineCells = 10.5F;
constexpr std::array<float, 3> kMenuAmmoInk{0.35F, 0.70F, 1.0F};
constexpr float kMenuTrackAlpha = 0.6F;
constexpr std::array<float, 3> kMenuTrackInk{0.0F, 0.0F, 0.0F};
constexpr float kMenuRingAlpha = 0.55F;
constexpr float kMenuRingCells = 0.5F;
/// The glyphs' outline is their coverage grown by half a layout cell (1 px) all round, in black at
/// that grown coverage, drawn between the face and the ink so the ink always wins.
constexpr float kMenuOutlineCells = 0.5F;
constexpr float kMenuOutlineRatio = 1.3F;

/// The HUD text cell: 2.0 at the 720 reference -- the HUD's ONE type scale, which is BZ2's measured
/// glyph height. A fresh element joins that scale like every other one; "make it obvious" buys a
/// brighter ink and an opaque plate, never a bigger cell. (The amber `SCRAP` plate that stood above
/// the radar is gone, Harri 2026-11-03: the gauge below carries the same number.)
constexpr float kLedgerCell = 2.0F;

/// THE STORE GAUGE (Harri, 2026-11-01), beside the radar -- or beside the map when R has swapped
/// them. Every rectangle comes from `store_gauge_layout`; this only paints them, in pixels through
/// `ray_point` like every HUD layer, the pill first, then the chunks, then the white bar over
/// them, then the number. Depth is the ledger's own layering: each later layer a step nearer.
/// Drawn under `show_ledger`, the layer's switch since the ledger plate was its sibling.
void append_store_gauge(std::vector<SceneVertex>& out, const SceneOptions& options,
                        const sim::Sim& sim) {
    if (!options.show_ledger) {
        return;
    }
    const float px = static_cast<float>(options.extent.height) * kHudPerFrameHeight;
    const float cell = kLedgerCell * px;
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    RadarPanel beside = radar_panel(options.extent);
    if (options.show_world_map) {
        const WorldMapLayout map = world_map_layout(options.extent);
        beside = RadarPanel{map.x, map.y, map.size, map.size};
    }
    const sim::Side side = options.viewer_side;
    const StoreGaugeLayout gauge
        = store_gauge_layout(beside, sim.capacity(side), sim.fast_band(side), sim.ore(side), cell);
    const auto rect = [&](float distance, const GaugeRect& r, const std::array<float, 3>& rgb) {
        if (r.width <= 0.0F || r.height <= 0.0F) {
            return;
        }
        const auto at = [&](float x, float y) {
            const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(r.x, r.y), at(r.x + r.width, r.y), at(r.x + r.width, r.y + r.height),
                  at(r.x, r.y + r.height), rgb);
    };
    // The pill in bands, each no taller than a chunk (`StoreGaugeLayout::pill`), then every chunk a
    // hair nearer, then the white bar in the pieces the chunks cut it into, then the number.
    for (const GaugeRect& band : gauge.pill) {
        rect(marks_at * kHudBorderRatio, band, kGaugePill);
    }
    for (const GaugeChunk& chunk : gauge.chunks) {
        rect(marks_at * kHudBorderRatio * 0.99F, chunk.box, chunk.gold ? kGaugeGold : kGaugeGreen);
    }
    for (const GaugeRect& fill : gauge.fills) {
        rect(marks_at, fill, kGaugeLevel);
    }
    // The number, centred under the column, in the HUD's one 5x5 cell font.
    const float text_width = ((static_cast<float>(gauge.text.size()) * 6.0F) - 1.0F) * cell;
    const float left = gauge.number.x + ((gauge.number.width - text_width) * 0.5F);
    const float top = gauge.number.y + (((gauge.number.height) - (5.0F * cell)) * 0.5F);
    for (std::size_t index = 0; index < gauge.text.size(); ++index) {
        const std::array<std::uint8_t, 5> rows = mount_glyph(gauge.text.at(index));
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (int col = 0; col < 5; ++col) {
                if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                    continue;
                }
                const float x = left + (static_cast<float>(index) * 6.0F * cell)
                                + (static_cast<float>(col) * cell);
                rect(marks_at, GaugeRect{x, top + (static_cast<float>(row) * cell), cell, cell},
                     kGaugeInk);
            }
        }
    }
}

/// THE VERDICT (C8, retargeted by OBJ-a). The war is won and lost on recyclers: destroy the
/// enemy's and you have won, lose yours and you have lost, instantly and whatever else either side
/// has left. Nothing announced that, so a player could lose without being told and win without
/// noticing. This draws the one line that says which happened.
///
/// EMITS NOTHING while the war is undecided (`Sim::war_outcome` is `Undecided`), which is what
/// keeps the layer free for every frame of a battle still being fought: the vertex list, and so the
/// tail that other tests read, is exactly what it was. The outcome is read from the sim's own
/// accessor, never re-derived here, so the banner cannot disagree with the war it describes. It is
/// stated from the player's chair, so a viewer on the enemy side reads it reversed.
///
/// The cell is the HUD's own 2.0 at the 720 reference like every other element; the banner is made
/// to stand out by its opaque plate, its centring and its colour, never by a bigger cell.
constexpr std::array<float, 3> kOutcomePlate{0.0F, 0.0F, 0.0F};
constexpr std::array<float, 3> kOutcomeWonInk{0.35F, 1.0F, 0.40F};
constexpr std::array<float, 3> kOutcomeLostInk{1.0F, 0.30F, 0.25F};
constexpr float kOutcomeCell = 2.0F;
/// Where the banner's top sits, as a fraction of the frame's height: high enough to clear the
/// driving view's centre and the crosshair, low enough to be read without leaving the fight.
constexpr float kOutcomeTopFraction = 0.18F;

void append_outcome(std::vector<SceneVertex>& out, const SceneOptions& options,
                    const sim::Sim& sim) {
    if (!options.show_outcome) {
        return;
    }
    const sim::WarOutcome outcome = sim.war_outcome();
    if (outcome == sim::WarOutcome::Undecided) {
        return; // no recycler has died: there is nothing to announce
    }
    const bool viewer_lost
        = outcome
          == (options.viewer_side == sim::Side::Player ? sim::WarOutcome::PlayerLost
                                                       : sim::WarOutcome::PlayerWon);
    // The viewer's own loss is the message he most needs. (Both recyclers dead is a player loss.)
    const std::vector<std::string_view> words
        = viewer_lost ? std::vector<std::string_view>{"YOU", "HAVE", "LOST"}
                      : std::vector<std::string_view>{"ENEMY", "DEFEATED"};
    const std::array<float, 3>& ink = viewer_lost ? kOutcomeLostInk : kOutcomeWonInk;

    const float px = static_cast<float>(options.extent.height) * kHudPerFrameHeight;
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto at = [&](float x, float y) {
            const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };
    const auto word_width = [](std::string_view word, float cell) {
        return ((static_cast<float>(word.size()) * 6.0F) - 1.0F) * cell;
    };
    const float cell = kOutcomeCell * px;
    const float gap = cell;
    float total = 0.0F;
    for (std::size_t i = 0; i < words.size(); ++i) {
        total += word_width(words.at(i), cell);
        if (i + 1 < words.size()) {
            total += gap;
        }
    }
    const float pad = cell;
    const float text_h = 5.0F * cell;
    const float left = (static_cast<float>(options.extent.width) * 0.5F) - (total * 0.5F);
    const float top = static_cast<float>(options.extent.height) * kOutcomeTopFraction;
    rect(marks_at * kHudBorderRatio, left - pad, top - pad, left + total + pad, top + text_h + pad,
         kOutcomePlate);
    float pen = left;
    for (const std::string_view word : words) {
        for (std::size_t index = 0; index < word.size(); ++index) {
            const std::array<std::uint8_t, 5> rows = mount_glyph(word.at(index));
            const float glyph_left = pen + (static_cast<float>(index) * 6.0F * cell);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                        continue;
                    }
                    const float x = glyph_left + (static_cast<float>(col) * cell);
                    const float y = top + (static_cast<float>(row) * cell);
                    rect(marks_at, x, y, x + cell, y + cell, ink);
                }
            }
        }
        pen += word_width(word, cell) + gap;
    }
}

/// THE STATISTICS SCREEN (AF). Drawn while the game is paused, over the whole frame, and meant to
/// be read from across a room: an opaque black plate, white ink, a title in the ledger's gold, a
/// cell 2.5 times the HUD's own and the two sides in the verdict's two colours (green yours, red
/// theirs). The owner's eyesight is a known constraint, so nothing here is subtle or small.
///
/// EMITS NOTHING unless `SceneOptions::statistics` is set. It sits in front of every other HUD
/// layer (the nearest of them is the nav glyph at 0.87 of the marks), the ink in front of the
/// plate, so the HUD under it cannot show through. Letters come from the HUD's own font, which has
/// no colon: the time reads `12 MIN 07 SEC`.
constexpr std::array<float, 3> kStatsPlate{0.0F, 0.0F, 0.0F};
constexpr std::array<float, 3> kStatsInk{1.0F, 1.0F, 1.0F};
constexpr std::array<float, 3> kStatsTitleInk{1.0F, 0.85F, 0.30F};
constexpr std::array<float, 3> kStatsYouInk = kOutcomeWonInk;
constexpr std::array<float, 3> kStatsEnemyInk = kOutcomeLostInk;
constexpr float kStatsPlateRatio = 0.82F;
constexpr int kStatsPlateColumns = 32; ///< the plate's grid: see `append_statistics`
constexpr int kStatsPlateRows = 18;
constexpr float kStatsInkRatio = 0.78F;
constexpr float kStatsCell = 5.0F;       ///< the glyph cell at the 720 reference
constexpr float kStatsTitleScale = 1.6F; ///< the title's cell, relative to the rows'
/// The lines the screen stacks (title, the pause hint, the time, the sides' header and the six
/// counts, with a blank between), in row pitches, and the character advances the widest row takes:
/// the cell is the largest that fits both.
constexpr float kStatsRowPitches = 14.0F;
constexpr float kStatsRowAdvances = 36.0F;
constexpr float kStatsPitchCells = 9.0F; ///< a row's pitch, in cells: 5 of glyph and 4 of air
constexpr float kStatsFillFraction = 0.9F;

void append_statistics(std::vector<SceneVertex>& out, const SceneOptions& options,
                       const sim::Sim& sim) {
    if (options.statistics == nullptr) {
        return;
    }
    const BattleStats& stats = *options.statistics;
    const float frame_w = static_cast<float>(options.extent.width);
    const float frame_h = static_cast<float>(options.extent.height);
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto at = [&](float x, float y) {
            const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };
    // The plate is a GRID of small quads, not one big one: a quad is flat, so one the size of the
    // frame sits at the depth of its corners (the longest rays) and the ink near the middle of the
    // frame would lie BEHIND it. Small cells follow the same ray-distance surface every other HUD
    // layer sits on, so the ratios below order the layers everywhere.
    for (int row = 0; row < kStatsPlateRows; ++row) {
        for (int col = 0; col < kStatsPlateColumns; ++col) {
            const auto edge = [](int index, int count, float extent) {
                return extent * static_cast<float>(index) / static_cast<float>(count);
            };
            rect(marks_at * kStatsPlateRatio, edge(col, kStatsPlateColumns, frame_w),
                 edge(row, kStatsPlateRows, frame_h), edge(col + 1, kStatsPlateColumns, frame_w),
                 edge(row + 1, kStatsPlateRows, frame_h), kStatsPlate);
        }
    }

    const float px = frame_h * kHudPerFrameHeight;
    const float cell
        = std::min({kStatsCell * px, kStatsFillFraction * frame_w / (kStatsRowAdvances * 6.0F),
                    kStatsFillFraction * frame_h / (kStatsRowPitches * kStatsPitchCells)});
    const float pitch = cell * kStatsPitchCells;
    const auto text = [&](std::string_view word, float left, float top, float size,
                          const std::array<float, 3>& ink) {
        for (std::size_t index = 0; index < word.size(); ++index) {
            const std::array<std::uint8_t, 5> rows = mount_glyph(word.at(index));
            const float glyph_left = left + (static_cast<float>(index) * 6.0F * size);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                        continue;
                    }
                    const float x = glyph_left + (static_cast<float>(col) * size);
                    const float y = top + (static_cast<float>(row) * size);
                    rect(marks_at * kStatsInkRatio, x, y, x + size, y + size, ink);
                }
            }
        }
    };
    const auto centred
        = [&](std::string_view word, float top, float size, const std::array<float, 3>& ink) {
              const float width = ((static_cast<float>(word.size()) * 6.0F) - 1.0F) * size;
              text(word, (frame_w - width) * 0.5F, top, size, ink);
          };

    const float label_left = frame_w * 0.5F - (kStatsRowAdvances * 6.0F * cell * 0.5F);
    const float you_left = label_left + (20.0F * 6.0F * cell);
    const float enemy_left = label_left + (28.0F * 6.0F * cell);
    float top = (frame_h - (kStatsRowPitches * pitch)) * 0.5F;
    const float title_cell = cell * kStatsTitleScale;
    centred("STATISTICS", top, title_cell, kStatsTitleInk);
    top += pitch * kStatsTitleScale;
    centred("PAUSED - ESC TO RESUME", top, cell, kStatsInk);
    top += pitch * 1.5F;

    const std::uint32_t seconds = sim.tick() / static_cast<std::uint32_t>(sim::kTicksPerSecond);
    const std::string played = std::to_string(seconds / 60U) + " MIN "
                               + (seconds % 60U < 10U ? "0" : "") + std::to_string(seconds % 60U)
                               + " SEC";
    text("TIME PLAYED", label_left, top, cell, kStatsInk);
    text(played, you_left, top, cell, kStatsInk);
    top += pitch * 1.5F;

    text("YOU", you_left, top, cell, kStatsYouInk);
    text("ENEMY", enemy_left, top, cell, kStatsEnemyInk);
    top += pitch;
    const auto row = [&](std::string_view label, std::uint32_t yours, std::uint32_t theirs) {
        text(label, label_left, top, cell, kStatsInk);
        text(std::to_string(yours), you_left, top, cell, kStatsYouInk);
        text(std::to_string(theirs), enemy_left, top, cell, kStatsEnemyInk);
        top += pitch;
    };
    const auto of = [&](sim::Side side) -> const SideTally& { return stats.of(side); };
    constexpr sim::Side kYou = sim::Side::Player;
    constexpr sim::Side kThem = sim::Side::Enemy;
    row("VEHICLES BUILT", of(kYou).built, of(kThem).built);
    row("VEHICLES DESTROYED", of(kYou).destroyed, of(kThem).destroyed);
    row("BIOMETAL SCRAP", sim.collected(kYou), sim.collected(kThem));
    row("SHOTS FIRED", of(kYou).fired, of(kThem).fired);
    row("SHOTS HIT", of(kYou).hit, of(kThem).hit);
    row("SHOTS MISSED", stats.missed(kYou), stats.missed(kThem));
}

void append_hud_panels(std::vector<SceneVertex>& out, const SceneOptions& options,
                       const sim::Sim& sim, std::span<const sim::UnitId> selected) {
    const bool wants_menu = options.show_menu && options.menu != nullptr && !options.menu->empty();
    if (options.row == nullptr && !wants_menu) {
        return;
    }
    const float px = static_cast<float>(options.extent.height) * kHudPerFrameHeight;
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb, float alpha = 1.0F) {
        const auto at = [&](float x, float y) {
            const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb, alpha);
    };
    constexpr std::array<float, 3> kPlate{0.0F, 0.0F, 0.0F};
    // Every lit cell of `word`'s glyphs, by its top-left corner.
    const auto glyph_cells
        = [](std::string_view word, float left, float top, float cell, auto&& emit) {
              for (std::size_t index = 0; index < word.size(); ++index) {
                  const std::array<std::uint8_t, 5> rows = mount_glyph(
                      static_cast<char>(std::toupper(static_cast<unsigned char>(word.at(index)))));
                  const float glyph_left = left + (static_cast<float>(index) * 6.0F * cell);
                  for (std::size_t row = 0; row < rows.size(); ++row) {
                      for (int col = 0; col < 5; ++col) {
                          if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                              continue;
                          }
                          emit(glyph_left + (static_cast<float>(col) * cell),
                               top + (static_cast<float>(row) * cell));
                      }
                  }
              }
          };
    // Ink only; the plate under it is the caller's.
    const auto ink_text = [&](std::string_view word, float left, float top, float cell,
                              const std::array<float, 3>& ink) {
        glyph_cells(word, left, top, cell,
                    [&](float x, float y) { rect(marks_at, x, y, x + cell, y + cell, ink); });
    };
    const auto word_width = [](std::string_view word, float cell) {
        return ((static_cast<float>(word.size()) * 6.0F) - 1.0F) * cell;
    };
    // A plate with a rounded frame round it: a ring of four straight bars and four quarter-annulus
    // corners, the glass face inside, then the caller's ink. The frame is a ring and never one
    // filled rectangle on purpose: a filled rectangle sits behind the face and is then the only
    // thing the face ever blends with, so a translucent panel reads as flat grey instead of letting
    // the world through. Each corner is walked as short arcs so it reads round rather than
    // chamfered, and every point still goes through `ray_point` like everything else here.
    constexpr int kCornerSlices = 4;
    // The four arcs of one rounded rectangle, clockwise from the top left, in screen space with y
    // down. `emit` takes the corner's centre and the arc's start and end angles.
    const auto walk_corners
        = [](float left, float top, float right, float bottom, float radius, auto&& emit) {
              const float mid = kPi * 0.5F;
              emit(left + radius, top + radius, kPi, kPi + mid);
              emit(right - radius, top + radius, kPi + mid, 2.0F * kPi);
              emit(right - radius, bottom - radius, 0.0F, mid);
              emit(left + radius, bottom - radius, mid, kPi);
          };
    const auto plate = [&](float left, float top, float right, float bottom,
                           const std::array<float, 3>& frame, PlateStyle style) {
        const float edge = kStripFramePixels * px;
        const float radius = std::max(kHudCornerPixels * px, edge);
        const float bars_at = marks_at * kHudBorderRatio;
        const float face_at = marks_at * kHudFaceRatio;
        const auto at = [&](float distance, float x, float y) {
            const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
            return Vec3{point.x, point.y, point.z};
        };
        // One ring of the plate: the rounded rectangle `inset` in from the plate's box, `width`
        // thick, each straight bar in its own side's colour. A corner is walked as short arcs and
        // wears the side it comes from for the first half and the side it goes to for the second,
        // so a bevel turns the corner where the light does.
        const auto ring = [&](float inset, float width, const std::array<float, 3>& top_rgb,
                              const std::array<float, 3>& right_rgb,
                              const std::array<float, 3>& bottom_rgb,
                              const std::array<float, 3>& left_rgb) {
            const float l = left + inset;
            const float t = top + inset;
            const float r = right - inset;
            const float b = bottom - inset;
            const float outer = radius - inset;
            // The straight bars, held back from the corners by the radius.
            rect(bars_at, l + outer, t, r - outer, t + width, top_rgb);
            rect(bars_at, l + outer, b - width, r - outer, b, bottom_rgb);
            rect(bars_at, l, t + outer, l + width, b - outer, left_rgb);
            rect(bars_at, r - width, t + outer, r, b - outer, right_rgb);
            // The corners: a quarter annulus from the outer radius in to the inner one, one quad
            // per slice, opaque so the ink still has a crisp edge.
            const float inner = outer - width;
            const std::array<const std::array<float, 3>*, 4> before{&left_rgb, &top_rgb, &right_rgb,
                                                                    &bottom_rgb};
            const std::array<const std::array<float, 3>*, 4> after{&top_rgb, &right_rgb,
                                                                   &bottom_rgb, &left_rgb};
            std::size_t corner = 0;
            walk_corners(l, t, r, b, outer, [&](float cx, float cy, float from, float to) {
                for (int slice = 0; slice < kCornerSlices; ++slice) {
                    const float a = from
                                    + ((to - from) * static_cast<float>(slice)
                                       / static_cast<float>(kCornerSlices));
                    const float c = from
                                    + ((to - from) * static_cast<float>(slice + 1)
                                       / static_cast<float>(kCornerSlices));
                    const std::array<float, 3>& rgb
                        = slice < kCornerSlices / 2 ? *before.at(corner) : *after.at(corner);
                    emit_quad(
                        out, at(bars_at, cx + (outer * std::cos(a)), cy + (outer * std::sin(a))),
                        at(bars_at, cx + (outer * std::cos(c)), cy + (outer * std::sin(c))),
                        at(bars_at, cx + (inner * std::cos(c)), cy + (inner * std::sin(c))),
                        at(bars_at, cx + (inner * std::cos(a)), cy + (inner * std::sin(a))), rgb);
                }
                ++corner;
            });
        };
        // Everything inside this inset is the face: the frame, and on a keycap the outline before
        // it and the bevel after it, are rings the face must not run under.
        float rim_inset = edge;
        if (style == PlateStyle::Glass) {
            ring(0.0F, edge, frame, frame, frame, frame);
        } else {
            const float outline = kStripOutlinePixels * px;
            const float bevel = kStripBevelPixels * px;
            ring(0.0F, outline, kStripOutline, kStripOutline, kStripOutline, kStripOutline);
            ring(outline, edge - outline, frame, frame, frame, frame);
            ring(edge, bevel, kStripBevelLight, kStripBevelDark, kStripBevelDark, kStripBevelLight);
            rim_inset = edge + bevel;
        }
        // The glass face: the same rounded rectangle inset past the rings, as a fan from its
        // centre, translucent so the world shows through it.
        const float inset = radius - rim_inset;
        std::vector<std::array<float, 2>> rim;
        walk_corners(left + rim_inset, top + rim_inset, right - rim_inset, bottom - rim_inset,
                     inset, [&](float cx, float cy, float from, float to) {
                         for (int slice = 0; slice <= kCornerSlices; ++slice) {
                             const float a = from
                                             + ((to - from) * static_cast<float>(slice)
                                                / static_cast<float>(kCornerSlices));
                             rim.push_back(
                                 {cx + (inset * std::cos(a)), cy + (inset * std::sin(a))});
                         }
                     });
        const float centre_x = (left + right) * 0.5F;
        const float centre_y = (top + bottom) * 0.5F;
        const std::array<float, 3>& face = style == PlateStyle::Glass ? kPlate : kStripKeycapFace;
        for (std::size_t i = 0; i < rim.size(); ++i) {
            const std::array<float, 2>& a = rim.at(i);
            const std::array<float, 2>& b = rim.at((i + 1) % rim.size());
            emit_triangle(out, at(face_at, centre_x, centre_y), at(face_at, a.at(0), a.at(1)),
                          at(face_at, b.at(0), b.at(1)), face, kHudPanelAlpha);
        }
    };

    const float top = kStripMarginPixels * px;
    const float gap = kStripSlotGapPixels * px;
    // Fourteen fixed positions have to fit the frame whatever its aspect, so the slot narrows
    // before it would run off the right edge; it never grows past its design width.
    const float frame_width = static_cast<float>(options.extent.width);
    const float slot_w = std::min(
        kStripSlotWidthPixels * px,
        ((frame_width - (2.0F * kStripMarginPixels * px) + gap) / static_cast<float>(kRowSlots))
            - gap);
    const float slot_h = kStripSlotHeightPixels * px;
    const float pad = kStripPadPixels * px;
    // The notice's row, reserved whether or not there is a notice: the menu is drawn under
    // it, and a panel that moves when a notice appears or expires is a panel that gets misread.
    // The strip hangs 128 px (`kStripBottomPixels`) now that its icons are 72, so the rows under it
    // start below ITS bottom edge, not the plate's: otherwise a notice would be drawn over the
    // icons of the keys it sits under.
    const float notice_top = kNoticeTopPixels * px;
    const float stack_gap = kStripStackGapPixels * px;
    const float menu_cell = kMenuCell * px;
    MenuLayout menu_placed;
    if (wants_menu) {
        menu_placed = menu_layout(*options.menu, kStripMarginPixels * px,
                                  notice_top + (5.0F * menu_cell) + (2.0F * pad), menu_cell);
    }
    // THE COMMAND ROW. Every key has its own spot, by index, and a key that holds nothing is
    // skipped without moving the ones after it: its space stays blank for what is built next.
    for (std::size_t index = 0; options.row != nullptr && index < kRowSlots; ++index) {
        if (!options.row->occupied(index)) {
            continue;
        }
        const RowSlot& slot = options.row->slots.at(index);
        const bool is_navs = row_slot_kind(index) == RowSlotKind::Navs;
        const bool is_group = row_slot_kind(index) == RowSlotKind::Group;
        sim::UnitKind reading_kind{};
        std::vector<float> fractions;
        std::vector<std::size_t> picked; // the ticks `~` stepping has picked out (`picked_ticks`)
        // A key held for a shift batch still in a producer's queue (BATCH-PLATE): the units built
        // so far are its ticks as ever, the ones still to come are BLANK ticks after them, and its
        // icon fills as the unit on the line is built.
        std::optional<BatchReading> batch;
        if (!is_navs) {
            const std::optional<SlotReading> reading = read_slot(sim, slot);
            if (is_group) {
                batch = read_batch(sim, options.viewer_side, slot);
            }
            if (!reading.has_value() && !batch.has_value()) {
                continue; // everything in it died since the row was last updated
            }
            reading_kind = reading.has_value() ? reading->kind : batch->kind;
            // A vehicle key has a segment per living vehicle; a building key is one thing, so one.
            if (reading.has_value()) {
                if (is_group) {
                    picked = picked_ticks(*reading, selected);
                }
                fractions
                    = is_group
                          ? reading->fractions
                          : std::vector<float>{reading->max_hp == 0
                                                   ? 0.0F
                                                   : static_cast<float>(reading->hp)
                                                         / static_cast<float>(reading->max_hp)};
            }
        }
        const float left = (kStripMarginPixels * px) + (static_cast<float>(index) * (slot_w + gap));
        // Lit when ANY unit of the group is selected: pressing the key selects the whole group,
        // so a lit slot means "you have some of these".
        const bool chosen
            = std::any_of(slot.units.begin(), slot.units.end(), [selected](sim::UnitId id) {
                  return std::find(selected.begin(), selected.end(), id) != selected.end();
              });
        const std::array<float, 3>& frame = chosen ? kStripSelected : kStripFrame;
        plate(left, top, left + slot_w, top + slot_h, frame, PlateStyle::Keycap);
        // THE LABEL, centred on the plate's face on both axes: it is all the plate carries (Harri:
        // "Only the f key label, the icon under it, and the vertical health ticks under the
        // icon."), so nothing is left for it to collide with. It is centred on what is DRAWN, the
        // extent of its lit cells, not on its advance width: a `1` leaves its right column blank,
        // and centring on the box would put `F1` a pixel left of the middle.
        const float key_cell = kStripKeyCell * px;
        float lit_left = std::numeric_limits<float>::max();
        float lit_right = 0.0F;
        glyph_cells(row_key_label(index), 0.0F, 0.0F, 1.0F, [&](float x, float) {
            lit_left = std::min(lit_left, x);
            lit_right = std::max(lit_right, x + 1.0F);
        });
        ink_text(row_key_label(index),
                 left + ((slot_w - ((lit_left + lit_right) * key_cell)) * 0.5F),
                 top + ((slot_h - (5.0F * key_cell)) * 0.5F), key_cell, frame);
        if (is_navs) {
            continue; // marks have no hit points: no bar, and nothing to draw an icon of
        }
        // THE ICON, in its reserved square: one quad per horizontal run of lit cells of the kind's
        // mask, a cell a pixel. Every key but `~` has one, and only a living key gets this far (a
        // dead one `continue`d above). The armory's key is the one that is drawn by position: it
        // has no kind to look up.
        const IconBox icon = key_icon_box(left, top, px, slot_w);
        const float icon_bottom = icon.top + icon.size;
        const IconMask& mask = row_slot_kind(index) == RowSlotKind::ArmoryKey
                                   ? armory_icon()
                                   : kind_icon(reading_kind);
        const float cell = icon.size / static_cast<float>(kIconCells);
        // One quad per horizontal run of the cells `wanted` picks out, in `rgb`. The ink is the lit
        // cells; the OUTLINE is the clear cells that touch a lit one (the eight neighbours), a
        // cell wide, in the keycap's own near-black: pale ink alone vanishes against the bright
        // sky and the sand, and the outline is part of the icon, not a thing beside it.
        const auto lit = [&mask](std::size_t row, std::size_t col) {
            return row < kIconCells && col < kIconCells && mask.at(row).at(col) == '#';
        };
        const auto draw_runs = [&](auto&& wanted, const std::array<float, 3>& rgb) {
            for (std::size_t row = 0; row < kIconCells; ++row) {
                std::size_t col = 0;
                while (col < kIconCells) {
                    if (!wanted(row, col)) {
                        ++col;
                        continue;
                    }
                    const std::size_t start = col;
                    while (col < kIconCells && wanted(row, col)) {
                        ++col;
                    }
                    const float run_left = icon.left + (static_cast<float>(start) * cell);
                    const float run_top = icon.top + (static_cast<float>(row) * cell);
                    rect(marks_at, run_left, run_top, icon.left + (static_cast<float>(col) * cell),
                         run_top + cell, rgb);
                }
            }
        };
        if (batch.has_value()) {
            // The build's countdown, run as a share of the icon's height: the rows from the bottom
            // up to `filled` cells are the ink, the lit cells above them the ghost.
            const std::size_t filled = static_cast<std::size_t>(
                std::lround(batch->fill * static_cast<float>(kIconCells)));
            const std::size_t first_filled = kIconCells - std::min(filled, kIconCells);
            // THE GAUGE, on the plate and not on the glyph (the icon's square, which a short
            // glyph does not fill): the track over the whole square, the fill over it up to the
            // same line, both under the glyph.
            rect(marks_at * kStripUnderInkRatio, icon.left, icon.top, icon.left + icon.size,
                 icon_bottom, kStripBuildTrack);
            if (filled > 0) {
                rect(marks_at * kStripGaugeFillRatio, icon.left,
                     icon_bottom - (static_cast<float>(std::min(filled, kIconCells)) * cell),
                     icon.left + icon.size, icon_bottom, kStripBuildFill);
            }
            draw_runs(
                [&lit, first_filled](std::size_t row, std::size_t col) {
                    return row >= first_filled && lit(row, col);
                },
                kStripIconInk);
            draw_runs(
                [&lit, first_filled](std::size_t row, std::size_t col) {
                    return row < first_filled && lit(row, col);
                },
                kStripIconGhost);
        } else {
            draw_runs(lit, kStripIconInk);
        }
        draw_runs(
            [&lit](std::size_t row, std::size_t col) {
                if (lit(row, col)) {
                    return false;
                }
                // Unsigned wrap-around on row 0 / column 0 lands far outside the grid, which `lit`
                // reads as clear, so the border needs no special case.
                for (const std::size_t dr : {row - 1, row, row + 1}) {
                    for (const std::size_t dc : {col - 1, col, col + 1}) {
                        if (lit(dr, dc)) {
                            return true;
                        }
                    }
                }
                return false;
            },
            kStripIconOutline);
        // THE TICKS. Under the plate come the icon's reserved square, then one upright tick per
        // living vehicle, in spawn order from the key's left edge, every one
        // `kStripTickWidthPixels` by `kStripTickHeightPixels` whatever its vehicle's health --
        // COLOUR says how hurt a vehicle is, never size (Harri, BZ2: "it only changes colour, never
        // the size"). A dead vehicle is not in `fractions` at all, so the ones after it close up
        // with no blank. Each is ringed in dark grey, which is what lets the ramp's near-black read
        // against the world. Ten at most. Nothing is ever drawn over them: the notice, the attack
        // list and the command menu all start below the strip's bottom edge (`kStripBottomPixels`).
        const std::vector<BarSegment> segments = bar_segments(fractions);
        const float row_span = ((static_cast<float>(kStripTicks) * kStripTickWidthPixels)
                                + (static_cast<float>(kStripTicks - 1) * kStripTickGapPixels))
                               * px;
        const float fit = std::min(1.0F, slot_w / row_span);
        const float tick_w = kStripTickWidthPixels * px * fit;
        const float tick_pitch = (kStripTickWidthPixels + kStripTickGapPixels) * px * fit;
        const float tick_top = icon_bottom + stack_gap;
        const float tick_bottom = tick_top + (kStripTickHeightPixels * px);
        // The blank ticks follow the units built so far, inside the same ten.
        const std::size_t blanks = batch.has_value() ? batch->queued : 0;
        const std::size_t shown = std::min(segments.size() + blanks, kStripTicks);
        if (shown == 0) {
            continue;
        }
        const float outline = kStripTickOutlinePixels * px;
        for (std::size_t i = 0; i < shown; ++i) {
            const float tick_left = left + (static_cast<float>(i) * tick_pitch);
            // The tick's outline: its box grown a pixel each way, one quad BEHIND the tick, in
            // dark grey. What lets the ramp's near-black read against the world, and the one
            // thing between neighbours (it replaces the dark track that ran behind the row).
            rect(marks_at * kStripUnderInkRatio, tick_left - outline, tick_top - outline,
                 tick_left + tick_w + outline, tick_bottom + outline, kStripTickOutline);
            if (i < segments.size()) {
                rect(marks_at, tick_left, tick_top, tick_left + tick_w, tick_bottom,
                     segments.at(i).rgb);
                if (std::find(picked.begin(), picked.end(), i) != picked.end()) {
                    // The ring is four bars outside the box, meeting at the corners.
                    const float ring = kStripPickedTickEdgePixels * px;
                    const float outer_left = tick_left - ring;
                    const float outer_right = tick_left + tick_w + ring;
                    rect(marks_at, outer_left, tick_top - ring, outer_right, tick_top,
                         kStripSelected);
                    rect(marks_at, outer_left, tick_bottom, outer_right, tick_bottom + ring,
                         kStripSelected);
                    rect(marks_at, outer_left, tick_top, tick_left, tick_bottom, kStripSelected);
                    rect(marks_at, tick_left + tick_w, tick_top, outer_right, tick_bottom,
                         kStripSelected);
                }
                continue;
            }
            // A blank tick: the tick's box as a frame, four bars that meet at the corners, the dark
            // track showing through the middle.
            const float edge = kStripBlankTickEdgePixels * px;
            const float tick_right = tick_left + tick_w;
            rect(marks_at, tick_left, tick_top, tick_right, tick_top + edge, kStripBlankTickEdge);
            rect(marks_at, tick_left, tick_bottom - edge, tick_right, tick_bottom,
                 kStripBlankTickEdge);
            rect(marks_at, tick_left, tick_top + edge, tick_left + edge, tick_bottom - edge,
                 kStripBlankTickEdge);
            rect(marks_at, tick_right - edge, tick_top + edge, tick_right, tick_bottom - edge,
                 kStripBlankTickEdge);
        }
    }
    if (!options.notice.empty()) {
        const float cell = kStripNoticeCell * px;
        const float notice_left = kStripMarginPixels * px + pad;
        plate(kStripMarginPixels * px, notice_top - pad,
              notice_left + word_width(options.notice, cell) + pad,
              notice_top + (5.0F * cell) + pad, kStripSelected, PlateStyle::Glass);
        ink_text(options.notice, notice_left, notice_top, cell, kStripSelected);
    }
    // THE COMMAND MENU, in the order it was handed: never sorted, never renumbered, and the key is
    // drawn as the label it is ("SPACE", "~", "9"), so the harvester's missing 6, 7 and 8 stay a
    // visible gap. `menu_layout` places every bar; this draws them, then the glyphs, then the line.
    if (wants_menu) {
        const std::vector<MenuRow>& rows = *options.menu;
        const float cell = menu_cell;
        const MenuLayout& layout = menu_placed;
        const float face_at = marks_at * kHudFaceRatio;
        const float ring_at = marks_at * kHudBorderRatio;
        const float outline_at = marks_at * kMenuOutlineRatio;
        const auto at = [&](float distance, float x, float y) {
            const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
            return Vec3{point.x, point.y, point.z};
        };
        // One bar: a half-disc on its OUTER end and square on its inner one, where the gutter is.
        // The face is horizontal bands, one quad each, whose outer corners sit on the half-disc, so
        // the gradient and the rounding are the same quads. A live bar's face stops a ring's width
        // short of the rim and the ring fills the rest, so the two never blend over each other.
        const auto bar = [&](float left, float right, const MenuRowLayout& row, bool round_left) {
            const float top = row.top;
            const float bottom = row.top + row.height;
            const float radius = row.height * 0.5F;
            const float ring = row.enabled ? kMenuRingCells * cell : 0.0F;
            const float face_radius = radius - ring;
            const float cx = round_left ? left + radius : right - radius;
            const float cy = (top + bottom) * 0.5F;
            const float outward = round_left ? -1.0F : 1.0F;
            const float inner = round_left ? right : left;
            const auto rim_x = [&](float y) {
                const float dy = y - cy;
                return cx
                       + (outward
                          * std::sqrt(std::max(0.0F, (face_radius * face_radius) - (dy * dy))));
            };
            const float alpha = row.enabled ? kMenuFaceAlpha : kMenuUnlitAlpha;
            for (int band = 0; band < kMenuFaceBands; ++band) {
                const float y0 = top
                                 + (row.height * static_cast<float>(band)
                                    / static_cast<float>(kMenuFaceBands));
                const float y1 = top
                                 + (row.height * static_cast<float>(band + 1)
                                    / static_cast<float>(kMenuFaceBands));
                emit_quad(out, at(face_at, rim_x(y0), y0), at(face_at, inner, y0),
                          at(face_at, inner, y1), at(face_at, rim_x(y1), y1),
                          menu_face_colour(row.enabled, band), alpha);
            }
            if (!row.enabled) {
                return; // an unlit bar is flat: no ring
            }
            // The ring round the outer end only: a half annulus, bottom to top on the left end and
            // top to bottom on the right, as short arcs like the plates' corners.
            const float from = round_left ? kPi * 0.5F : -kPi * 0.5F;
            constexpr int kSlices = 2 * kCornerSlices;
            for (int slice = 0; slice < kSlices; ++slice) {
                const float a
                    = from + (kPi * static_cast<float>(slice) / static_cast<float>(kSlices));
                const float b
                    = from + (kPi * static_cast<float>(slice + 1) / static_cast<float>(kSlices));
                emit_quad(
                    out, at(ring_at, cx + (radius * std::cos(a)), cy + (radius * std::sin(a))),
                    at(ring_at, cx + (radius * std::cos(b)), cy + (radius * std::sin(b))),
                    at(ring_at, cx + (face_radius * std::cos(b)), cy + (face_radius * std::sin(b))),
                    at(ring_at, cx + (face_radius * std::cos(a)), cy + (face_radius * std::sin(a))),
                    kMenuRing, kMenuRingAlpha);
            }
        };
        // THE TYPE, in the smaller text cell on whole frame pixels: a capital is `cap` px square, a
        // lowercase letter its capital at `menu_glyph_grid`'s size on the same baseline, and glyphs
        // are a text cell apart. Each glyph is its coverage, one quad per covered pixel at that
        // coverage as alpha, so an edge pixel blends with the face and nothing is drawn where
        // nothing is covered. The outline goes first: the same coverage grown by `grow` px, each
        // pixel black at the most coverage within that reach.
        const float type = kMenuTypeCell * px;
        const int cap = std::max(1, static_cast<int>(std::lround(5.0F * type)));
        const int gap = std::max(1, static_cast<int>(std::lround(type)));
        const int grow = std::max(1, static_cast<int>(std::lround(kMenuOutlineCells * cell)));
        const auto run_width = [&](std::string_view word) {
            int width = 0;
            for (const char glyph : word) {
                width += menu_glyph_grid(glyph, cap).cols + gap;
            }
            return static_cast<float>(word.empty() ? 0 : width - gap);
        };
        // `word` from `left`, its capitals' tops on `top`, both already whole pixels.
        const auto smooth = [&](std::string_view word, float left, float top,
                                const std::array<float, 3>& ink) {
            constexpr std::array<float, 3> kOutline{0.0F, 0.0F, 0.0F};
            for (const bool inking : {false, true}) {
                float x = left;
                for (const char glyph : word) {
                    const MenuGlyphGrid grid = menu_glyph_grid(glyph, cap);
                    const std::vector<float> coverage = glyph_coverage(glyph, grid.rows, grid.cols);
                    const auto covered = [&](int i, int j) {
                        if (i < 0 || j < 0 || i >= grid.rows || j >= grid.cols) {
                            return 0.0F;
                        }
                        return coverage.at(
                            (static_cast<std::size_t>(i) * static_cast<std::size_t>(grid.cols))
                            + static_cast<std::size_t>(j));
                    };
                    const float y = top + static_cast<float>(cap - grid.rows);
                    const int reach = coverage.empty() || inking ? 0 : grow;
                    for (int i = -reach; !coverage.empty() && i < grid.rows + reach; ++i) {
                        for (int j = -reach; j < grid.cols + reach; ++j) {
                            float alpha = covered(i, j);
                            for (int di = -reach; di <= reach; ++di) {
                                for (int dj = -reach; dj <= reach; ++dj) {
                                    alpha = std::max(alpha, covered(i + di, j + dj));
                                }
                            }
                            if (alpha <= 0.0F) {
                                continue;
                            }
                            const float cx = x + static_cast<float>(j);
                            const float cy = y + static_cast<float>(i);
                            rect(inking ? marks_at : outline_at, cx, cy, cx + 1.0F, cy + 1.0F,
                                 inking ? ink : kOutline, alpha);
                        }
                    }
                    x += static_cast<float>(grid.cols + gap);
                }
            }
        };
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const MenuRow& row = rows.at(index);
            const MenuRowLayout& placed = layout.rows.at(index);
            bar(placed.key_left, placed.key_right, placed, true);
            if (!row.verb.empty()) {
                bar(placed.verb_left, placed.verb_right, placed, false);
            }
            // THE TWO LINES, for a row that names a unit: health in the ramp's colour, ammunition
            // in blue. The sim has no magazine (`kAmmoPlaceholderFraction`, Harri 2026-10-23:
            // always full), so the blue line is the placeholder and says so by being full.
            const sim::Unit* named
                = row.unit.valid() && !row.verb.empty() ? sim.find(row.unit) : nullptr;
            if (named != nullptr && named->alive) {
                const std::uint32_t max_hp = sim::spec_of(named->kind).hp;
                const std::uint32_t hp = std::min<std::uint32_t>(named->hp, max_hp);
                const float health
                    = max_hp > 0 ? static_cast<float>(hp) / static_cast<float>(max_hp) : 0.0F;
                const float from = placed.verb_text;
                const float span = placed.verb_right - (kMenuVerbPadCells * cell) - from;
                const auto line = [&](float cells, float fraction,
                                      const std::array<float, 3>& ink) {
                    const float y = placed.top + (cells * cell);
                    const float thick = kMenuBarLineCells * cell;
                    rect(ring_at, from, y, from + span, y + thick, kMenuTrackInk, kMenuTrackAlpha);
                    rect(marks_at, from, y, from + (span * fraction), y + thick, ink);
                };
                line(kMenuHealthLineCells, health, health_colour(hp, max_hp));
                line(kMenuAmmoLineCells, kAmmoPlaceholderFraction, kMenuAmmoInk);
            }
            const std::array<float, 3>& ink = row.enabled ? kStripFrame : kMenuDimInk;
            // The capitals centred down the layout's text slot; the key still ends on its pad and
            // the verb and the right column still start on theirs, so the room the smaller type
            // leaves is on each bar's rounded end.
            const float top
                = std::round(placed.text_top + (((5.0F * cell) - static_cast<float>(cap)) * 0.5F));
            const float key_end = placed.key_right - (kMenuKeyPadCells * cell);
            smooth(row.key, std::round(key_end - run_width(row.key)), top, ink);
            smooth(row.verb, std::round(placed.verb_text), top, ink);
            smooth(row.right, std::round(placed.right_left), top, ink);
        }
        // THE LEADER LINE, in the ledger's amber, after every bar so it is never drawn under one:
        // down the gutter at one x through every row's band, as BZ2 draws it, then on to the panel
        // in the corner (MENU-LEG): along its top edge and down onto each selected unit. The panel
        // is the radar's, or the world map's when R has put the map in its place, and the blips
        // are that surface's OWN marks -- the radar's from the call `append_radar` makes, the
        // map's from the call `append_world_map` makes -- so a dot is where the surface drew the
        // unit and nowhere else. One leader per selected unit; a unit the surface does not show (a
        // friend beyond the radar's range) has none.
        std::optional<float> radar_top;
        std::vector<std::array<float, 2>> blips;
        blips.reserve(selected.size());
        if (options.show_world_map) {
            const WorldMap map
                = world_map(sim, options.extent, options.viewer_side, options.viewer_unit, nullptr);
            radar_top = map.layout.y;
            for (const sim::UnitId id : selected) {
                const auto asset = std::find_if(map.assets.begin(), map.assets.end(),
                                                [id](const MapAsset& a) { return a.id == id; });
                if (asset != map.assets.end()) {
                    blips.push_back({asset->px, asset->py});
                }
            }
        } else {
            const std::vector<RadarMark> marks = frame_radar_marks(options, sim);
            if (!marks.empty()) {
                const RadarPanel panel = radar_panel(options.extent);
                radar_top = panel.y;
                for (const sim::UnitId id : selected) {
                    const auto mark
                        = std::find_if(marks.begin(), marks.end(),
                                       [id](const RadarMark& m) { return m.unit == id; });
                    if (mark != marks.end()) {
                        blips.push_back({panel.x + mark->px, panel.y + mark->py});
                    }
                }
            }
        }
        // One quad per segment: the box round it, grown half the line's width each way so that
        // every elbow is filled square. The path starts and ends on vertical segments, so the
        // first's top and the last's bottom are the path's own ends and are NOT grown: the line
        // starts under the state row and stops on the panel's edge or the blip.
        const float half = (layout.line_right - layout.line_left) * 0.5F;
        const float leader_at = marks_at * kMenuLeaderRatio;
        for (const MenuLeader& leader : menu_leaders(layout, radar_top, blips)) {
            for (std::size_t i = 1; i < leader.points.size(); ++i) {
                const std::array<float, 2>& a = leader.points.at(i - 1);
                const std::array<float, 2>& b = leader.points.at(i);
                const float upper = i == 1 ? a.at(1) : std::min(a.at(1), b.at(1)) - half;
                const float lower
                    = i + 1 == leader.points.size() ? b.at(1) : std::max(a.at(1), b.at(1)) + half;
                rect(leader_at, std::min(a.at(0), b.at(0)) - half, upper,
                     std::max(a.at(0), b.at(0)) + half, lower, kLedgerInk);
            }
            // The dot: filled, a fan round the blip's centre, on the line's own layer.
            if (leader.dot) {
                const float cx = leader.points.back().at(0);
                const float cy = leader.points.back().at(1);
                const float radius = kMenuDotPixels * px;
                const auto rim = [&](int slice) {
                    const float angle = (2.0F * kPi * static_cast<float>(slice))
                                        / static_cast<float>(kMenuDotSlices);
                    return at(leader_at, cx + (radius * std::cos(angle)),
                              cy + (radius * std::sin(angle)));
                };
                for (int slice = 0; slice < kMenuDotSlices; ++slice) {
                    emit_triangle(out, at(leader_at, cx, cy), rim(slice), rim(slice + 1),
                                  kLedgerInk);
                }
            }
        }

        // One slanted quad `width` px wide from `start` to `end` on the leader's layer: the shape
        // both offers' lines are.
        const auto line_between = [&](const std::array<float, 2>& start, const Projected& end,
                                      float width, const std::array<float, 3>& ink) {
            const float dx = end.x - start.at(0);
            const float dy = end.y - start.at(1);
            const float length = std::sqrt((dx * dx) + (dy * dy));
            if (end.depth <= math::kNearPlane || length <= 0.0F) {
                return;
            }
            const float nx = -dy / length * width * 0.5F;
            const float ny = dx / length * width * 0.5F;
            emit_quad(out, at(leader_at, start.at(0) + nx, start.at(1) + ny),
                      at(leader_at, end.x + nx, end.y + ny), at(leader_at, end.x - nx, end.y - ny),
                      at(leader_at, start.at(0) - nx, start.at(1) - ny), ink);
        };
        // The menu's leader dot, a filled disc of `kMenuDotSlices` triangles on `middle`, on the
        // leader's layer: what ends the repair line and every repair-list line.
        const auto dot_on = [&](const Projected& middle, const std::array<float, 3>& ink) {
            if (middle.depth <= math::kNearPlane) {
                return;
            }
            const float radius = kMenuDotPixels * px;
            const auto rim = [&](int slice) {
                const float angle
                    = (2.0F * kPi * static_cast<float>(slice)) / static_cast<float>(kMenuDotSlices);
                return at(leader_at, middle.x + (radius * std::cos(angle)),
                          middle.y + (radius * std::sin(angle)));
            };
            for (int slice = 0; slice < kMenuDotSlices; ++slice) {
                emit_triangle(out, at(leader_at, middle.x, middle.y), rim(slice), rim(slice + 1),
                              ink);
            }
        };
        // THE ATTACK LINE: only while the state row really reads `SPACE Attack`, so a line can
        // never point from a menu that is not offering the attack. From the row's own right edge
        // to the exact screen position of the unit, one slanted quad the leader's width, on the
        // leader's layer.
        const MenuRow& state = rows.front();
        if (options.attack_target.valid() && state.key == kRowKeySpace
            && state.verb == kVerbAttack) {
            const sim::Unit* target = sim.find(options.attack_target);
            if (target != nullptr && target->alive) {
                line_between(attack_line_origin(layout), attack_point(options, *target),
                             kMenuLineCells * cell, kAttackLineInk);
            }
        }
        // THE REPAIR LINE, the attack line's mirror: only while the state row really reads `SPACE
        // Repair`. The same origin, thickness and layer, in the green of what is yours, ending in
        // a dot on the patient -- the menu's own leader dot (`kMenuDotPixels`, `kMenuDotSlices`)
        // in the line's ink, on the line's layer.
        if (options.repair_target.valid() && state.key == kRowKeySpace
            && state.verb == kVerbRepair) {
            const sim::Unit* patient = sim.find(options.repair_target);
            if (patient != nullptr && patient->alive) {
                const Projected middle = attack_point(options, *patient);
                line_between(attack_line_origin(layout), middle, kMenuLineCells * cell,
                             kRepairLineInk);
                dot_on(middle, kRepairLineInk);
            }
        }
        // THE REPAIR LIST'S LEADERS: every list row that names an asset (`MenuRow::unit`) draws the
        // same green line and dot to that asset, from the right edge of its own row. Green, because
        // a line says whose the thing is and these are yours.
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const sim::Unit* named = rows.at(index).unit.valid() && rows.at(index).leads
                                         ? sim.find(rows.at(index).unit)
                                         : nullptr;
            if (named == nullptr || !named->alive) {
                continue;
            }
            const Projected middle = attack_point(options, *named);
            line_between(row_line_origin(layout, index, rows.at(index).right.size(), cell), middle,
                         kMenuLineCells * cell, kRepairLineInk);
            dot_on(middle, kRepairLineInk);
        }
        // THE WHITE GROUND LINES: only while the state row really reads the verb that owns the
        // line and the app named a tile, so each appears and disappears with its prompt. From the
        // same place the attack line leaves, to the middle of the tile on the ground, one pixel
        // wide and white. Three consumers of the one line: `SPACE Deploy` to the pool it would go
        // to, `SPACE Go Here` to the spot it would send the selection, and `SPACE GET NAV` to the
        // tile the beacon would be dropped on.
        const auto ground_line
            = [&](const std::optional<DeployLineTarget>& target, std::string_view verb) {
                  if (!target.has_value() || state.key != kRowKeySpace || state.verb != verb) {
                      return;
                  }
                  const float tile_x = static_cast<float>(target->tile_x) + 0.5F;
                  const float tile_y = static_cast<float>(target->tile_y) + 0.5F;
                  line_between(attack_line_origin(layout),
                               project(options.camera, options.extent, tile_x * kMetresPerTile,
                                       ground_metres(options.seed, tile_x, tile_y),
                                       tile_y * kMetresPerTile),
                               kDeployLinePixels * px, kDeployLineInk);
              };
        ground_line(options.deploy_pool, kVerbDeploy);
        ground_line(options.go_here_spot, kVerbGoHere);
        ground_line(options.nav_spot, kVerbGetNav);
    }
}

/// THE ATTACK READOUT (SPACE ATTACK): the label and the health bar over the unit SPACE would
/// attack. Both stand centred over the unit's projected top, ABOVE the slot its own floating bar
/// (`append_unit_bars`) takes, so the two never overlap: the label's plate, then under it the bar.
/// The label is the unit's kind in the HUD's capitals (`kind_label`, the command row's own names)
/// on a black plate. The bar is a black track with a fill on its LEFT: full width
/// at full health, shortened from the right as the unit is hurt (the empty part is the black
/// track), always `kEnemyBarRed`. The row's bars vary in colour ONLY (his ruling, BAR-LOOK) and
/// stay on `bar_segments`' ramp; this one shrinks as well, by his later word, and never takes the
/// ramp. A unit at zero hit points draws neither. Sizes are fractions of the frame, so they do not
/// change with range.
constexpr float kReadoutCellFraction = 2.0F / 450.0F;
constexpr float kReadoutPadFraction = 2.0F / 450.0F;
constexpr float kReadoutGapFraction = 2.0F / 450.0F;
constexpr float kReadoutBarWidthFraction = 48.0F / 800.0F;
constexpr float kReadoutBarHeightFraction = 5.0F / 450.0F;
constexpr std::array<float, 3> kReadoutBlack{0.0F, 0.0F, 0.0F};

void append_attack_readout(std::vector<SceneVertex>& out, const SceneOptions& options,
                           const sim::Sim& sim) {
    if (!options.attack_target.valid()) {
        return;
    }
    const sim::Unit* unit = sim.find(options.attack_target);
    if (unit == nullptr || !unit->alive || unit->hp == 0 || hidden_by_reach(sim, options, *unit)) {
        return;
    }
    const float frame_w = static_cast<float>(options.extent.width);
    const float frame_h = static_cast<float>(options.extent.height);
    const Projected top = project(
        options.camera, options.extent,
        static_cast<float>(unit->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile,
        ground_metres(options.seed, unit->x, unit->y) + kTankTopMetres,
        static_cast<float>(unit->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile);
    if (top.depth <= math::kNearPlane || top.x < 0.0F || top.x > frame_w || top.y < 0.0F
        || top.y > frame_h) {
        return;
    }
    const float cell = frame_h * kReadoutCellFraction;
    const float pad = frame_h * kReadoutPadFraction;
    const float gap = frame_h * kReadoutGapFraction;
    const float bar_w = frame_w * kReadoutBarWidthFraction;
    const float bar_h = frame_h * kReadoutBarHeightFraction;
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto at = [&](float px, float py) {
            const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };

    // The floating bar's slot, left clear: its gap, outline, track and outline.
    const float own_bar
        = frame_h
          * (kUnitBarGapFraction + (2.0F * kUnitBarOutlineFraction) + kUnitBarHeightFraction);
    // ... and the deploy bar's, stacked on it while the unit is counting a deploy.
    const float deploy_bar
        = unit->deploy_ticks == 0
              ? 0.0F
              : frame_h * ((3.0F * kUnitBarOutlineFraction) + kUnitBarHeightFraction);
    const float bar_bottom = top.y - own_bar - deploy_bar - gap;
    const float bar_top = bar_bottom - bar_h;
    const float bar_left = top.x - (bar_w * 0.5F);

    const std::string_view label = kind_label(unit->kind, faction_of(options.factions, unit->side));
    const float text_w = ((static_cast<float>(label.size()) * 6.0F) - 1.0F) * cell;
    const float plate_bottom = bar_top - gap;
    const float plate_top = plate_bottom - (5.0F * cell) - (2.0F * pad);
    const float text_left = top.x - (text_w * 0.5F);
    rect(marks_at * kHudFaceRatio, text_left - pad, plate_top, text_left + text_w + pad,
         plate_bottom, kReadoutBlack);
    for (std::size_t index = 0; index < label.size(); ++index) {
        const std::array<std::uint8_t, 5> rows = mount_glyph(label.at(index));
        const float glyph_left = text_left + (static_cast<float>(index) * 6.0F * cell);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (int col = 0; col < 5; ++col) {
                if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                    continue;
                }
                const float x = glyph_left + (static_cast<float>(col) * cell);
                const float y = plate_top + pad + (static_cast<float>(row) * cell);
                rect(marks_at, x, y, x + cell, y + cell, kStripFrame);
            }
        }
    }

    const std::uint32_t max_hp = sim::spec_of(unit->kind).hp;
    const std::uint32_t hp = std::min<std::uint32_t>(unit->hp, max_hp);
    const std::array<float, 1> fraction{
        max_hp > 0 ? static_cast<float>(hp) / static_cast<float>(max_hp) : 0.0F};
    rect(marks_at * kHudFaceRatio, bar_left, bar_top, bar_left + bar_w, bar_bottom, kReadoutBlack);
    rect(marks_at, bar_left, bar_top, bar_left + (bar_w * fraction.at(0)), bar_bottom,
         kEnemyBarRed);
}

/// THE ASSIST (`T`, game/assist.hpp): a thin red line from the latched asset's middle to a small
/// red dot at the point the barrel must be aimed at for the weapon that is up to hit it. Fractions
/// of the frame, like every HUD mark, so the same size at any range: a line 1.5 px thick and a dot
/// of 3 px radius at 450 px of frame height. Two layers BEHIND the reticle
/// (`kReticleDistanceMetres`) and in front of the world, the dot nearer than the line, so the cross
/// is drawn over the dot it is laid on and the dot over the line it ends. A HUD distance moves
/// depth only, never the pixel. Nothing is drawn for a target that is dead, hidden, behind the eye,
/// or with no solution
/// (`assist_shot`: out of reach, or never settling).
constexpr float kAssistLineHalfFraction = 0.75F / 450.0F;
constexpr float kAssistDotRadiusFraction = 3.0F / 450.0F;
constexpr float kAssistDotRatio = 1.05F;  ///< of the reticle's distance: just behind it
constexpr float kAssistLineRatio = 1.08F; ///< and the line behind the dot

void append_assist(std::vector<SceneVertex>& out, const SceneOptions& options,
                   const sim::Sim& sim) {
    if (!options.assist_target.valid()) {
        return;
    }
    const sim::Unit* target = sim.find(options.assist_target);
    const sim::Unit* shooter = sim.find(options.driven_unit);
    if (target == nullptr || !target->alive || shooter == nullptr || !shooter->alive
        || !available_to_viewer(sim, options, *target)) {
        return;
    }
    const std::optional<AssistShot> shot = assist_shot(options, options.weapon, *shooter, *target);
    if (!shot.has_value()) {
        return;
    }
    const Projected start = attack_point(options, *target);
    const Projected end = project(options.camera, options.extent, shot->dot_metres.at(0),
                                  shot->dot_metres.at(1), shot->dot_metres.at(2));
    if (start.depth <= math::kNearPlane || end.depth <= math::kNearPlane) {
        return;
    }
    const float frame_h = static_cast<float>(options.extent.height);
    const auto at = [&](float distance, float px, float py) {
        const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
        return Vec3{point.x, point.y, point.z};
    };
    const float line_at = kReticleDistanceMetres * kAssistLineRatio;
    const float dot_at = kReticleDistanceMetres * kAssistDotRatio;
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float length = std::sqrt((dx * dx) + (dy * dy));
    if (length > 0.0F) {
        const float half = frame_h * kAssistLineHalfFraction;
        const float nx = -dy / length * half;
        const float ny = dx / length * half;
        emit_quad(out, at(line_at, start.x + nx, start.y + ny), at(line_at, end.x + nx, end.y + ny),
                  at(line_at, end.x - nx, end.y - ny), at(line_at, start.x - nx, start.y - ny),
                  kAssistInk);
    }
    const float radius = frame_h * kAssistDotRadiusFraction;
    const auto rim = [&](int slice) {
        const float angle
            = (2.0F * math::kPi * static_cast<float>(slice)) / static_cast<float>(kAssistDotSlices);
        return at(dot_at, end.x + (radius * std::cos(angle)), end.y + (radius * std::sin(angle)));
    };
    for (int slice = 0; slice < kAssistDotSlices; ++slice) {
        emit_triangle(out, at(dot_at, end.x, end.y), rim(slice), rim(slice + 1), kAssistInk);
    }
}

/// How many quads (six vertices each) a reticle shape is made of, the centre dot included.
constexpr std::size_t reticle_quads(ReticleKind kind) noexcept {
    switch (kind) {
    case ReticleKind::FourTicks:
    case ReticleKind::Diagonals:
    case ReticleKind::Scatter:
        return 5;
    case ReticleKind::Brackets:
        return 9;
    case ReticleKind::Ring:
        return static_cast<std::size_t>(kRingSegments) + 1U;
    }
    return 5;
}

/// The weapon's crosshair: thin lines round an OPEN centre and a small dot in it, in the plane the
/// camera looks along, centred on the point the click's own ray reaches, in the shape
/// `reticle_for(options.weapon)` names (see the constants above). Six vertices per quad: two
/// triangles, and no winding to get wrong, because the pipeline culls nothing. The shapes reach the
/// old box's half-size at most; a pulse pushes them outward and swells the dot, and 0 is the
/// reticle at rest. The dot is the LAST quad of every shape.
void append_reticle(std::vector<SceneVertex>& out, const SceneOptions& options, const sim::Sim& sim,
                    const Pick& picked) {
    if (!options.aim.has_value()) {
        return;
    }
    const Camera& camera = options.camera;
    const float screen_x = options.aim->x;
    const float screen_y = options.aim->y;
    std::array<float, 3> colour = reticle_colour(sim, picked, options.viewer_side);
    if (options.reloading) {
        for (float& channel : colour) {
            channel *= kReticleSpentShade; // a ghost of itself until the gun is ready again
        }
    }
    const ReticleShape shape = reticle_for(options.weapon);
    const RayPoint centre
        = ray_point(camera, options.extent, screen_x, screen_y, kReticleDistanceMetres);
    // THE FRAME IS THE HULL'S, exactly as `ray_through` builds it (`pick.cpp`), so the lines and
    // the dot are drawn in the VIEW's axes and a leaning camera shows them square on the screen --
    // which is what the player reads them as. Omitting `camera.up` here left the reticle as the one
    // mark on this layer still plumb with the WORLD, so it visibly rotated against a tilted horizon
    // while the ground, the units and the HUD beside it did not.
    const math::Basis basis = math::basis_from(camera.yaw, camera.pitch, camera.up);
    const float half
        = kReticleDistanceMetres * std::tan(math::kFovRadians * 0.5F) * kReticleHalfFraction;
    const float pulse = std::clamp(options.reticle_pulse, 0.0F, 1.0F);
    const float thick = half * kReticleBarFraction * 0.5F; // half the line's width
    const float spread = half * kReticlePulseSpread * pulse;
    const float dot = half * kReticleDotFraction * (1.0F + (kReticlePulseDotGrowth * pulse));
    const auto corner = [&](float right, float up) {
        return std::array<float, 3>{centre.x + (right * basis.right.at(0)) + (up * basis.up.at(0)),
                                    centre.y + (right * basis.right.at(1)) + (up * basis.up.at(1)),
                                    centre.z + (right * basis.right.at(2)) + (up * basis.up.at(2))};
    };
    using Local = std::array<float, 2>; // (right, up) metres from the centre
    const auto quad = [&](const Local& p0, const Local& p1, const Local& p2, const Local& p3) {
        const std::array<float, 3> a = corner(p0.at(0), p0.at(1));
        const std::array<float, 3> b = corner(p1.at(0), p1.at(1));
        const std::array<float, 3> c = corner(p2.at(0), p2.at(1));
        const std::array<float, 3> d = corner(p3.at(0), p3.at(1));
        out.push_back(SceneVertex{a, colour});
        out.push_back(SceneVertex{b, colour});
        out.push_back(SceneVertex{c, colour});
        out.push_back(SceneVertex{a, colour});
        out.push_back(SceneVertex{c, colour});
        out.push_back(SceneVertex{d, colour});
    };
    const auto bar_quad = [&](float r0, float u0, float r1, float u1) {
        quad({r0, u0}, {r1, u0}, {r1, u1}, {r0, u1});
    };
    // A line from `from` to `to` (both on the line's axis), `thick` either side of it.
    const auto stroke = [&](const Local& from, const Local& to) {
        const float dx = to.at(0) - from.at(0);
        const float dy = to.at(1) - from.at(1);
        const float length = std::hypot(dx, dy);
        const float nx = -dy / length * thick;
        const float ny = dx / length * thick;
        quad({from.at(0) + nx, from.at(1) + ny}, {to.at(0) + nx, to.at(1) + ny},
             {to.at(0) - nx, to.at(1) - ny}, {from.at(0) - nx, from.at(1) - ny});
    };
    switch (shape.kind) {
    case ReticleKind::FourTicks: {
        const float inner = (half * kReticleGapFraction) + spread;
        const float outer = half + spread;
        bar_quad(-thick, inner, thick, outer);   // up
        bar_quad(-thick, -outer, thick, -inner); // down
        bar_quad(-outer, -thick, -inner, thick); // left
        bar_quad(inner, -thick, outer, thick);   // right
        break;
    }
    case ReticleKind::Brackets: {
        const float reach = (half * kBracketReachFraction) + spread;
        const float arm = half * kBracketArmFraction;
        for (const float sx : {-1.0F, 1.0F}) {
            for (const float sy : {-1.0F, 1.0F}) {
                // One corner: its horizontal arm along the top or bottom edge, its vertical one
                // down the side, meeting at (sx, sy) * reach.
                const float hx = reach - arm;
                bar_quad(std::min(sx * hx, sx * reach), (sy * reach) - thick,
                         std::max(sx * hx, sx * reach), (sy * reach) + thick);
                bar_quad((sx * reach) - thick, std::min(sy * hx, sy * reach), (sx * reach) + thick,
                         std::max(sy * hx, sy * reach));
            }
        }
        break;
    }
    case ReticleKind::Ring: {
        const float radius = (half * kRingRadiusFraction) + spread;
        const auto at = [&](int step, float r) {
            const float angle
                = (2.0F * math::kPi * static_cast<float>(step)) / static_cast<float>(kRingSegments);
            return Local{r * std::cos(angle), r * std::sin(angle)};
        };
        for (int step = 0; step < kRingSegments; ++step) {
            quad(at(step, radius - thick), at(step, radius + thick), at(step + 1, radius + thick),
                 at(step + 1, radius - thick));
        }
        break;
    }
    case ReticleKind::Diagonals: {
        const float from = (half * kDiagonalFromFraction) + spread;
        const float to = (half * kDiagonalToFraction) + spread;
        const float side = std::sqrt(0.5F);
        for (const float sx : {-1.0F, 1.0F}) {
            for (const float sy : {-1.0F, 1.0F}) {
                stroke({sx * side * from, sy * side * from}, {sx * side * to, sy * side * to});
            }
        }
        break;
    }
    case ReticleKind::Scatter: {
        const float at = (half * kScatterFraction) + spread;
        const float size = half * kScatterDotFraction;
        for (const float sx : {-1.0F, 1.0F}) {
            for (const float sy : {-1.0F, 1.0F}) {
                bar_quad((sx * at) - size, (sy * at) - size, (sx * at) + size, (sy * at) + size);
            }
        }
        break;
    }
    }
    bar_quad(-dot, -dot, dot, dot); // the centre dot
}

/// THE WORLD MAP (N4), on the radar's HUD layers and through the same pixel-to-world function: the
/// ground first (one quad per run of tiles of the same terrain along a row), then YOUR assets as
/// dark-edged squares, the driven one a white blip with a heading needle, then the nav marks as
/// the radar draws them (`append_nav_glyph`). Everything comes from `world_map`, which has no
/// enemy in it.
constexpr float kMapTerrainRatio = 2.4F;
constexpr float kMapFrameRatio = 2.8F;
constexpr float kMapRowStep = 0.0005F; ///< each row a hair further than the one below it
constexpr std::size_t kWorldMapSide = static_cast<std::size_t>(kWorldMapTiles);
constexpr float kMapEdgePixels = 3.0F;
/// A pool's marker sits BEHIND a unit's blip (whose edge is at 1.02), the black edge a hair behind
/// the white ring, because two layers at one distance lose the depth test to each other.
constexpr float kMapPoolRingRatio = 1.04F;
constexpr float kMapPoolEdgeRatio = 1.05F;
/// An enemy blip is NEARER than your assets (fill 1.0, edge 1.02), edge behind fill.
constexpr float kMapBlipRatio = 0.99F;
constexpr float kMapBlipEdgeRatio = 1.01F;
/// The target diamond is NEARER than the blips (0.99): it rings one, and equal depth would lose.
constexpr float kMapTargetRatio = 0.97F;

void append_world_map(std::vector<SceneVertex>& out, const SceneOptions& options,
                      const sim::Sim& sim) {
    if (!options.show_world_map) {
        return;
    }
    const WorldMap map = world_map(sim, options.extent, options.viewer_side, options.viewer_unit,
                                   options.nav_marks, options.radar_activity, options.readout_lock);
    const WorldMapLayout& layout = map.layout;
    const float scale = map_furniture_scale(layout);
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float ratio, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb, float alpha = 1.0F) {
        const auto at = [&](float x, float y) {
            const RayPoint point
                = ray_point(options.camera, options.extent, x, y, marks_at * ratio);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb, alpha);
    };
    constexpr std::array<float, 3> kInk{0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 3> kEdge{0.80F, 0.82F, 0.85F};
    constexpr std::array<float, 3> kOwn{0.30F, 0.85F, 0.35F};
    constexpr std::array<float, 3> kDriven{1.0F, 1.0F, 1.0F};
    constexpr std::array<float, 3> kPoolRing{1.0F, 1.0F,
                                             1.0F}; // the radar's white: a pool is white

    const float edge = kMapEdgePixels * scale;
    // The frame is four strips, never one square behind the ground: a big quad's depth is
    // interpolated across its corners and would beat the ground's own quads in the middle.
    const float frame_left = layout.x - edge;
    const float frame_top = layout.y - edge;
    const float right = layout.x + layout.size + edge;
    const float bottom = layout.y + layout.size + edge;
    rect(kMapFrameRatio, frame_left, frame_top, right, layout.y, kEdge);
    rect(kMapFrameRatio, frame_left, layout.y + layout.size, right, bottom, kEdge);
    rect(kMapFrameRatio, frame_left, layout.y, layout.x, layout.y + layout.size, kEdge);
    rect(kMapFrameRatio, layout.x + layout.size, layout.y, right, layout.y + layout.size, kEdge);

    const std::vector<world::Terrain>& grid = map_terrain_grid(options.seed);
    const auto terrain_at = [&](int x, int y) {
        return grid.at((static_cast<std::size_t>(y) * kWorldMapSide) + static_cast<std::size_t>(x));
    };
    for (int y = 0; y < kWorldMapTiles; ++y) {
        const float top = map_point(layout, 0.0F, static_cast<float>(y + 1)).y;
        const float bottom = map_point(layout, 0.0F, static_cast<float>(y)).y;
        int x = 0;
        while (x < kWorldMapTiles) {
            const world::Terrain terrain = terrain_at(x, y);
            int end = x + 1;
            while (end < kWorldMapTiles && terrain_at(end, y) == terrain) {
                ++end;
            }
            const MapColour colour = map_terrain_colour(terrain);
            // Each row reaches half a pixel into the one below, which is a little nearer: runs
            // end at different tiles from row to row, and without the overlap those T-junctions
            // leave hairline cracks that show the world behind.
            rect(kMapTerrainRatio + (kMapRowStep * static_cast<float>(y)),
                 map_point(layout, static_cast<float>(x), 0.0F).x, top,
                 map_point(layout, static_cast<float>(end), 0.0F).x, bottom + (0.6F * scale),
                 {colour.r, colour.g, colour.b});
            x = end;
        }
    }

    // The biometal pools, ALL of them (`world_map`): a thin white RING in a black edge, a bit
    // bigger than a vehicle's dot (`kMapPoolRingRadius`), drawn under the assets (a farther layer
    // than their edge) so a scavenger deployed on one still shows inside it.
    for (const MapPool& pool : map.pools) {
        const float bar = kMapPoolRingBar * scale;
        const auto ring
            = [&](float ratio, float outer, float inner, const std::array<float, 3>& rgb) {
                  for (const RingSegment& seg :
                       ring_segments(pool.px, pool.py, outer, inner, kMapPoolRingSegments)) {
                      const auto at = [&](const std::array<float, 2>& p) {
                          const RayPoint point = ray_point(options.camera, options.extent, p.at(0),
                                                           p.at(1), marks_at * ratio);
                          return Vec3{point.x, point.y, point.z};
                      };
                      emit_quad(out, at(seg.outer_a), at(seg.outer_b), at(seg.inner_b),
                                at(seg.inner_a), rgb, 1.0F);
                  }
              };
        ring(kMapPoolEdgeRatio, pool.radius + scale, pool.radius - bar - scale, kInk);
        ring(kMapPoolRingRatio, pool.radius, pool.radius - bar, kPoolRing);
    }

    // Your assets: a black edge, then the green square. Buildings are the larger squares. The
    // DRIVEN unit is NOT ringed: it is the same blip every tank gets, in white, plus a 3 mm needle
    // showing which way it is travelling. The owner's call, 2026-10-23: the old big white ring read
    // as "a large white square", and he asked for "a white dot the size of a tank ... with a 3mm
    // line pointing straight ahead". The needle takes the SAME scale as the blip it belongs to
    // (asset.half over kMapUnitHalf), so the two keep their proportion at any frame size.
    for (const MapAsset& asset : map.assets) {
        const float h = asset.half;
        rect(1.02F, asset.px - h - scale, asset.py - h - scale, asset.px + h + scale,
             asset.py + h + scale, kInk);
        rect(1.0F, asset.px - h, asset.py - h, asset.px + h, asset.py + h,
             asset.driven ? kDriven : kOwn);
        if (!asset.driven || (options.headings == nullptr && !options.facing.has_value())) {
            continue;
        }
        // WHICH WAY THE HULL POINTS (`map_line_heading` is the one place that is chosen): a thin
        // white needle from the blip's edge outward, following a turn as it is made. Map y grows
        // DOWN, so north, yaw 0, is (0, -1).
        const sim::Unit* you = nullptr;
        for (const sim::Unit& unit : sim.units()) {
            if (unit.id == asset.id) {
                you = &unit;
                break;
            }
        }
        if (you == nullptr) {
            continue;
        }
        const std::array<MapPoint, 4> needle
            = map_heading_needle(asset, map_line_heading(options, *you));
        // A hair in front of the blip's fill, so the needle is never buried by it.
        const float needle_at = marks_at * 0.995F;
        const auto at_needle = [&](const MapPoint& p) {
            const RayPoint point = ray_point(options.camera, options.extent, p.x, p.y, needle_at);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at_needle(needle.at(0)), at_needle(needle.at(1)), at_needle(needle.at(2)),
                  at_needle(needle.at(3)), kDriven);
    }

    // The enemy, as the radar's blips (`WorldMap::blips`): the radar's red, fading with the radar's
    // own alpha, a hair NEARER than your assets so a blip over one is not lost to the depth test.
    for (const MapBlip& blip : map.blips) {
        const float h = blip.half;
        rect(kMapBlipEdgeRatio, blip.px - h - scale, blip.py - h - scale, blip.px + h + scale,
             blip.py + h + scale, kInk, blip.alpha);
        rect(kMapBlipRatio, blip.px - h, blip.py - h, blip.px + h, blip.py + h,
             radar_colour(RadarTone::Hostile), blip.alpha);
    }

    // The target diamond, over the blips: the map's mark for the hostile `t` holds.
    if (map.target.has_value()) {
        append_target_diamond(out, options, marks_at * kMapTargetRatio, map.target->px,
                              map.target->py, map.target->half, kMapTargetBar * map.target->half);
    }

    // The nav marks, over the units, exactly as the radar draws them.
    for (const MapMark& mark : map.marks) {
        append_nav_glyph(out, options, marks_at * 0.89F, mark.px, mark.py, mark.half,
                         mark.highlighted);
    }
}

} // namespace

std::size_t reticle_vertex_count(Weapon weapon) noexcept {
    return reticle_quads(reticle_for(weapon).kind) * 6U;
}

IconBox key_icon_box(float slot_left, float plate_top, float px, float slot_width) noexcept {
    return IconBox{slot_left, plate_top + ((kStripSlotHeightPixels + kStripStackGapPixels) * px),
                   std::min(kStripIconPixels * px, slot_width)};
}

MenuLayout menu_layout(std::span<const MenuRow> rows, float left, float top, float cell) {
    const auto width = [cell](std::string_view text) {
        return text.empty() ? 0.0F : ((static_cast<float>(text.size()) * 6.0F) - 1.0F) * cell;
    };
    const float key_pad = kMenuKeyPadCells * cell;
    const float verb_pad = kMenuVerbPadCells * cell;
    const float height = kMenuBarCells * cell;
    const float pitch = kMenuPitchCells * cell;
    // The widest key's bar starts ON the margin and fixes the column's right edge; every other key
    // ends there too, so a narrow key sits a gutter from its verb and a wide one grows left.
    float widest = 0.0F;
    for (const MenuRow& row : rows) {
        widest = std::max(widest, width(row.key));
    }
    const float key_right = left + widest + (2.0F * key_pad);
    const float verb_left = key_right + (kMenuGutterCells * cell);
    MenuLayout layout;
    layout.rows.reserve(rows.size());
    float verbs_right = verb_left;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const MenuRow& row = rows[index];
        MenuRowLayout at;
        at.key_right = key_right;
        at.key_text = key_right - key_pad - width(row.key);
        at.key_left = at.key_text - key_pad;
        at.verb_left = verb_left;
        at.verb_text = verb_left + verb_pad;
        at.verb_right = row.verb.empty() ? verb_left : at.verb_text + width(row.verb) + verb_pad;
        at.top = top + (static_cast<float>(index) * pitch);
        at.height = height;
        at.text_top = at.top + ((kMenuBarCells - 5.0F) * 0.5F * cell);
        at.enabled = row.enabled;
        verbs_right = std::max(verbs_right, at.verb_right);
        layout.rows.push_back(at);
    }
    // The right column reads down the panel, clear of the widest verb bar.
    for (MenuRowLayout& at : layout.rows) {
        at.right_left = verbs_right + (kMenuRightGapCells * cell);
    }
    // The line leads from the menu's rows, not from the state row: it starts a cell under the state
    // row's bar and ends a cell under the last one, in the gaps between bars.
    layout.has_line = rows.size() >= 2;
    if (layout.has_line) {
        const float centre = key_right + (kMenuGutterCells * 0.5F * cell);
        layout.line_left = centre - (kMenuLineCells * 0.5F * cell);
        layout.line_right = centre + (kMenuLineCells * 0.5F * cell);
        layout.line_top = layout.rows.front().top + height + cell;
        layout.line_bottom = layout.rows.back().top + height + cell;
    }
    return layout;
}

std::array<float, 2> attack_line_origin(const MenuLayout& layout) noexcept {
    if (layout.rows.empty()) {
        return {0.0F, 0.0F};
    }
    const MenuRowLayout& state = layout.rows.front();
    return {state.verb_right, state.top + (state.height * 0.5F)};
}

std::array<float, 2> row_line_origin(const MenuLayout& layout, std::size_t index,
                                     std::size_t right_glyphs, float cell) noexcept {
    if (index >= layout.rows.size()) {
        return {0.0F, 0.0F};
    }
    const MenuRowLayout& row = layout.rows.at(index);
    const float edge
        = right_glyphs == 0
              ? row.verb_right
              : row.right_left + (((static_cast<float>(right_glyphs) * 6.0F) - 1.0F) * cell);
    return {edge + cell, row.top + (row.height * 0.5F)};
}

std::vector<MenuLeader> menu_leaders(const MenuLayout& layout, std::optional<float> radar_top,
                                     std::span<const std::array<float, 2>> blips) {
    std::vector<MenuLeader> leaders;
    if (blips.empty()) {
        leaders.push_back(menu_leader(layout, radar_top, std::nullopt));
        return leaders;
    }
    leaders.reserve(blips.size());
    for (const std::array<float, 2>& blip : blips) {
        leaders.push_back(menu_leader(layout, radar_top, blip));
    }
    return leaders;
}

MenuLeader menu_leader(const MenuLayout& layout, std::optional<float> radar_top,
                       std::optional<std::array<float, 2>> blip) {
    MenuLeader leader;
    if (!layout.has_line) {
        return leader;
    }
    // One x down the gutter, from the menu all the way to the radar: the line never jogs. A radar
    // that is not below the menu (a window too short to hold both) has nothing to lead down to.
    const float x = (layout.line_left + layout.line_right) * 0.5F;
    if (!radar_top.has_value() || *radar_top <= layout.line_bottom) {
        leader.points = {{x, layout.line_top}, {x, layout.line_bottom}};
        return leader;
    }
    leader.points = {{x, layout.line_top}, {x, *radar_top}};
    if (blip.has_value()) {
        // Along the top edge to the blip's x, whichever side of the gutter that is, then down.
        leader.points.push_back({blip->at(0), *radar_top});
        leader.points.push_back(*blip);
        leader.dot = true;
    }
    return leader;
}

std::array<float, 3> menu_face_colour(bool enabled, int band) noexcept {
    if (!enabled) {
        return kMenuUnlitFace;
    }
    const int clamped = std::clamp(band, 0, kMenuFaceBands - 1);
    if (clamped == 0) {
        return kMenuFaceHighlight;
    }
    // The rest run from the top colour to the bottom one, ending on it exactly.
    const float t = static_cast<float>(clamped - 1) / static_cast<float>(kMenuFaceBands - 2);
    std::array<float, 3> colour{};
    for (std::size_t channel = 0; channel < colour.size(); ++channel) {
        colour.at(channel) = kMenuFaceTop.at(channel)
                             + ((kMenuFaceBottom.at(channel) - kMenuFaceTop.at(channel)) * t);
    }
    return colour;
}

int hull_variant(std::uint64_t seed, std::uint32_t unit_id) noexcept {
    // splitmix64 over the pair: sequential ids land far apart before the modulo.
    std::uint64_t z = seed + (static_cast<std::uint64_t>(unit_id) * 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<int>(z % static_cast<std::uint64_t>(kHullVariantCount));
}

std::vector<RepairLink> repair_links(const sim::Sim& sim) {
    std::vector<RepairLink> links;
    for (const sim::Unit& patient : sim.units()) {
        if (!patient.alive || patient.hp >= sim::profile(patient.kind).hp) {
            continue;
        }
        for (const sim::Unit& truck : sim.units()) {
            if (!truck.alive || truck.kind != sim::UnitKind::ServiceTruck
                || truck.side != patient.side || truck.id == patient.id) {
                continue;
            }
            // The sim's own weight, cone and all: a link stands exactly when the truck mends.
            if (sim::service_repair_weight(truck, patient) > 0) {
                links.reserve(links.size() + 1);
                links.push_back(RepairLink{truck.id, patient.id});
            }
        }
    }
    return links;
}

float height_metres(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    return static_cast<float>(world::sample(seed, tile_x, tile_y).height) * kMetresPerHeightUnit;
}

namespace {

/// The drawn ground under a point inside a tile.
///
/// The terrain quad is emitted as two triangles split along the p00-p11 diagonal, so the surface
/// the player SEES is piecewise planar, not a bilinear blend. A bilinear blend would sit up to a
/// quarter of the tile's corner twist away from it (measured: up to 6.25 cm on seed 42, against a
/// 0.25 m ride height) -- enough to sink the hull into a slope or float it off one. This follows
/// the same two planes the mesh does, so the hull, the pick and the picture agree everywhere.
float blend_corners(std::uint64_t seed, int tx, int ty, float fx, float fy) noexcept {
    const float h00 = height_metres(seed, tx, ty);
    const float h10 = height_metres(seed, tx + 1, ty);
    const float h01 = height_metres(seed, tx, ty + 1);
    const float h11 = height_metres(seed, tx + 1, ty + 1);
    if (fx >= fy) {
        return h00 + ((h10 - h00) * fx) + ((h11 - h10) * fy); // the p00, p10, p11 triangle
    }
    return h00 + ((h01 - h00) * fy) + ((h11 - h01) * fx); // the p00, p01, p11 triangle
}

} // namespace

float ground_metres(std::uint64_t seed, sim::Fixed x, sim::Fixed y) noexcept {
    constexpr float kOne = static_cast<float>(sim::Fixed::kOne);
    const std::int32_t mask = sim::Fixed::kOne - 1;
    return blend_corners(seed, x.floor_int(), y.floor_int(),
                         static_cast<float>(x.raw & mask) / kOne,
                         static_cast<float>(y.raw & mask) / kOne);
}

float ground_metres(std::uint64_t seed, float tile_x, float tile_y) noexcept {
    const float fx = std::floor(tile_x);
    const float fy = std::floor(tile_y);
    return blend_corners(seed, static_cast<int>(fx), static_cast<int>(fy), tile_x - fx,
                         tile_y - fy);
}

math::Vec3 ground_normal(std::uint64_t seed, sim::Fixed x, sim::Fixed y) noexcept {
    constexpr float kOne = static_cast<float>(sim::Fixed::kOne);
    constexpr float kReachTiles = kHullHalfMetres / kMetresPerTile;
    const float tile_x = static_cast<float>(x.raw) / kOne;
    const float tile_y = static_cast<float>(y.raw) / kOne;
    // Centred differences over a hull's own width: the ground the body actually spans, not the
    // one point it stands on.
    const float run = 2.0F * kHullHalfMetres;
    float slope_x = (ground_metres(seed, tile_x + kReachTiles, tile_y)
                     - ground_metres(seed, tile_x - kReachTiles, tile_y))
                    / run;
    float slope_z = (ground_metres(seed, tile_x, tile_y + kReachTiles)
                     - ground_metres(seed, tile_x, tile_y - kReachTiles))
                    / run;
    const float steepness = std::hypot(slope_x, slope_z);
    if (steepness > kMaxTiltSlope) {
        slope_x *= kMaxTiltSlope / steepness;
        slope_z *= kMaxTiltSlope / steepness;
    }
    if (slope_x == 0.0F && slope_z == 0.0F) {
        return Vec3{0.0F, 1.0F, 0.0F}; // level: exactly up, so nothing is rotated at all
    }
    return normalise(Vec3{-slope_x, 1.0F, -slope_z});
}

bool stands_level(sim::UnitKind kind) noexcept {
    return kind == sim::UnitKind::Factory || sim::is_turret(kind)
           || kind == sim::UnitKind::DeployedRecycler || kind == sim::UnitKind::DeployedScavenger;
}

Suspension suspend(Suspension state, float target_z, std::uint32_t ticks) noexcept {
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        state.z_vel += ((target_z - state.z) * kSpringPerTick) - (state.z_vel * kDampPerTick);
        state.z += state.z_vel;
    }
    return state;
}

void Hover::advance(sim::Tick tick, float ground_metres, bool boost) noexcept {
    const float rest = ground_metres + kRideHeightMetres;
    const float target = rest + (boost ? kBoostLiftMetres : 0.0F);
    if (!clocked_) {
        // At rest at ride height even if boost is already held: it rises from there, not from
        // nowhere.
        state_ = Suspension{rest, 0.0F};
        clocked_ = true;
    } else {
        const sim::Tick elapsed = tick > last_tick_ ? tick - last_tick_ : 0;
        state_ = suspend(state_, target, static_cast<std::uint32_t>(elapsed));
    }
    last_tick_ = tick;
}

std::optional<float> Hover::z() const noexcept {
    return clocked_ ? std::optional<float>{state_.z} : std::nullopt;
}

void HitFlash::hit(sim::Tick now) noexcept {
    hit_at_ = now;
    armed_ = true;
}

float HitFlash::level(sim::Tick now) const noexcept {
    if (!armed_ || now < hit_at_) {
        return 0.0F;
    }
    const sim::Tick elapsed = now - hit_at_;
    if (elapsed >= kHitFlashTicks) {
        return 0.0F;
    }
    return 1.0F - (static_cast<float>(elapsed) / static_cast<float>(kHitFlashTicks));
}

void HitMark::hit(sim::Tick now) noexcept {
    hit_at_ = now;
    armed_ = true;
}

void WakeLog::after_frame(const sim::UnitSnapshot& before, const sim::Sim& sim, sim::Tick stamp) {
    const sim::Tick now = sim.tick();
    // What has gone cold is forgotten first, by the same age the scene stops drawing at.
    std::erase_if(kills_, [now](const sim::BattleEvent& kept) {
        return kept.tick <= now && now - kept.tick >= kSmokeTicks;
    });
    for (const sim::BattleEvent& event : sim::events_between(before, sim, stamp)) {
        if (event.kind != sim::EventKind::Killed) {
            continue;
        }
        const bool known = std::ranges::any_of(kills_, [&event](const sim::BattleEvent& kept) {
            return kept.subject == event.subject && kept.tick == event.tick;
        });
        if (!known) {
            kills_.push_back(event);
        }
    }
}

void LandingMarks::land(sim::Tick landed_at, std::int32_t tile_x, std::int32_t tile_y) {
    // What has run out by now is gone for good: the list never grows past what is showing.
    std::erase_if(entries_, [&](const Entry& entry) {
        return landed_at - entry.at >= kLandingMarkHoldTicks + kLandingMarkFadeTicks;
    });
    entries_.push_back(Entry{landed_at, tile_x, tile_y});
}

std::vector<LandingMarks::Mark> LandingMarks::active(sim::Tick now) const {
    std::vector<Mark> showing;
    for (const Entry& entry : entries_) {
        if (now < entry.at) {
            continue;
        }
        const sim::Tick age = now - entry.at;
        if (age >= kLandingMarkHoldTicks + kLandingMarkFadeTicks) {
            continue;
        }
        const float level = age < kLandingMarkHoldTicks
                                ? 1.0F
                                : 1.0F
                                      - (static_cast<float>(age - kLandingMarkHoldTicks)
                                         / static_cast<float>(kLandingMarkFadeTicks));
        showing.push_back(Mark{entry.tile_x, entry.tile_y, level});
    }
    return showing;
}

float HitMark::level(sim::Tick now) const noexcept {
    if (!armed_ || now < hit_at_) {
        return 0.0F;
    }
    const sim::Tick elapsed = now - hit_at_;
    if (elapsed >= kHitMarkTicks) {
        return 0.0F;
    }
    return 1.0F - (static_cast<float>(elapsed) / static_cast<float>(kHitMarkTicks));
}

bool shooter_connected(std::span<const sim::BattleEvent> events, sim::UnitId shooter,
                       std::span<const sim::Unit> units) noexcept {
    const auto side_of = [&units](sim::UnitId id, sim::Side& side) {
        for (const sim::Unit& unit : units) {
            if (unit.id == id) {
                side = unit.side;
                return true;
            }
        }
        return false;
    };
    sim::Side shooter_side{};
    if (!side_of(shooter, shooter_side)) {
        return false;
    }
    for (const sim::BattleEvent& event : events) {
        sim::Side victim_side{};
        if (event.kind == sim::EventKind::HitTaken && event.actor == shooter
            && side_of(event.subject, victim_side) && victim_side != shooter_side) {
            return true;
        }
    }
    return false;
}

bool unit_was_hit(std::span<const sim::BattleEvent> events, sim::UnitId unit) noexcept {
    // A HitTaken names the victim in `subject`, and there is one only when a bolt LANDED. This used
    // to infer the hit from a Fired event whose subject had lost hit points -- which stopped being
    // true the day damage moved to the projectile's arrival, and silently stopped the flash ever
    // firing. Reading the event that means what we want is the whole fix.
    for (const sim::BattleEvent& event : events) {
        if (event.kind == sim::EventKind::HitTaken && event.subject == unit) {
            return true;
        }
    }
    return false;
}

std::optional<float> Headings::heading_of(const sim::Unit& unit) noexcept {
    if (unit.vx.raw == 0 && unit.vy.raw == 0) {
        return std::nullopt;
    }
    // Yaw 0 is +Y (the world's forward) and a positive yaw turns toward +X.
    return std::atan2(static_cast<float>(unit.vx.raw), static_cast<float>(unit.vy.raw));
}

LegPose leg_pose(float phase, std::size_t leg, float half) noexcept {
    // Half a cycle between the two tripods. The odd set is a half-cycle behind the even one.
    float q = phase + (leg % 2U == 0U ? 0.0F : 0.5F);
    q -= std::floor(q);
    if (q < 0.5F) {
        // STANCE: planted. The foot slides aft from +half to -half over half a cycle, which is
        // `kStrideMetres / 2` of body travel against `2 * half` of reach: the same ground.
        return {half - 4.0F * half * q, 0.0F};
    }
    // SWING: carried forward from aft to fore, and lifted in a half sine.
    const float t = (q - 0.5F) * 2.0F;
    constexpr float kPi = 3.14159265F;
    return {-half + 2.0F * half * t, kStepLiftMetres * std::sin(kPi * t)};
}

float stride_metres(sim::UnitKind kind) noexcept {
    return 4.0F * (kind == sim::UnitKind::Walker ? kWalkerStrideHalfMetres : kStrideHalfMetres);
}

void Strides::after_frame(const sim::UnitSnapshot& before, const sim::Sim& sim) {
    std::map<std::uint32_t, float> kept;
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive) {
            continue;
        }
        const auto held = phase_.find(unit.id.value);
        float phase = held != phase_.end() ? held->second : 0.0F;
        bool known = held != phase_.end();
        const auto was = std::ranges::find_if(
            before, [&unit](const sim::Unit& old) { return old.id == unit.id; });
        if (was != before.end()) {
            const float dx = static_cast<float>(unit.x.raw - was->x.raw)
                             / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
            const float dy = static_cast<float>(unit.y.raw - was->y.raw)
                             / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
            const float moved = std::hypot(dx, dy);
            if (moved > 0.0F) {
                phase += moved / stride_metres(unit.kind);
                phase -= std::floor(phase);
                known = true;
            }
        }
        if (known) {
            kept.emplace(unit.id.value, phase);
        }
    }
    phase_ = std::move(kept);
}

std::optional<float> Strides::of(const sim::Unit& unit) const {
    const auto found = phase_.find(unit.id.value);
    return found == phase_.end() ? std::nullopt : std::optional<float>(found->second);
}

void Headings::observe(const sim::Sim& sim) {
    std::map<std::uint32_t, float> kept;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.alive) {
            kept.emplace(unit.id.value, of(unit));
        }
    }
    last_ = std::move(kept);
}

float Headings::of(const sim::Unit& unit) const {
    if (const std::optional<float> moving = heading_of(unit); moving.has_value()) {
        return *moving;
    }
    const auto found = last_.find(unit.id.value);
    return found != last_.end() ? found->second : 0.0F;
}

float gun_yaw(const sim::Sim& sim, const sim::Unit& unit, float heading) {
    const sim::UnitId attacking = unit.stance == sim::Stance::Attack ? unit.target : sim::UnitId{};
    for (const sim::UnitId source : {unit.focus_target, attacking, unit.aim_target}) {
        if (!source.valid()) {
            continue;
        }
        const sim::Unit* target = sim.find(source);
        if (target != nullptr && target->alive) {
            // The same yaw convention as `Headings::heading_of`, from the raw Q16.16 offsets.
            return std::atan2(static_cast<float>(target->x.raw - unit.x.raw),
                              static_cast<float>(target->y.raw - unit.y.raw));
        }
    }
    return heading;
}

float Camera::wrap_angle(float radians) noexcept {
    float wrapped = std::fmod(radians + kPi, 2.0F * kPi);
    if (wrapped < 0.0F) {
        wrapped += 2.0F * kPi;
    }
    return wrapped - kPi;
}

std::array<float, 16> view_projection(const Camera& camera, float aspect,
                                      float fov_radians) noexcept {
    return math::view_projection(Vec3{camera.x, camera.y, camera.z}, camera.yaw, camera.pitch,
                                 aspect, fov_radians, math::kNearPlane, math::kFarPlane, camera.up);
}

GroundTilt tilt_about(const Vec3& pivot, const Vec3& normal) noexcept {
    GroundTilt tilt;
    tilt.pivot = pivot;
    tilt.sine = std::hypot(normal[0], normal[2]);
    if (tilt.sine == 0.0F) {
        return tilt;
    }
    // Rodrigues: the axis is up x normal, horizontal, and the angle between them has sine |axis|
    // and cosine normal.y. Every point turns about the pivot.
    tilt.axis = Vec3{normal[2] / tilt.sine, 0.0F, -normal[0] / tilt.sine};
    tilt.cosine = normal[1];
    return tilt;
}

Vec3 GroundTilt::apply(const Vec3& point) const noexcept {
    if (level()) {
        return point;
    }
    const Vec3 v{point[0] - pivot[0], point[1] - pivot[1], point[2] - pivot[2]};
    const Vec3 swung = cross(axis, v);
    const float along = dot(axis, v) * (1.0F - cosine);
    Vec3 out{};
    for (std::size_t c = 0; c < 3; ++c) {
        out.at(c) = pivot.at(c) + (v.at(c) * cosine) + (swung.at(c) * sine) + (axis.at(c) * along);
    }
    return out;
}

Vec3 GroundTilt::turn(const Vec3& direction) const noexcept {
    if (level()) {
        return direction;
    }
    const Vec3 swung = cross(axis, direction);
    const float along = dot(axis, direction) * (1.0F - cosine);
    Vec3 out{};
    for (std::size_t c = 0; c < 3; ++c) {
        out.at(c) = (direction.at(c) * cosine) + (swung.at(c) * sine) + (axis.at(c) * along);
    }
    return out;
}

float face_light(const Vec3& normal) {
    const float facing = std::clamp(dot(normal, normalise(kSunDirection)), -1.0F, 1.0F);
    return kShadeFloor + ((1.0F - kShadeFloor) * (0.5F + (0.5F * facing)));
}

Vec3 terrain_base_colour() {
    const float flat = face_light(Vec3{0.0F, 1.0F, 0.0F});
    return Vec3{kTerrainLight[0] / flat, kTerrainLight[1] / flat, kTerrainLight[2] / flat};
}

void smooth_vertex_normals(std::vector<SceneVertex>& vertices, std::size_t first, std::size_t last,
                           float crease_degrees) {
    last = std::min(last, vertices.size());
    if (first >= last) {
        return;
    }
    const float cos_crease = std::cos(crease_degrees * (kPi / 180.0F));
    // One entry per LIT corner: its position, its face's unit normal and the face's area (the
    // weight), found from the triangle it belongs to.
    struct Corner {
        std::array<float, 3> at;
        Vec3 normal;
        float weight;
        std::size_t index;
    };
    std::vector<Corner> corners;
    corners.reserve(last - first);
    for (std::size_t i = first; i + 2 < last; i += 3) {
        const Vec3& a = vertices.at(i).position;
        const Vec3& b = vertices.at(i + 1).position;
        const Vec3& c = vertices.at(i + 2).position;
        const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const Vec3 doubled = cross(ab, ac);
        const float weight = std::sqrt(dot(doubled, doubled)) * 0.5F;
        for (std::size_t k = 0; k < 3; ++k) {
            const Vec3& given = vertices.at(i + k).normal;
            const float length = std::sqrt(dot(given, given));
            if (length > 0.0F) {
                corners.push_back(Corner{
                    vertices.at(i + k).position,
                    Vec3{given[0] / length, given[1] / length, given[2] / length}, weight, i + k});
            }
        }
    }
    // Sorted by a 64-bit hash of the position, which keeps the sort's comparisons to integers (this
    // runs every frame on tens of thousands of corners). A run is then cut by the EXACT position,
    // so a hash collision between two different positions can only fail to blend them, never blend
    // two that are not the same place.
    struct Keyed {
        std::uint64_t key;
        std::size_t corner;
    };
    std::vector<Keyed> order;
    order.reserve(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        std::uint64_t key = 0x9E3779B97F4A7C15ULL;
        for (const float coordinate : corners.at(i).at) {
            key = (key ^ std::bit_cast<std::uint32_t>(coordinate)) * 0x100000001B3ULL;
            key ^= key >> 29U;
        }
        order.push_back(Keyed{key, i});
    }
    std::sort(order.begin(), order.end(), [](const Keyed& l, const Keyed& r) {
        return l.key != r.key ? l.key < r.key : l.corner < r.corner;
    });
    std::vector<Vec3> smoothed(corners.size());
    std::vector<Vec3> sums; // one running sum per corner of the run being worked on, reused
    std::size_t run_first = 0;
    while (run_first < order.size()) {
        std::size_t run_last = run_first + 1;
        while (run_last < order.size()
               && corners.at(order.at(run_last).corner).at
                      == corners.at(order.at(run_first).corner).at) {
            ++run_last;
        }
        const std::size_t count = run_last - run_first;
        if (count == 1) {
            // Alone at its position: nothing to blend with, and a unit normal averaged with
            // itself is itself.
            const Corner& only = corners.at(order.at(run_first).corner);
            smoothed.at(order.at(run_first).corner) = only.normal;
            run_first = run_last;
            continue;
        }
        // Each corner's sum starts with its own face, then takes in every other face of the run
        // within the crease of it. The angle test is symmetric, so each PAIR is tested once and
        // credits both ends.
        sums.assign(count, Vec3{0.0F, 0.0F, 0.0F});
        for (std::size_t i = 0; i < count; ++i) {
            const Corner& own = corners.at(order.at(run_first + i).corner);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                sums.at(i).at(axis) += own.normal.at(axis) * own.weight;
            }
            for (std::size_t j = i + 1; j < count; ++j) {
                const Corner& other = corners.at(order.at(run_first + j).corner);
                if (dot(own.normal, other.normal) >= cos_crease) {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        sums.at(i).at(axis) += other.normal.at(axis) * other.weight;
                        sums.at(j).at(axis) += own.normal.at(axis) * own.weight;
                    }
                }
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3& sum = sums.at(i);
            const float length = std::sqrt(dot(sum, sum));
            const std::size_t corner = order.at(run_first + i).corner;
            smoothed.at(corner) = length > 0.0F
                                      ? Vec3{sum[0] / length, sum[1] / length, sum[2] / length}
                                      : corners.at(corner).normal;
        }
        run_first = run_last;
    }
    for (std::size_t i = 0; i < corners.size(); ++i) {
        vertices.at(corners.at(i).index).normal = smoothed.at(i);
    }
}

namespace {

/// Turns the vertices from `first` on (one vehicle, just built level) so its up is the ground's
/// normal under it. The turn is about the point on the ground below the unit's centre, `pivot_y`
/// being that point's height, so the footing stays where the sim put it and the elevation the
/// builder chose is untouched at the centre; only pitch and roll are added. A level normal rotates
/// nothing, so a vehicle on flat ground is the same bytes it was before the tilt existed.
void tilt_to_ground(std::vector<SceneVertex>& out, std::size_t first, const sim::Unit& unit,
                    float pivot_y, const Vec3& normal) {
    const GroundTilt tilt = tilt_about(
        Vec3{static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile,
             pivot_y,
             static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne)
                 * kMetresPerTile},
        normal);
    if (tilt.level()) {
        return;
    }
    for (std::size_t i = first; i < out.size(); ++i) {
        out.at(i).position = tilt.apply(out.at(i).position);
        out.at(i).normal = tilt.turn(out.at(i).normal);
    }
}

/// THE SITES IN VIEW, for `build_terrain`: every pool (when the pools layer is on) and every living
/// deployed recycler and scavenger the viewer could see (`available_to_viewer`, the rule the unit's
/// own mesh follows, so the ground never shows what the units do not). A building is anchored by
/// its `footprint_around`, the sim's own answer to which tiles it stands on.
SiteGroundMap sites_in_view(const sim::Sim& sim, const SceneOptions& options) {
    SiteGroundMap sites;
    if (options.show_pools) {
        for (const sim::Pool& pool : sim.pools()) {
            sites.add(SiteKind::Pool, pool.tile_x, pool.tile_y);
        }
    }
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || !available_to_viewer(sim, options, unit)) {
            continue;
        }
        if (unit.kind == sim::UnitKind::DeployedRecycler) {
            const sim::Footprint footprint = sim::footprint_around(unit.x, unit.y);
            sites.add(SiteKind::DeployedRecycler, footprint.centre_x(), footprint.centre_y());
        } else if (unit.kind == sim::UnitKind::DeployedScavenger) {
            sites.add(SiteKind::DeployedScavenger, unit.x.floor_int(), unit.y.floor_int());
        }
    }
    return sites;
}

} // namespace

Scene build_scene(const sim::Sim& sim, const SceneOptions& options,
                  std::span<const sim::UnitId> selected) {
    Scene scene;
    const float aspect
        = static_cast<float>(options.extent.width) / static_cast<float>(options.extent.height);
    scene.view_projection = view_projection(options.camera, aspect);

    const int centre_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int centre_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    build_terrain(scene.vertices, options.terrain, options.seed, centre_x, centre_y,
                  options.radius_tiles, sites_in_view(sim, options));
    const std::size_t terrain_end = scene.vertices.size();
    append_sky(scene.vertices, options.camera);
    const std::size_t sky_end = scene.vertices.size();

    // The one pick the frame makes: the reticle and the unit tint both read it, so they can never
    // disagree about what is under the crosshair.
    Pick aimed;
    if (options.aim.has_value()) {
        aimed = pick(sim, options, options.aim->x, options.aim->y);
    }

    append_unit_shadows(scene.vertices, options, sim);

    for (const sim::Unit& unit : sim.units()) {
        // Only draw units the player could pick: inside the terrain that was actually meshed (a
        // unit far outside the rendered radius has no ground to stand on in this scene) and not
        // beyond the radar's reach. `available_to_viewer` is the rule the pick asks too.
        if (!unit.alive) {
            continue;
        }
        if (available_to_viewer(sim, options, unit)) {
            const bool is_selected
                = std::find(selected.begin(), selected.end(), unit.id) != selected.end();
            Mark mark = Mark::None;
            if (is_selected) {
                mark = Mark::Selected;
            } else if (aimed.has_unit() && aimed.unit == unit.id) {
                mark = unit.side == options.viewer_side ? Mark::AimedOwn : Mark::AimedHostile;
            }
            const std::size_t body_first = scene.vertices.size();
            const bool hovering = options.hover.has_value() && options.hover->unit == unit.id;
            const float lift_z = hovering ? options.hover->z : 0.0F;
            const bool faced = options.facing.has_value() && options.facing->unit == unit.id;
            // The hull's heading, and -- for every gun but the driven one -- the turret turned
            // from it to where `gun_yaw` says the gun points. The driven unit's turret is the
            // player's, untouched.
            const float heading = faced ? options.facing->heading
                                  : options.headings != nullptr
                                      ? options.headings->of(unit)
                                      : Headings::heading_of(unit).value_or(0.0F);
            const float turret
                = faced ? options.facing->turret : gun_yaw(sim, unit, heading) - heading;
            // The barrel's elevation is the driven unit's alone; every other gun is level.
            const float pitch = faced ? options.facing->pitch : 0.0F;
            // THE ONE DECISION the frame makes about a faction, for the three kinds the aliens
            // draw their own way (the Harvester, the Guardian, the Hauler): by the unit's FACTION,
            // never its side.
            const bool alien = faction_of(options.factions, unit.side) == Faction::Alien;
            if (unit.kind == sim::UnitKind::HoverTank) {
                build_hover_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                 heading, turret, pitch);
            } else if (unit.kind == sim::UnitKind::MortarBike) {
                build_mortar_bike(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                  heading, heading + turret);
            } else if (unit.kind == sim::UnitKind::RocketTank) {
                build_rocket_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                  heading, turret);
            } else if (unit.kind == sim::UnitKind::AssaultTank) {
                build_assault_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                   heading, turret, pitch);
            } else if (unit.kind == sim::UnitKind::Walker) {
                build_walker(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading,
                             turret, pitch,
                             options.strides != nullptr ? options.strides->of(unit) : std::nullopt);
            } else if (unit.kind == sim::UnitKind::Apc) {
                build_apc(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading,
                          turret);
            } else if (unit.kind == sim::UnitKind::Bomber) {
                build_bomber(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading);
            } else if (alien && unit.kind == sim::UnitKind::Tug) {
                build_hauler(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading);
            } else if (unit.kind == sim::UnitKind::Tug || unit.kind == sim::UnitKind::Constructor
                       || unit.kind == sim::UnitKind::ServiceTruck
                       || unit.kind == sim::UnitKind::ServicePod) {
                build_support_vehicle(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                      heading);
            } else if (alien && sim::is_turret(unit.kind)) {
                build_guardian(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading,
                               heading + turret, pitch);
            } else if (sim::is_turret(unit.kind)) {
                // Before the tank branch below: a driven turret is FACED too, and it is built by
                // the SAME function as one nobody drives -- the player's pitch reaches the barrel
                // (`pitch` is 0 for every gun but the driven one), the mesh is otherwise the same.
                build_defense_turret(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                     heading, heading + turret, pitch);
            } else if (unit.kind == sim::UnitKind::Scavenger) {
                // Before the tank branch below: a driven scavenger is FACED too, and it must not
                // borrow the tank's turret. Nothing on a scavenger aims. THE one place a kind's
                // silhouette follows the unit's FACTION (never its side): the human hauler or the
                // alien harvester, the same kind either way.
                if (alien) {
                    build_harvester(
                        scene.vertices, options.seed, unit, mark, lift_z, hovering, heading,
                        options.strides != nullptr ? options.strides->of(unit) : std::nullopt);
                } else {
                    build_scavenger(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                    heading);
                }
            } else if (unit.kind == sim::UnitKind::Recycler
                       || unit.kind == sim::UnitKind::DeployedRecycler) {
                // Beside the scavenger's, and before the tank branch: a driven recycler is FACED
                // too, and must neither borrow the tank's turret nor fall through to a factory. A
                // DEPLOYED one is drawn as the hull that drove there, parked on its site: the
                // building has no look of its own yet, and DEPLOY adds no art.
                build_recycler(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading);
            } else if (unit.kind == sim::UnitKind::Tank || unit.kind == sim::UnitKind::Scout
                       || unit.kind == sim::UnitKind::MslScout) {
                // The three kinds built by the tank's builder, each from its own shape. Tested by
                // KIND, never by `faced`: a driven kind with no shape of its own used to be built
                // here with the tank's hand-built hull, and every kind has a builder now.
                build_facing_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                  heading, turret, pitch);
            } else {
                build_factory(scene.vertices, options.seed, unit, mark, lift_z, hovering);
            }
            if (!stands_level(unit.kind)) {
                tilt_to_ground(scene.vertices, body_first, unit,
                               hovering ? lift_z : ground_metres(options.seed, unit.x, unit.y),
                               ground_normal(options.seed, unit.x, unit.y));
            }
        }
    }

    append_projectiles(scene.vertices, options, sim);
    append_landing_marks(scene.vertices, options, sim.tick());
    append_scrap(scene.vertices, options, sim);
    append_pools(scene.vertices, options, sim);
    std::vector<VertexRun> emissive; // the sparks: the fog leaves these runs alone
    append_repair_beams(scene.vertices, emissive, options, sim);
    append_effects(scene.vertices, emissive, options, sim);
    append_motes(scene.vertices, options, sim);
    append_nav_beacons(scene.vertices, options);
    append_deploy_preview(scene.vertices, options);
    // THE LINE BETWEEN WORLD AND HUD, DRAWN HERE: everything above is the world -- the terrain,
    // the units and their shadows, bolts, landing marks, scrap, wrecks and smoke, nav beacons and
    // the deploy wireframe -- and goes through the fog, by its distance from the eye. The sky is
    // what the fog fades TO and is skipped. Everything below is the HUD, which is laid out in
    // screen space on the near-plane layers: a bar over a unit 50 m away is a label, and it must
    // read like the one over a unit 5 m away, so none of it is fogged. Nav GLYPHS are the radar's
    // and the world map's marks, and sit on the HUD side of this line.
    // SMOOTH SHADING (AP): the world's face normals become blended vertex normals, here, once the
    // units have been turned, tilted and scaled into place, so "faces that share a position" is
    // judged on the final geometry. The sky and the HUD carry no normal and are not touched.
    smooth_vertex_normals(scene.vertices, 0, scene.vertices.size());
    fog_range(scene.vertices, 0, terrain_end, options.camera, options.radius_tiles);
    fog_world(scene.vertices, sky_end, scene.vertices.size(), emissive, options.camera,
              options.radius_tiles);
    // The renderer smooths the world's edges and draws everything past this index after it.
    scene.world_vertex_count = scene.vertices.size();
    // The map and the radar share the bottom-left corner: BZ2's R turns one into the other rather
    // than stacking them, so the radar (and the hull bar it carries) is not drawn while the map is
    // up. Hiding the radar must not change the sim, and it does not: this is a view.
    if (!options.show_world_map) {
        append_radar(scene.vertices, options, sim);
    }
    // The ship instrument is a HUD layer of its own, in the corner OPPOSITE the radar, so it is
    // drawn whether or not R has turned the radar into the map.
    if (options.show_ship_gauge) {
        append_ship_gauge(scene.vertices, options, sim);
    }
    append_mount_plates(scene.vertices, options, sim);
    // "Pointing approximately at it": the unit the crosshair is on, else the nearest within the
    // assist's own cone (any side, unlike the assist's). One answer for the whole frame.
    const sim::UnitId pointed = pointed_asset(sim, options, aimed);
    append_unit_bars(scene.vertices, options, sim, selected, pointed);
    append_attack_readout(scene.vertices, options, sim);
    append_assist(scene.vertices, options, sim);
    append_hud_panels(scene.vertices, options, sim, selected);
    append_store_gauge(scene.vertices, options, sim);
    append_outcome(scene.vertices, options, sim);
    append_reticle(scene.vertices, options, sim, aimed);
    append_world_map(scene.vertices, options, sim);
    append_statistics(scene.vertices, options, sim);

    // LAST, so it tints everything under it. The vertex format has no alpha, so the wash is each
    // colour taken a fraction of the way to deep red; zero returns before touching anything.
    if (options.hit_flash > 0.0F) {
        const float amount = std::clamp(options.hit_flash, 0.0F, 1.0F) * kHitFlashStrength;
        constexpr std::array<float, 3> kDeepRed{0.55F, 0.0F, 0.0F};
        for (SceneVertex& vertex : scene.vertices) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                float& value = vertex.colour.at(channel);
                value += (kDeepRed.at(channel) - value) * amount;
            }
        }
    }

    return scene;
}

Scene build_calibration_scene(const SceneOptions& options) {
    Scene scene;
    const float aspect
        = static_cast<float>(options.extent.width) / static_cast<float>(options.extent.height);
    scene.view_projection = view_projection(options.camera, aspect);

    const float distance = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const float width = static_cast<float>(options.extent.width);
    const float height = static_cast<float>(options.extent.height);
    const auto at = [&](float px, float py) {
        const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
        return Vec3{point.x, point.y, point.z};
    };
    constexpr std::array<std::array<float, 3>, 3> kBars{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    for (std::size_t i = 0; i < kBars.size(); ++i) {
        const float x0 = width * static_cast<float>(i) / 3.0F;
        const float x1 = width * static_cast<float>(i + 1) / 3.0F;
        emit_quad(scene.vertices, at(x0, 0.0F), at(x1, 0.0F), at(x1, height), at(x0, height),
                  kBars.at(i));
    }
    return scene;
}

} // namespace protekt::game
