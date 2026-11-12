#include "protekt/game/deploy_aim.hpp"

#include <cmath>
#include <vector>

#include "protekt/game/attack_aim.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/sim/pool.hpp"

namespace protekt::game {
namespace {

/// The pool nearest the crosshair's ray within the pointing cone, that the viewer's radar reaches
/// and no deployed scavenger holds. The angle is taken in 3D to the middle of the pool's tile on
/// the ground, by `degrees_off_ray` -- the measure every other "near the crosshair" uses.
[[nodiscard]] const sim::Pool* pool_in_cone(const sim::Sim& sim, const SceneOptions& options) {
    const sim::Unit* viewer = sim.find(options.viewer_unit);
    const float reach = static_cast<float>(viewer_radar_radius(sim, options.viewer_unit));
    const float cone = static_cast<float>(sim::kPointingConeDegrees);
    const sim::Pool* best = nullptr;
    float best_degrees = cone;
    for (const sim::Pool& pool : sim.pools()) {
        const float tile_x = static_cast<float>(pool.tile_x) + 0.5F;
        const float tile_y = static_cast<float>(pool.tile_y) + 0.5F;
        if (viewer != nullptr) {
            const float from_x
                = static_cast<float>(viewer->x.raw) / static_cast<float>(sim::Fixed::kOne);
            const float from_y
                = static_cast<float>(viewer->y.raw) / static_cast<float>(sim::Fixed::kOne);
            if (std::hypot(tile_x - from_x, tile_y - from_y) > reach) {
                continue; // beyond the radar: the player cannot see it to point at it
            }
        }
        if (sim::pool_taken(sim.units(), pool)) {
            continue; // the sim would refuse it, so it is not offered
        }
        const std::optional<float> degrees = degrees_off_ray(
            options, {tile_x * kMetresPerTile, ground_metres(options.seed, tile_x, tile_y),
                      tile_y * kMetresPerTile});
        // A STRICTLY nearer ray replaces the holder, so a tie goes to the pool listed first.
        if (degrees.has_value() && *degrees <= best_degrees
            && (best == nullptr || *degrees < best_degrees)) {
            best = &pool;
            best_degrees = *degrees;
        }
    }
    return best;
}

} // namespace

std::optional<DeployOffer> deploy_offer(const sim::Sim& sim, const SceneOptions& options,
                                        std::span<const sim::UnitId> selected,
                                        sim::UnitId driven) noexcept {
    if (!options.aim.has_value()) {
        return std::nullopt;
    }
    // Who could take the order: the player's living units but the one being driven.
    std::vector<const sim::Unit*> orderable;
    for (const sim::UnitId id : selected) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->alive && unit->side == sim::Side::Player && id != driven) {
            orderable.push_back(unit);
        }
    }
    if (orderable.empty()) {
        return std::nullopt;
    }
    // The scavenger's prompt: ONE scavenger selected. A group does not get it, so the selection
    // itself is counted, not what could be ordered of it.
    if (selected.size() == 1 && orderable.front()->kind == sim::UnitKind::Scavenger) {
        if (const sim::Pool* pool = pool_in_cone(sim, options)) {
            return DeployOffer{DeploySite::Pool, pool->tile_x, pool->tile_y};
        }
        return std::nullopt;
    }
    for (const sim::Unit* unit : orderable) {
        if (!sim::is_turret(unit->kind)) {
            return std::nullopt;
        }
    }
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    if (picked.has_unit() || !picked.has_tile()
        || !sim::tile_passable(sim.seed(), picked.tile_x, picked.tile_y)
        || sim::building_blocks(sim.units(), picked.tile_x, picked.tile_y)) {
        return std::nullopt;
    }
    return DeployOffer{DeploySite::Spot, picked.tile_x, picked.tile_y};
}

DeployPreview placement_preview(const sim::Sim& sim, const Placing& placing, int tile_x,
                                int tile_y) noexcept {
    // The footprint as the sim will stand it (clipped onto the map) and the SIM'S verdict on it:
    // green is a promise that SPACE places there, red that it refuses -- never re-derived here.
    return DeployPreview{
        sim::footprint_at(placing.size, tile_x, tile_y),
        sim::place_ok(sim.seed(), sim.units(), placing.kind, placing.turned, tile_x, tile_y)};
}

} // namespace protekt::game
