#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "protekt/sim/commander.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::sim {

enum class EventKind : std::uint8_t { Ordered = 0, Fired, Killed };

/// One thing that happened, derived by DIFFING consecutive states -- the sim itself knows nothing
/// about observers and carries no logging hooks.
struct BattleEvent {
    Tick tick{};
    EventKind kind{};
    UnitId actor{};
    UnitId subject{};
    std::uint16_t subject_hp{}; ///< after the event, for Fired
};

struct BattleLog {
    Tick ticks{};
    std::uint64_t seed{};
    std::vector<BattleEvent> events{};
    std::uint64_t final_hash{};
    [[nodiscard]] std::string to_text(Tick status_period = 60) const;
};

/// Drives a battle and records it. `player`/`enemy` may be null (then that side receives no
/// orders and simply fights back with whatever is in range).
[[nodiscard]] BattleLog run_battle(const Scenario& scenario, std::span<const Command> script,
                                   Tick ticks, sim::ICommander* player, sim::ICommander* enemy);

/// The frozen line grammar (tests and the CLI both read this):
///   tick <t> ordered unit <id> <verb>
///   tick <t> fired unit <actor> -> unit <subject> hp <hp>
///   tick <t> killed unit <subject> by unit <actor>
///   tick <t> status unit <id> <kind> side <side> pos <x> <y> hp <hp> stance <stance>
///   end tick <t> seed <seed> units <n> hash <hash>
/// One status line per living unit every `status_period` ticks, and always on tick 0. Positions
/// print as tiles with ONE decimal digit (e.g. 24.0), computed with integer arithmetic.
/// Names are lowercase and fixed: kinds tank/turret/factory, sides player/enemy, stances
/// idle/move/guard/patrol/attack/dead, verbs move/guard/patrol/attack/stop/focusfire.
[[nodiscard]] const char* to_string(UnitKind kind) noexcept;
[[nodiscard]] const char* to_string(Side side) noexcept;
[[nodiscard]] const char* to_string(Stance stance) noexcept;
[[nodiscard]] const char* to_string(OrderKind kind) noexcept;

} // namespace protekt::sim
