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

/// `shooter` shoots `target` and does nothing else: no stance, no destination, no velocity.
///
/// This is the TRIGGER, and it is deliberately not `attack`. `attack` gives a unit a hunting
/// stance, which makes `steer` drive it at the target every tick -- fine for a unit obeying orders,
/// fatal for the one the player is sitting in and steering on the keyboard. So the trigger is its
/// own verb, it takes ONE unit rather than a roster, and an invalid target is dropped rather than
/// repaired.
[[nodiscard]] std::vector<sim::Order> focus_fire(sim::UnitId shooter, sim::UnitId target);

/// Stop where you are, keep firing if armed.
[[nodiscard]] std::vector<sim::Order> stop(std::span<const sim::UnitId> roster);

} // namespace protekt::cmd
