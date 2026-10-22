#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "protekt/sim/commander.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::sim {

enum class EventKind : std::uint8_t { Ordered = 0, Fired, Killed, HitTaken };

/// One thing that happened, derived by DIFFING consecutive states -- the sim itself knows nothing
/// about observers and carries no logging hooks.
struct BattleEvent {
    Tick tick{};
    EventKind kind{};
    UnitId actor{};
    UnitId subject{};
    /// After the event: for HitTaken and Killed the victim's hit points once the bolt landed. For
    /// Fired the target's hit points as the shot leaves the gun, which a shot in the air has not
    /// yet changed.
    std::uint16_t subject_hp{};
};

/// The units as they stood at one instant. `events_between` diffs one of these against a later
/// `Sim`; taking it is a copy of the unit table and nothing else, so a caller that wants events
/// pays for them only on the ticks it asks.
using UnitSnapshot = std::vector<Unit>;

/// The HitTaken, Killed and Fired events of the step that took `before` to `after`, in ascending
/// shooter id, stamped `tick`. Empty when the two tables disagree in size (the sim never resizes
/// its table, so that is a snapshot of some other battle, not a tick with nothing in it). This is
/// the ONE place the diff is written: the replay log and the game's sound both read it, so they
/// cannot disagree about who shot whom.
[[nodiscard]] std::vector<BattleEvent> events_between(const UnitSnapshot& before, const Sim& after,
                                                      Tick tick);

/// The packed per-unit attributes the status events carry in `subject_hp` (16 bits):
///   bits 0..5   kind   (6 bits, ceiling 64 kinds; was bits 0..3, ceiling 16)
///   bits 6..7   side   (2 bits; was bits 4..7)
///   bits 8..15  stance (unchanged)
inline constexpr unsigned kKindMask = 0x3FU;
inline constexpr unsigned kSideShift = 6;
inline constexpr unsigned kSideMask = 0x3U;
inline constexpr unsigned kStanceShift = 8;
[[nodiscard]] std::uint16_t pack_unit_attrs(UnitKind kind, Side side, Stance stance) noexcept;
[[nodiscard]] UnitKind unpack_unit_kind(std::uint16_t attrs) noexcept;
[[nodiscard]] Side unpack_unit_side(std::uint16_t attrs) noexcept;
[[nodiscard]] Stance unpack_unit_stance(std::uint16_t attrs) noexcept;

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
///   tick <t> hit unit <subject> by unit <actor> hp <hp>
///   tick <t> killed unit <subject> by unit <actor>
///   tick <t> status unit <id> <kind> side <side> pos <x> <y> hp <hp> stance <stance>
///   end tick <t> seed <seed> units <n> hash <hash>
/// One status line per living unit every `status_period` ticks, and always on tick 0. Positions
/// print as tiles with ONE decimal digit (e.g. 24.0), computed with integer arithmetic.
/// Names are lowercase and fixed: kinds tank/turret/factory/hovertank/mortar/rocket (the names in
/// sim/spec.hpp), sides player/enemy, stances idle/move/guard/patrol/attack/dead/follow, and the
/// `ordered` verbs move/guard/patrol/attack/focus/firetowards/strafe/stop/follow/firelob (the
/// names of `to_string(OrderKind)`; note `focus`, where the replay file says `focusfire`).
[[nodiscard]] const char* to_string(UnitKind kind) noexcept;
[[nodiscard]] const char* to_string(Side side) noexcept;
[[nodiscard]] const char* to_string(Stance stance) noexcept;
[[nodiscard]] const char* to_string(OrderKind kind) noexcept;

} // namespace protekt::sim
