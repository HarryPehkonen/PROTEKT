#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::game {

/// A kind that counts as a VEHICLE for the statistics screen: anything that is not a building. The
/// turrets, the factory and the three deployed fixtures stand where they are put; "vehicles built
/// and destroyed" (Harri, round 11) is about the things that drive.
[[nodiscard]] constexpr bool is_counted_vehicle(sim::UnitKind kind) noexcept {
    return !sim::is_turret(kind) && kind != sim::UnitKind::Factory
           && kind != sim::UnitKind::DeployedRecycler && kind != sim::UnitKind::DeployedScavenger;
}

/// What one side has done, over the battle so far.
struct SideTally {
    std::uint32_t built{};     ///< vehicles a factory queue delivered
    std::uint32_t destroyed{}; ///< this side's own vehicles that were shot to pieces
    std::uint32_t fired{};     ///< shots this side's guns made
    std::uint32_t hit{};       ///< bolts and shells of this side that struck a unit
};

/// THE STATISTICS SCREEN'S COUNTS (AF). A view-side tally, read from what the sim already reports
/// each step -- the unit table growing, `Sim::impacts()`, `Sim::shots()` -- exactly as the replay
/// log and the sound are. It is NOT sim state: nothing here is hashed, nothing in the sim reads it,
/// and a battle that nobody counts is the same battle. One mechanism sees both sides, because the
/// human and the AI emit the same orders through the same queue and the sim reports them alike.
///
/// Scrap collected and time played are not kept here: the sim carries `collected` and its tick.
/// SHOTS MISSED IS NOT COUNTED either -- it is `missed`, fired minus hit.
class BattleStats {
public:
    /// Counts from `sim` as it stands: the units already in the table were not built in this
    /// battle, so they are not counted.
    explicit BattleStats(const sim::Sim& sim) noexcept : known_units_(sim.units().size()) {}

    /// Reads one step's worth, right after `Sim::step`. Called once per step, never skipped: the
    /// per-step outputs are cleared by the next one.
    void observe(const sim::Sim& sim);

    [[nodiscard]] const SideTally& of(sim::Side side) const noexcept {
        return sides_.at(static_cast<std::size_t>(side));
    }

    /// Shots that struck nothing: fired minus hit, floored at zero. A scatter-gun's one shot throws
    /// several pellets and a splash strikes several units, so a side can strike more often than it
    /// fires; that side has missed nothing.
    [[nodiscard]] std::uint32_t missed(sim::Side side) const noexcept {
        const SideTally& tally = of(side);
        return tally.fired > tally.hit ? tally.fired - tally.hit : 0U;
    }

private:
    std::array<SideTally, 2> sides_{};
    std::size_t known_units_{};
    [[nodiscard]] SideTally& mine(sim::Side side) noexcept {
        return sides_.at(static_cast<std::size_t>(side));
    }
};

} // namespace protekt::game
