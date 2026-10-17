#pragma once

#include <cstdint>

#include "protekt/sim/sim.hpp"

namespace protekt::sim {

/// The standard skirmish for a seed: each side gets the same complement, placed on passable
/// ground in opposite corners. A PURE FUNCTION OF THE SEED, which is what makes a replay
/// self-describing -- a seed plus a command list is the entire battle, so no scenario file and no
/// stored world are ever needed.
///
/// Frozen composition, both sides identical: 3 tanks, 1 factory, 2 turrets.
/// Player starts in the low-coordinate quadrant, enemy in the high one, at least 100 tiles apart.
/// Every spawn is nudged to passable ground using world::sample() before it is handed back, so a
/// unit never starts inside a crater.
[[nodiscard]] Scenario skirmish(std::uint64_t seed);

} // namespace protekt::sim
