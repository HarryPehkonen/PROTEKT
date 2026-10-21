#include "protekt/game/scene.hpp"

#include "protekt/game/atlas.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/math/math.hpp"

#include <algorithm>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
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

Vec3 terrain_colour(world::Terrain terrain) {
    switch (terrain) {
    case world::Terrain::Flat:
        return {0.25F, 0.55F, 0.20F};
    case world::Terrain::Rough:
        return {0.45F, 0.42F, 0.18F};
    case world::Terrain::Ridge:
        return {0.55F, 0.50F, 0.48F};
    case world::Terrain::Crater:
        return {0.15F, 0.12F, 0.10F};
    }
    return {0.0F, 0.0F, 0.0F};
}

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
    for (int ty = centre_y - radius - 1; ty <= centre_y + radius; ++ty) {
        for (int tx = centre_x - radius - 1; tx <= centre_x + radius; ++tx) {
            const Vec3 colour = terrain_colour(world::sample(seed, tx, ty).terrain);
            const float x0 = static_cast<float>(tx) * kMetresPerTile;
            const float x1 = static_cast<float>(tx + 1) * kMetresPerTile;
            const float z0 = static_cast<float>(ty) * kMetresPerTile;
            const float z1 = static_cast<float>(ty + 1) * kMetresPerTile;
            const Vec3 p00{x0, height_metres(seed, tx, ty), z0};
            const Vec3 p10{x1, height_metres(seed, tx + 1, ty), z0};
            const Vec3 p11{x1, height_metres(seed, tx + 1, ty + 1), z1};
            const Vec3 p01{x0, height_metres(seed, tx, ty + 1), z1};
            emit_quad(out, p00, p10, p11, p01, colour);
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

/// A box standing on the ground beneath (x, z): wider and shorter for a tank or turret, taller
/// for a factory.
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
/// a slanted front plate (a glacis) is `front_top` short of `front_bottom`. The faces are the same
/// six, in the same order and winding, as `build_unit_box`; a turn about the vertical axis keeps
/// the winding.
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
void emit_turned_prism(std::vector<SceneVertex>& out, float x, float z, float angle, const Prism& s,
                       const Vec3& colour, const UvRect& tile, const UvRect& top_tile) {
    const std::array<Vec3, 8> p = prism_corners(x, z, angle, s);
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

void build_unit_box(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                    Mark mark, float lift_z, bool lifted) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    // float + bool rather than optional<float>: gcc -O2 wrongly warns may-be-uninitialized on the
    // optional's storage at the call site (the same false alarm hover_.z() once raised).
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const float half_width = kMetresPerTile * 0.3F;
    const float height = unit.kind == sim::UnitKind::Factory ? 4.5F : 1.8F;
    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }

    // An axis-aligned box is a prism at angle 0, so it is shaded like every other unit part. A
    // factory's walls and roof take their own tiles; a turret is turret body all over.
    const AtlasManifest& atlas = atlas_manifest();
    const bool factory = unit.kind == sim::UnitKind::Factory;
    const UvRect sides = atlas.rect(factory ? "factory_walls" : "turret_body");
    const UvRect top = factory ? atlas.rect("factory_roof") : sides;
    // Pale-mixed like the hull: the deep side colour would multiply the tile's detail down to a
    // tenth of a luma step; the pale tint lets the metal carry and the team read through as a tint.
    for (float& channel : colour) {
        channel += (1.0F - channel) * kHullPaleMix;
    }
    emit_turned_prism(out, x, z, 0.0F,
                      Prism{-half_width, half_width, -half_width, -half_width, half_width,
                            half_width, ground, ground + height, ground + height},
                      colour, sides, top);
}

/// A tank, abstracted from a side-on battle tank and built only of flat-shaded prisms: two dark
/// track skirts running past the hull at both ends, a hull between them with a sloped glacis and a
/// slightly taller engine deck, a squat turret set back on the hull with a rear bustle, and a long
/// thin barrel from the turret's front face with a thicker muzzle collar, and a flat team-colour
/// panel on the turret roof. The hull and tracks are
/// turned by `heading`, the turret group by `heading + turret`. Lateral extent stays the tank's
/// 0.3 tile half-width (the outer face of a track), so only the length and the profile are new.
void build_facing_tank(std::vector<SceneVertex>& out, std::uint64_t seed, const sim::Unit& unit,
                       Mark mark, float lift_z, bool lifted, float heading, float turret) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = lifted ? lift_z : ground_metres(seed, unit.x, unit.y);
    const float half_width = kMetresPerTile * 0.3F; ///< the outer face of a track
    constexpr float kTrackInner = 0.7F;             ///< of half_width; the hull fills the rest
    // The skirts are dark, with their links, because the TRACK TILE is, and the hull, turret and
    // bustle wear the camo because their tile does: the vertex keeps the side colour, the selection
    // tint and the face light, and the texel multiplies them. Which of its team's three camo tiles
    // is this tank's -- one pattern for the whole tank. The roof panel wears the plain turret
    // metal.
    const AtlasManifest& atlas = atlas_manifest();
    const UvRect hull_tile
        = atlas.rect(kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                         .at(static_cast<std::size_t>(hull_variant(seed, unit.id.value))));
    const UvRect tracks_tile = atlas.rect("tank_tracks");
    const UvRect panel_tile = atlas.rect("tank_turret");
    const UvRect barrel_tile = atlas.rect("tank_barrel");
    constexpr float kTrackHeight = 0.4F;
    constexpr float kTrackBack = -1.3F; ///< metres along the heading from the centre
    constexpr float kTrackFront = 1.3F;
    constexpr float kHullBack = -1.1F;
    constexpr float kHullFront = 1.2F;
    constexpr float kGlacisTop = 0.75F; ///< where the slanted front plate meets the deck
    constexpr float kHullFrontHeight = 0.7F;
    constexpr float kHullRearHeight = 0.85F; ///< the engine deck, a little above the front
    constexpr float kTurretBase = 0.8F;      ///< above the ground; the hull's mean top
    constexpr float kTurretHeight = 0.4F;
    constexpr float kTurretHalfAcross = 0.36F;
    constexpr float kTurretCentre = -0.3F; ///< set back from the hull's centre
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

    const float inner = half_width * kTrackInner;
    for (const float side : {-1.0F, 1.0F}) {
        const float a = side * inner;
        const float b = side * half_width;
        const Prism track{std::min(a, b), std::max(a, b),        kTrackBack,
                          kTrackBack,     kTrackFront,           kTrackFront,
                          ground,         ground + kTrackHeight, ground + kTrackHeight};
        emit_turned_prism(out, x, z, heading, track, colour, tracks_tile);
    }
    const Prism hull{-inner,
                     inner,
                     kHullBack,
                     kHullBack,
                     kHullFront,
                     kGlacisTop,
                     ground,
                     ground + kHullRearHeight,
                     ground + kHullFrontHeight};
    // The camo parts' colour is a PALE tint of the marked side colour, so the camo tile's tones
    // dominate.
    Vec3 camo_colour = colour;
    for (float& channel : camo_colour) {
        channel += (1.0F - channel) * kHullPaleMix;
    }
    emit_turned_prism(out, x, z, heading, hull, camo_colour, hull_tile);

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

/// A tracer is brighter than anything it flies over: a unit colour is a thing standing on the
/// ground, but a bolt is a few decimetres wide and must not be lost against grass, rock or a
/// side's own tanks. Two hues, so incoming fire and outgoing fire read apart at a glance --
/// a bright cyan for the viewer's side, a bright orange-yellow for the other.
constexpr Vec3 kOwnBoltColour{0.30F, 1.0F, 1.0F};
constexpr Vec3 kHostileBoltColour{1.0F, 0.85F, 0.10F};
/// A tracer's half-width (not a tank-sized cube). Its height is the bolt's own: the sim says how
/// high it is, and it is drawn there, not at some height re-derived from the ground it is over.
constexpr float kBoltHalfWidthMetres = 0.12F;

/// One small box per projectile the sim has in the air, built the way `build_unit_box` builds a
/// unit: the same tile-to-metre conversion, the same six faces, the same winding.
void append_projectiles(std::vector<SceneVertex>& out, const SceneOptions& options,
                        const sim::Sim& sim) {
    for (const sim::Projectile& bolt : sim.projectiles()) {
        const float x = static_cast<float>(bolt.x.raw) / static_cast<float>(sim::Fixed::kOne)
                        * kMetresPerTile;
        const float z = static_cast<float>(bolt.y.raw) / static_cast<float>(sim::Fixed::kOne)
                        * kMetresPerTile;
        const float centre = static_cast<float>(bolt.z.raw) / static_cast<float>(sim::Fixed::kOne)
                             * kMetresPerHeightUnit;
        const Vec3 colour = bolt.side == options.viewer_side ? kOwnBoltColour : kHostileBoltColour;

        const float x0 = x - kBoltHalfWidthMetres;
        const float x1 = x + kBoltHalfWidthMetres;
        const float z0 = z - kBoltHalfWidthMetres;
        const float z1 = z + kBoltHalfWidthMetres;
        const float y0 = centre - kBoltHalfWidthMetres;
        const float y1 = centre + kBoltHalfWidthMetres;
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

/// The reticle's colour is the ANSWER to "what would a click here do": red fires, green selects,
/// white is ground. Text is not drawn anywhere in this project yet, so colour is the only channel
/// there is.
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
        const std::uint32_t max_hp = sim::profile(viewer->kind).hp;
        const std::uint32_t hp = std::min<std::uint32_t>(viewer->hp, max_hp);
        if (viewer->alive && max_hp > 0 && hp > 0) {
            const float fill
                = x0 + ((x1 - x0) * static_cast<float>(hp) / static_cast<float>(max_hp));
            rect(marks_at, x0, top, fill, bottom, health_colour(hp, max_hp));
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

constexpr float kAimedBarWidthFraction = 0.22F;  ///< of the frame width: 176 px at 800
constexpr float kAimedBarHeightFraction = 0.05F; ///< of the frame height: 22 px at 450
constexpr float kAimedBarGapFraction = 0.06F;    ///< from the crosshair to the bar, of the height
constexpr float kAimedBarMinWidth = 96.0F;
constexpr float kAimedBarMinHeight = 16.0F;
constexpr float kAimedBarBorderPixels = 3.0F;

/// The health bar under the crosshair, in frame pixels on the radar's HUD layers: a light border, a
/// dark track and a fill coloured like the reticle (the hostile family or the friendly one), so a
/// bar says whose unit it is as well as how hurt. Centred on the aim point and just below it, and
/// deliberately big -- a fifth of the frame wide -- because a small bar is one that cannot be read.
/// Always `kAimedBarVertexCount` vertices when a unit is aimed at, a zero-width fill at zero hp.
void append_aimed_bar(std::vector<SceneVertex>& out, const SceneOptions& options,
                      const sim::Sim& sim, const Pick& picked) {
    if (!options.aim.has_value() || !picked.has_unit()) {
        return;
    }
    const sim::Unit* unit = sim.find(picked.unit);
    if (unit == nullptr) {
        return;
    }
    const float frame_w = static_cast<float>(options.extent.width);
    const float frame_h = static_cast<float>(options.extent.height);
    const float width = std::max(kAimedBarMinWidth, frame_w * kAimedBarWidthFraction);
    const float height = std::max(kAimedBarMinHeight, frame_h * kAimedBarHeightFraction);
    const float gap = frame_h * kAimedBarGapFraction;
    const float left
        = std::clamp(options.aim->x - (width * 0.5F), 0.0F, std::max(0.0F, frame_w - width));
    const float right = left + width;
    const float top = options.aim->y + gap;
    const float bottom = top + height;

    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        const auto at = [&](float px, float py) {
            const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
            return Vec3{point.x, point.y, point.z};
        };
        emit_quad(out, at(ax, ay), at(bx, ay), at(bx, by), at(ax, by), rgb);
    };
    constexpr std::array<float, 3> kBorder{0.80F, 0.82F, 0.85F};
    constexpr std::array<float, 3> kTrack{0.04F, 0.05F, 0.06F};
    rect(marks_at * kHudBorderRatio, left, top, right, bottom, kBorder);
    const float inner_left = left + kAimedBarBorderPixels;
    const float inner_right = right - kAimedBarBorderPixels;
    const float inner_top = top + kAimedBarBorderPixels;
    const float inner_bottom = bottom - kAimedBarBorderPixels;
    rect(marks_at * kHudFaceRatio, inner_left, inner_top, inner_right, inner_bottom, kTrack);
    const std::uint32_t max_hp = sim::profile(unit->kind).hp;
    const std::uint32_t hp = std::min<std::uint32_t>(unit->hp, max_hp);
    const float share = max_hp > 0 ? static_cast<float>(hp) / static_cast<float>(max_hp) : 0.0F;
    rect(marks_at, inner_left, inner_top, inner_left + ((inner_right - inner_left) * share),
         inner_bottom, unit->side == options.viewer_side ? kReticleOwn : kReticleHostile);
}

/// Four bars forming a square outline, in the plane the camera looks along, centred on the point
/// the click's own ray reaches. Six vertices per bar: two triangles, and no winding to get wrong,
/// because the pipeline culls nothing.
void append_reticle(std::vector<SceneVertex>& out, const SceneOptions& options, const sim::Sim& sim,
                    const Pick& picked) {
    if (!options.aim.has_value()) {
        return;
    }
    const Camera& camera = options.camera;
    const float screen_x = options.aim->x;
    const float screen_y = options.aim->y;
    const std::array<float, 3> colour = reticle_colour(sim, picked, options.viewer_side);
    const RayPoint centre
        = ray_point(camera, options.extent, screen_x, screen_y, kReticleDistanceMetres);
    const math::Basis basis = math::basis_from(camera.yaw, camera.pitch);
    const float half
        = kReticleDistanceMetres * std::tan(math::kFovRadians * 0.5F) * kReticleHalfFraction;
    const float bar = half * kReticleBarFraction;
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
    bar_quad(-half, half - bar, half, half);               // top
    bar_quad(-half, -half, half, -half + bar);             // bottom
    bar_quad(-half, -half + bar, -half + bar, half - bar); // left
    bar_quad(half - bar, -half + bar, half, half - bar);   // right
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
            if (options.facing.has_value() && options.facing->unit == unit.id) {
                build_facing_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                  options.facing->heading, options.facing->turret);
            } else if (unit.kind == sim::UnitKind::Tank) {
                const float heading = options.headings != nullptr
                                          ? options.headings->of(unit)
                                          : Headings::heading_of(unit).value_or(0.0F);
                build_facing_tank(scene.vertices, options.seed, unit, mark, lift_z, hovering,
                                  heading, 0.0F);
            } else {
                build_unit_box(scene.vertices, options.seed, unit, mark, lift_z, hovering);
            }
        }
    }

    append_projectiles(scene.vertices, options, sim);
    append_radar(scene.vertices, options, sim);
    append_aimed_bar(scene.vertices, options, sim, aimed);
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
