// BIOMETAL POOLS and the scrap store's capacity (Harri, 2026-10-31 and 2026-11-01). One test per
// rule, each isolating its own number, so a red names the rule that broke.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/ai/utility.hpp"
#include "protekt/game/assist.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace {

using protekt::ai::UtilityCommander;
using protekt::sim::Fixed;
using protekt::sim::Observation;
using protekt::sim::Order;
using protekt::sim::Pool;
using protekt::sim::Scenario;
using protekt::sim::Scrap;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::Unit;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;
using protekt::sim::UnitView;
namespace orders = protekt::sim::orders;

constexpr std::uint64_t kSeed = 20261023;
constexpr std::uint32_t kDeployDone = protekt::sim::kDeployTicks + 60; ///< walk + countdown, ample

/// A tile with open ground all round it, found by scanning from (x, y), so no test depends on a
/// hand-picked coordinate happening to be passable.
Pool open_pool(int x, int y) {
    for (int r = 0; r < 60; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                bool open = true;
                for (int oy = -2; oy <= 2 && open; ++oy) {
                    for (int ox = -2; ox <= 2 && open; ++ox) {
                        open = protekt::sim::tile_passable(kSeed, x + dx + ox, y + dy + oy);
                    }
                }
                if (open) {
                    return Pool{x + dx, y + dy};
                }
            }
        }
    }
    return Pool{x, y};
}

/// Two pools on open ground with a player scavenger beside each, a player factory, and (if asked)
/// an enemy tank far to the east. Unit ids follow spawn order: 1 factory, 2 and 3 scavengers.
Scenario pool_battle(bool with_tank = false) {
    Scenario s;
    s.seed = kSeed;
    const Pool a = open_pool(60, 40);
    const Pool b = open_pool(60, 60);
    s.pools = {a, b};
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 52, 45},
                Spawn{UnitKind::Scavenger, Side::Player, a.tile_x - 3, a.tile_y},
                Spawn{UnitKind::Scavenger, Side::Player, b.tile_x - 3, b.tile_y}};
    if (with_tank) {
        s.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, a.tile_x + 18, a.tile_y});
    }
    return s;
}

/// A SCAVENGER WITH NO ORDER NOW DEPLOYS ITSELF onto a pool it sees (Harri, 2026-11-01), so a test
/// that wants a scavenger to stay a vehicle gives it an order that stands: it follows the factory
/// (id 1) until a test orders it somewhere else.
void hold_back(Sim& sim, std::uint32_t scavenger) {
    sim.enqueue(Order{UnitId{scavenger}, orders::Follow{UnitId{1}}});
}

void deploy(Sim& sim, std::uint32_t scavenger, const Pool& pool) {
    sim.enqueue(Order{UnitId{scavenger}, orders::Deploy{pool.tile_x, pool.tile_y}});
}

void run(Sim& sim, std::uint32_t ticks) {
    for (std::uint32_t i = 0; i < ticks; ++i) {
        sim.step();
    }
}

std::size_t deployed_count(const Sim& sim, Side side) {
    return static_cast<std::size_t>(
        std::count_if(sim.units().begin(), sim.units().end(), [side](const Unit& u) {
            return u.alive && u.side == side && u.kind == UnitKind::DeployedScavenger;
        }));
}

// ---- RULE 1: where the pools come from ---------------------------------------------------------

TEST(Pools, ASeedLaysThreeToTenPools) {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        const int count = protekt::sim::pool_count_for(seed);
        EXPECT_GE(count, protekt::sim::kMinPools) << "seed " << seed;
        EXPECT_LE(count, protekt::sim::kMaxPools) << "seed " << seed;
    }
    // ...and the seeds do not all agree: the count is a function of the seed, not a constant.
    bool seen_other = false;
    for (std::uint64_t seed = 2; seed <= 200; ++seed) {
        seen_other
            = seen_other || protekt::sim::pool_count_for(seed) != protekt::sim::pool_count_for(1);
    }
    EXPECT_TRUE(seen_other);
}

TEST(Pools, ASkirmishLaysTheSeedsPoolsOnPassableGroundAwayFromBothBases) {
    for (const std::uint64_t seed : {1ULL, 7ULL, 42ULL, 20261023ULL}) {
        const Scenario s = protekt::sim::skirmish(seed);
        ASSERT_GE(s.pools.size(), 3U);
        ASSERT_EQ(static_cast<int>(s.pools.size()), protekt::sim::pool_count_for(seed));
        for (const Pool& pool : s.pools) {
            EXPECT_TRUE(protekt::sim::tile_passable(seed, pool.tile_x, pool.tile_y));
            for (const Spawn& spawn : s.spawns) {
                if (spawn.kind == UnitKind::Factory) {
                    const int dx = pool.tile_x - spawn.tile_x;
                    const int dy = pool.tile_y - spawn.tile_y;
                    EXPECT_GT(dx * dx + dy * dy, protekt::sim::kPoolBaseClearTiles
                                                     * protekt::sim::kPoolBaseClearTiles / 2)
                        << "a pool lies on a base";
                }
            }
        }
        // A pure function of the seed: the same call lays the same pools.
        const Scenario again = protekt::sim::skirmish(seed);
        ASSERT_EQ(again.pools.size(), s.pools.size());
        for (std::size_t i = 0; i < s.pools.size(); ++i) {
            EXPECT_EQ(again.pools.at(i).tile_x, s.pools.at(i).tile_x);
            EXPECT_EQ(again.pools.at(i).tile_y, s.pools.at(i).tile_y);
        }
    }
}

TEST(Pools, ADesignedMapSetsItsOwnCountAndItsOwnStore) {
    protekt::sim::ScenarioData data = protekt::sim::default_data();
    data.pool_count = 5;
    data.store_capacity = 60;
    const Scenario s = protekt::sim::skirmish(42, data);
    EXPECT_EQ(s.pools.size(), 5U);
    EXPECT_EQ(s.store_capacity, 60);
}

// ---- RULE 5: capacity --------------------------------------------------------------------------

TEST(Store, CapacityStartsAtFortyAndTheScenarioMaySupplyAnother) {
    Sim sim = Sim::create(pool_battle());
    EXPECT_EQ(sim.capacity(Side::Player), 40);
    EXPECT_EQ(sim.capacity(Side::Enemy), 40);
    EXPECT_EQ(sim.ore(Side::Player), 0) << "capacity is space, never scrap";

    Scenario story = pool_battle();
    story.store_capacity = 100;
    EXPECT_EQ(Sim::create(story).capacity(Side::Player), 100);
}

TEST(Store, ACompletedDeployAddsTwentyOfSpaceAndNoScrapAndNotBefore) {
    Sim sim = Sim::create(pool_battle());
    hold_back(sim, 3);
    deploy(sim, 2, sim.pools().at(0));
    run(sim, protekt::sim::scavenger_deploy_ticks(Side::Player) - 20);
    EXPECT_EQ(sim.capacity(Side::Player), 40) << "still deploying: nothing is built yet";
    run(sim, kDeployDone);
    ASSERT_EQ(deployed_count(sim, Side::Player), 1U);
    EXPECT_EQ(sim.capacity(Side::Player), 60);
    EXPECT_EQ(sim.capacity(Side::Enemy), 40) << "the other side's store is its own";
    deploy(sim, 3, sim.pools().at(1));
    run(sim, kDeployDone);
    EXPECT_EQ(sim.capacity(Side::Player), 80);
}

TEST(Store, TheDeployTakesTheScavengersDeployTimeToTheTick) {
    Sim sim = Sim::create(pool_battle());
    // Standing on the pool already: the countdown is the whole wait.
    const Pool pool = sim.pools().at(0);
    Scenario s = pool_battle();
    s.spawns.at(1).tile_x = pool.tile_x;
    s.spawns.at(1).tile_y = pool.tile_y;
    sim = Sim::create(s);
    deploy(sim, 2, pool);
    std::uint32_t done = 0;
    for (std::uint32_t i = 1; i <= 600 && done == 0; ++i) {
        sim.step();
        if (sim.find(UnitId{2})->kind == UnitKind::DeployedScavenger) {
            done = i;
        }
    }
    EXPECT_NEAR(static_cast<int>(done), protekt::sim::scavenger_deploy_ticks(Side::Player), 5);
}

// ---- RULE 7: losing a deployed scavenger -------------------------------------------------------

TEST(Store, LosingADeployedScavengerDropsCapacityByTwentyAndClampsTheBank) {
    Sim sim = Sim::create(pool_battle(true));
    deploy(sim, 2, sim.pools().at(0));
    deploy(sim, 3, sim.pools().at(1));
    // Fill the store past 60 from the two pools (one unit a second), then lose a scavenger.
    while (sim.ore(Side::Player) < 70 && sim.tick() < 6000) {
        sim.step();
    }
    ASSERT_GE(sim.ore(Side::Player), 70);
    ASSERT_EQ(sim.capacity(Side::Player), 80);
    sim.enqueue(Order{UnitId{4}, orders::Attack{UnitId{2}}});
    std::uint16_t before = sim.ore(Side::Player);
    bool dropped = false;
    for (int i = 0; i < 3000 && !dropped; ++i) {
        before = sim.ore(Side::Player);
        sim.step();
        EXPECT_LE(sim.ore(Side::Player), sim.capacity(Side::Player)) << "tick " << sim.tick();
        dropped = sim.capacity(Side::Player) == 60;
    }
    ASSERT_TRUE(dropped) << "the tank never killed the deployed scavenger";
    EXPECT_GT(before, 60) << "precondition: the bank held more than the new capacity";
    EXPECT_EQ(sim.ore(Side::Player), 60) << "scrap above the new capacity is destroyed";
}

// ---- RULE 3: one scavenger per pool ------------------------------------------------------------

TEST(Pools, OnlyOneScavengerMayDeployOnAPool) {
    Scenario s = pool_battle();
    s.pools.pop_back(); // the loser would otherwise go and deploy itself on the second pool
    Sim sim = Sim::create(s);
    const Pool pool = sim.pools().at(0);
    deploy(sim, 2, pool);
    deploy(sim, 3, pool);
    run(sim, kDeployDone * 2);
    EXPECT_EQ(deployed_count(sim, Side::Player), 1U);
    EXPECT_EQ(sim.find(UnitId{2})->kind, UnitKind::DeployedScavenger);
    EXPECT_EQ(sim.find(UnitId{3})->kind, UnitKind::Scavenger) << "the second is still a vehicle";
    EXPECT_EQ(sim.capacity(Side::Player), 60);
    // A fresh order to the taken pool is refused outright.
    const std::size_t logged = sim.command_log().size();
    deploy(sim, 3, pool);
    EXPECT_EQ(sim.command_log().size(), logged);
}

TEST(Pools, ADestroyedScavengerFreesItsPoolForADifferentOne) {
    Scenario s = pool_battle(true);
    s.pools.pop_back(); // see OnlyOneScavengerMayDeployOnAPool
    Sim sim = Sim::create(s);
    const Pool pool = sim.pools().at(0);
    deploy(sim, 2, pool);
    run(sim, kDeployDone);
    sim.enqueue(Order{UnitId{4}, orders::Attack{UnitId{2}}});
    for (int i = 0; i < 3000 && sim.find(UnitId{2})->alive; ++i) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(UnitId{2})->alive);
    const std::size_t logged = sim.command_log().size();
    deploy(sim, 3, pool);
    EXPECT_EQ(sim.command_log().size(), logged + 1) << "the pool is free again";
}

TEST(Pools, AScavengerCanOnlyDeployOnAPool) {
    Sim sim = Sim::create(pool_battle());
    const std::size_t logged = sim.command_log().size();
    sim.enqueue(Order{UnitId{2}, orders::Deploy{52, 52}});
    EXPECT_EQ(sim.command_log().size(), logged) << "not a pool";
}

// ---- RULE 8: no un-deploy ----------------------------------------------------------------------

TEST(Pools, ADeployedScavengerTakesNoOrderAndIsNeverUndeployed) {
    Sim sim = Sim::create(pool_battle());
    hold_back(sim, 3);
    deploy(sim, 2, sim.pools().at(0));
    run(sim, kDeployDone);
    ASSERT_EQ(sim.find(UnitId{2})->kind, UnitKind::DeployedScavenger);
    const Unit before = *sim.find(UnitId{2});
    const std::size_t logged = sim.command_log().size();
    sim.enqueue(Order{UnitId{2}, orders::MoveTo{Fixed::from_int(90), Fixed::from_int(90)}});
    sim.enqueue(Order{UnitId{2}, orders::Stop{}});
    sim.enqueue(Order{UnitId{2}, orders::Follow{UnitId{1}}});
    sim.enqueue(Order{UnitId{2}, orders::Recycle{UnitId{1}}});
    sim.enqueue(
        Order{UnitId{2}, orders::Deploy{sim.pools().at(1).tile_x, sim.pools().at(1).tile_y}});
    EXPECT_EQ(sim.command_log().size(), logged) << "every order to it is refused";
    run(sim, 300);
    const Unit& after = *sim.find(UnitId{2});
    EXPECT_EQ(after.kind, UnitKind::DeployedScavenger);
    EXPECT_EQ(after.x.raw, before.x.raw);
    EXPECT_EQ(after.y.raw, before.y.raw);
    EXPECT_EQ(sim.capacity(Side::Player), 60);
}

// ---- RULE 4: the rate does not scale -----------------------------------------------------------

TEST(Pools, TwoPoolsExtractNoFasterThanOne) {
    const auto gain = [](bool both) {
        Sim sim = Sim::create(pool_battle());
        deploy(sim, 2, sim.pools().at(0));
        if (both) {
            deploy(sim, 3, sim.pools().at(1));
        }
        run(sim, kDeployDone);
        const int start = sim.ore(Side::Player);
        run(sim, 300); // ten seconds
        return static_cast<int>(sim.ore(Side::Player)) - start;
    };
    EXPECT_EQ(gain(false), 10) << "one unit a second";
    EXPECT_EQ(gain(true), 10) << "a second pool buys capacity, not speed";
}

// ---- RULE 6: a full store stops collection -----------------------------------------------------

TEST(Store, AFullStoreStopsExtractionAtAPool) {
    Scenario s = pool_battle();
    s.store_capacity = 3;
    Sim sim = Sim::create(s);
    hold_back(sim, 3);
    deploy(sim, 2, sim.pools().at(0));
    // 20 units at the fast beat, then the last 3 at the slow one.
    run(sim, kDeployDone + 1500);
    EXPECT_EQ(sim.capacity(Side::Player), 23);
    EXPECT_EQ(sim.ore(Side::Player), 23) << "full, and exactly full";
    run(sim, 300);
    EXPECT_EQ(sim.ore(Side::Player), 23) << "nothing overflows";
}

TEST(Store, AFullStoreLeavesLitterWhereItLies) {
    Scenario s = pool_battle();
    s.store_capacity = 3;
    const Pool pool = s.pools.at(0);
    for (int i = 0; i < 8; ++i) {
        s.scrap.push_back(Scrap{Fixed::from_int(pool.tile_x - 3 + (i % 4)),
                                Fixed::from_int(pool.tile_y + 1 + (i / 4))});
    }
    Sim sim = Sim::create(s);
    const std::size_t laid = sim.scraps().size();
    ASSERT_EQ(laid, 8U);
    run(sim, 900);
    EXPECT_EQ(sim.ore(Side::Player), 3) << "capacity, no more";
    EXPECT_EQ(sim.scraps().size(), laid - 3) << "the rest is still on the ground, not destroyed";
}

// ---- RULE 10: who may deploy -------------------------------------------------------------------

/// An observation for the enemy: a tank (id 10) at (tx, ty) tiles moving at velocity (vx, vy); an
/// enemy scavenger (id 11) at (sx, sy); one pool.
Observation enemy_view(int tx, int ty, double vx, double vy, const Pool& pool, int sx, int sy,
                       Side own = Side::Enemy) {
    Observation o{};
    o.tick = 0;
    const auto raw = [](double v) { return static_cast<std::int32_t>(v * 65536.0); };
    // Both units stand on the MIDDLE of their tiles, as a spawned unit does.
    o.own.push_back(UnitView{UnitId{10}, UnitKind::Tank, own, tx, ty, 100, raw(tx + 0.5),
                             raw(ty + 0.5), true, raw(vx), raw(vy)});
    o.own.push_back(UnitView{UnitId{11}, UnitKind::Scavenger, own, sx, sy, 100, raw(sx + 0.5),
                             raw(sy + 0.5), true, 0, 0});
    o.pools.push_back(pool);
    return o;
}

std::optional<orders::Deploy> deploy_order(const std::vector<Order>& out, std::uint32_t unit) {
    for (const Order& order : out) {
        if (const auto* d = std::get_if<orders::Deploy>(&order.payload);
            d && order.unit.value == unit) {
            return *d;
        }
    }
    return std::nullopt;
}

TEST(Pools, AnEnemyVehiclePointingAtAPoolOrdersAScavengerToDeployWithoutTheScavengerSeeingIt) {
    // The tank at (100,100) drives due east; the pool lies 20 tiles dead ahead. The scavenger is
    // 90 tiles away -- far outside its own 40-tile radar of the pool.
    const Pool pool{120, 100};
    UtilityCommander ai;
    std::vector<Order> out;
    ai.think(enemy_view(100, 100, 0.2, 0.0, pool, 100, 190), out);
    const auto order = deploy_order(out, 11);
    if (!order.has_value()) {
        FAIL() << "no deploy order";
    }
    EXPECT_EQ(order->tile_x, 120);
    EXPECT_EQ(order->tile_y, 100);
}

TEST(Pools, TheOrderingVehicleMustFaceThePoolWithinTheAssistCone) {
    const Pool pool{120, 100}; // 20 tiles east: 4 degrees is 1.4 tiles off the line
    const auto orders_for = [&](double vy) {
        UtilityCommander ai;
        std::vector<Order> out;
        ai.think(enemy_view(100, 100, 0.2, vy, pool, 100, 110), out);
        return deploy_order(out, 11).has_value();
    };
    EXPECT_TRUE(orders_for(0.2 * 0.06)) << "3.4 degrees off: inside the cone";
    EXPECT_FALSE(orders_for(0.2 * 0.08)) << "4.6 degrees off: outside it";
    EXPECT_FALSE(orders_for(-0.2 * 0.08));
    EXPECT_FALSE(orders_for(0.2)) << "45 degrees";
    // Facing away, even dead along the line.
    UtilityCommander ai;
    std::vector<Order> out;
    ai.think(enemy_view(100, 100, -0.2, 0.0, pool, 100, 110), out);
    EXPECT_FALSE(deploy_order(out, 11).has_value());
    EXPECT_EQ(static_cast<float>(protekt::sim::kPointingConeDegrees),
              protekt::game::kAssistConeDegrees);
}

TEST(Pools, ThePoolMustBeInTheOrderingVehiclesRadar) {
    UtilityCommander ai;
    std::vector<Order> out;
    // 60 tiles dead ahead of a tank (radar 40): it cannot see the pool, so it does not order.
    ai.think(enemy_view(100, 100, 0.2, 0.0, Pool{160, 100}, 100, 110), out);
    EXPECT_FALSE(deploy_order(out, 11).has_value());
}

TEST(Pools, AStoppedVehicleKeepsTheFacingItLastHad) {
    const Pool pool{120, 100};
    UtilityCommander ai;
    std::vector<Order> out;
    Observation moving = enemy_view(100, 100, 0.2, 0.0, pool, 100, 190);
    moving.tick = 1; // not on the commander's beat: it learns the heading and decides nothing
    ai.think(moving, out);
    Observation parked = enemy_view(100, 100, 0.0, 0.0, pool, 100, 190);
    parked.tick = 15;
    ai.think(parked, out);
    EXPECT_TRUE(deploy_order(out, 11).has_value());
}

TEST(Pools, NoDeployOnATakenPoolOrWithLitterInReach) {
    const Pool pool{120, 100};
    {
        UtilityCommander ai;
        std::vector<Order> out;
        Observation o = enemy_view(100, 100, 0.2, 0.0, pool, 100, 190);
        const auto raw = [](int v) { return v * 65536; };
        o.enemy.push_back(UnitView{UnitId{20}, UnitKind::DeployedScavenger, Side::Player, 120, 100,
                                   100, raw(120), raw(100), true, 0, 0});
        ai.think(o, out);
        EXPECT_FALSE(deploy_order(out, 11).has_value()) << "one scavenger per pool";
    }
    {
        UtilityCommander ai;
        std::vector<Order> out;
        Observation o = enemy_view(100, 100, 0.2, 0.0, pool, 100, 190);
        o.scrap.push_back(Scrap{Fixed::from_int(100), Fixed::from_int(180)}); // 10 tiles from it
        ai.think(o, out);
        EXPECT_FALSE(deploy_order(out, 11).has_value()) << "litter worth collecting is in reach";
    }
    {
        UtilityCommander ai;
        std::vector<Order> out;
        Observation o = enemy_view(100, 100, 0.2, 0.0, pool, 100, 190);
        o.pools.push_back(Pool{120, 100});
        o.scrap.push_back(Scrap{Fixed::from_int(10), Fixed::from_int(10)}); // far beyond its radar
        ai.think(o, out);
        EXPECT_TRUE(deploy_order(out, 11).has_value()) << "litter out of reach does not count";
    }
}

// SUPERSEDED (Harri, 2026-11-01): this test pinned "on my side, deploy on my command only". He
// ruled otherwise -- a scavenger with no order deploys itself on EITHER side (test_auto_deploy.cpp)
// -- so the sim half of it is gone. What stands is the COMMANDER's path: a vehicle pointing at a
// pool orders a scavenger over it for the enemy side only, and the player's observation gets no
// order.
TEST(Pools, TheCommandersPoolOrderIsTheEnemysAloneAndThePlayerIsNeverOrdered) {
    const Pool pool{120, 100};
    UtilityCommander ai;
    std::vector<Order> out;
    ai.think(enemy_view(100, 100, 0.2, 0.0, pool, 100, 190, Side::Player), out);
    EXPECT_FALSE(deploy_order(out, 11).has_value());
}

/// A pool with a dead-straight run of open ground on its row, 14 tiles to the west and 30 to the
/// east, so a tank driving at it keeps one heading.
Pool pool_on_a_clear_row() {
    for (int k = 0; k < 40; ++k) {
        const Pool pool = open_pool(60 + (7 * k), 40 + (3 * k));
        bool clear = true;
        for (int x = pool.tile_x - 14; x <= pool.tile_x + 30 && clear; ++x) {
            clear = protekt::sim::tile_passable(kSeed, x, pool.tile_y)
                    && protekt::sim::tile_passable(kSeed, x, pool.tile_y - 1)
                    && protekt::sim::tile_passable(kSeed, x, pool.tile_y + 1);
        }
        if (clear) {
            return pool;
        }
    }
    return open_pool(60, 40);
}

TEST(Pools, TheEnemysDeployOrderIsOneTheSimAcceptsAndCompletes) {
    Scenario s;
    s.seed = kSeed;
    const Pool pool = pool_on_a_clear_row();
    s.pools = {pool};
    s.spawns = {Spawn{UnitKind::Factory, Side::Enemy, 52, 45},
                Spawn{UnitKind::Scavenger, Side::Enemy, pool.tile_x, pool.tile_y + 4},
                Spawn{UnitKind::Tank, Side::Enemy, pool.tile_x - 14, pool.tile_y}};
    Sim sim = Sim::create(s);
    // The tank drives east along the pool's row long enough to have a velocity to face along.
    sim.enqueue(Order{UnitId{3}, orders::MoveTo{Fixed::from_int(pool.tile_x + 30),
                                                Fixed::from_int(pool.tile_y)}});
    run(sim, 15);
    UtilityCommander ai;
    std::vector<Order> out;
    ai.think(protekt::sim::observe(sim, Side::Enemy), out);
    const auto order = deploy_order(out, 2);
    if (!order.has_value()) {
        FAIL() << "no deploy order";
    }
    const std::size_t logged = sim.command_log().size();
    sim.enqueue(Order{UnitId{2}, *order});
    EXPECT_EQ(sim.command_log().size(), logged + 1);
    run(sim, kDeployDone);
    EXPECT_EQ(sim.find(UnitId{2})->kind, UnitKind::DeployedScavenger);
    EXPECT_EQ(sim.capacity(Side::Enemy), 60);
    EXPECT_EQ(sim.capacity(Side::Player), 40);
}

} // namespace
