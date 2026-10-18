#include "protekt/game/scene.hpp"

#include "protekt/game/pick.hpp"
#include "protekt/math/math.hpp"

#include <algorithm>

#include <cmath>
#include <cstdlib>

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

void emit_triangle(std::vector<render::SceneVertex>& out, const Vec3& a, const Vec3& b,
                   const Vec3& c, const Vec3& colour) {
    out.push_back(render::SceneVertex{a, colour});
    out.push_back(render::SceneVertex{b, colour});
    out.push_back(render::SceneVertex{c, colour});
}

void emit_quad(std::vector<render::SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
               const Vec3& d, const Vec3& colour) {
    emit_triangle(out, a, b, c, colour);
    emit_triangle(out, a, c, d, colour);
}

void build_terrain(std::vector<render::SceneVertex>& out, std::uint64_t seed, int centre_x,
                   int centre_y, int radius) {
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

/// A box standing on the ground beneath (x, z): wider and shorter for a tank or turret, taller
/// for a factory.
/// A selected unit keeps its side's colour and is mixed towards white, so a glance tells you both
/// whose it is and that it is yours to move.
Vec3 highlight(const Vec3& colour) {
    constexpr float kMix = 0.55F;
    return Vec3{colour[0] + ((1.0F - colour[0]) * kMix), colour[1] + ((1.0F - colour[1]) * kMix),
                colour[2] + ((1.0F - colour[2]) * kMix)};
}

void build_unit_box(std::vector<render::SceneVertex>& out, std::uint64_t seed,
                    const sim::Unit& unit, bool selected) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(seed, unit.x.floor_int(), unit.y.floor_int());
    const float half_width = kMetresPerTile * 0.3F;
    const float height = unit.kind == sim::UnitKind::Factory ? 4.5F : 1.8F;
    const Vec3 colour = selected ? highlight(side_colour(unit.side)) : side_colour(unit.side);

    const float x0 = x - half_width;
    const float x1 = x + half_width;
    const float z0 = z - half_width;
    const float z1 = z + half_width;
    const float y0 = ground;
    const float y1 = ground + height;
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

/// The reticle's distance from the eye, in metres. This close, nothing can stand between the eye
/// and the mark: it is a screen mark, and a crater rim must not be able to hide it.
constexpr float kReticleDistanceMetres = 1.5F;
/// Its half-size as a fraction of the half-screen at that distance, so it looks the same at any
/// extent.
constexpr float kReticleHalfFraction = 0.055F;
/// How thick each bar is, as a fraction of the half-size.
constexpr float kReticleBarFraction = 0.22F;

/// The reticle's colour is the ANSWER to "what would a click here do": red fires, green selects,
/// white is ground. Text is not drawn anywhere in this project yet, so colour is the only channel
/// there is.
std::array<float, 3> reticle_colour(const sim::Sim& sim, const Pick& picked, sim::Side viewer) {
    constexpr std::array<float, 3> kGround{0.92F, 0.92F, 0.92F};
    constexpr std::array<float, 3> kHostile{1.0F, 0.25F, 0.25F};
    constexpr std::array<float, 3> kOwn{0.45F, 1.0F, 0.45F};
    if (!picked.has_unit()) {
        return kGround;
    }
    const sim::Unit* unit = sim.find(picked.unit);
    if (unit == nullptr) {
        return kGround;
    }
    return unit->side == viewer ? kOwn : kHostile;
}

/// Four bars forming a square outline, in the plane the camera looks along, centred on the point
/// the click's own ray reaches. Six vertices per bar: two triangles, and no winding to get wrong,
/// because the pipeline culls nothing.
void append_reticle(std::vector<render::SceneVertex>& out, const SceneOptions& options,
                    const sim::Sim& sim) {
    if (!options.aim.has_value()) {
        return;
    }
    const Camera& camera = options.camera;
    const float screen_x = options.aim->x;
    const float screen_y = options.aim->y;
    const std::array<float, 3> colour
        = reticle_colour(sim, pick(sim, options, screen_x, screen_y), options.viewer_side);
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
        out.push_back(render::SceneVertex{a, colour});
        out.push_back(render::SceneVertex{b, colour});
        out.push_back(render::SceneVertex{c, colour});
        out.push_back(render::SceneVertex{a, colour});
        out.push_back(render::SceneVertex{c, colour});
        out.push_back(render::SceneVertex{d, colour});
    };
    bar_quad(-half, half - bar, half, half);               // top
    bar_quad(-half, -half, half, -half + bar);             // bottom
    bar_quad(-half, -half + bar, -half + bar, half - bar); // left
    bar_quad(half - bar, -half + bar, half, half - bar);   // right
}

} // namespace

float height_metres(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    return static_cast<float>(world::sample(seed, tile_x, tile_y).height) * kMetresPerHeightUnit;
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

render::Scene build_scene(const sim::Sim& sim, const SceneOptions& options,
                          std::span<const sim::UnitId> selected) {
    render::Scene scene;
    const float aspect
        = static_cast<float>(options.extent.width) / static_cast<float>(options.extent.height);
    scene.view_projection = view_projection(options.camera, aspect);

    const int centre_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int centre_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    build_terrain(scene.vertices, options.seed, centre_x, centre_y, options.radius_tiles);

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
            build_unit_box(scene.vertices, options.seed, unit, is_selected);
        }
    }

    append_reticle(scene.vertices, options, sim);

    return scene;
}

} // namespace protekt::game
