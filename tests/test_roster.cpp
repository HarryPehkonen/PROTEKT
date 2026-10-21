#include "protekt/ai/utility.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/spec.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// Side rosters: which kinds each side spawns, in spawn order. The enemy mirrors the player,
/// hover tank included, and the utility commander (which commands by role) drives it like a tank.
namespace protekt::sim {
namespace {

constexpr std::size_t kHoverSlot = 3;
constexpr std::size_t kRosterSize = 7;

TEST(Roster, BothSidesListTheSameKindsInSpawnOrder) {
    const std::vector<UnitKind> want{UnitKind::Tank,      UnitKind::Tank,    UnitKind::Tank,
                                     UnitKind::HoverTank, UnitKind::Factory, UnitKind::Turret,
                                     UnitKind::Turret};
    for (const Side side : {Side::Player, Side::Enemy}) {
        const std::span<const UnitKind> roster = side_roster(side);
        ASSERT_EQ(roster.size(), want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            EXPECT_EQ(roster[i], want[i]) << "slot " << i;
        }
        EXPECT_EQ(roster[kHoverSlot], UnitKind::HoverTank);
    }
}

TEST(Roster, TheEnemyHoverTankSpawnsInThePlayersSlotOfItsOwnGroup) {
    for (const std::uint64_t seed : {1ULL, 7ULL, 42ULL, 777ULL}) {
        const Scenario s = skirmish(seed);
        ASSERT_EQ(s.spawns.size(), 2 * kRosterSize) << "seed " << seed;
        for (std::size_t i = 0; i < kRosterSize; ++i) {
            EXPECT_EQ(s.spawns.at(i).side, Side::Player);
            EXPECT_EQ(s.spawns.at(kRosterSize + i).side, Side::Enemy);
            EXPECT_EQ(s.spawns.at(i).kind, side_roster(Side::Player)[i]);
            EXPECT_EQ(s.spawns.at(kRosterSize + i).kind, side_roster(Side::Enemy)[i]);
        }
        EXPECT_EQ(s.spawns.at(kRosterSize + kHoverSlot).kind, UnitKind::HoverTank);
        EXPECT_EQ(s.spawns.at(kRosterSize + kHoverSlot).side, Side::Enemy);
    }
}

TEST(Roster, HoverTankPositionsArePinnedAtSeed42) {
    // The player's hover tank wished for +5,+5 of its factory (25,45) and was nudged one tile off
    // the unpassable ground; the enemy's wished for -5,-5 of its factory (147,166) and got it.
    const Scenario s = skirmish(42);
    const Spawn& ph = s.spawns.at(kHoverSlot);
    const Spawn& eh = s.spawns.at(kRosterSize + kHoverSlot);
    EXPECT_EQ(ph.tile_x, 29);
    EXPECT_EQ(ph.tile_y, 50);
    EXPECT_EQ(eh.tile_x, 142);
    EXPECT_EQ(eh.tile_y, 161);
}

TEST(Roster, NoKindIsBuildableYet) {
    for (int i = 0; i < kUnitKindCount; ++i) {
        EXPECT_FALSE(spec_of(static_cast<UnitKind>(i)).buildable) << "kind " << i;
    }
}

/// An enemy hover tank 8 tiles from a player tank (inside tank range), the enemy under the utility
/// commander and the player with none: the hover tank must shoot, as a tank does.
TEST(Roster, TheEnemyAiDrivesItsHoverTank) {
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::HoverTank, Side::Enemy, 60, 60});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 60, 68});
    const UnitId hover{1};
    ai::UtilityCommander enemy{ai::UtilityTuning{}};
    const BattleLog log = run_battle(scenario, {}, 300, nullptr, &enemy);
    int shots = 0;
    bool ordered = false;
    for (const BattleEvent& e : log.events) {
        if (e.kind == EventKind::Fired && e.actor == hover) {
            ++shots;
        }
        if (e.kind == EventKind::Ordered && e.actor == hover) {
            ordered = true;
        }
    }
    EXPECT_TRUE(ordered) << "the commander issued the hover tank an order";
    EXPECT_GT(shots, 0) << "and it engaged";
}

TEST(Roster, SameSeedBattlesAreByteIdentical) {
    ai::UtilityCommander p1{ai::UtilityTuning{}};
    ai::UtilityCommander e1{ai::UtilityTuning{}};
    ai::UtilityCommander p2{ai::UtilityTuning{}};
    ai::UtilityCommander e2{ai::UtilityTuning{}};
    const BattleLog a = run_battle(skirmish(9), {}, 600, &p1, &e1);
    const BattleLog b = run_battle(skirmish(9), {}, 600, &p2, &e2);
    EXPECT_EQ(a.final_hash, b.final_hash);
    EXPECT_EQ(a.to_text(1000000), b.to_text(1000000));
}

} // namespace
} // namespace protekt::sim
