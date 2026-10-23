#include "protekt/game/targets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "protekt/game/scene.hpp"
#include "protekt/sim/fixed.hpp"

namespace protekt::game {
namespace {

struct Candidate {
    std::int64_t d2{}; ///< squared tile distance in raw Q16.16 units, so exact
    const sim::Unit* unit{};
};

} // namespace

std::vector<TargetRow> nearest_enemies(const sim::Sim& sim, sim::Side from, sim::UnitId centre,
                                       int radius_tiles) {
    const sim::Unit* origin = sim.find(centre);
    if (origin == nullptr || !origin->alive || origin->side != from) {
        return {}; // nothing to measure from: not the camera, not the map centre, not the origin
    }
    // The radius test and the order are integer arithmetic on the sim's own Fixed, so they cannot
    // disagree between machines. The float below is for the printed metres and nothing else.
    const std::int64_t reach = static_cast<std::int64_t>(radius_tiles) * sim::Fixed::kOne;
    std::vector<Candidate> found;
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.side == from) {
            continue;
        }
        const std::int64_t dx = static_cast<std::int64_t>(unit.x.raw) - origin->x.raw;
        const std::int64_t dy = static_cast<std::int64_t>(unit.y.raw) - origin->y.raw;
        const std::int64_t d2 = dx * dx + dy * dy;
        if (d2 <= reach * reach) {
            found.push_back(Candidate{d2, &unit});
        }
    }
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        return a.d2 != b.d2 ? a.d2 < b.d2 : a.unit->id < b.unit->id;
    });

    std::vector<TargetRow> rows;
    for (std::size_t i = 0; i < found.size() && i < kAttackListRows; ++i) {
        const float tiles
            = std::sqrt(static_cast<float>(found.at(i).d2)) / static_cast<float>(sim::Fixed::kOne);
        rows.push_back(TargetRow{static_cast<int>((i + 1) % kAttackListRows),
                                 found.at(i).unit->kind, found.at(i).unit->id,
                                 static_cast<int>(std::floor(tiles * kMetresPerTile + 0.5F))});
    }
    return rows;
}

} // namespace protekt::game
