#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "protekt/ai/commander.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::cmd {

/// What the player tells a controller to do. A POLICY, not a click: "don't let them through" is
/// the Deny policy, and the controller decides which orders that means every time it thinks.
///
/// Three to start with, deliberately: a policy vocabulary is a design surface, and twenty of them
/// is a second AI project. Each maps to orders the sim already understands.
enum class PolicyKind : std::uint8_t {
    Hold = 0,  ///< every unit holds the point (GuardAt) -- the turret-on-a-corner policy
    Deny = 1,  ///< get to the area and hold the line in front of it -- "don't let them through"
    Focus = 2, ///< concentrate fire: every unit attacks the enemy nearest the point
};

struct Policy {
    PolicyKind kind{PolicyKind::Hold};
    sim::Fixed x{}; ///< the centre of the area this policy is about
    sim::Fixed y{};
    /// How often the controller may think, in ticks. Not a compromise: the sim runs at 30 Hz and a
    /// player clicks at about 1 Hz, so a controller that thought every tick would be a
    /// micromanager. Zero is refused -- see `Controller::create`.
    std::uint32_t cadence_ticks{15};
};

inline constexpr int kPolicyKindCount = 3;

/// How close a unit has to be to a Deny area to be considered "on the line". A unit inside this
/// radius holds its own tile; one outside it moves to the point. Balance, so it lives in the
/// contract: a test asserts the boundary, and the implementation cannot quietly pick its own.
inline constexpr int kDenyRadiusTiles = 6;

/// Distances are SQUARED EUCLIDEAN, in whole tiles, compared against squared radii. Named here
/// because it is not a detail: a radius should be a circle, and Manhattan distance is a diamond
/// that quietly changes where a unit stops on a diagonal. It is also the convention the scripted
/// opponent already uses to break a tie, so the two commanders agree about "nearest".

/// A standing controller: it owns a roster, thinks on a cadence, and issues orders through the
/// SAME channel the player does. Nothing about it is special to the simulation.
///
/// Determinism is not optional here. The cadence is driven by the simulation's tick counter and
/// nothing else -- no clock, no random, no floating point -- so a battle containing controllers
/// replays exactly like any other.
class Controller {
public:
    /// Refuses a cadence of zero (a controller that thinks every tick) and an empty roster (a
    /// controller with nothing to command): both are configuration mistakes, and a mistake that
    /// throws nowhere is a mistake that ships.
    /// Not `noexcept`: it builds a roster, so it allocates, and a function that terminates on a bad
    /// allocation is a worse contract than one that returns `nullopt`.
    [[nodiscard]] static std::optional<Controller> create(const Policy& policy,
                                                          std::vector<sim::UnitId> roster);

    /// True on ticks this controller is allowed to think: `tick % cadence_ticks == 0` and
    /// `tick != 0`. Tick 0 is the deployment tick: the scenario has already placed everything, and
    /// a controller that fires on tick 0 would override that placement before the player sees it.
    [[nodiscard]] bool due(sim::Tick tick) const noexcept;

    /// Emits this controller's orders for the tick, appending to `out`. Does nothing when the tick
    /// is not due. Orders only its OWN units, and only the living ones -- its roster filtered
    /// against `observation.own`.
    ///
    /// A roster is NORMALISED on the way in (by `create` and by every verb): ascending, no
    /// duplicates, and `UnitId{}` dropped. A caller that hands over a sloppy list gets the same
    /// orders as one that hands over a tidy one.
    void think(const ai::Observation& observation, sim::Tick tick, std::vector<sim::Order>& out);

    [[nodiscard]] const Policy& policy() const noexcept;
    [[nodiscard]] std::span<const sim::UnitId> roster() const noexcept;

private:
    Policy policy_{};
    std::vector<sim::UnitId> roster_{}; ///< ascending
};

/// The controllers a player has standing, with the one invariant that matters: no unit is owned by
/// two controllers. Without it two of them fight over one turret and the last order to arrive
/// wins by accident -- which is exactly the kind of bug a replay makes almost impossible to see.
class ControllerSet {
public:
    /// Adds a controller. Units already owned by an existing controller are DROPPED from the new
    /// roster and returned, rather than taken over silently: the caller can tell the player.
    ///
    /// If every unit it was given is already owned, the controller is NOT added at all and all of
    /// the ids come back -- a controller with an empty roster cannot exist, and one that did would
    /// be a thing that thinks about nothing.
    [[nodiscard]] std::vector<sim::UnitId> add(Controller controller);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<const Controller> controllers() const noexcept;
    /// Which controller owns this unit, if any -- index into `controllers()`.
    [[nodiscard]] std::optional<std::size_t> owner_of(sim::UnitId id) const noexcept;

    /// Every controller due on this tick, in insertion order (which is the order the player placed
    /// them, and therefore stable across a replay).
    void think_due(const ai::Observation& observation, sim::Tick tick,
                   std::vector<sim::Order>& out);

private:
    std::vector<Controller> controllers_{};
};

} // namespace protekt::cmd
