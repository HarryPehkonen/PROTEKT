#pragma once

#include <cstdint>
#include <span>

#include "protekt/sim/sim.hpp"

namespace protekt::sim {

/// The kinds a side spawns with, in spawn order. Both sides carry the same list: the enemy mirrors
/// the player, hover tank, mortar and rocket included, each in the same slot.
[[nodiscard]] std::span<const UnitKind> side_roster(Side side) noexcept;

/// The standard skirmish for a seed: each side gets the same complement, placed on passable
/// ground in opposite corners. A PURE FUNCTION OF THE SEED, which is what makes a replay
/// self-describing -- a seed plus a command list is the entire battle, so no scenario file and no
/// stored world are ever needed.
///
/// Composition, both sides identical (see side_roster): 3 tanks, 1 hover tank, 1 factory,
/// 2 turrets, 1 mortar, 1 rocket -- nine a side, eighteen in all.
/// Player starts in the low-coordinate quadrant, enemy in the high one, at least 100 tiles apart.
/// Every spawn is nudged to passable ground using world::sample() before it is handed back, so a
/// unit never starts inside a crater.
[[nodiscard]] Scenario skirmish(std::uint64_t seed);

} // namespace protekt::sim
