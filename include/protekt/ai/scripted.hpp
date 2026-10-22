#pragma once

#include <cstdint>
#include <vector>

#include "protekt/sim/commander.hpp"

namespace protekt::ai {

/// The trivial opponent M2 ships with, and the baseline any cleverer commander must beat.
/// Frozen rules, so it is testable without asserting anything about the sim:
///   - it does nothing except on ticks where tick % period_ticks == 0;
///   - for each of its own living COMBATANTS (tank, hover tank, mortar,
///     rocket: `ai::is_combatant`) it orders an Attack on the nearest living enemy, ties to
///     the lowest id (its own position is the reference point);
///   - for each of its own TURRETS it orders GuardAt its current tile, so it holds the line;
///   - it never orders a factory, and never orders a unit it does not own.
class ScriptedCommander final : public sim::ICommander {
public:
    explicit ScriptedCommander(std::uint32_t period_ticks = 30) noexcept;
    void think(const sim::Observation& observation, std::vector<sim::Order>& out) override;

private:
    std::uint32_t period_ticks_;
};

} // namespace protekt::ai
