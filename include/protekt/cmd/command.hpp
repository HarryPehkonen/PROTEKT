#pragma once

#include <span>
#include <vector>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::cmd {

/// The player's verbs: intent in, orders out.
///
/// Every one is a pure function of (roster, aim) -- no sim, no globals, no clock -- which is what
/// lets a click be tested without a window and a replay of human play be exact. Two rules hold for
/// all of them:
///
///   1. one order per roster unit, emitted in ASCENDING UNIT ID order, so the order the player
///      selected units in never changes what the simulation sees;
///   2. an impossible order is DROPPED, not repaired. Attack on `UnitId{}` is not a move, and a
///      move to a tile that does not exist is not clamped -- the caller gets nothing and can say
///      why. A verb that silently becomes another verb is worse than one that refuses.
///
/// A tile exists when it is inside the playable map: `[0, sim::kMapTiles)` on each axis. That bound
/// is part of this contract rather than the implementation's private opinion, because "a click at
/// the edge of the screen" has to mean the same thing to the game, to a test and to a replay.

[[nodiscard]] std::vector<sim::Order> move_to(std::span<const sim::UnitId> roster, sim::Fixed x,
                                              sim::Fixed y);

[[nodiscard]] std::vector<sim::Order> guard_at(std::span<const sim::UnitId> roster, sim::Fixed x,
                                               sim::Fixed y);

[[nodiscard]] std::vector<sim::Order> patrol(std::span<const sim::UnitId> roster, sim::Fixed x,
                                             sim::Fixed y, sim::Fixed x2, sim::Fixed y2);

/// Every unit in the roster hunts `target`. Refuses an invalid target: an attack order with no
/// target is a move order with extra steps, and the sim would hold a stale Stance::Attack.
[[nodiscard]] std::vector<sim::Order> attack(std::span<const sim::UnitId> roster,
                                             sim::UnitId target);

/// Stop where you are, keep firing if armed.
[[nodiscard]] std::vector<sim::Order> stop(std::span<const sim::UnitId> roster);

} // namespace protekt::cmd
