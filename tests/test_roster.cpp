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
/// hover tank, mortar and rocket included, and the utility commander (which commands by role)
/// drives the hover tank like a tank.
namespace protekt::sim {
namespace {

constexpr std::size_t kHoverSlot = 3;
constexpr std::size_t kMortarSlot = 7;
constexpr std::size_t kRocketSlot = 8;
constexpr std::size_t kScavengerSlot = 9;
constexpr std::size_t kRosterSize = 11;

// Updated (HAULER): a scavenger joins both rosters -- it is the one vehicle that makes the
// economy playable, and until it spawned nothing could harvest in the game the player plays.
// It sits BEFORE the recycler, so the recycler stays the last id on each side (RB1b's rule).
TEST(Roster, BothSidesListTheSameKindsInSpawnOrder) {
    const std::vector<UnitKind> want{UnitKind::Tank,      UnitKind::Tank,    UnitKind::Tank,
                                     UnitKind::HoverTank, UnitKind::Factory, UnitKind::Turret,
                                     UnitKind::Turret,    UnitKind::Mortar,  UnitKind::Rocket,
                                     UnitKind::Scavenger, UnitKind::Recycler};
    for (const Side side : {Side::Player, Side::Enemy}) {
        const std::vector<UnitKind> roster = side_roster(default_data(), side);
        ASSERT_EQ(roster.size(), want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            EXPECT_EQ(roster[i], want[i]) << "slot " << i;
        }
        EXPECT_EQ(roster[kHoverSlot], UnitKind::HoverTank);
        EXPECT_EQ(roster[kMortarSlot], UnitKind::Mortar);
        EXPECT_EQ(roster[kRocketSlot], UnitKind::Rocket);
        EXPECT_EQ(roster[kScavengerSlot], UnitKind::Scavenger);
    }
}

// Updated (RB1b): ten per side, so the enemy's group starts at index 10.
TEST(Roster, TheEnemyHoverTankSpawnsInThePlayersSlotOfItsOwnGroup) {
    for (const std::uint64_t seed : {1ULL, 7ULL, 42ULL, 777ULL}) {
        const Scenario s = skirmish(seed);
        ASSERT_EQ(s.spawns.size(), 2 * kRosterSize) << "seed " << seed;
        for (std::size_t i = 0; i < kRosterSize; ++i) {
            EXPECT_EQ(s.spawns.at(i).side, Side::Player);
            EXPECT_EQ(s.spawns.at(kRosterSize + i).side, Side::Enemy);
            EXPECT_EQ(s.spawns.at(i).kind, side_roster(default_data(), Side::Player)[i]);
            EXPECT_EQ(s.spawns.at(kRosterSize + i).kind,
                      side_roster(default_data(), Side::Enemy)[i]);
        }
        EXPECT_EQ(s.spawns.at(kRosterSize + kHoverSlot).kind, UnitKind::HoverTank);
        EXPECT_EQ(s.spawns.at(kRosterSize + kHoverSlot).side, Side::Enemy);
    }
}

// Updated (RB1b): the enemy's hover tank is index 13 now; its tile, and the player's, did not move.
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

/// The support weapons stand at the REAR of each cluster -- behind the factory, on the side away
/// from the enemy -- so the tanks screen them, and the enemy's mirror the player's.
TEST(Roster, MortarAndRocketSpawnAtTheRearOfEachClusterMirroredAtSeed42) {
    // Player factory (25,45): the mortar wished for -4,-1 and the rocket -1,-4. Enemy factory
    // (147,166): +4,+1 and +1,+4.
    const Scenario s = skirmish(42);
    ASSERT_EQ(s.spawns.size(), 2 * kRosterSize);
    const Spawn& pf = s.spawns.at(4);
    const Spawn& ef = s.spawns.at(kRosterSize + 4);
    ASSERT_EQ(pf.kind, UnitKind::Factory);
    ASSERT_EQ(ef.kind, UnitKind::Factory);
    const Spawn& pm = s.spawns.at(kMortarSlot);
    const Spawn& pr = s.spawns.at(kRocketSlot);
    const Spawn& em = s.spawns.at(kRosterSize + kMortarSlot);
    const Spawn& er = s.spawns.at(kRosterSize + kRocketSlot);
    EXPECT_EQ(pm.kind, UnitKind::Mortar);
    EXPECT_EQ(pr.kind, UnitKind::Rocket);
    EXPECT_EQ(em.kind, UnitKind::Mortar);
    EXPECT_EQ(er.kind, UnitKind::Rocket);
    EXPECT_EQ(pm.side, Side::Player);
    EXPECT_EQ(em.side, Side::Enemy);
    EXPECT_EQ(pm.tile_x, 21);
    EXPECT_EQ(pm.tile_y, 44);
    EXPECT_EQ(pr.tile_x, 24);
    EXPECT_EQ(pr.tile_y, 41);
    EXPECT_EQ(em.tile_x, 151);
    EXPECT_EQ(em.tile_y, 167);
    EXPECT_EQ(er.tile_x, 148);
    EXPECT_EQ(er.tile_y, 170);
    // Behind the factory, away from the other side: the player's lie low, the enemy's high.
    EXPECT_LT(pm.tile_x + pm.tile_y, pf.tile_x + pf.tile_y);
    EXPECT_LT(pr.tile_x + pr.tile_y, pf.tile_x + pf.tile_y);
    EXPECT_GT(em.tile_x + em.tile_y, ef.tile_x + ef.tile_y);
    EXPECT_GT(er.tile_x + er.tile_y, ef.tile_x + ef.tile_y);
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

/// The R3b2 gap, closed by AID: the utility commander commands by role, so the rocket (frontline)
/// closes on the war like a tank and the mortar (standoff) is a combatant that fires and is
/// ordered. The rocket used to stand where it spawned for good; this pins the opposite.
TEST(Roster, TheAiOrdersARocketForwardAndAMortarStillFiresAtWhatIsInRange) {
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Mortar, Side::Enemy, 60, 60});
    scenario.spawns.push_back(Spawn{UnitKind::Rocket, Side::Enemy, 90, 60});
    scenario.spawns.push_back(Spawn{UnitKind::Factory, Side::Enemy, 75, 50});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 60, 68});
    scenario.spawns.push_back(Spawn{UnitKind::Factory, Side::Player, 75, 120});
    const UnitId mortar{1};
    const UnitId rocket{2};
    ai::UtilityCommander enemy{ai::UtilityTuning{}};
    Sim sim = Sim::create(scenario);
    std::vector<Order> orders;
    int mortar_shots = 0;
    bool rocket_ordered = false;
    for (int t = 0; t < 300; ++t) {
        orders.clear();
        enemy.think(observe(sim, Side::Enemy), orders);
        for (const Order& order : orders) {
            rocket_ordered = rocket_ordered || order.unit == rocket;
            sim.enqueue(order);
        }
        sim.step();
        for (const Sim::Shot& shot : sim.shots()) {
            if (shot.shooter == mortar) {
                ++mortar_shots;
            }
        }
    }
    EXPECT_GT(mortar_shots, 0) << "the mortar engages the tank 8 tiles off";
    EXPECT_TRUE(rocket_ordered) << "the rocket is commanded now";
    const Unit* moved = sim.find(rocket);
    ASSERT_NE(moved, nullptr);
    EXPECT_TRUE(moved->x != Fixed::from_int(90) || moved->y != Fixed::from_int(60))
        << "the rocket closes on the war";
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
