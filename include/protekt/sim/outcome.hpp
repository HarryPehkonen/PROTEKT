#pragma once

#include <cstdint>

namespace protekt::sim {

/// How the war stands, from the player's chair. Never stored and never hashed: `Sim::war_outcome()`
/// derives it from the unit table every time it is asked.
enum class WarOutcome : std::uint8_t { Undecided = 0, PlayerWon = 1, PlayerLost = 2 };

} // namespace protekt::sim
