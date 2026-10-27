#include "protekt/game/scene.hpp"

#include "protekt/game/atlas.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/game/world_map.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/spec.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
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
                   const Vec3& colour, float alpha = 1.0F) {
    out.push_back(SceneVertex{a, colour, alpha});
    out.push_back(SceneVertex{b, colour, alpha});
    out.push_back(SceneVertex{c, colour, alpha});
}

void emit_quad(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
               const Vec3& d, const Vec3& colour, float alpha = 1.0F) {
    emit_triangle(out, a, b, c, colour, alpha);
    emit_triangle(out, a, c, d, colour, alpha);
}

/// A quad that samples one whole atlas tile: its corners a, b, c, d take the tile's corners
/// top-left, top-right, bottom-right, bottom-left. The colour still multiplies the texel.
void emit_tiled_quad(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                     const Vec3& d, const Vec3& colour, const UvRect& tile, int orientation = 0) {
    const auto corner = [&](const Vec3& p, int which) {
        const auto uv = oriented_corner_uv(tile, orientation, which);
        return SceneVertex{p, colour, 1.0F, uv.at(0), uv.at(1)};
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

void build_terrain(std::vector<SceneVertex>& out, const TerrainCache* cache, std::uint64_t seed,
                   int centre_x, int centre_y, int radius) {
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
    for (int ty = centre_y - radius - 1; ty <= centre_y + radius; ++ty) {
        for (int tx = centre_x - radius - 1; tx <= centre_x + radius; ++tx) {
            const world::Terrain terrain = cached_terrain(cache, seed, tx, ty);
            const UvRect& tile = tiles.at(static_cast<std::size_t>(terrain))
                                     .at(static_cast<std::size_t>(variant_index(terrain, tx, ty)));
            const float x0 = static_cast<float>(tx) * kMetresPerTile;
            const float x1 = static_cast<float>(tx + 1) * kMetresPerTile;
            const float z0 = static_cast<float>(ty) * kMetresPerTile;
            const float z1 = static_cast<float>(ty + 1) * kMetresPerTile;
            const Vec3 p00{x0, cached_height_metres(cache, seed, tx, ty), z0};
            const Vec3 p10{x1, cached_height_metres(cache, seed, tx + 1, ty), z0};
            const Vec3 p11{x1, cached_height_metres(cache, seed, tx + 1, ty + 1), z1};
            const Vec3 p01{x0, cached_height_metres(cache, seed, tx, ty + 1), z1};
            emit_tiled_quad(out, p00, p10, p11, p01, kTerrainLight, tile,
                            orientation_index(terrain, tx, ty));
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

    // The sun disc, on exactly the direction the prisms are lit from.
    constexpr float kDiscRadius = 0.98F * kSkyRadiusMetres;
    constexpr float kDiscAngularSize = 0.06F; ///< rim radius as a fraction of the distance
    const Vec3 direction = normalise(kSunDirection);
    const Vec3 across = normalise(cross(Vec3{0.0F, 1.0F, 0.0F}, direction));
    const Vec3 up = cross(direction, across);
    const Vec3 centre = at(direction, kDiscRadius);
    const auto rim = [&](int segment) {
        const float angle = 2.0F * kPi * static_cast<float>(segment) / kSkySegments;
        const float scale = kDiscRadius * kDiscAngularSize;
        const float u = std::cos(angle) * scale;
        const float v = std::sin(angle) * scale;
        return Vec3{centre[0] + (across[0] * u) + (up[0] * v),
                    centre[1] + (across[1] * u) + (up[1] * v),
                    centre[2] + (across[2] * u) + (up[2] * v)};
    };
    for (int segment = 0; segment < kSkySegments; ++segment) {
        emit_triangle(out, centre, rim(segment), rim(segment + 1), kSunDiscColour);
    }
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

/// How lit a face is: its outward unit normal against the sun (`kSunDirection`), mapped so a face
/// square to the sun is 1.0 and one square away is `kShadeFloor` -- never black. A half-Lambert
/// ramp rather than a clamped dot, so a face turned away from the sun still differs from the
/// underside: top > sun side > shade side > bottom.
float face_light(const Vec3& normal) {
    constexpr float kShadeFloor = 0.45F;
    const float facing = std::clamp(dot(normal, normalise(kSunDirection)), -1.0F, 1.0F);
    return kShadeFloor + ((1.0F - kShadeFloor) * (0.5F + (0.5F * facing)));
}

/// One face of a prism, shaded by its WORLD normal. The normal comes from the face's own corners,
/// so the turn by `angle` and any slant are already in it; it is flipped to point away from the
/// prism's centre, so it does not depend on the winding.
void emit_shaded_face(std::vector<SceneVertex>& out, const std::array<Vec3, 8>& p,
                      const Vec3& centre, std::size_t a, std::size_t b, std::size_t c,
                      std::size_t d, const Vec3& colour, const UvRect& tile) {
    const Vec3 ab{p.at(b)[0] - p.at(a)[0], p.at(b)[1] - p.at(a)[1], p.at(b)[2] - p.at(a)[2]};
    const Vec3 ac{p.at(c)[0] - p.at(a)[0], p.at(c)[1] - p.at(a)[1], p.at(c)[2] - p.at(a)[2]};
    Vec3 normal = normalise(cross(ab, ac));
    const Vec3 outward{(p.at(a)[0] + p.at(c)[0]) * 0.5F - centre[0],
                       (p.at(a)[1] + p.at(c)[1]) * 0.5F - centre[1],
                       (p.at(a)[2] + p.at(c)[2]) * 0.5F - centre[2]};
    if (dot(normal, outward) < 0.0F) {
        normal = {-normal[0], -normal[1], -normal[2]};
    }
    const float light = face_light(normal);
    const Vec3 shaded{colour[0] * light, colour[1] * light, colour[2] * light};
    emit_tiled_quad(out, p.at(a), p.at(b), p.at(c), p.at(d), shaded, tile);
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

/// A prism whose top is `inset` narrower than its base on each side, so its long sides lean in --
/// an armour plate's chamfer, which a `Prism` (upright sides) cannot be. The base is `s` as given,
/// the top's corners are those of `s` drawn `inset` in from `left` and `right`.
void emit_turned_chamfer(std::vector<SceneVertex>& out, float x, float z, float angle,
                         const Prism& s, float inset, const Vec3& colour, const UvRect& tile,
                         const UvRect& top_tile) {
    std::array<Vec3, 8> p = prism_corners(x, z, angle, s);
    Prism top = s;
    top.left += inset;
    top.right -= inset;
    const std::array<Vec3, 8> q = prism_corners(x, z, angle, top);
    for (const std::size_t corner : {2U, 3U, 6U, 7U}) {
        p.at(corner) = q.at(corner);
    }
    emit_shaded_solid(out, p, colour, tile, top_tile);
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

/// THE FACTORY, a skyline rather than a crate, inside the old box's footprint (0.3 tile either side
/// of the unit) with its hall roof at the old box's 4.5 m, which is what the picker tests against:
/// - the HALL, its front face set back by a shallow recess, with the walls tile on its sides and
///   the roof tile (ribs, vents, hatch) on top;
/// - a PORTAL standing proud of that face to the old front -- two jambs and a lintel framing the
///   painted door, so the door sits in the recess. Each portal piece samples exactly the strip of
///   the wall tile it covers, so the painted front reads on unbroken and only the door steps in;
/// - a lower ANNEX beside the hall and set back from its front, its roof well under the hall's;
/// - a STACK at the annex's rear corner rising well over the hall roof, the silhouette against the
///   sky, with a team-colour band near its top (the fleet's saturated accent, as a barrel's).
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
    const float half_width = kMetresPerTile * 0.3F; ///< the footprint, every part inside it
    const float angle = unit.side == sim::Side::Player ? 0.0F : kPi;
    constexpr float kHallTop = 4.5F;   ///< the picker's housing height
    constexpr float kHallRight = 0.2F; ///< the annex takes the rest of the width
    constexpr float kRecess = 0.15F;   ///< how far the door's face is set back
    // The door's painted frame, in the walls tile's texels: its outer edge is the opening.
    constexpr int kTileTexels = 64;
    constexpr int kOpeningLeft = 4;
    constexpr int kOpeningRight = 52;
    constexpr int kOpeningTop = 13;
    constexpr float kAnnexTop = 2.6F;
    constexpr float kAnnexBack = -0.5F;
    constexpr float kAnnexFront = 0.3F; ///< set back from the portal's face
    constexpr float kStackLeft = 0.3F;
    constexpr float kStackRight = 0.54F;
    constexpr float kStackBack = -0.56F;
    constexpr float kStackFront = -0.32F;
    constexpr float kStackTop = 6.2F;
    constexpr float kBandOverhang = 0.03F; ///< a collar just proud of the stack
    constexpr float kBandBottom = 5.7F;
    constexpr float kBandTop = 5.95F;

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

    emit_turned_prism(out, x, z, angle, box(left, kHallRight, -half_width, face, 0.0F, kHallTop),
                      pale, walls, roof);
    emit_turned_prism(out, x, z, angle,
                      box(across(kOpeningLeft), kHallRight, face, front, 0.0F, lintel_bottom), pale,
                      upright_part(walls_tile, 0, kOpeningTop, kOpeningLeft, kTileTexels));
    emit_turned_prism(
        out, x, z, angle, box(left, across(kOpeningRight), face, front, 0.0F, lintel_bottom), pale,
        upright_part(walls_tile, kOpeningRight, kOpeningTop, kTileTexels, kTileTexels));
    emit_turned_prism(out, x, z, angle, box(left, kHallRight, face, front, lintel_bottom, kHallTop),
                      pale, upright_part(walls_tile, 0, 0, kTileTexels, kOpeningTop), roof);
    emit_turned_prism(out, x, z, angle,
                      box(kHallRight, half_width, kAnnexBack, kAnnexFront, 0.0F, kAnnexTop), pale,
                      walls, roof);
    emit_turned_prism(out, x, z, angle,
                      box(kStackLeft, kStackRight, kStackBack, kStackFront, 0.0F, kStackTop), pale,
                      roof);
    emit_turned_prism(out, x, z, angle,
                      box(kStackLeft - kBandOverhang, kStackRight + kBandOverhang,
                          kStackBack - kBandOverhang, kStackFront + kBandOverhang, kBandBottom,
                          kBandTop),
                      colour, roof);
}

/// A defense turret, built of the same shaded prisms as a tank and all of the `turret_body` metal
/// tile: a square pedestal on the ground turned by `heading` (the turret crawls, and faces its
/// course as a tank does), a housing on top of it, and a barrel from the housing's front face with
/// a thicker muzzle collar AT ITS TIP -- the fleet's convention. The housing, collar and barrel
/// turn together by `aim`. The top of the housing is the old box's 1.8 m, which is what the picker
/// tests against. The pedestal and housing are the pale tint the box was; the collar and barrel
/// carry the full side colour, as a tank's barrel does.
void build_defense_turret(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                          Mark mark, float lift_z, bool lifted, float heading, float aim) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const float half_width = kMetresPerTile * 0.3F;
    constexpr float kPedestalHeight = 1.1F;
    constexpr float kHousingHalfAcross = 0.45F;
    constexpr float kHousingBack = -0.5F;
    constexpr float kHousingFront = 0.45F;
    constexpr float kHousingTop = 1.8F; ///< above the ground
    constexpr float kBarrelHalfAcross = 0.08F;
    constexpr float kBarrelBottom = 1.36F;
    constexpr float kBarrelTop = 1.52F;
    constexpr float kBarrelTip = 2.0F; ///< from the turret's centre, well past the pedestal
    constexpr float kCollarLength = 0.3F;
    constexpr float kCollarHalfAcross = 0.13F;
    constexpr float kCollarBottom = 1.32F;
    constexpr float kCollarTop = 1.56F;

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
    const UvRect metal = atlas_manifest().rect("turret_body");
    emit_turned_box(out, x, z, heading, half_width, -half_width, half_width, ground,
                    ground + kPedestalHeight, pale, metal);
    emit_turned_box(out, x, z, aim, kHousingHalfAcross, kHousingBack, kHousingFront,
                    ground + kPedestalHeight, ground + kHousingTop, pale, metal);
    emit_turned_box(out, x, z, aim, kCollarHalfAcross, kBarrelTip - kCollarLength, kBarrelTip,
                    ground + kCollarBottom, ground + kCollarTop, colour, metal);
    emit_turned_box(out, x, z, aim, kBarrelHalfAcross, kHousingFront, kBarrelTip - kCollarLength,
                    ground + kBarrelBottom, ground + kBarrelTop, colour, metal);
}

/// THE TANK'S HULL, shared by the tank and the rocket: two dark tracks with a camo fender over
/// each, a shelf a little wider than the track under it, and a hull between them whose raised
/// fighting compartment meets the ground in a sloped front glacis and steps down aft to a lower
/// engine deck with a dark grille band -- seven prisms, all turned by `heading`, in that order:
/// tracks, the compartment, the engine deck, the grille, fenders. `colour` is the marked side
/// colour (the tracks and grille), `camo_colour` its pale tint (the rest).
void emit_tank_hull(std::vector<SceneVertex>& out, float x, float z, float ground, float heading,
                    const Vec3& colour, const Vec3& camo_colour, const UvRect& hull_tile,
                    const UvRect& tracks_tile) {
    const float half_width = kMetresPerTile * 0.3F; ///< the outer edge of a fender
    constexpr float kTrackInner = 0.7F;             ///< of half_width; the hull fills the rest
    constexpr float kTrackOuter = 0.95F;            ///< of half_width; the fender overhangs it
    constexpr float kTrackHeight = 0.4F;
    constexpr float kTrackBack = -1.3F; ///< metres along the heading from the centre
    constexpr float kTrackFront = 1.3F;
    constexpr float kHullBack = -1.2F;
    constexpr float kHullFront = 1.2F;  ///< the foot of the glacis
    constexpr float kGlacisTop = 0.55F; ///< where the glacis meets the compartment roof
    constexpr float kDeckStep = -0.45F; ///< where the compartment steps down to the engine deck
    constexpr float kCompartmentTop = 0.85F;
    constexpr float kEngineDeckTop = 0.62F;
    constexpr float kDeckTuck = 0.05F; ///< the engine deck runs on inside the compartment
    constexpr float kGrilleHalfAcross = 0.28F;
    constexpr float kGrilleBack = -1.1F;
    constexpr float kGrilleFront = -0.55F;
    constexpr float kGrilleThickness = 0.03F; ///< standing just proud of the deck
    constexpr float kFenderInner = 0.65F;     ///< of half_width: just inside the hull's side
    constexpr float kFenderBottom = 0.38F;    ///< a little into the track, so its top is hidden
    constexpr float kFenderTop = 0.44F;

    const float inner = half_width * kTrackInner;
    for (const float side : {-1.0F, 1.0F}) {
        const float a = side * inner;
        const float b = side * half_width * kTrackOuter;
        const Prism track{std::min(a, b), std::max(a, b),        kTrackBack,
                          kTrackBack,     kTrackFront,           kTrackFront,
                          ground,         ground + kTrackHeight, ground + kTrackHeight};
        emit_turned_prism(out, x, z, heading, track, colour, tracks_tile);
    }
    // The fighting compartment: its front face leans back from the foot of the glacis to the roof.
    const Prism hull{-inner,
                     inner,
                     kDeckStep,
                     kDeckStep,
                     kHullFront,
                     kGlacisTop,
                     ground,
                     ground + kCompartmentTop,
                     ground + kCompartmentTop};
    emit_turned_prism(out, x, z, heading, hull, camo_colour, hull_tile);
    emit_turned_box(out, x, z, heading, inner, kHullBack, kDeckStep + kDeckTuck, ground,
                    ground + kEngineDeckTop, camo_colour, hull_tile);
    const float deck = ground + kEngineDeckTop;
    emit_turned_box(out, x, z, heading, kGrilleHalfAcross, kGrilleBack, kGrilleFront, deck,
                    deck + kGrilleThickness, colour, tracks_tile);
    for (const float side : {-1.0F, 1.0F}) {
        const float a = side * half_width * kFenderInner;
        const float b = side * half_width;
        const Prism fender{std::min(a, b),
                           std::max(a, b),
                           kTrackBack,
                           kTrackBack,
                           kTrackFront,
                           kTrackFront,
                           ground + kFenderBottom,
                           ground + kFenderTop,
                           ground + kFenderTop};
        emit_turned_prism(out, x, z, heading, fender, camo_colour, hull_tile);
    }
}

/// A tank, abstracted from a side-on battle tank and built only of flat-shaded prisms: the hull
/// (`emit_tank_hull`), a squat turret on the compartment roof with a rear bustle over the engine
/// deck, a long thin barrel from the turret's front face with a thicker muzzle collar, and a flat
/// team-colour panel on the turret roof. The hull, tracks and fenders are turned by `heading`, the
/// turret group by `heading + turret`. Lateral extent stays the tank's 0.3 tile half-width -- the
/// outer edge of a fender, with the track tucked just inside it -- and the barrel is always the
/// last prism.
void build_facing_tank(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                       Mark mark, float lift_z, bool lifted, float heading, float turret) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    // The tracks and grille are dark, with their links, because the TRACK TILE is, and the hull,
    // engine deck, fenders, turret and bustle wear the camo because their tile does: the vertex
    // keeps the side colour, the selection tint and the face light, and the texel multiplies them.
    // Which of its team's three camo tiles is this tank's -- one pattern for the whole tank. The
    // roof panel wears the plain turret metal.
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect panel_tile = atlas.rect("tank_turret");
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    constexpr float kTurretBase = 0.8F; ///< above the ground; sunk into the compartment roof
    constexpr float kTurretHeight = 0.4F;
    constexpr float kTurretHalfAcross = 0.36F;
    constexpr float kTurretCentre = -0.15F; ///< set back from the hull's centre
    constexpr float kTurretHalfLength = 0.5F;
    constexpr float kBustleLength = 0.25F;
    constexpr float kBustleHalfAcross = 0.25F;
    constexpr float kBustleBottom = 0.05F; ///< above the turret's base
    constexpr float kBustleTop = 0.3F;
    constexpr float kPanelHalfAcross = 0.22F; ///< of the turret's 0.36: camo shows either side
    constexpr float kPanelBack = -0.4F;       ///< along the turret from its centre
    constexpr float kPanelFront = 0.25F;
    constexpr float kPanelThickness = 0.03F; ///< standing just proud of the roof
    constexpr float kBarrelHalfAcross = 0.06F;
    constexpr float kBarrelBottom = 0.18F; ///< above the turret's base
    constexpr float kBarrelTop = 0.3F;
    constexpr float kBarrelTip = 2.0F; ///< from the tank's centre, well past the hull front
    constexpr float kCollarLength = 0.3F;
    constexpr float kCollarHalfAcross = 0.1F;
    constexpr float kCollarBottom = 0.14F;
    constexpr float kCollarTop = 0.34F;

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

    emit_tank_hull(out, x, z, ground, heading, colour, camo_colour, hull_tile, tracks_tile);

    // The turret group shares one frame at the turret's centre; the barrel starts at its front
    // face and is measured from the tank's centre along the same axis, so it points where the
    // turret looks. The turret and bustle are camo -- from the driver's seat they are most of the
    // tank -- and the team reads from the two saturated accents: the roof panel and the barrel.
    const float turret_angle = heading + turret;
    const float turret_y0 = ground + kTurretBase;
    const std::array<float, 2> turret_at = rotate_offset(x, z, 0.0F, kTurretCentre, turret_angle);
    const Prism turret_box{
        -kTurretHalfAcross, kTurretHalfAcross,         -kTurretHalfLength,
        -kTurretHalfLength, kTurretHalfLength,         kTurretHalfLength,
        turret_y0,          turret_y0 + kTurretHeight, turret_y0 + kTurretHeight};
    emit_turned_prism(out, turret_at.at(0), turret_at.at(1), turret_angle, turret_box, camo_colour,
                      hull_tile);
    emit_turned_box(out, turret_at.at(0), turret_at.at(1), turret_angle, kBustleHalfAcross,
                    -kTurretHalfLength - kBustleLength, -kTurretHalfLength,
                    turret_y0 + kBustleBottom, turret_y0 + kBustleTop, camo_colour, hull_tile);
    const float roof = turret_y0 + kTurretHeight;
    emit_turned_box(out, turret_at.at(0), turret_at.at(1), turret_angle, kPanelHalfAcross,
                    kPanelBack, kPanelFront, roof, roof + kPanelThickness, colour, panel_tile);
    const float turret_front = kTurretCentre + kTurretHalfLength;
    emit_turned_box(out, x, z, turret_angle, kCollarHalfAcross, kBarrelTip - kCollarLength,
                    kBarrelTip, turret_y0 + kCollarBottom, turret_y0 + kCollarTop, colour,
                    barrel_tile);
    emit_turned_box(out, x, z, turret_angle, kBarrelHalfAcross, turret_front,
                    kBarrelTip - kCollarLength, turret_y0 + kBarrelBottom, turret_y0 + kBarrelTop,
                    colour, barrel_tile);
}

/// THE SCAVENGER (C2). It is built on the TANK's own hull -- tracks, compartment, fenders, engine
/// deck -- with the turret ring left empty and a low sloped cab where the turret would sit: an
/// unarmed hauler has no gun to show, and a hull cut down from the tank is what it is. It is turned
/// by `heading` alone; there is nothing on it that aims. The cab and deck wear the team's camo (the
/// fleet's tiles, as a tank does) and the cab's roof panel the plain turret metal, so the colour
/// language stays the army's. A PLACEHOLDER look: the kind's own machine is a later slice, and this
/// one exists so that a new kind is not an invisible one.
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
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect panel_tile = atlas.rect("tank_turret");
    constexpr float kCabBase = 0.75F; ///< above the ground: the deck it stands on
    constexpr float kCabFront = 0.1F; ///< along the hull from its centre
    constexpr float kCabBack = -0.5F;
    constexpr float kCabHalfAcross = 0.28F;
    constexpr float kCabRoof = kCabBase + 0.3F;
    constexpr float kRoofThickness = 0.03F;

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

    emit_tank_hull(out, x, z, ground, heading, colour, camo_colour, hull_tile, tracks_tile);
    emit_turned_box(out, x, z, heading, kCabHalfAcross, kCabBack, kCabFront, ground + kCabBase,
                    ground + kCabRoof, camo_colour, hull_tile);
    emit_turned_box(out, x, z, heading, kCabHalfAcross, kCabBack, kCabFront, ground + kCabRoof,
                    ground + kCabRoof + kRoofThickness, colour, panel_tile);
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
void build_recycler(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
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
    const float half_width = kMetresPerTile * 0.3F; ///< the footprint, every part inside it
    constexpr float kHalfLength = 1.3F;             ///< the tank's, fore and aft
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
    constexpr float kBodyChamfer = 0.04F;
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
    constexpr float kCabChamfer = 0.07F;
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
        emit_turned_prism(out, x, z, heading, s, tint, tile);
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
        part(plate(track[0], track[1], -kTrackGround, -kTrackRun, kTrackGround, kTrackRun, 0.0F,
                   kTrackTop),
             colour, tracks_tile);
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
        part(plate(skirt[0], skirt[1], -kSkirtFoot, -kSkirtShoulder, kSkirtFoot, kSkirtShoulder,
                   kSkirtBottom, kFender),
             pale, hull_tile);
    }
    part(box(-kBellyHalfAcross, kBellyHalfAcross, kBellyBack, kBellyFront, kBellyBottom, kTrackTop),
         colour, tracks_tile);
    part(box(kPanelInner, kPanelOuter, kPanelBack, kPanelFront, kPanelBottom, kPanelTop), colour,
         tracks_tile);
    part(box(kStripInner, half_width, kPanelBack + kStripInset, kPanelFront - kStripInset,
             kStripBottom, kStripTop),
         colour, silver_tile);

    // The body: its rear face leans out from its foot to the deck's overhang.
    emit_turned_chamfer(out, x, z, heading,
                        plate(-kBodyHalfAcross, kBodyHalfAcross, kBodyBack, -kHalfLength,
                              kBodyFront, kBodyFront, kBodyBottom, kDeck),
                        kBodyChamfer, pale, hull_tile, hull_tile);
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

    emit_turned_chamfer(out, x, z, heading,
                        plate(-kCabHalfAcross, kCabHalfAcross, kCabBack, kCabBackTop, kCabFoot,
                              kCabBrow, kCabBottom, kCabRoof),
                        kCabChamfer, pale, hull_tile, hatch);
    part(plate(-kBandHalfAcross, kBandHalfAcross, kBandBack, kBandBack,
               raked(kCabFoot, kCabBrow, kCabBottom, kCabRoof, kBandBottom) + kProud,
               raked(kCabFoot, kCabBrow, kCabBottom, kCabRoof, kBandTop) + kProud, kBandBottom,
               kBandTop),
         colour, tracks_tile);

    part(box(kRadiatorInner, kRadiatorOuter, kRadiatorBack, kRadiatorFront, kFender, kRadiatorTop),
         pale, ribs);
    part(box(kBoomInner, kBoomOuter, kBoomBack, kBoomFront, kFender, kBoomTop), pale, silver_tile);
    part(box(kHoseInner, kHoseOuter, kHoseBack, kHoseFront, kBoomTop, kHoseTop), colour,
         tracks_tile);
    part(box(kElbowInner, kElbowOuter, kElbowBack, kElbowFront, kFender, kElbowTop), pale,
         steel_tile);
    part(plate(kUpperInner, kUpperOuter, kUpperTip, kUpperTipTop, kElbowBack, kElbowBack,
               kUpperBottom, kUpperTop),
         pale, silver_tile);
}

/// THE HOVER TANK, built to read as NOT a tank at a glance: no track skirts at all, a low wedge
/// that tapers in plan to a narrow nose and slopes down to it in profile, riding on a bright band
/// of lift -- the skirt -- with a gap of air under it. A small turret sits on the deck ahead of
/// the wedge's tallest point with a short barrel over the nose, and a dorsal fin stands at the
/// tail. The wedge and turret wear the team's camo (the fleet's own tiles, as a tank does); the
/// barrel and the fin are the saturated side-colour accents, as the tank's barrel and roof panel
/// are. The skirt is RAW colour: not lit, not marked, the glow of the lift. The hull and skirt are
/// turned by `heading`, the turret and barrel by `heading + turret`.
void build_hover_tank(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
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
    const UvRect skirt_tile = atlas.rect("hover_skirt");
    const UvRect fin_tile = atlas.rect("tank_turret");
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    const float tail_half = kMetresPerTile * 0.3F; ///< the tank's own half-width, at the tail
    constexpr float kNoseHalf = 0.12F;             ///< the wedge's half-width at its nose
    constexpr float kTail = -1.3F;                 ///< metres along the heading from the centre
    constexpr float kNose = 1.5F;
    constexpr float kHullBottom = 0.4F; ///< above the ground: the skirt and the air under it
    constexpr float kTailTop = 0.85F;   ///< the deck at the tail, its tallest point
    constexpr float kNoseTop = 0.5F;    ///< the deck at the nose, just over the hull's floor
    constexpr float kSkirtBottom = 0.15F;
    constexpr float kSkirtInset = 0.9F; ///< of the hull's outline: tucked under the hull
    constexpr float kTurretCentre = 0.1F;
    constexpr float kTurretBase = 0.6F; ///< sunk into the sloping deck
    constexpr float kTurretHeight = 0.3F;
    constexpr float kTurretHalfAcross = 0.26F;
    constexpr float kTurretHalfLength = 0.32F;
    constexpr float kBarrelHalfAcross = 0.05F;
    constexpr float kBarrelBottom = 0.1F; ///< above the turret's base
    constexpr float kBarrelTop = 0.2F;
    constexpr float kBarrelTip = 1.8F; ///< from the hover tank's centre, past the nose
    constexpr float kFinHalfAcross = 0.04F;
    constexpr float kFinBack = -1.25F;
    constexpr float kFinFront = -0.75F;
    constexpr float kFinTop = 1.1F; ///< the model's highest point: the health bar sits over it

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

    const auto corner = [&](float dx, float dz, float y) {
        const std::array<float, 2> at = rotate_offset(x, z, dx, dz, heading);
        return Vec3{at.at(0), y, at.at(1)};
    };
    // The wedge, in prism corner order: tail-bottom left/right, tail-top right/left, then the same
    // at the nose -- narrower, and with its deck brought down to just over the floor.
    const float floor = ground + kHullBottom;
    emit_shaded_solid(
        out,
        {{corner(-tail_half, kTail, floor), corner(tail_half, kTail, floor),
          corner(tail_half, kTail, ground + kTailTop), corner(-tail_half, kTail, ground + kTailTop),
          corner(-kNoseHalf, kNose, floor), corner(kNoseHalf, kNose, floor),
          corner(kNoseHalf, kNose, ground + kNoseTop),
          corner(-kNoseHalf, kNose, ground + kNoseTop)}},
        camo_colour, hull_tile, hull_tile);

    // The skirt: one band quad on each of the wedge's four sides, under the hull's floor.
    const float skirt_tail_half = tail_half * kSkirtInset;
    const float skirt_nose_half = kNoseHalf * kSkirtInset;
    const float skirt_tail = kTail * kSkirtInset;
    const float skirt_nose = kNose * kSkirtInset;
    const float top = floor;
    const float bottom = ground + kSkirtBottom;
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

    // The turret group shares one frame at the turret's centre, as the tank's does.
    const float turret_angle = heading + turret;
    const float turret_y0 = ground + kTurretBase;
    const std::array<float, 2> turret_at = rotate_offset(x, z, 0.0F, kTurretCentre, turret_angle);
    emit_turned_box(out, turret_at.at(0), turret_at.at(1), turret_angle, kTurretHalfAcross,
                    -kTurretHalfLength, kTurretHalfLength, turret_y0, turret_y0 + kTurretHeight,
                    camo_colour, hull_tile);
    emit_turned_box(out, x, z, turret_angle, kBarrelHalfAcross, kTurretCentre + kTurretHalfLength,
                    kBarrelTip, turret_y0 + kBarrelBottom, turret_y0 + kBarrelTop, colour,
                    barrel_tile);
    // The fin rides the hull, so it turns with the heading, from the deck at the tail up.
    emit_turned_box(out, x, z, heading, kFinHalfAcross, kFinBack, kFinFront, ground + kTailTop,
                    ground + kFinTop, colour, fin_tile);
}

/// A solid tube in a frame turned by `angle` about (x, z), its axis in the frame's upright plane:
/// from the breech at `breech` along the heading and `breech_y` up, `length` long, raised
/// `elevation` radians from level. `half_across` is its half-width, `half_thick` its half-depth
/// square to the axis. Corners in prism order, so it shades as any other part does.
void emit_raised_tube(std::vector<SceneVertex>& out, float x, float z, float angle, float breech,
                      float breech_y, float length, float elevation, float half_across,
                      float half_thick, const Vec3& colour, const UvRect& tile) {
    const float along = std::cos(elevation);
    const float up = std::sin(elevation);
    const auto corner = [&](float dx, float distance, float side) {
        const float dz = breech + (distance * along) - (side * half_thick * up);
        const float y = breech_y + (distance * up) + (side * half_thick * along);
        const std::array<float, 2> at = rotate_offset(x, z, dx, dz, angle);
        return Vec3{at.at(0), y, at.at(1)};
    };
    emit_shaded_solid(out,
                      {{corner(-half_across, 0.0F, -1.0F), corner(half_across, 0.0F, -1.0F),
                        corner(half_across, 0.0F, 1.0F), corner(-half_across, 0.0F, 1.0F),
                        corner(-half_across, length, -1.0F), corner(half_across, length, -1.0F),
                        corner(half_across, length, 1.0F), corner(-half_across, length, 1.0F)}},
                      colour, tile, tile);
}

/// THE MORTAR: a low wide base box on the ground, turned by `heading`; a small mount on it; and a
/// SHORT STUBBY TUBE angled up 45 degrees -- it lobs -- with a thicker collar at its mouth (the
/// fleet's convention), all three turned by `gun_yaw`. The base and mount wear the team's camo;
/// the tube and collar are the saturated side-colour accents, as a tank's barrel is. Its top stays
/// under the health bar's anchor.
void build_mortar(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
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
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    const float half_width = kMetresPerTile * 0.3F; ///< a tank's: the base is as wide as one
    constexpr float kBaseHalfLength = 0.7F;
    constexpr float kBaseTop = 0.4F;
    constexpr float kMountHalfAcross = 0.22F;
    constexpr float kMountHalfLength = 0.25F;
    constexpr float kMountTop = 0.58F;
    constexpr float kBreech = -0.15F; ///< along the aim from the centre: the tube leans forward
    constexpr float kBreechHeight = 0.5F;
    constexpr float kElevation = 0.785398F; ///< 45 degrees: a lob, not a flat shot
    constexpr float kTubeLength = 0.55F;
    constexpr float kTubeHalf = 0.12F; ///< twice a tank barrel's: stubby
    constexpr float kCollarLength = 0.2F;
    constexpr float kCollarHalf = 0.16F;

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

    emit_turned_box(out, x, z, heading, half_width, -kBaseHalfLength, kBaseHalfLength, ground,
                    ground + kBaseTop, camo_colour, hull_tile);
    emit_turned_box(out, x, z, aim, kMountHalfAcross, -kMountHalfLength, kMountHalfLength,
                    ground + kBaseTop, ground + kMountTop, camo_colour, hull_tile);
    emit_raised_tube(out, x, z, aim, kBreech, ground + kBreechHeight, kTubeLength, kElevation,
                     kTubeHalf, kTubeHalf, colour, barrel_tile);
    const float mouth = kTubeLength;
    emit_raised_tube(out, x, z, aim, kBreech + (mouth * std::cos(kElevation)),
                     ground + kBreechHeight + (mouth * std::sin(kElevation)), kCollarLength,
                     kElevation, kCollarHalf, kCollarHalf, colour, barrel_tile);
}

/// THE ROCKET: the tank's hull (`emit_tank_hull`, the same seven prisms in the same camo) with,
/// where the turret and barrel would be, a small mount and a broad RECTANGULAR LAUNCHER RACK on
/// it -- no gun. The mount wears the camo; the rack is the side-colour accent on the turret's
/// plain metal, as a tank's roof panel is. Mount and rack turn by `heading + turret`. The rack
/// stays inside the hull's length and under the health bar's anchor.
void build_rocket(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
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
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect rack_tile = atlas.rect("tank_turret");
    constexpr float kMountHalfAcross = 0.2F;
    constexpr float kMountCentre = -0.15F; ///< where a tank's turret stands
    constexpr float kMountHalfLength = 0.25F;
    constexpr float kMountBottom = 0.8F; ///< sunk into the compartment roof
    constexpr float kMountTop = 0.98F;
    constexpr float kRackHalfAcross = 0.42F;
    constexpr float kRackBack = -0.75F; ///< along the aim from the centre
    constexpr float kRackFront = 0.55F;
    constexpr float kRackBottom = 0.95F;
    constexpr float kRackTop = 1.18F;

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

    emit_tank_hull(out, x, z, ground, heading, colour, camo_colour, hull_tile, tracks_tile);
    const float rack_angle = heading + turret;
    emit_turned_box(out, x, z, rack_angle, kMountHalfAcross, kMountCentre - kMountHalfLength,
                    kMountCentre + kMountHalfLength, ground + kMountBottom, ground + kMountTop,
                    camo_colour, hull_tile);
    emit_turned_box(out, x, z, rack_angle, kRackHalfAcross, kRackBack, kRackFront,
                    ground + kRackBottom, ground + kRackTop, colour, rack_tile);
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
                   const Vec3& colour) {
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
    emit_quad(out, p[0], p[1], p[2], p[3], colour); // near
    emit_quad(out, p[5], p[4], p[7], p[6], colour); // far
    emit_quad(out, p[4], p[0], p[3], p[7], colour); // left
    emit_quad(out, p[1], p[5], p[6], p[2], colour); // right
    emit_quad(out, p[3], p[2], p[6], p[7], colour); // top
    emit_quad(out, p[4], p[5], p[1], p[0], colour); // bottom
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

/// THE DEPLOY PREVIEW (DEPLOY): the footprint the armed deploy would build, as its tile grid lying
/// on the terrain -- the outline and the lines between its tiles, so it reads as the 2x2 it is.
/// Each stroke runs one tile edge, from corner height to corner height, so it follows the ground.
/// There is no rotation to draw: the footprint is a square, and a square turned a quarter is
/// itself.
void append_deploy_preview(std::vector<SceneVertex>& out, const SceneOptions& options) {
    if (!options.deploy_preview.has_value()) {
        return;
    }
    const sim::Footprint site = *options.deploy_preview;
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
                  Vec3{a.at(0) + across_x, a.at(1), a.at(2) + across_z}, kDeployPreviewColour);
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
/// How thick each bar is, as a fraction of the half-size.
constexpr float kTriangleBaseFraction = 0.85F;
constexpr float kTriangleTurn = 2.35619449F; ///< 135 degrees: where a triangle's base corners sit
constexpr float kReticleBarFraction = 0.22F;
/// The crosshair's shape, as fractions of the half-size: where the ticks start (the open centre is
/// everything inside), the dot's half-width, and how far a full pulse pushes the ticks out and how
/// much it swells the dot.
constexpr float kReticleGapFraction = 0.35F;
constexpr float kReticleDotFraction = 0.09F;
/// THE ROCKET'S RETICLE (M1): the gun's family read heavier -- a dot 3.3x the gun's (the mortar's
/// lone dot is this size too), ticks reaching 0.75 of the half-size instead of 1 (0.4 long, not
/// 0.65) and nearly twice as thick.
constexpr float kRocketDotFraction = 0.3F;
constexpr float kRocketReachFraction = 0.75F;
constexpr float kRocketBarFraction = 0.4F;
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
constexpr float kRadarOutlineFraction = 0.5F; ///< an outline's bar thickness, of its half-size

std::array<float, 3> radar_colour(RadarTone tone) {
    switch (tone) {
    case RadarTone::You:
        return {0.55F, 1.0F, 0.55F};
    case RadarTone::Yours:
        return {0.30F, 0.85F, 0.35F};
    case RadarTone::Hostile:
        return {1.0F, 0.30F, 0.25F};
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
constexpr float kLetterCellPixels = 2.0F;
constexpr float kLetterPlatePadPixels = 2.0F;
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

/// The radar's marks for this frame, in panel pixels: the ONE call both the radar and the command
/// menu's leader line make, so the line's dot lands on the blip the radar drew and not on a second
/// opinion of where the unit is.
[[nodiscard]] std::vector<RadarMark> frame_radar_marks(const SceneOptions& options,
                                                       const sim::Sim& sim) {
    return radar_marks(sim.units(), options.viewer_unit, options.camera.yaw,
                       radar_panel(options.extent), viewer_radar_radius(sim, options.viewer_unit));
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
                          const std::array<float, 3>& rgb) {
        emit_quad(out, at(distance, ax, ay), at(distance, bx, ay), at(distance, bx, by),
                  at(distance, ax, by), rgb);
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
                        options.radar_mesh_step_tiles)) {
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

    for (const RadarMark& mark : marks) {
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
            rect(marks_at, left, top, right, bottom, rgb);
            continue;
        }
        if (mark.shape == RadarShape::Hollow) {
            const float bar = mark.half * kRadarOutlineFraction;
            rect(marks_at, left, top, right, top + bar, rgb);
            rect(marks_at, left, bottom - bar, right, bottom, rgb);
            rect(marks_at, left, top + bar, left + bar, bottom - bar, rgb);
            rect(marks_at, right - bar, top + bar, right, bottom - bar, rgb);
            continue;
        }
        // A triangle pointing along its angle: the apex, and two corners 135 degrees either side.
        const auto corner = [&](float radius, float turn) {
            const float angle = mark.angle + turn;
            return std::array<float, 2>{centre_x + (radius * std::sin(angle)),
                                        centre_y - (radius * std::cos(angle))};
        };
        const std::array<float, 2> apex = corner(mark.half, 0.0F);
        const std::array<float, 2> side_a
            = corner(mark.half * kTriangleBaseFraction, kTriangleTurn);
        const std::array<float, 2> side_b
            = corner(mark.half * kTriangleBaseFraction, -kTriangleTurn);
        out.push_back(SceneVertex{at(marks_at, apex.at(0), apex.at(1)), rgb});
        out.push_back(SceneVertex{at(marks_at, side_a.at(0), side_a.at(1)), rgb});
        out.push_back(SceneVertex{at(marks_at, side_b.at(0), side_b.at(1)), rgb});
    }

    // THE NAV MARKS: `append_nav_glyph`'s plus, nearer than the compass so a mark pinned to the rim
    // is not hidden by a letter. BZ2 shows a placed nav point as a tiny plus in its radar, no
    // bigger than the unit blips, and no number: the numbers are the nav menu's (`~`).
    if (options.nav_marks != nullptr) {
        constexpr float kNavRatio = 0.87F;
        for (const RadarNav& nav :
             radar_nav_marks(sim.units(), options.viewer_unit, options.nav_marks->marks(),
                             options.camera.yaw, panel, reach)) {
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

/// One small health bar over each living field unit in view (not the factory, not a turret), on the
/// radar's HUD layers: a near-black outline, a dark track and a fill in the reticle's colours
/// (green yours, red hostile), centred over the tank and sitting above its projected top. Fixed
/// size, whatever the range. The viewer's own tank gets none -- its hull is in the HUD panel -- and
/// with no viewer there are no bars at all, which keeps a frame with no player in it as it was.
/// Always `kUnitBarVertexCount` vertices per bar, in sim order; a bar with a zero-width fill stays
/// whole.
void append_unit_bars(std::vector<SceneVertex>& out, const SceneOptions& options,
                      const sim::Sim& sim) {
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
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto at = [&](float px, float py) {
            const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };
    constexpr std::array<float, 3> kOutline{0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 3> kPlate{0.10F, 0.10F, 0.12F};

    const int centre_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int centre_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    for (const sim::Unit& unit : sim.units()) {
        // Every unit in the field wears one -- tank, hover tank, mortar, rocket alike. The
        // structures do not: the factory is the base, and a turret wears its DEPLOYING plate there.
        const bool structure
            = unit.kind == sim::UnitKind::Factory || unit.kind == sim::UnitKind::Turret;
        if (!unit.alive || structure || unit.id == options.viewer_unit) {
            continue;
        }
        // Only a unit that is drawn gets a bar: the same terrain window the units use.
        if (std::abs(unit.x.floor_int() - centre_x) > options.radius_tiles
            || std::abs(unit.y.floor_int() - centre_y) > options.radius_tiles) {
            continue;
        }
        const bool hovering = options.hover.has_value() && options.hover->unit == unit.id;
        const float ground
            = hovering ? options.hover->z : ground_metres(options.seed, unit.x, unit.y);
        const Projected top = project(
            options.camera, options.extent,
            static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile,
            ground + kTankTopMetres,
            static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile);
        if (top.depth <= math::kNearPlane || top.x < 0.0F || top.x > frame_w || top.y < 0.0F
            || top.y > frame_h) {
            continue; // behind the eye, or off the frame: not visible
        }
        const float left = top.x - (width * 0.5F);
        const float bottom = top.y - gap - outline;
        const float track_top = bottom - height;
        rect(marks_at * kHudBorderRatio, left - outline, track_top - outline,
             left + width + outline, bottom + outline, kOutline);
        rect(marks_at * kHudFaceRatio, left, track_top, left + width, bottom, kPlate);
        const std::uint32_t max_hp = sim::spec_of(unit.kind).hp;
        const std::uint32_t hp = std::min<std::uint32_t>(unit.hp, max_hp);
        const float share = max_hp > 0 ? static_cast<float>(hp) / static_cast<float>(max_hp) : 0.0F;
        rect(marks_at, left, track_top, left + (width * share), bottom,
             unit.side == options.viewer_side ? kReticleOwn : kReticleHostile);
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

/// The HUD's 5x5 capitals and digits: the DEPLOYING plate's letters, the weapon names and the
/// ladder's distances. Rows top to bottom, bit 4 left; anything else is blank.
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

    const int centre_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int centre_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.kind != sim::UnitKind::Turret || unit.side != options.viewer_side
            || sim::is_mounted(unit)) {
            continue;
        }
        if (std::abs(unit.x.floor_int() - centre_x) > options.radius_tiles
            || std::abs(unit.y.floor_int() - centre_y) > options.radius_tiles) {
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

/// THE WEAPON'S HUD (M1), in frame pixels on the HUD layer like the DEPLOYING plate, scaled by the
/// frame's height (the px below are at 720). Both hang off the aim point and come BEFORE the
/// reticle, so the reticle stays the tail of the vertex list.
///  * The label, when `show_weapon` asks: the weapon's name in white 5x5 capitals, cell 4.8 px
///    (24 px tall letters), on a black plate with 4.8 px of padding, its left edge 50 px right of
///    the aim point and centred on it.
///  * The mortar's RANGE LADDER: `kMortarRungCount` rungs under the aim point, the first 54 px
///    below it and 36 px apart (the last 234 px below). A rung is a bar 50 px wide and 4 px thick
///    in white; the LIT rung -- the band the view's pitch is in -- is 80 px wide, 6.5 px thick and
///    yellow. Each rung sits in a 2 px black outline, and to its right, on a black plate, is its
///    level-ground landing distance in tiles to a tenth (`mortar_landing_tiles`), digits 4 px a
///    cell (20 px tall), white, or yellow on the lit rung.
constexpr float kHudPerFrameHeight = 1.0F / 720.0F;
constexpr float kLabelGapPixels = 50.0F;
constexpr float kRungFirstPixels = 54.0F;
constexpr float kRungStepPixels = 36.0F;
constexpr float kRungHalfWidthPixels = 25.0F;
constexpr float kRungLitHalfWidthPixels = 40.0F;
constexpr float kRungHalfThickPixels = 2.0F;
constexpr float kRungLitHalfThickPixels = 3.25F;
constexpr float kRungOutlinePixels = 2.0F;
constexpr float kRungLabelGapPixels = 10.0F;
constexpr float kRungCellPixels = 4.0F;
constexpr float kRungPadPixels = 3.0F;
constexpr std::array<float, 3> kRungInk{0.95F, 0.95F, 0.95F};
constexpr std::array<float, 3> kRungLitInk{1.0F, 0.85F, 0.10F};

void append_weapon_hud(std::vector<SceneVertex>& out, const SceneOptions& options) {
    if (!options.aim.has_value()) {
        return;
    }
    const ReticleShape shape = reticle_for(options.weapon, options.camera.pitch);
    if (!options.show_weapon && shape.kind != ReticleKind::Ladder) {
        return;
    }
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
    constexpr std::array<float, 3> kPlate{0.0F, 0.0F, 0.0F};
    // `word` with its top-left at (left, top), `cell` px a cell, on a plate `pad` px wider all
    // round: the plate, then the ink.
    const auto text = [&](std::string_view word, float left, float top, float cell, float pad,
                          const std::array<float, 3>& ink) {
        const float width = ((static_cast<float>(word.size()) * 6.0F) - 1.0F) * cell;
        rect(marks_at * kHudFaceRatio, left - pad, top - pad, left + width + pad,
             top + (5.0F * cell) + pad, kPlate);
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
                    rect(marks_at, x, y, x + cell, y + cell, ink);
                }
            }
        }
    };
    const float aim_x = options.aim->x;
    const float aim_y = options.aim->y;

    if (options.show_weapon) {
        const float cell = static_cast<float>(options.extent.height) * kMountCellFraction;
        const float pad = static_cast<float>(options.extent.height) * kMountPadFraction;
        text(weapon_name(options.weapon), aim_x + (kLabelGapPixels * px), aim_y - (2.5F * cell),
             cell, pad, kRungInk);
    }
    if (shape.kind != ReticleKind::Ladder) {
        return;
    }
    for (std::size_t rung = 0; rung < kMortarRungCount; ++rung) {
        const bool lit = rung == shape.rung;
        const float y
            = aim_y + ((kRungFirstPixels + (static_cast<float>(rung) * kRungStepPixels)) * px);
        const float half_w = (lit ? kRungLitHalfWidthPixels : kRungHalfWidthPixels) * px;
        const float half_t = (lit ? kRungLitHalfThickPixels : kRungHalfThickPixels) * px;
        const float edge = kRungOutlinePixels * px;
        const std::array<float, 3>& ink = lit ? kRungLitInk : kRungInk;
        rect(marks_at * kHudBorderRatio, aim_x - half_w - edge, y - half_t - edge,
             aim_x + half_w + edge, y + half_t + edge, kPlate);
        rect(marks_at, aim_x - half_w, y - half_t, aim_x + half_w, y + half_t, ink);
        // The distance to a tenth of a tile: "6.9", and "13.9" once it passes ten.
        const int tenths = static_cast<int>(
            std::lround(mortar_landing_tiles(kMortarRungPitches.at(rung)) * 10.0F));
        std::array<char, 4> digits{};
        std::size_t count = 0;
        if (tenths >= 100) {
            digits.at(count++) = static_cast<char>('0' + ((tenths / 100) % 10));
        }
        digits.at(count++) = static_cast<char>('0' + ((tenths / 10) % 10));
        digits.at(count++) = '.';
        digits.at(count++) = static_cast<char>('0' + (tenths % 10));
        const float cell = kRungCellPixels * px;
        text(std::string_view{digits.data(), count},
             aim_x + half_w + edge + ((kRungLabelGapPixels + kRungPadPixels) * px),
             y - (2.5F * cell), cell, kRungPadPixels * px, ink);
    }
}

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
/// remaster, so it is a fixed pixel font that ignores the resolution. Top row: the key, and how
/// many vehicles the group holds beside it; then the kind (cell 2.0, or smaller for a name that
/// would not fit the plate's inside); then a 3 px bar with ONE SEGMENT PER LIVING VEHICLE, in
/// the group's spawn order, 1 px apart (BAR-LOOK, HIS RULING 2026-10-24: "They vary in COLOUR, not
/// length"). The segments divide the slot's inside equally and never shorten with damage; each is
/// coloured by its own vehicle's hit points on `bar_segments`'s ramp, bright green through yellow
/// and red to a near-black that still reads against the black plate. A vehicle that dies takes
/// its segment with it and the rest re-divide the same width. `~` says how many nav marks there
/// are and has no bar; the building on `2` has one segment and no count. Under the row,
/// `options.notice` in 10 px yellow capitals.
///
/// THE ATTACK LIST (BZ2A): the nearest enemies T offered, by the same plate and the same type, one
/// row per enemy. It sits in the row BELOW the notice, which is reserved whether or not there is a
/// notice, so the list never moves and never overlaps: the player is choosing under time pressure
/// and a panel that jumps is a panel that gets misread.
constexpr float kStripMarginPixels = 6.0F;
constexpr float kStripSlotWidthPixels = 88.0F;
constexpr float kStripSlotGapPixels = 6.0F;
constexpr float kStripSlotHeightPixels = 34.0F;
constexpr float kStripFramePixels = 2.0F;
constexpr float kStripPadPixels = 3.0F;
constexpr float kStripKeyCell = 2.0F;
constexpr float kStripHpCell = 2.0F;
constexpr float kStripKindCell = 2.0F;
constexpr float kStripBarPixels = 3.0F;
constexpr float kStripSegmentGapPixels = 1.0F;
constexpr float kStripNoticeCell = 2.0F;
/// The corner radius of a HUD panel, at 720p. Panels are rounded like BZ2's rather than square, and
/// the frame ring clamps the radius to at least its own width so the face inside it can never be
/// handed a negative radius.
constexpr float kHudCornerPixels = 5.0F;
/// How much of the world shows through a HUD panel's face. The frame and every glyph stay opaque,
/// so the ink keeps its full contrast against the panel and only the panel itself is translucent --
/// which is how BZ2's panels read.
constexpr float kHudPanelAlpha = 0.65F;
constexpr std::array<float, 3> kStripFrame{0.95F, 0.95F, 0.95F};
constexpr std::array<float, 3> kStripSelected{1.0F, 0.85F, 0.10F};
/// THE COMMAND MENU (RB0.5, the look MENU-READ): `menu_rows`'s rows, key then verb, down the left
/// margin in the row under the notice -- the same reserved slot the attack list takes, so neither
/// panel moves. Each row is two of BZ2's translucent blue bars, laid out by `menu_layout`: a key
/// bar right-aligned on one column edge and a verb bar a gutter past it, rounded on their OUTER
/// ends and square where they face the gutter, so the amber leader line down it reads as cutting a
/// slice out of the background (the user's correction, 2026-10-27). A live row is the strip's white
/// on a blue face that fades top to bottom; a dimmed row is grey on a flat dark face whatever the
/// reason, because `MenuRow::reason` is for the code and the look is the same for both. The glyphs
/// gain weight from a 1 px black outline, never from a bigger cell.
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
constexpr float kMenuRingAlpha = 0.55F;
constexpr float kMenuRingCells = 0.5F;
/// The glyphs' outline is their coverage grown by half a layout cell (1 px) all round, in black at
/// that grown coverage, drawn between the face and the ink so the ink always wins.
constexpr float kMenuOutlineCells = 0.5F;
constexpr float kMenuOutlineRatio = 1.3F;

/// THE LEDGER (C6). C3 keeps a per-side count of the loads a hauler has banked and C4 spends it: it
/// is the war's only currency, and until this layer nothing showed it, so the player could not see
/// how the build the factory was making got paid for. This draws the word and the side's own count
/// on a black plate just above the bottom-left panel, so the reading sits with the map and the
/// command area rather than on the ship instrument.
///
/// The cell is 2.0 at the 720 reference -- the HUD's ONE type scale, which is BZ2's measured glyph
/// height. A fresh element joins that scale like every other one; "make it obvious" buys a brighter
/// ink and an opaque plate, never a bigger cell.
constexpr std::array<float, 3> kLedgerPlate{0.0F, 0.0F, 0.0F};
// The ink is `kLedgerInk` (scene.hpp): amber, deliberately unlike the HUD's white, because it is a
// resource reading and not a label. The command menu's leader line shares it.
constexpr std::string_view kLedgerWord = "SCRAP";
constexpr float kLedgerCell = 2.0F;

void append_ledger(std::vector<SceneVertex>& out, const SceneOptions& options,
                   const sim::Sim& sim) {
    if (!options.show_ledger) {
        return;
    }
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
    const std::string reading = std::to_string(sim.ore(options.viewer_side));
    const float cell = kLedgerCell * px;
    const float pad = cell;
    const float text_h = 5.0F * cell;
    // Above whichever view holds the bottom-left corner: R swaps the radar and the map, so the
    // ledger clears the TALLER of the two rather than the one that happens to be up.
    const float panel_x = radar_panel(options.extent).x;
    const float panel_top = options.show_world_map ? world_map_layout(options.extent).y
                                                   : radar_panel(options.extent).y;
    const float top = panel_top - text_h - (3.0F * pad);
    const float left = panel_x;
    // The word, a cell's gap, then the number: the same walk `append_hud_panels` uses, over the
    // same 5x5 cell font, so the ledger cannot drift into a second type scale.
    const float word_left = left + pad;
    const float number_left = word_left + word_width(kLedgerWord, cell) + cell;
    const float right = number_left + word_width(reading, cell) + pad;
    const float bottom = top + text_h + (2.0F * pad);
    const auto draw = [&](std::string_view text, float start) {
        for (std::size_t index = 0; index < text.size(); ++index) {
            const std::array<std::uint8_t, 5> rows = mount_glyph(text.at(index));
            const float glyph_left = start + (static_cast<float>(index) * 6.0F * cell);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                        continue;
                    }
                    const float x = glyph_left + (static_cast<float>(col) * cell);
                    const float y = top + pad + (static_cast<float>(row) * cell);
                    rect(marks_at, x, y, x + cell, y + cell, kLedgerInk);
                }
            }
        }
    };
    rect(marks_at * kHudBorderRatio, left, top, right, bottom, kLedgerPlate);
    draw(kLedgerWord, word_left);
    draw(reading, number_left);
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

void append_hud_panels(std::vector<SceneVertex>& out, const SceneOptions& options,
                       const sim::Sim& sim, std::span<const sim::UnitId> selected) {
    const bool wants_list = options.attack_list != nullptr && !options.attack_list->empty();
    const bool wants_menu = options.show_menu && options.menu != nullptr && !options.menu->empty();
    if (options.row == nullptr && !wants_list && !wants_menu) {
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
                           const std::array<float, 3>& frame) {
        const float edge = kStripFramePixels * px;
        const float radius = std::max(kHudCornerPixels * px, edge);
        const float bars_at = marks_at * kHudBorderRatio;
        const float face_at = marks_at * kHudFaceRatio;
        const auto at = [&](float distance, float x, float y) {
            const RayPoint point = ray_point(options.camera, options.extent, x, y, distance);
            return Vec3{point.x, point.y, point.z};
        };
        // The straight bars, held back from the corners by the radius.
        rect(bars_at, left + radius, top, right - radius, top + edge, frame);
        rect(bars_at, left + radius, bottom - edge, right - radius, bottom, frame);
        rect(bars_at, left, top + radius, left + edge, bottom - radius, frame);
        rect(bars_at, right - edge, top + radius, right, bottom - radius, frame);
        // The corners: a quarter annulus from the outer radius in to the inner one, one quad
        // per slice, in the frame's own opaque colour so the ink still has a crisp edge.
        const float inner = radius - edge;
        walk_corners(
            left, top, right, bottom, radius, [&](float cx, float cy, float from, float to) {
                for (int slice = 0; slice < kCornerSlices; ++slice) {
                    const float a = from
                                    + ((to - from) * static_cast<float>(slice)
                                       / static_cast<float>(kCornerSlices));
                    const float b = from
                                    + ((to - from) * static_cast<float>(slice + 1)
                                       / static_cast<float>(kCornerSlices));
                    emit_quad(
                        out, at(bars_at, cx + (radius * std::cos(a)), cy + (radius * std::sin(a))),
                        at(bars_at, cx + (radius * std::cos(b)), cy + (radius * std::sin(b))),
                        at(bars_at, cx + (inner * std::cos(b)), cy + (inner * std::sin(b))),
                        at(bars_at, cx + (inner * std::cos(a)), cy + (inner * std::sin(a))), frame);
                }
            });
        // The glass face: the same rounded rectangle inset by the frame, as a fan from its
        // centre, translucent so the world shows through it.
        const float inset = radius - edge;
        std::vector<std::array<float, 2>> rim;
        walk_corners(left + edge, top + edge, right - edge, bottom - edge, inset,
                     [&](float cx, float cy, float from, float to) {
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
        for (std::size_t i = 0; i < rim.size(); ++i) {
            const std::array<float, 2>& a = rim.at(i);
            const std::array<float, 2>& b = rim.at((i + 1) % rim.size());
            emit_triangle(out, at(face_at, centre_x, centre_y), at(face_at, a.at(0), a.at(1)),
                          at(face_at, b.at(0), b.at(1)), kPlate, kHudPanelAlpha);
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
    // The notice's row, reserved whether or not there is a notice: the attack list is drawn under
    // it, and a panel that moves when a notice appears or expires is a panel that gets misread.
    const float notice_top = top + slot_h + (kStripMarginPixels * px) + pad;
    // THE COMMAND ROW. Every key has its own spot, by index, and a key that holds nothing is
    // skipped without moving the ones after it: its space stays blank for what is built next.
    for (std::size_t index = 0; options.row != nullptr && index < kRowSlots; ++index) {
        if (!options.row->occupied(index)) {
            continue;
        }
        const RowSlot& slot = options.row->slots.at(index);
        const bool is_navs = row_slot_kind(index) == RowSlotKind::Navs;
        const bool is_group = row_slot_kind(index) == RowSlotKind::Group;
        std::string label = "NAVS";
        std::size_t count = options.row->nav_marks;
        std::vector<float> fractions;
        if (!is_navs) {
            const std::optional<SlotReading> reading = read_slot(sim, slot);
            if (!reading.has_value()) {
                continue; // everything in it died since the row was last updated
            }
            label = std::string(kind_label(reading->kind));
            count = reading->count;
            // A vehicle key has a segment per living vehicle; a building key is one thing, so one.
            fractions = is_group
                            ? reading->fractions
                            : std::vector<float>{reading->max_hp == 0
                                                     ? 0.0F
                                                     : static_cast<float>(reading->hp)
                                                           / static_cast<float>(reading->max_hp)};
        }
        const float left = (kStripMarginPixels * px) + (static_cast<float>(index) * (slot_w + gap));
        // Lit when ANY unit of the group is selected: pressing the key selects the whole group,
        // so a lit slot means "you have some of these".
        const bool chosen
            = std::any_of(slot.units.begin(), slot.units.end(), [selected](sim::UnitId id) {
                  return std::find(selected.begin(), selected.end(), id) != selected.end();
              });
        const std::array<float, 3>& frame = chosen ? kStripSelected : kStripFrame;
        plate(left, top, left + slot_w, top + slot_h, frame);
        const float key_cell = kStripKeyCell * px;
        const float count_cell = kStripHpCell * px;
        ink_text(row_key_label(index), left + pad, top + pad, key_cell, frame);
        // How many, not how hurt. The "x" is what keeps a bare digit from reading as hit points.
        // A building is one thing, not a head count, so only `~` and the vehicle groups say it.
        if (is_navs || is_group) {
            const std::string text = "x" + std::to_string(count);
            const float count_left = left + slot_w - pad - word_width(text, count_cell);
            ink_text(text, count_left, top + pad + (2.5F * key_cell) - (2.5F * count_cell),
                     count_cell, kStripFrame);
        }
        // The label takes the largest cell that fits the slot's inside, up to the design size:
        // the longest names would run out of a narrow slot at full size.
        const float kind_top = top + pad + (5.0F * key_cell) + pad;
        const float kind_cell
            = std::min(kStripKindCell * px,
                       (slot_w - (2.0F * pad)) / static_cast<float>((label.size() * 6) - 1));
        ink_text(label, left + pad, kind_top, kind_cell, kStripFrame);
        if (is_navs) {
            continue; // marks have no hit points: no bar
        }
        // The bar: a dark track the width of the slot's inside, then one segment per vehicle,
        // 1 px apart, dividing that width equally whatever their health -- COLOUR says how hurt a
        // vehicle is, never length -- with the last taking the whole pixels left over. A group too
        // big for that gets 1 px segments, left to right, until the bar's right edge.
        const float bar_top = top + slot_h - pad - (kStripBarPixels * px) + (2.0F * px);
        const float bar_bottom = bar_top + (kStripBarPixels * px);
        const float bar_left = left + pad;
        const float bar_width = slot_w - (2.0F * pad);
        const float bar_right = bar_left + bar_width;
        rect(marks_at * kHudFaceRatio * 0.999F, bar_left, bar_top, bar_right, bar_bottom,
             {0.25F, 0.25F, 0.25F});
        const std::vector<BarSegment> segments = bar_segments(fractions);
        const float segment_gap = kStripSegmentGapPixels * px;
        const float n = std::max(1.0F, static_cast<float>(segments.size()));
        const float segment_w
            = std::max(px, std::floor((bar_width - ((n - 1.0F) * segment_gap)) / n));
        float segment_left = bar_left;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (segment_left + segment_w > bar_right + (0.01F * px)) {
                break; // no room left: an absurd group shows as many as fit, never overlapping
            }
            const float segment_right
                = i + 1 == segments.size() ? bar_right : segment_left + segment_w;
            rect(marks_at, segment_left, bar_top, segment_right, bar_bottom, segments.at(i).rgb);
            segment_left = segment_right + segment_gap;
        }
    }
    if (!options.notice.empty()) {
        const float cell = kStripNoticeCell * px;
        const float notice_left = kStripMarginPixels * px + pad;
        plate(kStripMarginPixels * px, notice_top - pad,
              notice_left + word_width(options.notice, cell) + pad,
              notice_top + (5.0F * cell) + pad, kStripSelected);
        ink_text(options.notice, notice_left, notice_top, cell, kStripSelected);
    }
    // THE ATTACK LIST. The key to press, the kind in a column of its own, then the distance in
    // whole metres: the hundred-metre figures are the ones worth comparing, and a column of them
    // reads down the panel. The metres column is as wide as the widest row, so no row is pushed
    // out of the plate by a number longer than its neighbours.
    if (wants_list) {
        const std::vector<TargetRow>& rows = *options.attack_list;
        const float cell = kStripNoticeCell * px;
        const float pitch = 6.0F * cell; // a 5-cell glyph and one blank row between rows
        const float key_left = kStripMarginPixels * px + pad;
        const float label_left = key_left + (7.0F * cell); // one glyph (5 cells) and a blank column
        float label_width = 0.0F;
        float metres_width = 0.0F;
        for (const TargetRow& row : rows) {
            label_width = std::max(label_width, word_width(kind_label(row.kind), cell));
            metres_width = std::max(metres_width, word_width(metres_text(row.metres), cell));
        }
        const float metres_left = label_left + label_width + (2.0F * cell);
        const float list_top = notice_top + (5.0F * cell) + (2.0F * pad);
        const float list_left = kStripMarginPixels * px;
        plate(list_left, list_top, metres_left + metres_width + pad,
              list_top + pad + (static_cast<float>(rows.size() - 1) * pitch) + (5.0F * cell) + pad,
              kStripFrame);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const float row_top = list_top + pad + (static_cast<float>(index) * pitch);
            const char key = static_cast<char>('0' + rows.at(index).key);
            ink_text(std::string_view{&key, 1}, key_left, row_top, cell, kStripFrame);
            ink_text(kind_label(rows.at(index).kind), label_left, row_top, cell, kStripFrame);
            ink_text(metres_text(rows.at(index).metres), metres_left, row_top, cell, kStripFrame);
        }
    }
    // THE COMMAND MENU, in the order it was handed: never sorted, never renumbered, and the key is
    // drawn as the label it is ("SPACE", "~", "9"), so the harvester's missing 6, 7 and 8 stay a
    // visible gap. `menu_layout` places every bar; this draws them, then the glyphs, then the line.
    if (wants_menu) {
        const std::vector<MenuRow>& rows = *options.menu;
        const float cell = kMenuCell * px;
        const float menu_top = notice_top + (5.0F * cell) + (2.0F * pad);
        const MenuLayout layout = menu_layout(rows, kStripMarginPixels * px, menu_top, cell);
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
        // down the gutter at one x through every row's band, as BZ2 draws it, then on to the radar
        // (MENU-LEG): along its top edge and down onto the selected unit's blip. The blip is the
        // radar's OWN mark, from the call `append_radar` makes, so the dot is where the radar drew
        // the unit and nowhere else. No radar, no leg: the world map has taken its corner.
        const std::vector<RadarMark> marks
            = options.show_world_map ? std::vector<RadarMark>{} : frame_radar_marks(options, sim);
        std::optional<float> radar_top;
        std::optional<std::array<float, 2>> blip;
        if (!marks.empty()) {
            const RadarPanel panel = radar_panel(options.extent);
            radar_top = panel.y;
            const auto mark
                = std::find_if(marks.begin(), marks.end(), [&](const RadarMark& candidate) {
                      return !selected.empty() && candidate.unit == selected.front();
                  });
            if (mark != marks.end()) {
                blip = std::array<float, 2>{panel.x + mark->px, panel.y + mark->py};
            }
        }
        const MenuLeader leader = menu_leader(layout, radar_top, blip);
        // One quad per segment: the box round it, grown half the line's width each way so that
        // every elbow is filled square. The path starts and ends on vertical segments, so the
        // first's top and the last's bottom are the path's own ends and are NOT grown: the line
        // starts under the state row and stops on the radar's edge or the blip.
        const float half = (layout.line_right - layout.line_left) * 0.5F;
        const float leader_at = marks_at * kMenuLeaderRatio;
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
                const float angle
                    = (2.0F * kPi * static_cast<float>(slice)) / static_cast<float>(kMenuDotSlices);
                return at(leader_at, cx + (radius * std::cos(angle)),
                          cy + (radius * std::sin(angle)));
            };
            for (int slice = 0; slice < kMenuDotSlices; ++slice) {
                emit_triangle(out, at(leader_at, cx, cy), rim(slice), rim(slice + 1), kLedgerInk);
            }
        }
    }
}

/// Four ticks round an OPEN centre and a small dot in it, in the plane the camera looks along,
/// centred on the point the click's own ray reaches. Six vertices per quad: two triangles, and no
/// winding to get wrong, because the pipeline culls nothing. The ticks reach the old box's
/// half-size; a pulse pushes them outward and swells the dot, and 0 is the reticle at rest.
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
    // The weapon picks the shape (M1): the gun's is E12's, untouched; the rocket's is heavier;
    // the mortar's is the dot alone, its ladder drawn on the HUD layer by `append_weapon_hud`.
    const ReticleShape shape = reticle_for(options.weapon, camera.pitch);
    const bool heavy = shape.kind != ReticleKind::FourTicks;
    const RayPoint centre
        = ray_point(camera, options.extent, screen_x, screen_y, kReticleDistanceMetres);
    const math::Basis basis = math::basis_from(camera.yaw, camera.pitch);
    const float half
        = kReticleDistanceMetres * std::tan(math::kFovRadians * 0.5F) * kReticleHalfFraction;
    const float pulse = std::clamp(options.reticle_pulse, 0.0F, 1.0F);
    const float thick
        = half * (shape.kind == ReticleKind::Heavy ? kRocketBarFraction : kReticleBarFraction)
          * 0.5F;
    const float spread = half * kReticlePulseSpread * pulse;
    const float inner = (half * kReticleGapFraction) + spread;
    const float outer
        = (half * (shape.kind == ReticleKind::Heavy ? kRocketReachFraction : 1.0F)) + spread;
    const float dot = half * (heavy ? kRocketDotFraction : kReticleDotFraction)
                      * (1.0F + (kReticlePulseDotGrowth * pulse));
    const auto corner = [&](float right, float up) {
        return std::array<float, 3>{centre.x + (right * basis.right.at(0)) + (up * basis.up.at(0)),
                                    centre.y + (right * basis.right.at(1)) + (up * basis.up.at(1)),
                                    centre.z + (right * basis.right.at(2)) + (up * basis.up.at(2))};
    };
    const auto bar_quad = [&](float r0, float u0, float r1, float u1) {
        const std::array<float, 3> a = corner(r0, u0);
        const std::array<float, 3> b = corner(r1, u0);
        const std::array<float, 3> c = corner(r1, u1);
        const std::array<float, 3> d = corner(r0, u1);
        out.push_back(SceneVertex{a, colour});
        out.push_back(SceneVertex{b, colour});
        out.push_back(SceneVertex{c, colour});
        out.push_back(SceneVertex{a, colour});
        out.push_back(SceneVertex{c, colour});
        out.push_back(SceneVertex{d, colour});
    };
    if (shape.kind != ReticleKind::Ladder) {
        bar_quad(-thick, inner, thick, outer);   // up
        bar_quad(-thick, -outer, thick, -inner); // down
        bar_quad(-outer, -thick, -inner, thick); // left
        bar_quad(inner, -thick, outer, thick);   // right
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
constexpr std::string_view kMapCaption = "WORLD MAP - R CLOSES";

void append_world_map(std::vector<SceneVertex>& out, const SceneOptions& options,
                      const sim::Sim& sim) {
    if (!options.show_world_map) {
        return;
    }
    const WorldMap map = world_map(sim, options.extent, options.viewer_side, options.viewer_unit,
                                   options.nav_marks);
    const WorldMapLayout& layout = map.layout;
    const float scale = map_furniture_scale(layout);
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float ratio, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto at = [&](float x, float y) {
            const RayPoint point
                = ray_point(options.camera, options.extent, x, y, marks_at * ratio);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };
    constexpr std::array<float, 3> kInk{0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 3> kEdge{0.80F, 0.82F, 0.85F};
    constexpr std::array<float, 3> kOwn{0.30F, 0.85F, 0.35F};
    constexpr std::array<float, 3> kDriven{1.0F, 1.0F, 1.0F};

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
        if (!asset.driven || options.headings == nullptr) {
            continue;
        }
        // WHICH WAY YOU ARE GOING. The presentation's own heading for this unit -- its velocity
        // while it moves, the last one it had while it stands -- drawn as a thin white needle from
        // the blip's edge outward. Map y grows DOWN, so north, yaw 0, is (0, -1).
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
            = map_heading_needle(asset, options.headings->of(*you));
        // A hair in front of the blip's fill, so the needle is never buried by it.
        const float needle_at = marks_at * 0.995F;
        const auto at_needle = [&](const MapPoint& p) {
            const RayPoint point = ray_point(options.camera, options.extent, p.x, p.y, needle_at);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at_needle(needle.at(0)), at_needle(needle.at(1)), at_needle(needle.at(2)),
                  at_needle(needle.at(3)), kDriven);
    }

    // The nav marks, over the units, exactly as the radar draws them.
    for (const MapMark& mark : map.marks) {
        append_nav_glyph(out, options, marks_at * 0.89F, mark.px, mark.py, mark.half,
                         mark.highlighted);
    }

    // The caption says what this is and how to close it. The map now sits in the bottom-left corner
    // (BZ2's minimap footprint), so the caption goes ABOVE it: below it there is only the frame's
    // margin, and the text would run off the bottom of the screen.
    const float cell = 2.0F * scale;
    const float width = ((static_cast<float>(kMapCaption.size()) * 6.0F) - 1.0F) * cell;
    const float left = layout.x + ((layout.size - width) * 0.5F);
    const float top = layout.y - edge - (17.0F * scale);
    rect(0.9F, left - (4.0F * scale), top - (3.0F * scale), left + width + (4.0F * scale),
         top + (13.0F * scale), kInk);
    for (std::size_t index = 0; index < kMapCaption.size(); ++index) {
        const std::array<std::uint8_t, 5> rows = mount_glyph(kMapCaption.at(index));
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (int col = 0; col < 5; ++col) {
                if (((rows.at(row) >> (4 - col)) & 1U) == 0U) {
                    continue;
                }
                const float x = left + (static_cast<float>(index) * 6.0F * cell)
                                + (static_cast<float>(col) * cell);
                const float y = top + (static_cast<float>(row) * cell);
                rect(0.88F, x, y, x + cell, y + cell, kEdge);
            }
        }
    }
}

} // namespace

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
                                 aspect, fov_radians);
}

Scene build_scene(const sim::Sim& sim, const SceneOptions& options,
                  std::span<const sim::UnitId> selected) {
    Scene scene;
    const float aspect
        = static_cast<float>(options.extent.width) / static_cast<float>(options.extent.height);
    scene.view_projection = view_projection(options.camera, aspect);

    const int centre_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int centre_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    build_terrain(scene.vertices, options.terrain, options.seed, centre_x, centre_y,
                  options.radius_tiles);
    append_sky(scene.vertices, options.camera);

    // The one pick the frame makes: the reticle and the unit tint both read it, so they can never
    // disagree about what is under the crosshair.
    Pick aimed;
    if (options.aim.has_value()) {
        aimed = pick(sim, options, options.aim->x, options.aim->y);
    }

    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive) {
            continue;
        }
        // Only draw units within the terrain that was actually meshed: a unit far outside the
        // rendered radius has no ground to stand on in this scene.
        const int dx = unit.x.floor_int() - centre_x;
        const int dy = unit.y.floor_int() - centre_y;
        if (std::abs(dx) <= options.radius_tiles && std::abs(dy) <= options.radius_tiles) {
            const bool is_selected
                = std::find(selected.begin(), selected.end(), unit.id) != selected.end();
            Mark mark = Mark::None;
            if (is_selected) {
                mark = Mark::Selected;
            } else if (aimed.has_unit() && aimed.unit == unit.id) {
                mark = unit.side == options.viewer_side ? Mark::AimedOwn : Mark::AimedHostile;
            }
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
            if (unit.kind == sim::UnitKind::HoverTank) {
                build_hover_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                 heading, turret);
            } else if (unit.kind == sim::UnitKind::Mortar) {
                build_mortar(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading,
                             heading + turret);
            } else if (unit.kind == sim::UnitKind::Rocket) {
                build_rocket(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading,
                             turret);
            } else if (unit.kind == sim::UnitKind::Scavenger) {
                // Before the tank fallback below: a driven scavenger is FACED too, and it must not
                // borrow the tank's turret. Nothing on a scavenger aims.
                build_scavenger(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                heading);
            } else if (unit.kind == sim::UnitKind::Recycler
                       || unit.kind == sim::UnitKind::DeployedRecycler) {
                // Beside the scavenger's, and before the tank fallback: a driven recycler is FACED
                // too, and must neither borrow the tank's turret nor fall through to a factory. A
                // DEPLOYED one is drawn as the hull that drove there, parked on its site: the
                // building has no look of its own yet, and DEPLOY adds no art.
                build_recycler(scene.vertices, options.seed, unit, mark, lift_z, hovering, heading);
            } else if (faced || unit.kind == sim::UnitKind::Tank
                       || unit.kind == sim::UnitKind::Scout) {
                // The scout borrows the tank's body until it has art of its own (VIS adds none):
                // left to the final branch it would be drawn as a factory.
                build_facing_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                  heading, turret);
            } else if (unit.kind == sim::UnitKind::Turret) {
                build_defense_turret(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                     heading, heading + turret);
            } else {
                build_factory(scene.vertices, options.seed, unit, mark, lift_z, hovering);
            }
        }
    }

    append_projectiles(scene.vertices, options, sim);
    append_landing_marks(scene.vertices, options, sim.tick());
    append_scrap(scene.vertices, options, sim);
    append_nav_beacons(scene.vertices, options);
    append_deploy_preview(scene.vertices, options);
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
    append_unit_bars(scene.vertices, options, sim);
    append_weapon_hud(scene.vertices, options);
    append_hud_panels(scene.vertices, options, sim, selected);
    append_ledger(scene.vertices, options, sim);
    append_outcome(scene.vertices, options, sim);
    append_reticle(scene.vertices, options, sim, aimed);
    append_world_map(scene.vertices, options, sim);

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
