#include "protekt/game/attack_aim.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "protekt/game/assist.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

/// A point the ray is cast to when its direction is wanted: any distance in front of the eye works,
/// because `ray_point` puts a pixel on the same ray at every distance.
constexpr float kRayProbeMetres = 100.0F;

/// The crosshair's ray as a vector from the eye (any length: `ray_point` is on the same line at
/// every distance).
[[nodiscard]] std::array<float, 3> crosshair_ray(const SceneOptions& options,
                                                 const Aim& aim) noexcept {
    const RayPoint probe = ray_point(options.camera, options.extent, aim.x, aim.y, kRayProbeMetres);
    return {probe.x - options.camera.x, probe.y - options.camera.y, probe.z - options.camera.z};
}

[[nodiscard]] std::array<float, 3> world_middle(const SceneOptions& options,
                                                const sim::Unit& unit) noexcept {
    const float one = static_cast<float>(sim::Fixed::kOne);
    return {static_cast<float>(unit.x.raw) / one * kMetresPerTile,
            ground_metres(options.seed, unit.x, unit.y) + (unit_box_height_metres(unit) * 0.5F),
            static_cast<float>(unit.y.raw) / one * kMetresPerTile};
}

} // namespace

Projected attack_point(const SceneOptions& options, const sim::Unit& unit) noexcept {
    const std::array<float, 3> at = world_middle(options, unit);
    return project(options.camera, options.extent, at.at(0), at.at(1), at.at(2));
}

float distance_metres(const SceneOptions& options, const sim::Unit& unit) noexcept {
    const std::array<float, 3> at = world_middle(options, unit);
    const float dx = at.at(0) - options.camera.x;
    const float dy = at.at(1) - options.camera.y;
    const float dz = at.at(2) - options.camera.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

std::optional<float> degrees_off_ray(const SceneOptions& options,
                                     const std::array<float, 3>& point) noexcept {
    if (!options.aim.has_value()) {
        return std::nullopt;
    }
    const std::array<float, 3> ray = crosshair_ray(options, options.aim.value());
    const std::array<float, 3> to{point.at(0) - options.camera.x, point.at(1) - options.camera.y,
                                  point.at(2) - options.camera.z};
    const float ray_length
        = std::sqrt((ray.at(0) * ray.at(0)) + (ray.at(1) * ray.at(1)) + (ray.at(2) * ray.at(2)));
    const float length
        = std::sqrt((to.at(0) * to.at(0)) + (to.at(1) * to.at(1)) + (to.at(2) * to.at(2)));
    if (length <= 0.0F || ray_length <= 0.0F) {
        return std::nullopt;
    }
    const float cosine
        = std::clamp(((ray.at(0) * to.at(0)) + (ray.at(1) * to.at(1)) + (ray.at(2) * to.at(2)))
                         / (ray_length * length),
                     -1.0F, 1.0F);
    return std::acos(cosine) * 180.0F / math::kPi;
}

namespace {

/// Whose assets the one scan below counts.
enum class Whose : std::uint8_t { Hostile, Friendly, Any };

/// THE ONE SCAN behind every "near the crosshair" in the game: the living, available unit whose
/// middle lies nearest the crosshair's ray within `degrees`, ties to the lower id. `Whose` narrows
/// it to the other side's units (the attack offer and the assist) or to the player's own (the
/// repair offer); `Any` counts every asset (the health bar's "pointing approximately at it"). The
/// driven hull is never a candidate: the eye sits on its barrel, so it is the thing being looked
/// out of, as `pick` has it.
sim::UnitId nearest_ray(const sim::Sim& sim, const SceneOptions& options, float degrees,
                        Whose whose) noexcept {
    if (!options.aim.has_value()) {
        return sim::UnitId{};
    }
    const std::array<float, 3> ray = crosshair_ray(options, options.aim.value());
    const float ray_length
        = std::sqrt((ray.at(0) * ray.at(0)) + (ray.at(1) * ray.at(1)) + (ray.at(2) * ray.at(2)));
    const float cos_window = std::cos(degrees * math::kPi / 180.0F);
    sim::UnitId best{};
    float best_cos = -1.0F;
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || (whose == Whose::Hostile && unit.side == sim::Side::Player)
            || (whose == Whose::Friendly && unit.side != sim::Side::Player)
            || unit.id == options.driven_unit || !available_to_viewer(sim, options, unit)) {
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

} // namespace

sim::UnitId hostile_nearest_ray(const sim::Sim& sim, const SceneOptions& options,
                                float degrees) noexcept {
    return nearest_ray(sim, options, degrees, Whose::Hostile);
}

sim::UnitId friendly_nearest_ray(const sim::Sim& sim, const SceneOptions& options,
                                 float degrees) noexcept {
    return nearest_ray(sim, options, degrees, Whose::Friendly);
}

sim::UnitId asset_nearest_ray(const sim::Sim& sim, const SceneOptions& options,
                              float degrees) noexcept {
    return nearest_ray(sim, options, degrees, Whose::Any);
}

sim::UnitId pointed_asset(const sim::Sim& sim, const SceneOptions& options,
                          const Pick& aimed) noexcept {
    if (aimed.has_unit() && aimed.unit != options.driven_unit) {
        return aimed.unit;
    }
    return asset_nearest_ray(sim, options, kAssistConeDegrees);
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
            && sim::spec_of(unit->kind).range().raw > 0) {
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
    return hostile_nearest_ray(sim, options, kAttackAimToleranceDegrees);
}

} // namespace protekt::game
