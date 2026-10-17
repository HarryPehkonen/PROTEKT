#include "protekt/game/scene.hpp"

#include <cmath>
#include <cstdlib>

namespace protekt::game {

namespace {

using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;

constexpr float kPi = 3.14159265358979F;

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (std::size_t col = 0; col < 4; ++col) {
        for (std::size_t row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a.at(k * 4 + row) * b.at(col * 4 + k);
            }
            r.at(col * 4 + row) = sum;
        }
    }
    return r;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

Vec3 normalise(const Vec3& v) {
    const float len = std::sqrt(dot(v, v));
    return {v[0] / len, v[1] / len, v[2] / len};
}

/// Right-handed look-at, the camera looking down `target - eye`. `s` is the RIGHT vector: with
/// up = (0,1,0) and forward = (0,0,1) (yaw 0), smaller world x must land left of screen centre,
/// which is cross(up, forward), not cross(forward, up).
Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 f = normalise({target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]});
    // f x up, not up x f: the other order makes u = -Y, and since the projection already flips Y
    // for Vulkan's clip space the two cancel into an upside-down frame. Measured, not guessed:
    // with up x f the sky rendered along the bottom edge.
    const Vec3 s = normalise(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 m{};
    m[0] = s[0];
    m[4] = s[1];
    m[8] = s[2];
    m[1] = u[0];
    m[5] = u[1];
    m[9] = u[2];
    m[2] = -f[0];
    m[6] = -f[1];
    m[10] = -f[2];
    m[12] = -dot(s, eye);
    m[13] = -dot(u, eye);
    m[14] = dot(f, eye);
    m[15] = 1.0F;
    return m;
}

/// Vulkan clip space: Z in [0,1], Y pointing down.
Mat4 perspective(float fovy_radians, float aspect, float near_plane, float far_plane) {
    const float f = 1.0F / std::tan(fovy_radians * 0.5F);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = far_plane / (near_plane - far_plane);
    m[11] = -1.0F;
    m[14] = far_plane * near_plane / (near_plane - far_plane);
    return m;
}

Vec3 forward_of(const Camera& camera) {
    return {std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
            std::cos(camera.yaw) * std::cos(camera.pitch)};
}

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

float height_metres(std::uint64_t seed, int tile_x, int tile_y) {
    return static_cast<float>(world::sample(seed, tile_x, tile_y).height) * kMetresPerHeightUnit;
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
void build_unit_box(std::vector<render::SceneVertex>& out, std::uint64_t seed,
                    const sim::Unit& unit) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(seed, unit.x.floor_int(), unit.y.floor_int());
    const float half_width = kMetresPerTile * 0.3F;
    const float height = unit.kind == sim::UnitKind::Factory ? 4.5F : 1.8F;
    const Vec3 colour = side_colour(unit.side);

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

} // namespace

float Camera::wrap_angle(float radians) noexcept {
    float wrapped = std::fmod(radians + kPi, 2.0F * kPi);
    if (wrapped < 0.0F) {
        wrapped += 2.0F * kPi;
    }
    return wrapped - kPi;
}

std::array<float, 16> view_projection(const Camera& camera, float aspect,
                                      float fov_radians) noexcept {
    const Vec3 eye{camera.x, camera.y, camera.z};
    const Vec3 target = add(eye, forward_of(camera));
    const Mat4 view = look_at(eye, target, {0.0F, 1.0F, 0.0F});
    const Mat4 proj = perspective(fov_radians, aspect, 0.1F, 2000.0F);
    return multiply(proj, view);
}

render::Scene build_scene(const sim::Sim& sim, const SceneOptions& options) {
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
            build_unit_box(scene.vertices, options.seed, unit);
        }
    }

    return scene;
}

} // namespace protekt::game
