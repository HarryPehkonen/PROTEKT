#include "protekt/game/targets.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/fixed.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

struct Candidate {
    std::int64_t d2{}; ///< squared tile distance in raw Q16.16 units, so exact
    const sim::Unit* unit{};
};

/// `found` nearest first (ties to the lower id), cut to the list's length and keyed 1-9 then 0. The
/// radius test and the order are integer arithmetic on the sim's own Fixed, so they cannot disagree
/// between machines; the float is for the printed metres and nothing else.
[[nodiscard]] std::vector<TargetRow> ranked(std::vector<Candidate> found) {
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        return a.d2 != b.d2 ? a.d2 < b.d2 : a.unit->id < b.unit->id;
    });
    std::vector<TargetRow> rows;
    rows.reserve(std::min(found.size(), kAttackListRows));
    for (std::size_t i = 0; i < found.size() && i < kAttackListRows; ++i) {
        const float tiles
            = std::sqrt(static_cast<float>(found.at(i).d2)) / static_cast<float>(sim::Fixed::kOne);
        rows.push_back(TargetRow{static_cast<int>((i + 1) % kAttackListRows),
                                 found.at(i).unit->kind, found.at(i).unit->id,
                                 static_cast<int>(std::floor(tiles * kMetresPerTile + 0.5F))});
    }
    return rows;
}

/// Squared Q16.16 distance from `origin` to `unit`.
[[nodiscard]] std::int64_t squared_distance(const sim::Unit& origin, const sim::Unit& unit) {
    const std::int64_t dx = static_cast<std::int64_t>(unit.x.raw) - origin.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(unit.y.raw) - origin.y.raw;
    return dx * dx + dy * dy;
}

} // namespace

std::vector<TargetRow> nearest_enemies(const sim::Sim& sim, sim::Side from, sim::UnitId centre,
                                       int radius_tiles, const SceneOptions* view) {
    const sim::Unit* origin = sim.find(centre);
    if (origin == nullptr || !origin->alive || origin->side != from) {
        return {}; // nothing to measure from: not the camera, not the map centre, not the origin
    }
    const std::int64_t reach = static_cast<std::int64_t>(radius_tiles) * sim::Fixed::kOne;
    std::vector<Candidate> found;
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.side == from
            || (view != nullptr && !available_to_viewer(sim, *view, unit))) {
            continue;
        }
        const std::int64_t d2 = squared_distance(*origin, unit);
        if (d2 <= reach * reach) {
            found.push_back(Candidate{d2, &unit});
        }
    }
    return ranked(std::move(found));
}

std::vector<TargetRow> nearest_wounded(const sim::Sim& sim, sim::Side side, sim::UnitId centre,
                                       int radius_tiles, std::span<const sim::UnitId> trucks) {
    const sim::Unit* origin = sim.find(centre);
    if (origin == nullptr || !origin->alive || origin->side != side) {
        return {};
    }
    const std::int64_t reach = static_cast<std::int64_t>(radius_tiles) * sim::Fixed::kOne;
    // A truck that could be sent: living, the side's own, and not the patient it would be sent to.
    const auto sendable_to = [&](sim::UnitId patient) {
        return std::any_of(trucks.begin(), trucks.end(), [&](sim::UnitId id) {
            const sim::Unit* truck = sim.find(id);
            return truck != nullptr && truck->alive && truck->side == side && id != patient
                   && truck->kind == sim::UnitKind::ServiceTruck;
        });
    };
    std::vector<Candidate> found;
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.side != side || unit.id == centre
            || unit.hp >= sim::spec_of(unit.kind).hp || !sendable_to(unit.id)) {
            continue;
        }
        const std::int64_t d2 = squared_distance(*origin, unit);
        if (d2 <= reach * reach) {
            found.push_back(Candidate{d2, &unit});
        }
    }
    return ranked(std::move(found));
}

} // namespace protekt::game
