#pragma once

#include <cstdint>
#include <vector>

#include "protekt/sim/objective.hpp"
#include "protekt/sim/scrap.hpp"

#include "protekt/sim/sim.hpp"

namespace protekt::sim {

/// A batch of `count` units of one kind, wished onto the tile (dx, dy) from the side's base
/// centre. +x,+y is toward the enemy; the enemy's side mirrors the offsets. Units of one batch
/// share the wish, and the placement rule nudges each to the nearest free passable tile.
struct Group {
    UnitKind kind{};
    int count{1};
    int dx{};
    int dy{};
};

/// The composition of a skirmish: groups in SPAWN ORDER, the same list for both sides (per-side
/// lists are a deliberate non-goal). Spawn order is unit id order and function-key order, so the
/// order of the groups is the order of the F-row.
///
/// Plain data, no I/O: the sim reads no file. The bounds below are enforced by the parser that
/// builds one from text, not by skirmish(), which trusts a ScenarioData it is handed.
struct ScenarioData {
    std::vector<Group> groups{};
    /// How many pieces of scrap each side's base opens with (C0). One piece is one load, so this is
    /// the economy's starting allowance; a composition that asks for none starts on clean ground.
    int scrap_per_side{kInitialScrapPerSide};
    /// The conditions this composition plays under (OBJ). A composition that says nothing about
    /// them plays the built-in rule -- the war still has to be winnable when the file only lists
    /// units -- and a mission is a composition with its own list.
    std::vector<Objective> objectives = built_in_objectives();
};

inline constexpr int kMaxGroupCount = 64; ///< units in one group, at least 1
inline constexpr int kMaxSideUnits = 64;  ///< units a side spawns with, over all its groups
inline constexpr int kMaxOffset = 32;     ///< |dx| and |dy| of a group, in tiles

/// The built-in composition, and what a game started with no scenario file plays: 3 tanks, 1 hover
/// tank, 1 factory, 2 turrets, 1 mortar bike, 1 rocket tank and 1 recycler -- ten a side, as ten
/// groups of one, because the three tanks and the two turrets each stand at their own offset.
[[nodiscard]] ScenarioData default_data();

/// The kinds a side spawns with, in spawn order, each group expanded to `count` entries. Both
/// sides carry the same list: the enemy mirrors the player slot for slot.
[[nodiscard]] std::vector<UnitKind> side_roster(const ScenarioData& data, Side side);

/// The skirmish for a seed and a composition: each side gets the composition, placed on passable
/// ground in opposite corners. A PURE FUNCTION OF THE SEED AND THE DATA. The seed owns the terrain,
/// the player's base position and the choice of the enemy's base; the data owns what stands there.
/// A seed plus a command list is the whole battle ONLY for the built-in composition: a battle on
/// any other is a seed, a ScenarioData and a command list, and a replay of it must record the data
/// too. The replay format records the seed alone today, so it replays the built-in one.
///
/// Player starts in the low-coordinate quadrant, enemy in the high one, at least 100 tiles apart.
/// Every spawn is nudged to passable ground using world::sample() before it is handed back, so a
/// unit never starts inside a crater.
[[nodiscard]] Scenario skirmish(std::uint64_t seed, const ScenarioData& data);

/// The standard skirmish for a seed: skirmish(seed, default_data()).
[[nodiscard]] Scenario skirmish(std::uint64_t seed);

} // namespace protekt::sim
