#include "protekt/game/stats.hpp"

#include <cstddef>
#include <vector>

namespace protekt::game {

void BattleStats::observe(const sim::Sim& sim) {
    const std::vector<sim::Unit>& units = sim.units();
    // The table only grows, and only by a queue's finished build: everything past what was known is
    // new this step.
    for (std::size_t index = known_units_; index < units.size(); ++index) {
        const sim::Unit& unit = units.at(index);
        if (is_counted_vehicle(unit.kind)) {
            ++mine(unit.side).built;
        }
    }
    known_units_ = units.size();
    for (const sim::Sim::Shot& shot : sim.shots()) {
        if (const sim::Unit* shooter = sim.find(shot.shooter); shooter != nullptr) {
            ++mine(shooter->side).fired;
        }
    }
    for (const sim::Impact& impact : sim.impacts()) {
        if (const sim::Unit* shooter = sim.find(impact.shooter); shooter != nullptr) {
            ++mine(shooter->side).hit;
        }
        // An impact that leaves no hit points is a kill, and a dead unit is never hit again.
        if (const sim::Unit* victim = sim.find(impact.victim);
            victim != nullptr && impact.victim_hp == 0 && is_counted_vehicle(victim->kind)) {
            ++mine(victim->side).destroyed;
        }
    }
}

} // namespace protekt::game
