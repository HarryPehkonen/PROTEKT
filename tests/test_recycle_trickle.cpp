#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

/// SLICE G3: RECYCLE COMPLETES, AND SCRAP TRICKLES IN WITH NO SCAVENGER.
/// The owner, from play:
///   "[a turret and a tank] return to the deployed recycler, but don't get reclaimed into scrap.
///    They just keep trying to get closer to the recycler."
///   "When no scavenger is deployed, and no scavenger exists, scrap stores should still
///    accumulate, but VERY slowly. Right now, I have no scavengers, and 17 in scrap, so I can't
///    build anything."
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kPatch = 34;

struct Level {
    int x{};
    int y{};
};

/// The first kPatch x kPatch square of seed 42 with no crater in it (as test_deploy_clear).
[[nodiscard]] Level find_open() {
    for (int y = 40; y < kMapTiles - 40 - kPatch; ++y) {
        for (int x = 40; x < kMapTiles - 60 - kPatch; ++x) {
            bool clear = true;
            for (int dy = 0; dy < kPatch && clear; ++dy) {
                for (int dx = 0; dx < kPatch && clear; ++dx) {
                    clear = world::sample(kSeed, x + dx, y + dy).terrain != world::Terrain::Crater;
                }
            }
            if (clear) {
                return Level{x, y};
            }
        }
    }
    return Level{-1, -1};
}

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, int y) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = y;
    return s;
}

void run(Sim& sim, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
}

/// How many living units of `kind` the player has.
[[nodiscard]] int living(const Sim& sim, UnitKind kind) {
    int n = 0;
    for (const Unit& u : sim.units()) {
        n += (u.alive && u.kind == kind && u.side == Side::Player) ? 1 : 0;
    }
    return n;
}

/// A DEPLOYED recycler at the middle of an open patch, plus `more` units, and a far enemy
/// factory. The recycler is unit 0; the deploy has finished when this returns.
[[nodiscard]] Sim deployed_rig(const std::vector<Spawn>& more) {
    const Level open = find_open();
    const Level site{open.x + 12, open.y + 12};
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns.push_back(spawn(UnitKind::Recycler, Side::Player, site.x + 4, site.y + 4));
    for (const Spawn& s : more) {
        scenario.spawns.push_back(s);
    }
    scenario.spawns.push_back(spawn(UnitKind::Factory, Side::Enemy, 200, 200));
    Sim sim = Sim::create(scenario);
    sim.enqueue(Order{sim.units().at(0).id, orders::Deploy{site.x, site.y}});
    run(sim, static_cast<int>(kDeployTicks) + 60);
    return sim;
}

[[nodiscard]] Sim recycle_rig(UnitKind kind) {
    const Level open = find_open();
    const Level site{open.x + 12, open.y + 12};
    Sim sim = deployed_rig({spawn(kind, Side::Player, site.x - 6, site.y + 4)});
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::DeployedRecycler);
    return sim;
}

void recycle_into_base(Sim& sim, UnitId unit) {
    sim.enqueue(Order{unit, orders::Recycle{sim.units().at(0).id}});
}

/// Steps until `unit` is gone (at most `limit` ticks) and returns how far the player's stores rose
/// on THAT tick -- the trickle also runs, so the bank's total is not the recycle's alone.
/// Returns -1 when the unit was never un-built.
[[nodiscard]] int stores_gained_on_recycle(Sim& sim, UnitId unit, int limit) {
    for (int t = 0; t < limit; ++t) {
        const int before = sim.ore(Side::Player);
        sim.step();
        if (!sim.find(unit)->alive) {
            return sim.ore(Side::Player) - before;
        }
    }
    return -1;
}

// --- BUG ONE: A UNIT SENT TO A DEPLOYED RECYCLER IS UN-BUILT

TEST(RecycleCompletes, ThePatchIsOpen) { ASSERT_GE(find_open().x, 0); }

TEST(RecycleCompletes, ATankSentToTheDeployedRecyclerIsGoneAndTheStoresGoUpByItsValue) {
    Sim sim = recycle_rig(UnitKind::Tank);
    const UnitId tank = sim.units().at(1).id;
    recycle_into_base(sim, tank);
    EXPECT_EQ(stores_gained_on_recycle(sim, tank, 1500), recycle_refund(UnitKind::Tank))
        << "the tank was never un-built, or the stores did not go up by its value";
    EXPECT_FALSE(sim.find(tank)->alive);
    EXPECT_EQ(recycle_refund(UnitKind::Tank), 27) << "half of the tank's 55";
}

TEST(RecycleCompletes, ATurretSentToTheDeployedRecyclerIsGoneAndTheStoresGoUpByItsValue) {
    Sim sim = recycle_rig(UnitKind::Turret);
    const UnitId turret = sim.units().at(1).id;
    recycle_into_base(sim, turret);
    // a turret crawls, and spends 90 ticks unmounting first
    EXPECT_EQ(stores_gained_on_recycle(sim, turret, 3000), recycle_refund(UnitKind::Turret))
        << "the turret was never un-built, or the stores did not go up by its value";
    EXPECT_FALSE(sim.find(turret)->alive);
    EXPECT_EQ(recycle_refund(UnitKind::Turret), 20) << "half of the turret's 40";
}

TEST(RecycleCompletes, NoUnitLingersAtTheRecyclerAfterTheRecycle) {
    const Level open = find_open();
    const Level site{open.x + 12, open.y + 12};
    Sim sim = deployed_rig({spawn(UnitKind::Tank, Side::Player, site.x - 6, site.y + 4),
                            spawn(UnitKind::Scout, Side::Player, site.x - 6, site.y + 6),
                            spawn(UnitKind::Scavenger, Side::Player, site.x + 4, site.y - 6)});
    ASSERT_EQ(living(sim, UnitKind::Tank) + living(sim, UnitKind::Scout)
                  + living(sim, UnitKind::Scavenger),
              3);
    for (std::size_t i = 1; i <= 3; ++i) {
        recycle_into_base(sim, sim.units().at(i).id);
    }
    const int before = sim.ore(Side::Player);
    run(sim, 1500);
    const int trickle = 1500 / kTrickleIntervalTicks + 1; // beats that may fall in the window
    EXPECT_EQ(living(sim, UnitKind::Tank), 0);
    EXPECT_EQ(living(sim, UnitKind::Scout), 0);
    EXPECT_EQ(living(sim, UnitKind::Scavenger), 0);
    int pressing = 0;
    for (const Unit& u : sim.units()) {
        pressing += (u.alive && u.stance == Stance::Recycle) ? 1 : 0;
    }
    EXPECT_EQ(pressing, 0) << "a unit is still trying to get closer";
    // The refunds are 27 + 25 + 10 = 62, but the store holds 40: the rest lands as litter. So the
    // bank is full OR 62 up, and nothing is lost -- stores plus litter pieces account for all 62.
    const int gained = sim.ore(Side::Player) - before;
    EXPECT_GE(gained + static_cast<int>(sim.scraps().size()), 62);
    EXPECT_LE(gained + static_cast<int>(sim.scraps().size()), 62 + trickle);
    EXPECT_GT(gained, 0) << "the stores did not go up";
}

TEST(RecycleCompletes, TheValueFollowsTheKindsOwnCost) {
    EXPECT_EQ(recycle_refund(UnitKind::Scavenger), 10);
    EXPECT_EQ(recycle_refund(UnitKind::Scout), 25);
    EXPECT_EQ(recycle_refund(UnitKind::Recycler), kRecycleOre) << "no price: the old floor";
}

// --- BUG TWO: THE TRICKLE

/// A side with a recycler and nothing else: no scavenger, no litter.
[[nodiscard]] Sim bare_base() {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = {spawn(UnitKind::Recycler, Side::Player, 60, 60),
                       spawn(UnitKind::Factory, Side::Enemy, 200, 200)};
    return Sim::create(scenario);
}

TEST(Trickle, TheRateIsOneScrapEveryTwentySeconds) {
    EXPECT_EQ(kTrickleIntervalTicks, 600);
    EXPECT_EQ(kTrickleIntervalTicks, 20 * kTicksPerSecond);
}

TEST(Trickle, WithNoScavengerAndNoLitterTheStoresStillRise) {
    Sim sim = bare_base();
    ASSERT_EQ(living(sim, UnitKind::Scavenger), 0);
    ASSERT_TRUE(sim.scraps().empty());
    run(sim, 6001); // ten trickle beats (tick 0 is not one: 600, 1200 ... 6000)
    EXPECT_EQ(sim.ore(Side::Player), 10);
}

TEST(Trickle, TheRiseIsSlowNotAFlood) {
    Sim sim = bare_base();
    run(sim, (30 * 60) + 1); // one minute and a tick: beats at 600, 1200, 1800
    EXPECT_EQ(sim.ore(Side::Player), 3) << "three a minute";
    EXPECT_LT(sim.ore(Side::Player), 5);
}

TEST(Trickle, AFullStoreDoesNotOverflowIntoLitter) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.store_capacity = 2;
    scenario.spawns = {spawn(UnitKind::Recycler, Side::Player, 60, 60),
                       spawn(UnitKind::Factory, Side::Enemy, 200, 200)};
    Sim sim = Sim::create(scenario);
    run(sim, 6000);
    EXPECT_EQ(sim.ore(Side::Player), 2);
    EXPECT_TRUE(sim.scraps().empty());
}

TEST(Trickle, ASideWithNoRecyclerGetsNothing) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = {spawn(UnitKind::Tank, Side::Player, 60, 60),
                       spawn(UnitKind::Factory, Side::Enemy, 200, 200)};
    Sim sim = Sim::create(scenario);
    run(sim, 3000);
    EXPECT_EQ(sim.ore(Side::Player), 0);
}

TEST(Trickle, TheRecoveryCaseFromZeroWaitingAffordsAScavenger) {
    // THE SOFT-LOCK: no scavenger, no scrap. Waiting must be enough, and the number is the proof:
    // a scavenger costs 20, one scrap comes every 600 ticks, so 12000 ticks -- 400 s -- it is
    // there.
    const int cost = spec_of(UnitKind::Scavenger).cost;
    ASSERT_EQ(cost, 20);
    Sim sim = bare_base();
    int affordable_at = -1;
    for (int t = 1; t <= 20000; ++t) {
        sim.step();
        if (sim.ore(Side::Player) >= cost) {
            affordable_at = t;
            break;
        }
    }
    ASSERT_GE(affordable_at, 0) << "waiting never affords a scavenger: the soft-lock stands";
    EXPECT_EQ(affordable_at, cost * kTrickleIntervalTicks + 1) << "tick 0 is not a beat";
    EXPECT_EQ(affordable_at / kTicksPerSecond, 400);
}

TEST(Trickle, ADeployedScavengerStillOutEarnsItByALot) {
    // Both numbers: the trickle is 1 per 600 ticks; the pool is 1 per 30 in the gold band.
    EXPECT_EQ(kTrickleIntervalTicks / kPoolFastIntervalTicks, 20) << "20x in the gold band";
    EXPECT_EQ(kTrickleIntervalTicks / kPoolSlowIntervalTicks, 4) << "4x even in the green";
    EXPECT_GE(kTrickleIntervalTicks, 4 * kPoolSlowIntervalTicks);
}

} // namespace
} // namespace protekt::sim
