#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "protekt/sim/units.hpp"

namespace protekt::cmd {

/// The player's command set: what a left-click, a drag-box or a key press builds up before an
/// order is given.
///
/// It is a SET, not a list. Kept sorted ascending and unique, because everything downstream reads
/// this order -- the orders emitted, the HUD, a replay -- and "the same selection" has to mean the
/// same bytes however the player arrived at it. Selecting the same unit twice is a no-op; a
/// duplicate order to one turret is not harmless, it is a bug that hides in a replay.
class Selection {
public:
    /// A bound, so that a drag-box across the map cannot select the world. Past this, additions
    /// are refused rather than silently reordering what is already selected.
    static constexpr std::size_t kMax = 64;

    /// True when the unit was added; false when it was already there or the set is full.
    /// A dead or invalid id is refused: `UnitId{}` is "no unit", and selecting nothing must not
    /// become a way to order every unit.
    bool add(sim::UnitId id) noexcept;
    /// Add if absent, remove if present. Returns true when the unit is IN the set afterwards.
    bool toggle(sim::UnitId id) noexcept;
    bool remove(sim::UnitId id) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool contains(sim::UnitId id) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<const sim::UnitId> units() const noexcept;
    [[nodiscard]] bool full() const noexcept;

    /// Drops every id that is not in `living`. A unit that has died must stop receiving orders,
    /// and a selection that quietly keeps dead ids is how a player ends up commanding ghosts.
    void retain(std::span<const sim::UnitId> living) noexcept;

private:
    std::vector<sim::UnitId> ids_{}; ///< ascending, unique
};

} // namespace protekt::cmd
