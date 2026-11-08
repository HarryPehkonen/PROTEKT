// A SCAVENGER WITH NO ORDER DEPLOYS ITSELF, the store's two rates, and a recycle the store cannot
// absorb (Harri, 2026-11-01, watched in BattleZone 2). One test per rule, each isolating its own
// number, so a red names the rule that broke.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace {

using protekt::sim::Fixed;
using protekt::sim::Order;
using protekt::sim::Pool;
using protekt::sim::Scenario;
using protekt::sim::Scrap;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;
namespace orders = protekt::sim::orders;

constexpr std::uint64_t kSeed = 20261101;
/// Ample for a walk of a few tiles, the beat that notices the pool, and the whole countdown.
constexpr std::uint32_t kDeployDone = protekt::sim::kScavengerDeployTicks.at(0) + 300;

/// A tile with open ground all round it, found by scanning from (x, y).
bool open_at(int x, int y) {
    for (int oy = -2; oy <= 2; ++oy) {
        for (int ox = -2; ox <= 2; ++ox) {
            if (!protekt::sim::tile_passable(kSeed, x + ox, y + oy)) {
                return false;
            }
        }
    }
    return true;
}

Pool open_pool(int x, int y) {
    for (int r = 0; r < 60; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (open_at(x + dx, y + dy)) {
                    return Pool{x + dx, y + dy};
                }
            }
        }
    }
    return Pool{x, y};
}

void run(Sim& sim, std::uint32_t ticks) {
    for (std::uint32_t i = 0; i < ticks; ++i) {
        sim.step();
    }
}

/// One pool, a player scavenger (id 2) `gap` tiles west of it, and a player factory (id 1) to
/// follow. No enemy and no litter, so the scavenger has nothing to do but this.
Scenario lone(int gap = 8, Side side = Side::Player) {
    Scenario s;
    s.seed = kSeed;
    const Pool pool = open_pool(70, 60);
    s.pools = {pool};
    const Pool spot = open_pool(pool.tile_x - gap, pool.tile_y);
    s.spawns = {Spawn{UnitKind::Factory, side, 50, 60},
                Spawn{UnitKind::Scavenger, side, spot.tile_x, spot.tile_y}};
    return s;
}

/// Tiles from the scavenger to the pool's middle, as the sim measures its radar.
double distance_to_pool(const Sim& sim) {
    const auto& u = *sim.find(UnitId{2});
    const double dx = sim.pools().at(0).tile_x + 0.5 - u.x.raw / 65536.0;
    const double dy = sim.pools().at(0).tile_y + 0.5 - u.y.raw / 65536.0;
    return std::sqrt((dx * dx) + (dy * dy));
}

bool deployed(const Sim& sim) { return sim.find(UnitId{2})->kind == UnitKind::DeployedScavenger; }

// ---- 1. a scavenger with no order deploys itself -----------------------------------------------

TEST(AutoDeploy, AnIdleScavengerThatSeesAPoolAndNoLitterDeploysItselfOnIt) {
    Sim sim = Sim::create(lone());
    ASSERT_LE(distance_to_pool(sim), protekt::sim::kRadarRadiusTiles);
    run(sim, kDeployDone);
    ASSERT_TRUE(deployed(sim));
    const auto& u = *sim.find(UnitId{2});
    EXPECT_EQ(u.x.floor_int(), sim.pools().at(0).tile_x);
    EXPECT_EQ(u.y.floor_int(), sim.pools().at(0).tile_y);
    EXPECT_EQ(sim.capacity(Side::Player), 60);
}

TEST(AutoDeploy, TheEnemysIdleScavengerDecidesTheSameWay) {
    Sim sim = Sim::create(lone(8, Side::Enemy));
    run(sim, kDeployDone);
    EXPECT_TRUE(deployed(sim));
}

TEST(AutoDeploy, AnExplicitOrderSuspendsItForAsLongAsTheOrderStandsAndIdlenessResumesIt) {
    Sim sim = Sim::create(lone());
    sim.enqueue(Order{UnitId{2}, orders::Follow{UnitId{1}}});
    run(sim, kDeployDone * 2);
    EXPECT_FALSE(deployed(sim)) << "led past the pool, it does not stop to deploy";
    EXPECT_NE(sim.find(UnitId{2})->stance, protekt::sim::Stance::Idle);
    sim.enqueue(Order{UnitId{2}, orders::Stop{}});
    run(sim, kDeployDone * 3);
    EXPECT_TRUE(deployed(sim)) << "idle again, it sees the pool and goes";
}

TEST(AutoDeploy, ALitterPieceInItsOwnRadarKeepsItCollectingInsteadOfDeploying) {
    Scenario s = lone();
    s.store_capacity = 1; // one piece fills the store, so the rest lies there, seen and untaken
    const Pool spot = open_pool(s.spawns.at(1).tile_x + 2, s.spawns.at(1).tile_y + 2);
    for (int i = 0; i < 4; ++i) {
        s.scrap.push_back(Scrap{Fixed::from_int(spot.tile_x + i), Fixed::from_int(spot.tile_y)});
    }
    Sim sim = Sim::create(s);
    run(sim, kDeployDone * 2);
    EXPECT_FALSE(deployed(sim)) << "it sees litter, so it is not the pool's business yet";
    EXPECT_EQ(sim.scraps().size(), 3U) << "the full store left the rest where it lay";
}

TEST(AutoDeploy, WhenTheLitterIsGoneItDeploys) {
    Scenario s = lone();
    const Pool spot = open_pool(s.spawns.at(1).tile_x + 2, s.spawns.at(1).tile_y + 2);
    s.scrap.push_back(Scrap{Fixed::from_int(spot.tile_x), Fixed::from_int(spot.tile_y)});
    Sim sim = Sim::create(s);
    run(sim, kDeployDone * 2);
    EXPECT_EQ(sim.scraps().size(), 0U);
    EXPECT_TRUE(deployed(sim));
}

TEST(AutoDeploy, ItNeverActsOnAPoolItsOwnRadarCannotSee) {
    Sim sim = Sim::create(lone(protekt::sim::kRadarRadiusTiles + 6));
    ASSERT_GT(distance_to_pool(sim), protekt::sim::kRadarRadiusTiles);
    run(sim, kDeployDone * 3);
    const auto& u = *sim.find(UnitId{2});
    EXPECT_EQ(u.kind, UnitKind::Scavenger);
    EXPECT_EQ(u.stance, protekt::sim::Stance::Idle) << "it did not even start walking";
}

TEST(AutoDeploy, ASecondIdleScavengerDoesNotFollowTheFirstOntoTheSamePool) {
    Scenario s = lone();
    const Pool spot = open_pool(s.spawns.at(1).tile_x, s.spawns.at(1).tile_y + 4);
    s.spawns.push_back(Spawn{UnitKind::Scavenger, Side::Player, spot.tile_x, spot.tile_y});
    Sim sim = Sim::create(s);
    run(sim, kDeployDone * 2);
    const bool first = sim.find(UnitId{2})->kind == UnitKind::DeployedScavenger;
    const bool second = sim.find(UnitId{3})->kind == UnitKind::DeployedScavenger;
    EXPECT_NE(first, second) << "exactly one of them deployed: one scavenger per pool";
    EXPECT_EQ(sim.capacity(Side::Player), 60);
}

// ---- 3. the two rates --------------------------------------------------------------------------

/// A deployed scavenger on each of `count` pools (deployed by order, so the rates are all that is
/// measured), run until the bank holds `level`. No litter anywhere.
Sim banked(int count, std::uint16_t level) {
    Scenario s;
    s.seed = kSeed;
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 50, 60}};
    for (int i = 0; i < count; ++i) {
        const Pool pool = open_pool(70, 20 + (i * 30));
        s.pools.push_back(pool);
        s.spawns.push_back(Spawn{UnitKind::Scavenger, Side::Player, pool.tile_x, pool.tile_y});
    }
    Sim sim = Sim::create(s);
    for (int i = 0; i < count; ++i) {
        sim.enqueue(Order{UnitId{static_cast<std::uint32_t>(2 + i)},
                          orders::Deploy{s.pools.at(static_cast<std::size_t>(i)).tile_x,
                                         s.pools.at(static_cast<std::size_t>(i)).tile_y}});
    }
    while (sim.ore(Side::Player) < level && sim.tick() < 20000) {
        sim.step();
    }
    return sim;
}

int gained_in(Sim& sim, std::uint32_t window) {
    const int before = sim.ore(Side::Player);
    run(sim, window);
    return static_cast<int>(sim.ore(Side::Player)) - before;
}

TEST(StoreRates, InsideTheGoldBandTheStoreFillsAtTheFastRate) {
    Sim sim = banked(1, 2);
    ASSERT_EQ(sim.fast_band(Side::Player), 20);
    ASSERT_LT(sim.ore(Side::Player), 20);
    EXPECT_EQ(gained_in(sim, 300), 300 / protekt::sim::kPoolFastIntervalTicks);
}

TEST(StoreRates, OnceTheLevelClimbsIntoTheGreenItIsASlowTrickle) {
    Sim sim = banked(1, 20);
    ASSERT_EQ(sim.capacity(Side::Player), 60);
    ASSERT_GE(sim.ore(Side::Player), 20);
    EXPECT_EQ(gained_in(sim, 300), 300 / protekt::sim::kPoolSlowIntervalTicks);
    EXPECT_GT(protekt::sim::kPoolSlowIntervalTicks, protekt::sim::kPoolFastIntervalTicks);
}

TEST(StoreRates, MoreScavengersMakeTheFastRegionBiggerNeverTheRateFaster) {
    // Level 30 sits in the GREEN of one scavenger's store (slow) and in the GOLD of two (fast): the
    // same level, the same two rates, a band twice as tall.
    Sim one = banked(1, 30);
    ASSERT_EQ(one.fast_band(Side::Player), 20);
    EXPECT_EQ(gained_in(one, 300), 300 / protekt::sim::kPoolSlowIntervalTicks);
    Sim two = banked(2, 30);
    ASSERT_EQ(two.fast_band(Side::Player), 40);
    EXPECT_EQ(gained_in(two, 300), 300 / protekt::sim::kPoolFastIntervalTicks);
    // And at the bottom two fill exactly as one does: the rate itself never scaled.
    Sim fresh_one = banked(1, 2);
    Sim fresh_two = banked(2, 2);
    EXPECT_EQ(gained_in(fresh_one, 300), gained_in(fresh_two, 300));
}

TEST(StoreRates, LitterKeepsItsOwnFastRateWhateverBandTheLevelSitsIn) {
    // Above the gold band, where the pool is a trickle, an idle scavenger still takes a piece a
    // second.
    Scenario s;
    s.seed = kSeed;
    s.store_capacity = 40;
    const Pool pool = open_pool(70, 60);
    s.pools = {pool};
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 50, 60},
                Spawn{UnitKind::Scavenger, Side::Player, pool.tile_x, pool.tile_y},
                Spawn{UnitKind::Scavenger, Side::Player, pool.tile_x - 3, pool.tile_y}};
    const Pool spot = open_pool(pool.tile_x - 6, pool.tile_y + 4);
    for (int i = 0; i < 40; ++i) {
        s.scrap.push_back(
            Scrap{Fixed::from_int(spot.tile_x + (i % 8)), Fixed::from_int(spot.tile_y + (i / 8))});
    }
    Sim sim = Sim::create(s);
    sim.enqueue(Order{UnitId{2}, orders::Deploy{pool.tile_x, pool.tile_y}});
    sim.enqueue(Order{UnitId{3}, orders::Follow{UnitId{2}}}); // not idle: it must not collect
    while (sim.ore(Side::Player) < 25 && sim.tick() < 5000) {
        sim.step();
    }
    ASSERT_GE(sim.ore(Side::Player), 20) << "precondition: the level is in the green";
    // The follower is not collecting; the deployed one does not collect litter either. Freed, it
    // is the one thing that does.
    sim.enqueue(Order{UnitId{3}, orders::Stop{}});
    // AR: the freed scavenger first DRIVES to the litter (a few seconds' trip), so the window is
    // longer and the allowance for the drive is wider than it was when it took from where it stood.
    EXPECT_GE(gained_in(sim, 450), 450 / protekt::sim::kScavengeIntervalTicks - 5)
        << "litter is not slowed by the band";
}

// ---- 4. a recycle the store cannot absorb scatters as litter -----------------------------------

Scenario recycle_scene(std::uint16_t store) {
    Scenario s;
    s.seed = kSeed;
    s.store_capacity = store;
    s.spawns = {Spawn{UnitKind::Recycler, Side::Player, 50, 50},
                Spawn{UnitKind::Tank, Side::Player, 54, 50},
                Spawn{UnitKind::Tank, Side::Enemy, 110, 110}};
    return s;
}

void recycle_the_tank(Sim& sim) {
    sim.enqueue(Order{UnitId{2}, orders::Recycle{UnitId{1}}});
    for (int i = 0; i < 600 && sim.find(UnitId{2})->alive; ++i) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(UnitId{2})->alive);
}

TEST(RecycleOverflow, TheRemainderTheStoreCannotHoldLandsAsLitterWhereTheUnitStood) {
    Sim sim = Sim::create(recycle_scene(1)); // room for one of the two units a recycle returns
    ASSERT_EQ(sim.scraps().size(), 0U);
    recycle_the_tank(sim);
    EXPECT_EQ(sim.ore(Side::Player), 1) << "the store took what it could";
    ASSERT_EQ(sim.scraps().size(), 1U) << "and the rest is on the ground, not destroyed";
    const Scrap& piece = sim.scraps().front();
    EXPECT_LE(std::abs(piece.x.floor_int() - 50), protekt::sim::kScrapScatterRadius + 1)
        << "where the unit stood, the way a destroyed building scatters its litter";
    EXPECT_LE(std::abs(piece.y.floor_int() - 50), protekt::sim::kScrapScatterRadius + 1);
}

TEST(RecycleOverflow, AFullStoreTurnsTheWholeRefundIntoLitter) {
    Sim sim = Sim::create(recycle_scene(0));
    recycle_the_tank(sim);
    EXPECT_EQ(sim.ore(Side::Player), 0);
    EXPECT_EQ(sim.scraps().size(), protekt::sim::kRecycleOre);
}

TEST(RecycleOverflow, AStoreWithRoomTakesItAllAndLeavesNoLitter) {
    Sim sim = Sim::create(recycle_scene(40));
    recycle_the_tank(sim);
    EXPECT_EQ(sim.ore(Side::Player), protekt::sim::kRecycleOre);
    EXPECT_EQ(sim.scraps().size(), 0U);
}

} // namespace
