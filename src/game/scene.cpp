#include "protekt/game/scene.hpp"

#include "protekt/game/atlas.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/spec.hpp"

#include <algorithm>
#include <array>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
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

void emit_triangle(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                   const Vec3& colour) {
    out.push_back(SceneVertex{a, colour});
    out.push_back(SceneVertex{b, colour});
    out.push_back(SceneVertex{c, colour});
}

void emit_quad(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
               const Vec3& d, const Vec3& colour) {
    emit_triangle(out, a, b, c, colour);
    emit_triangle(out, a, c, d, colour);
}

/// A quad that samples one whole atlas tile: its corners a, b, c, d take the tile's corners
/// top-left, top-right, bottom-right, bottom-left. The colour still multiplies the texel.
void emit_tiled_quad(std::vector<SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                     const Vec3& d, const Vec3& colour, const UvRect& tile) {
    const SceneVertex va{a, colour, tile.u0, tile.v0};
    const SceneVertex vb{b, colour, tile.u1, tile.v0};
    const SceneVertex vc{c, colour, tile.u1, tile.v1};
    const SceneVertex vd{d, colour, tile.u0, tile.v1};
    for (const SceneVertex& v : {va, vb, vc, va, vc, vd}) {
        out.push_back(v);
    }
}

void build_terrain(std::vector<SceneVertex>& out, std::uint64_t seed, int centre_x, int centre_y,
                   int radius) {
    // Each tile's corners are sampled one tile beyond it on the far edge (tx+1, ty+1); widening
    // the low edge by one tile too keeps the sampled HEIGHT field symmetric around the camera,
    // even though the tolerance on the mesh's visual extent absorbs the extra row.
    // Each quad takes its tile with x along u and z along v, the same way round everywhere, so
    // neighbours of one type and variant continue each other's (wrapping) paint, and neighbours of
    // two variants meet in the same tones.
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
            const world::Terrain terrain = world::sample(seed, tx, ty).terrain;
            const UvRect& tile = tiles.at(static_cast<std::size_t>(terrain))
                                     .at(static_cast<std::size_t>(variant_index(terrain, tx, ty)));
            const float x0 = static_cast<float>(tx) * kMetresPerTile;
            const float x1 = static_cast<float>(tx + 1) * kMetresPerTile;
            const float z0 = static_cast<float>(ty) * kMetresPerTile;
            const float z1 = static_cast<float>(ty + 1) * kMetresPerTile;
            const Vec3 p00{x0, height_metres(seed, tx, ty), z0};
            const Vec3 p10{x1, height_metres(seed, tx + 1, ty), z0};
            const Vec3 p11{x1, height_metres(seed, tx + 1, ty + 1), z1};
            const Vec3 p01{x0, height_metres(seed, tx, ty + 1), z1};
            emit_tiled_quad(out, p00, p10, p11, p01, kTerrainLight, tile);
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

void append_projectiles(std::vector<SceneVertex>& out, const SceneOptions& options,
                        const sim::Sim& sim) {
    constexpr float kOne = static_cast<float>(sim::Fixed::kOne);
    for (const sim::Projectile& bolt : sim.projectiles()) {
        const float x = static_cast<float>(bolt.x.raw) / kOne * kMetresPerTile;
        const float z = static_cast<float>(bolt.y.raw) / kOne * kMetresPerTile;
        const float centre = static_cast<float>(bolt.z.raw) / kOne * kMetresPerHeightUnit;
        const Vec3 colour = bolt.side == options.viewer_side ? kOwnBoltColour : kHostileBoltColour;
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
/// height is what keeps it in the frame), but its width follows the range -- 0.3 of it, from 0.7 to
/// 1.6 m either side -- so a shell that lands at your feet is a tall narrow cross, not a wall.
constexpr float kLandingMarkWidthPerMetre = 0.3F;
constexpr float kLandingMarkMinHalfWidth = 0.7F;
constexpr float kLandingMarkMaxHalfWidth = 1.6F;
constexpr float kLandingMarkBarFraction = 0.125F; ///< half-thickness of a bar, of the half-width
constexpr float kLandingMarkArmFraction = 0.55F;  ///< how far a bracket reaches, of the half-width

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
        const float bar = half * kLandingMarkBarFraction;
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

constexpr int kRadarSegments = 48;                ///< slices in the disc and the ring
constexpr float kHealthBarHeightFraction = 0.08F; ///< the bar's height, of the panel's size
constexpr float kHealthBarMinPixels = 8.0F;
constexpr float kHealthBarGapPixels = 4.0F; ///< between the bar and the panel's border

/// Green from half health up, amber from a fifth, red below -- in integers, so the thresholds are
/// exact. Green and red are the radar's own "yours" and "hostile" colours.
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
    const std::vector<RadarMark> marks
        = radar_marks(sim.units(), options.viewer_unit, options.camera.yaw, panel);
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
    const float x0 = panel.x - kRadarBorderPixels;
    const float y0 = panel.y - kRadarBorderPixels;
    const float x1 = panel.x + panel.size + kRadarBorderPixels;
    constexpr std::array<float, 3> kBorder{0.80F, 0.82F, 0.85F};
    constexpr std::array<float, 3> kFace{0.04F, 0.05F, 0.06F};

    // The panel is ROUND: a disc inscribed in its square, ringed by the border. The corners of the
    // square are not painted. Both are fans and strips of `kRadarSegments` slices, walked clockwise
    // on the screen like `rect`, and every point goes through `at` like everything else.
    const float centre_x = panel.x + (panel.size * 0.5F);
    const float centre_y = panel.y + (panel.size * 0.5F);
    const float inner = panel.size * 0.5F;
    const float outer = inner + kRadarBorderPixels;
    const auto rim = [&](float radius, int slice) {
        const float angle = (2.0F * kPi * static_cast<float>(slice)) / kRadarSegments;
        return std::array<float, 2>{centre_x + (radius * std::cos(angle)),
                                    centre_y + (radius * std::sin(angle))};
    };
    for (int slice = 0; slice < kRadarSegments; ++slice) {
        const std::array<float, 2> in_a = rim(inner, slice);
        const std::array<float, 2> in_b = rim(inner, slice + 1);
        const std::array<float, 2> out_a = rim(outer, slice);
        const std::array<float, 2> out_b = rim(outer, slice + 1);
        emit_quad(out, at(border_at, out_a.at(0), out_a.at(1)),
                  at(border_at, out_b.at(0), out_b.at(1)), at(border_at, in_b.at(0), in_b.at(1)),
                  at(border_at, in_a.at(0), in_a.at(1)), kBorder);
    }
    for (int slice = 0; slice < kRadarSegments; ++slice) {
        const std::array<float, 2> a = rim(inner, slice);
        const std::array<float, 2> b = rim(inner, slice + 1);
        emit_triangle(out, at(face_at, centre_x, centre_y), at(face_at, a.at(0), a.at(1)),
                      at(face_at, b.at(0), b.at(1)), kFace);
    }

    // The viewer's hull as a bar directly above the panel: the same width as the panel with its
    // border, a small gap up. The sim owns the numbers; nothing is added to `Unit` for this.
    const sim::Unit* viewer = sim.find(options.viewer_unit);
    if (viewer != nullptr) {
        const float height = std::max(kHealthBarMinPixels, panel.size * kHealthBarHeightFraction);
        const float bottom = y0 - kHealthBarGapPixels;
        const float top = bottom - height;
        rect(face_at, x0, top, x1, bottom, kFace);
        const std::uint32_t max_hp = sim::spec_of(viewer->kind).hp;
        const std::uint32_t hp = std::min<std::uint32_t>(viewer->hp, max_hp);
        if (viewer->alive && max_hp > 0 && hp > 0) {
            const float fill
                = x0 + ((x1 - x0) * static_cast<float>(hp) / static_cast<float>(max_hp));
            rect(marks_at, x0, top, fill, bottom, health_colour(hp, max_hp));
        }
    }

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
    case 'C':
        return {0b01111, 0b10000, 0b10000, 0b10000, 0b01111};
    case 'D':
        return {0b11110, 0b10001, 0b10001, 0b10001, 0b11110};
    case 'E':
        return {0b11111, 0b10000, 0b11110, 0b10000, 0b11111};
    case 'G':
        return {0b01111, 0b10000, 0b10011, 0b10001, 0b01111};
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
    case 'U':
        return {0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
    case 'Y':
        return {0b10001, 0b01010, 0b00100, 0b00100, 0b00100};
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
    default:
        return {};
    }
}

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

} // namespace

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
    build_terrain(scene.vertices, options.seed, centre_x, centre_y, options.radius_tiles);
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
            } else if (faced || unit.kind == sim::UnitKind::Tank) {
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
    append_radar(scene.vertices, options, sim);
    append_mount_plates(scene.vertices, options, sim);
    append_unit_bars(scene.vertices, options, sim);
    append_weapon_hud(scene.vertices, options);
    append_reticle(scene.vertices, options, sim, aimed);

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
