#include "protekt/game/pick.hpp"

#include "protekt/game/radar.hpp"
#include "protekt/math/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace protekt::game {

float unit_box_height_metres(sim::UnitKind kind) noexcept {
    return kind == sim::UnitKind::Factory ? 4.5F : 1.8F;
}

namespace {

float aspect_of(Extent extent) {
    return static_cast<float>(extent.width) / static_cast<float>(extent.height);
}

struct Ray {
    std::array<float, 3> origin;
    std::array<float, 3> dir; ///< unit length
};

Ray ray_through(const Camera& camera, Extent extent, float screen_x, float screen_y) {
    // The camera's axes come from the module the renderer builds its view matrix from, so a click
    // cannot land somewhere other than where the crosshair is drawn.
    const math::Basis b = math::basis_from(camera.yaw, camera.pitch);
    const float half_tan = std::tan(math::kFovRadians * 0.5F);
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

/// Slab test: distance along the ray to the box, or a negative number for a miss. A ray that
/// STARTS inside the box misses it: the driver's eye sits on the barrel's line, inside the driven
/// tank's own pick box, and a view cannot be looking at the thing it is looking out of.
float hit_box(const Ray& ray, const std::array<float, 3>& lo, const std::array<float, 3>& hi) {
    const bool inside
        = std::ranges::all_of(std::array<std::size_t, 3>{0, 1, 2}, [&](std::size_t i) {
              return ray.origin.at(i) >= lo.at(i) && ray.origin.at(i) <= hi.at(i);
          });
    if (inside) {
        return -1.0F;
    }
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

bool hidden_by_reach(const sim::Sim& sim, const SceneOptions& options, const sim::Unit& unit) {
    if (unit.side == options.viewer_side) {
        return false;
    }
    const sim::Unit* viewer = sim.find(options.viewer_unit);
    if (viewer == nullptr) {
        return false;
    }
    const auto tiles = [](const sim::Fixed& value) {
        return static_cast<float>(value.raw) / static_cast<float>(sim::Fixed::kOne);
    };
    const float distance
        = std::hypot(tiles(unit.x) - tiles(viewer->x), tiles(unit.y) - tiles(viewer->y));
    return distance > static_cast<float>(viewer_radar_radius(sim, options.viewer_unit));
}

bool outside_drawn_radius(const SceneOptions& options, const sim::Unit& unit) noexcept {
    const int centre_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int centre_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    return std::abs(unit.x.floor_int() - centre_x) > options.radius_tiles
           || std::abs(unit.y.floor_int() - centre_y) > options.radius_tiles;
}

bool available_to_viewer(const sim::Sim& sim, const SceneOptions& options, const sim::Unit& unit) {
    return !outside_drawn_radius(options, unit) && !hidden_by_reach(sim, options, unit);
}

Projected project(const Camera& camera, Extent extent, float world_x, float world_y,
                  float world_z) noexcept {
    const std::array<float, 16> m = view_projection(camera, aspect_of(extent), math::kFovRadians);
    const std::array<float, 4> clip = math::transform(m, {world_x, world_y, world_z});
    const float cx = clip.at(0);
    const float cy = clip.at(1);
    const float cw = clip.at(3); // the distance along the view axis, for a perspective matrix
    // Viewport transform: NDC -1..1 -> 0..size. Vulkan's Y already runs top-down.
    const float safe_w = std::abs(cw) < 1e-9F ? 1e-9F : cw;
    return Projected{(((cx / safe_w) * 0.5F) + 0.5F) * static_cast<float>(extent.width),
                     (((cy / safe_w) * 0.5F) + 0.5F) * static_cast<float>(extent.height), cw};
}

RayPoint ray_point(const Camera& camera, Extent extent, float screen_x, float screen_y,
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
        if (!unit.alive || !available_to_viewer(sim, options, unit)) {
            continue;
        }
        const float x = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne)
                        * kMetresPerTile;
        const float z = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne)
                        * kMetresPerTile;
        const float ground = ground_metres(options.seed, unit.x, unit.y);
        // A hovering hull is DRAWN above the ground, so its pick box has to be up there too:
        // otherwise a click just under the tank falls through to the terrain behind it. ABSENT
        // hover leaves every unit on the ground, which is what a headless frame gets.
        const bool hovering
            = options.hover.has_value() && options.hover->unit.value == unit.id.value;
        const float base = hovering ? options.hover->z : ground;
        const float height = unit_box_height_metres(unit.kind);
        const float t
            = hit_box(ray, {x - radius, base, z - radius}, {x + radius, base + height, z + radius});
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
        return py - ground_metres(options.seed, px / kMetresPerTile, pz / kMetresPerTile);
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
