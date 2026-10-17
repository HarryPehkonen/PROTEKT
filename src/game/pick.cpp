#include "protekt/game/pick.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace protekt::game {

namespace {

constexpr float kFovRadians = 1.0472F; // view_projection's default, the one the renderer draws with

struct Basis {
    std::array<float, 3> forward;
    std::array<float, 3> right;
    std::array<float, 3> up;
};

/// The camera's axes, built exactly as `look_at` builds them: s = f x worldUp, u = s x f.
Basis basis_of(const Camera& camera) {
    const float cp = std::cos(camera.pitch);
    const std::array<float, 3> f{std::sin(camera.yaw) * cp, std::sin(camera.pitch),
                                 std::cos(camera.yaw) * cp};
    // f x (0,1,0) = (-f.z, 0, f.x)
    const float sl = std::sqrt((f[2] * f[2]) + (f[0] * f[0]));
    const std::array<float, 3> s{-f[2] / sl, 0.0F, f[0] / sl};
    const std::array<float, 3> u{(s[1] * f[2]) - (s[2] * f[1]), (s[2] * f[0]) - (s[0] * f[2]),
                                 (s[0] * f[1]) - (s[1] * f[0])};
    return Basis{f, s, u};
}

float aspect_of(render::Extent extent) {
    return static_cast<float>(extent.width) / static_cast<float>(extent.height);
}

struct Ray {
    std::array<float, 3> origin;
    std::array<float, 3> dir; ///< unit length
};

Ray ray_through(const Camera& camera, render::Extent extent, float screen_x, float screen_y) {
    const Basis b = basis_of(camera);
    const float half_tan = std::tan(kFovRadians * 0.5F);
    // Pixel -> NDC. Y is measured from the top, which is the way Vulkan's clip Y already points.
    const float ndc_x = (2.0F * screen_x / static_cast<float>(extent.width)) - 1.0F;
    const float ndc_y = (2.0F * screen_y / static_cast<float>(extent.height)) - 1.0F;
    // The projection has m[5] = -f: clip y = -f * view_y, so view_y / depth = -ndc_y * tan(half).
    const float rx = ndc_x * aspect_of(extent) * half_tan;
    const float ry = -ndc_y * half_tan;
    std::array<float, 3> d{};
    for (std::size_t i = 0; i < 3; ++i) {
        d.at(i) = b.forward.at(i) + (rx * b.right.at(i)) + (ry * b.up.at(i));
    }
    const float len = std::sqrt((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]));
    return Ray{{camera.x, camera.y, camera.z}, {d[0] / len, d[1] / len, d[2] / len}};
}

/// Slab test: distance along the ray to the box, or a negative number for a miss.
float hit_box(const Ray& ray, const std::array<float, 3>& lo, const std::array<float, 3>& hi) {
    float t_near = 0.0F;
    float t_far = kPickMaxDistanceTiles * kMetresPerTile;
    for (std::size_t i = 0; i < 3; ++i) {
        const float o = ray.origin.at(i);
        const float d = ray.dir.at(i);
        if (std::abs(d) < 1e-9F) {
            if (o < lo.at(i) || o > hi.at(i)) {
                return -1.0F;
            }
            continue;
        }
        float t0 = (lo.at(i) - o) / d;
        float t1 = (hi.at(i) - o) / d;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_near = std::max(t_near, t0);
        t_far = std::min(t_far, t1);
        if (t_near > t_far) {
            return -1.0F;
        }
    }
    return t_near;
}

} // namespace

Projected project(const Camera& camera, render::Extent extent, float world_x, float world_y,
                  float world_z) noexcept {
    const std::array<float, 16> m = view_projection(camera, aspect_of(extent), kFovRadians);
    const auto row = [&m, world_x, world_y, world_z](std::size_t r) {
        return (m.at(r) * world_x) + (m.at(4 + r) * world_y) + (m.at(8 + r) * world_z)
               + m.at(12 + r);
    };
    const float cx = row(0);
    const float cy = row(1);
    const float cw = row(3); // the distance along the view axis, for a perspective matrix
    // Viewport transform: NDC -1..1 -> 0..size. Vulkan's Y already runs top-down.
    const float safe_w = std::abs(cw) < 1e-9F ? 1e-9F : cw;
    return Projected{(((cx / safe_w) * 0.5F) + 0.5F) * static_cast<float>(extent.width),
                     (((cy / safe_w) * 0.5F) + 0.5F) * static_cast<float>(extent.height), cw};
}

RayPoint ray_point(const Camera& camera, render::Extent extent, float screen_x, float screen_y,
                   float distance_metres) noexcept {
    const Ray ray = ray_through(camera, extent, screen_x, screen_y);
    return RayPoint{ray.origin[0] + (ray.dir[0] * distance_metres),
                    ray.origin[1] + (ray.dir[1] * distance_metres),
                    ray.origin[2] + (ray.dir[2] * distance_metres)};
}

Pick pick(const sim::Sim& sim, const SceneOptions& options, float screen_x,
          float screen_y) noexcept {
    const Ray ray = ray_through(options.camera, options.extent, screen_x, screen_y);
    Pick result{};
    float best_unit_t = -1.0F;

    const float radius = kPickUnitRadiusTiles * kMetresPerTile;
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive) {
            continue;
        }
        const float x = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne)
                        * kMetresPerTile;
        const float z = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne)
                        * kMetresPerTile;
        const float ground = height_metres(options.seed, unit.x.floor_int(), unit.y.floor_int());
        const float height = unit.kind == sim::UnitKind::Factory ? 4.5F : 1.8F;
        const float t = hit_box(ray, {x - radius, ground, z - radius},
                                {x + radius, ground + height, z + radius});
        if (t < 0.0F) {
            continue;
        }
        const bool better = best_unit_t < 0.0F || t < best_unit_t
                            || (t == best_unit_t && unit.id.value < result.unit.value);
        if (better) {
            best_unit_t = t;
            result.unit = unit.id;
        }
    }

    // Ground: march in tile steps, stop at the first sample under the terrain, interpolate.
    const auto height_above = [&](float t_tiles) {
        const float t = t_tiles * kMetresPerTile;
        const float px = ray.origin[0] + (ray.dir[0] * t);
        const float py = ray.origin[1] + (ray.dir[1] * t);
        const float pz = ray.origin[2] + (ray.dir[2] * t);
        const int tx = static_cast<int>(std::floor(px / kMetresPerTile));
        const int tz = static_cast<int>(std::floor(pz / kMetresPerTile));
        return py - height_metres(options.seed, tx, tz);
    };
    float prev_t = 0.0F;
    float prev_h = height_above(0.0F);
    float hit_t = prev_h <= 0.0F ? 0.0F : -1.0F;
    for (float t = kPickStepTiles; hit_t < 0.0F && t <= kPickMaxDistanceTiles;
         t += kPickStepTiles) {
        const float h = height_above(t);
        if (h <= 0.0F) {
            const float span = prev_h - h;
            hit_t = span > 0.0F ? prev_t + ((t - prev_t) * (prev_h / span)) : t;
        }
        prev_t = t;
        prev_h = h;
    }
    if (hit_t >= 0.0F) {
        const float hx = ray.origin[0] + (ray.dir[0] * hit_t * kMetresPerTile);
        const float hz = ray.origin[2] + (ray.dir[2] * hit_t * kMetresPerTile);
        const int tx = static_cast<int>(std::floor(hx / kMetresPerTile));
        const int tz = static_cast<int>(std::floor(hz / kMetresPerTile));
        if (tx >= 0 && tz >= 0) {
            result.tile_x = tx;
            result.tile_y = tz;
            result.distance_tiles = hit_t;
        }
    }
    if (best_unit_t >= 0.0F) {
        result.distance_tiles = std::min(result.distance_tiles, best_unit_t / kMetresPerTile);
    }
    return result;
}

} // namespace protekt::game
