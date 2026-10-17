#include "protekt/ai/scripted.hpp"
#include "protekt/sim/commander.hpp"

#include <cstddef>

namespace protekt::ai {

ScriptedCommander::ScriptedCommander(std::uint32_t period_ticks) noexcept
    : period_ticks_(period_ticks) {}

void ScriptedCommander::think(const sim::Observation& observation, std::vector<sim::Order>& out) {
    // A period of zero means "never": there is no tick it is a multiple of.
    if (period_ticks_ == 0 || observation.tick % period_ticks_ != 0) {
        return;
    }
    for (const sim::UnitView& unit : observation.own) {
        if (unit.kind == sim::UnitKind::Turret) {
            out.push_back(sim::Order{unit.id, sim::orders::GuardAt{sim::Fixed::from_int(unit.x),
                                                                   sim::Fixed::from_int(unit.y)}});
        } else if (unit.kind == sim::UnitKind::Tank) {
            const sim::UnitView* nearest = nullptr;
            std::int64_t nearest_d2 = 0;
            for (const sim::UnitView& enemy : observation.enemy) {
                const std::int64_t dx = static_cast<std::int64_t>(enemy.x) - unit.x;
                const std::int64_t dy = static_cast<std::int64_t>(enemy.y) - unit.y;
                const std::int64_t d2 = dx * dx + dy * dy;
                if (nearest == nullptr || d2 < nearest_d2) { // strict: ties keep the lowest id
                    nearest = &enemy;
                    nearest_d2 = d2;
                }
            }
            if (nearest != nullptr) {
                out.push_back(sim::Order{unit.id, sim::orders::Attack{nearest->id}});
            }
        }
    }
}

} // namespace protekt::ai
