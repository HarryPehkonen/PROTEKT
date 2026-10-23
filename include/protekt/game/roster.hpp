#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "protekt/sim/sim.hpp"

/// THE ROSTER (K1): your own units grouped by KIND, one group per key, from anywhere.
///
/// A key belongs to a KIND, not to a unit: press 3 and you get every unit of that kind you own,
/// wherever they are, in spawn order. So there is one row per kind that has living units, and a
/// kind with a hundred units is still one row.
///
/// Keys come out of a POOL. Walking your living units in spawn order, the first kind seen takes the
/// lowest free key, the next new kind takes the next, and so on -- the keys in use are always 1..n
/// with no gaps, because the walk hands them out in order and never skips one. A kind with no
/// living units holds no key at all, and that is what puts its key back in the pool for whatever
/// kind appears next. The rule is the user's (2026-10-22), given as how BattleZone works: "when the
/// last unit dies, the function key space gets returned to the pool."
///
/// Nothing is stored. The whole assignment is a function of the living units, so it cannot drift
/// from the sim: a replay, an observer and the HUD all see the same keys on the same tick.
///
/// One consequence is worth stating plainly, because a pool has no memory. The assignment is made
/// when the roster is LOOKED AT, not when the units were built, so a surviving kind moves DOWN into
/// a freed key: wipe every tank and your turrets, one key up until that moment, take the tank key.
/// Freezing a group's key until that group itself dies would need the sim to record when a unit
/// died, and it does not.
namespace protekt::game {

/// Digits 1-9. There are only `kUnitKindCount` kinds, so no kind is ever left without a key; the
/// static assertion below is what keeps that true if a seventh kind is ever added.
inline constexpr int kRosterKeys = 10;

/// One kind's living units, under one key.
struct RosterGroup {
    int key{}; ///< 1-9
    sim::UnitKind kind{};
    std::vector<sim::UnitId> units{}; ///< this kind's living units, in spawn order
    std::uint16_t hp{};               ///< summed over `units`
    std::uint16_t max_hp{};           ///< summed over `units`
};

/// The strip: one entry per kind of `side` that has living units, in key order (so ascending key).
struct Roster {
    std::vector<RosterGroup> groups{};
};

static_assert(sim::kUnitKindCount <= kRosterKeys,
              "every kind must be able to take a key: the pool is digits 1-9");

[[nodiscard]] Roster roster_of(const sim::Sim& sim, sim::Side side);

/// The group holding `key`, or nullptr when no kind holds it. There are two states, not three: a
/// key is held or it is free. A returned key is not remembered as the group it came from -- the
/// pool keeps no history -- so "that group is gone" and "you never had that group" are the same
/// thing to press, and there is one message for both.
[[nodiscard]] const RosterGroup* group_for_key(const Roster& roster, int key) noexcept;

/// `group_for_key(roster_of(...), key)` would hand back a pointer into a Roster that dies with the
/// call. Deleted so that mistake is a compile error rather than a dangling pointer: keep the Roster
/// in a named local first.
const RosterGroup* group_for_key(Roster&& roster, int key) = delete;

/// The strip's short name for a kind, in the capitals the HUD font has.
[[nodiscard]] std::string_view kind_label(sim::UnitKind kind) noexcept;

/// What pressing a key no group holds says. A literal, because a message is a view.
[[nodiscard]] std::string_view empty_key_message(int key) noexcept;

} // namespace protekt::game
