#include "protekt/ai/commander.hpp"

#include <cstddef>

#include "protekt/sim/sim.hpp"

namespace protekt::ai {

Observation observe(const sim::Sim& sim, sim::Side side) {
    Observation observation{};
    observation.tick = sim.tick();
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive) {
            continue;
        }
        const UnitView view{unit.id, unit.kind, unit.side, unit.x.floor_int(), unit.y.floor_int(),
                            unit.hp};
        (unit.side == side ? observation.own : observation.enemy).push_back(view);
    }
    return observation;
}

ScriptedCommander::ScriptedCommander(std::uint32_t period_ticks) noexcept
    : period_ticks_(period_ticks) {}

void ScriptedCommander::think(const Observation& observation, std::vector<sim::Order>& out) {
    // A period of zero means "never": there is no tick it is a multiple of.
    if (period_ticks_ == 0 || observation.tick % period_ticks_ != 0) {
        return;
    }
    for (const UnitView& unit : observation.own) {
        if (unit.kind == sim::UnitKind::Turret) {
            sim::Order order{};
            order.kind = sim::OrderKind::GuardAt;
            order.unit = unit.id;
            order.x = sim::Fixed::from_int(unit.x);
            order.y = sim::Fixed::from_int(unit.y);
            out.push_back(order);
        } else if (unit.kind == sim::UnitKind::Tank) {
            const UnitView* nearest = nullptr;
            std::int64_t nearest_d2 = 0;
            for (const UnitView& enemy : observation.enemy) {
                const std::int64_t dx = static_cast<std::int64_t>(enemy.x) - unit.x;
                const std::int64_t dy = static_cast<std::int64_t>(enemy.y) - unit.y;
                const std::int64_t d2 = dx * dx + dy * dy;
                if (nearest == nullptr || d2 < nearest_d2) { // strict: ties keep the lowest id
                    nearest = &enemy;
                    nearest_d2 = d2;
                }
            }
            if (nearest != nullptr) {
                sim::Order order{};
                order.kind = sim::OrderKind::Attack;
                order.unit = unit.id;
                order.target = nearest->id;
                out.push_back(order);
            }
        }
    }
}

} // namespace protekt::ai
