// The standard skirmish is a pure function of the seed. If it is not, a replay stops being
// self-describing and every determinism guarantee downstream is void.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>
#include <vector>

#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/world/world.hpp"

namespace {

using namespace protekt::sim;
namespace world = protekt::world;

int count_of(const Scenario& s, Side side, UnitKind kind) {
    int n = 0;
    for (const Spawn& sp : s.spawns) {
        if (sp.side == side && sp.kind == kind) {
            ++n;
        }
    }
    return n;
}

// Updated (HAULER): eleven a side -- the scavenger joined, before the recycler.
TEST(Scenario, BothSidesGetTheFrozenComposition) {
    const Scenario s = skirmish(42);
    EXPECT_EQ(s.seed, 42U);
    ASSERT_EQ(s.spawns.size(), 22U) << "11 units a side, the hover tank, mortar bike, rocket tank, "
                                       "scavenger and recycler included";
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::HoverTank), 1);
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::HoverTank), 1);
    EXPECT_EQ(s.spawns.at(3).kind, UnitKind::HoverTank) << "the player's fourth unit";
    EXPECT_EQ(s.spawns.at(3).side, Side::Player);
    for (const Side side : {Side::Player, Side::Enemy}) {
        EXPECT_EQ(count_of(s, side, UnitKind::Tank), 3);
        EXPECT_EQ(count_of(s, side, UnitKind::Factory), 1);
        EXPECT_EQ(count_of(s, side, UnitKind::Turret), 2);
        EXPECT_EQ(count_of(s, side, UnitKind::MortarBike), 1);
        EXPECT_EQ(count_of(s, side, UnitKind::RocketTank), 1);
        EXPECT_EQ(count_of(s, side, UnitKind::Recycler), 1);
    }
}

// Updated (HAULER): 22 spawns now; the hover tank is still the player's fourth.
TEST(Scenario, ThePlayersFourthUnitIsAHoverTankDeterministicallyFromTheSeed) {
    for (const std::uint64_t seed : {7ULL, 42ULL}) {
        const Scenario a = skirmish(seed);
        const Scenario b = skirmish(seed);
        ASSERT_EQ(a.spawns.size(), 22U);
        EXPECT_EQ(a.spawns.at(3).kind, UnitKind::HoverTank);
        EXPECT_EQ(a.spawns.at(3).side, Side::Player);
        EXPECT_EQ(a.spawns.at(3).tile_x, b.spawns.at(3).tile_x);
        EXPECT_EQ(a.spawns.at(3).tile_y, b.spawns.at(3).tile_y);
        EXPECT_EQ(count_of(a, Side::Player, UnitKind::Tank), 3);
    }
}

TEST(Scenario, UnitsStartOnPassableGround) {
    for (const std::uint64_t seed : {1ULL, 42ULL, 777ULL, 0xDEADBEEFULL}) {
        const Scenario s = skirmish(seed);
        for (const Spawn& sp : s.spawns) {
            ASSERT_GE(sp.tile_x, 0);
            ASSERT_GE(sp.tile_y, 0);
            ASSERT_LT(sp.tile_x, kMapTiles);
            ASSERT_LT(sp.tile_y, kMapTiles);
            const world::Terrain ground = world::sample(seed, sp.tile_x, sp.tile_y).terrain;
            EXPECT_NE(ground, world::Terrain::Crater)
                << "seed " << seed << " spawns inside a crater at " << sp.tile_x << ","
                << sp.tile_y;
        }
    }
}

TEST(Scenario, IsAPureFunctionOfTheSeed) {
    const Scenario a = skirmish(42);
    const Scenario b = skirmish(42);
    ASSERT_EQ(a.spawns.size(), b.spawns.size());
    for (std::size_t i = 0; i < a.spawns.size(); ++i) {
        EXPECT_EQ(a.spawns[i].kind, b.spawns[i].kind);
        EXPECT_EQ(a.spawns[i].side, b.spawns[i].side);
        EXPECT_EQ(a.spawns[i].tile_x, b.spawns[i].tile_x);
        EXPECT_EQ(a.spawns[i].tile_y, b.spawns[i].tile_y);
    }
    const Scenario other = skirmish(43);
    bool differs = other.spawns.size() != a.spawns.size();
    for (std::size_t i = 0; !differs && i < other.spawns.size(); ++i) {
        differs = other.spawns[i].tile_x != a.spawns[i].tile_x
                  || other.spawns[i].tile_y != a.spawns[i].tile_y;
    }
    EXPECT_TRUE(differs) << "two seeds must not produce the same battlefield run for run";
}

// Updated (HAULER): 22 units.
TEST(Scenario, IdsAreAscendingAndUnique) {
    Sim sim = Sim::create(skirmish(42));
    ASSERT_EQ(sim.units().size(), 22U);
    std::set<std::uint32_t> seen;
    for (const Unit& u : sim.units()) {
        EXPECT_TRUE(u.id.valid());
        EXPECT_TRUE(seen.insert(u.id.value).second) << "duplicate id";
    }
    EXPECT_EQ(*seen.begin(), 1U) << "ids are handed out from 1 in creation order";
}

TEST(Scenario, TheTwoSidesStartWellApart) {
    const Scenario s = skirmish(42);
    int closest = std::numeric_limits<int>::max(); // the earlier value (512) capped every result
    for (const Spawn& p : s.spawns) {
        if (p.side != Side::Player) {
            continue;
        }
        for (const Spawn& e : s.spawns) {
            if (e.side != Side::Enemy) {
                continue;
            }
            const int dx = p.tile_x - e.tile_x;
            const int dy = p.tile_y - e.tile_y;
            const int d = dx * dx + dy * dy;
            closest = std::min(closest, d);
        }
    }
    EXPECT_GT(closest, 100 * 100) << "there must be ground to cross before the fight starts";
}

// RB1b: the recycler is one per side, ever -- and the LAST id on each side, because the newest
// kind is appended so nothing before it moves.
TEST(Scenario, EachSideSpawnsExactlyOneRecyclerAndItIsTheLastOfItsSide) {
    for (const std::uint64_t seed : {1ULL, 42ULL, 777ULL}) {
        const Scenario s = skirmish(seed);
        for (const Side side : {Side::Player, Side::Enemy}) {
            EXPECT_EQ(count_of(s, side, UnitKind::Recycler), 1) << "seed " << seed;
            const Spawn* last = nullptr;
            for (const Spawn& sp : s.spawns) {
                if (sp.side == side) {
                    last = &sp;
                }
            }
            ASSERT_NE(last, nullptr);
            EXPECT_EQ(last->kind, UnitKind::Recycler) << "seed " << seed;
        }
        EXPECT_EQ(s.spawns.at(10).kind, UnitKind::Recycler) << "the player's eleventh unit";
        EXPECT_EQ(s.spawns.at(21).kind, UnitKind::Recycler) << "the enemy's eleventh unit";
    }
}

// RB1b: the recycler is the base on wheels: it stands on its own half, close to the base.
TEST(Scenario, TheRecyclerStartsOnItsOwnHalfWithinTheBaseCluster) {
    for (const std::uint64_t seed : {1ULL, 42ULL, 777ULL}) {
        const Scenario s = skirmish(seed);
        const Spawn& player_factory = s.spawns.at(4);
        const Spawn& enemy_factory = s.spawns.at(15);
        ASSERT_EQ(player_factory.kind, UnitKind::Factory);
        ASSERT_EQ(enemy_factory.kind, UnitKind::Factory);
        for (const Spawn& sp : s.spawns) {
            if (sp.kind != UnitKind::Recycler) {
                continue;
            }
            const Spawn& factory = sp.side == Side::Player ? player_factory : enemy_factory;
            const int dx = sp.tile_x - factory.tile_x;
            const int dy = sp.tile_y - factory.tile_y;
            EXPECT_LE(std::max(std::abs(dx), std::abs(dy)), 8) << "seed " << seed;
            EXPECT_FALSE(dx == 0 && dy == 0) << "not on the factory's tile";
            // The rear: away from the other side, as the support weapons stand.
            if (sp.side == Side::Player) {
                EXPECT_LT(sp.tile_x + sp.tile_y, factory.tile_x + factory.tile_y);
                EXPECT_LT(sp.tile_x + sp.tile_y, kMapTiles) << "the low half";
            } else {
                EXPECT_GT(sp.tile_x + sp.tile_y, factory.tile_x + factory.tile_y);
                EXPECT_GT(sp.tile_x + sp.tile_y, kMapTiles) << "the high half";
            }
        }
    }
}

// RB1b: the recycler is unarmed, so its arrival changes no side's armed count: the AI's threat list
// counts the same units as before and after.
TEST(Scenario, TheRecyclerIsUnarmedSoTheArmedCountPerSideIsUnchanged) {
    const Scenario s = skirmish(42);
    for (const Side side : {Side::Player, Side::Enemy}) {
        int armed = 0;
        for (const Spawn& sp : s.spawns) {
            if (sp.side == side && spec_of(sp.kind).range.raw > 0) {
                ++armed;
            }
        }
        EXPECT_EQ(armed, 8)
            << "3 tanks, the hover tank, 2 turrets, the mortar bike and the rocket tank "
               "(the recycler adds none)";
    }
    EXPECT_EQ(spec_of(UnitKind::Recycler).range.raw, 0);
}

// RB1b: a battle with a recycler on each side is still a pure function of the seed.
TEST(Scenario, ABattleWithRecyclersInItIsDeterministic) {
    const Scenario scenario = skirmish(42);
    const BattleLog a = run_battle(scenario, {}, 300, nullptr, nullptr);
    const BattleLog b = run_battle(scenario, {}, 300, nullptr, nullptr);
    EXPECT_NE(a.final_hash, 0U);
    EXPECT_EQ(a.final_hash, b.final_hash);
    EXPECT_EQ(a.to_text(60), b.to_text(60));
}

// HAULER: the roster the game plays carries the harvester. Until it did, the fetch loop
// test_scavenger.cpp exercises in hand-built battles had no vehicle in a real skirmish to drive --
// the harvester menu was wired and unreachable, and the economy could not be played at all.
TEST(Scenario, BothSidesSpawnOneHarvesterAndItIsInsideTheBaseCluster) {
    const Scenario s = skirmish(42);
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::Scavenger), 1);
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::Scavenger), 1);
    EXPECT_TRUE(is_drivable(UnitKind::Scavenger)) << "the player must be able to drive it";
    const Spawn& player_factory = s.spawns.at(4);
    ASSERT_EQ(player_factory.kind, UnitKind::Factory);
    for (const Spawn& sp : s.spawns) {
        if (sp.kind != UnitKind::Scavenger) {
            continue;
        }
        const Spawn& factory = sp.side == Side::Player ? player_factory : s.spawns.at(15);
        EXPECT_LE(
            std::max(std::abs(sp.tile_x - factory.tile_x), std::abs(sp.tile_y - factory.tile_y)), 8)
            << "a hauler starts with the base it hauls to";
    }
}

} // namespace
