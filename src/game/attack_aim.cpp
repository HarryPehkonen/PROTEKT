#include "protekt/game/attack_aim.hpp"

#include <array>
#include <cmath>

#include "protekt/math/math.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

/// A point the ray is cast to when its direction is wanted: any distance in front of the eye works,
/// because `ray_point` puts a pixel on the same ray at every distance.
constexpr float kRayProbeMetres = 100.0F;

[[nodiscard]] std::array<float, 3> world_middle(const SceneOptions& options,
                                                const sim::Unit& unit) noexcept {
    const float one = static_cast<float>(sim::Fixed::kOne);
    return {static_cast<float>(unit.x.raw) / one * kMetresPerTile,
            ground_metres(options.seed, unit.x, unit.y)
                + (unit_box_height_metres(unit.kind) * 0.5F),
            static_cast<float>(unit.y.raw) / one * kMetresPerTile};
}

} // namespace

Projected attack_point(const SceneOptions& options, const sim::Unit& unit) noexcept {
    const std::array<float, 3> at = world_middle(options, unit);
    return project(options.camera, options.extent, at.at(0), at.at(1), at.at(2));
}

sim::UnitId attack_target(const sim::Sim& sim, const SceneOptions& options,
                          std::span<const sim::UnitId> selected, sim::UnitId driven) noexcept {
    if (!options.aim.has_value()) {
        return sim::UnitId{};
    }
    bool armed = false;
    for (const sim::UnitId id : selected) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->alive && unit->side == sim::Side::Player && id != driven
            && sim::spec_of(unit->kind).range.raw > 0) {
            armed = true;
            break;
        }
    }
    if (!armed) {
        return sim::UnitId{};
    }
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    if (picked.has_unit()) {
        const sim::Unit* hit = sim.find(picked.unit);
        if (hit != nullptr && hit->alive) {
            return hit->side != sim::Side::Player ? hit->id : sim::UnitId{};
        }
    }
    const RayPoint probe = ray_point(options.camera, options.extent, options.aim->x, options.aim->y,
                                     kRayProbeMetres);
    const std::array<float, 3> ray{probe.x - options.camera.x, probe.y - options.camera.y,
                                   probe.z - options.camera.z};
    const float ray_length
        = std::sqrt((ray.at(0) * ray.at(0)) + (ray.at(1) * ray.at(1)) + (ray.at(2) * ray.at(2)));
    const float cos_window = std::cos(kAttackAimToleranceDegrees * math::kPi / 180.0F);
    sim::UnitId best{};
    float best_cos = -1.0F;
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.side == sim::Side::Player) {
            continue;
        }
        const std::array<float, 3> at = world_middle(options, unit);
        const std::array<float, 3> to{at.at(0) - options.camera.x, at.at(1) - options.camera.y,
                                      at.at(2) - options.camera.z};
        const float length
            = std::sqrt((to.at(0) * to.at(0)) + (to.at(1) * to.at(1)) + (to.at(2) * to.at(2)));
        if (length <= 0.0F || ray_length <= 0.0F) {
            continue;
        }
        const float cosine
            = ((ray.at(0) * to.at(0)) + (ray.at(1) * to.at(1)) + (ray.at(2) * to.at(2)))
              / (ray_length * length);
        // Units come in ascending id order and only a STRICTLY nearer ray replaces the holder,
        // so a tie goes to the lower id.
        if (cosine >= cos_window && (!best.valid() || cosine > best_cos)) {
            best_cos = cosine;
            best = unit.id;
        }
    }
    return best;
}

} // namespace protekt::game
