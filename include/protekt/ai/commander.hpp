#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {
class Sim;
}

namespace protekt::ai {

/// What a commander is allowed to see. Deliberately a plain data snapshot rather than the sim
/// itself, so a commander cannot reach around behind the interface -- the human, the scripted
/// opponent and (later) the AI all get exactly this and nothing else.
struct UnitView {
    sim::UnitId id{};
    sim::UnitKind kind{};
    sim::Side side{};
    std::int32_t x{};
    std::int32_t y{}; ///< tile-floor of the unit's fixed-point position
    std::uint16_t hp{};
};

struct Observation {
    sim::Tick tick{};
    std::vector<UnitView> own{};
    std::vector<UnitView> enemy{};
};

/// The one interface every commander implements: perception in, orders out. A human's input
/// layer, the scripted opponent and a future AI are interchangeable behind it.
class ICommander {
public:
    virtual ~ICommander() = default;
    virtual void think(const Observation& observation, std::vector<sim::Order>& out) = 0;

protected:
    // Copying or moving through the interface would slice the derived commander, so only the
    // derived classes may copy and move it.
    ICommander() = default;
    ICommander(const ICommander&) = default;
    ICommander& operator=(const ICommander&) = default;
    ICommander(ICommander&&) = default;
    ICommander& operator=(ICommander&&) = default;
};

/// Living units only, in ascending id order.
[[nodiscard]] Observation observe(const sim::Sim& sim, sim::Side side);

/// The trivial opponent M2 ships with, and the baseline any cleverer commander must beat.
/// Frozen rules, so it is testable without asserting anything about the sim:
///   - it does nothing except on ticks where tick % period_ticks == 0;
///   - for each of its own living TANKS it orders an Attack on the nearest living enemy, ties to
///     the lowest id (its own position is the reference point);
///   - for each of its own TURRETS it orders GuardAt its current tile, so it holds the line;
///   - it never orders a factory, and never orders a unit it does not own.
class ScriptedCommander final : public ICommander {
public:
    explicit ScriptedCommander(std::uint32_t period_ticks = 30) noexcept;
    void think(const Observation& observation, std::vector<sim::Order>& out) override;

private:
    std::uint32_t period_ticks_;
};

} // namespace protekt::ai
