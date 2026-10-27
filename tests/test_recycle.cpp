#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace {

using protekt::sim::Fixed;
using protekt::sim::Order;
using protekt::sim::Scenario;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::Unit;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;

constexpr std::uint64_t kSeed = 20261026;

/// A tank of the player's, four tiles from its own recycler, with an enemy parked far enough away
/// that nothing shoots first: the recycle is the only thing this battle does.
Scenario recycle_battle() {
    Scenario s;
    s.seed = kSeed;
    s.spawns = {Spawn{UnitKind::Recycler, Side::Player, 50, 50},
                Spawn{UnitKind::Tank, Side::Player, 54, 50},
                Spawn{UnitKind::Tank, Side::Enemy, 110, 110}};
    return s;
}

/// The first living unit of `kind` on `side`, or an invalid id.
UnitId find_unit(const Sim& sim, UnitKind kind, Side side) {
    for (const Unit& unit : sim.units()) {
        if (unit.alive && unit.kind == kind && unit.side == side) {
            return unit.id;
        }
    }
    return UnitId{};
}

/// The unit with `id`, or nullptr when the id names nothing in the sim.
const Unit* unit_of(const Sim& sim, UnitId id) {
    for (const Unit& unit : sim.units()) {
        if (unit.id == id) {
            return &unit;
        }
    }
    return nullptr;
}

/// Step until `done`, or until `limit` ticks have passed. Returns true when `done` held.
template <typename Predicate> bool step_until(Sim& sim, int limit, Predicate done) {
    for (int i = 0; i < limit; ++i) {
        if (done(sim)) {
            return true;
        }
        sim.step();
    }
    return done(sim);
}

TEST(Recycle, TheTankDrivesToItsRecyclerAndIsConsumedIntoTheScrapReserve) {
    Sim sim = Sim::create(recycle_battle());
    const UnitId tank = find_unit(sim, UnitKind::Tank, Side::Player);
    const UnitId recycler = find_unit(sim, UnitKind::Recycler, Side::Player);
    ASSERT_TRUE(tank.valid());
    ASSERT_TRUE(recycler.valid());
    ASSERT_EQ(sim.ore(Side::Player), 0) << "nothing banked before the recycle";
    ASSERT_EQ(sim.scraps().size(), 0U);

    const Unit* start = unit_of(sim, tank);
    ASSERT_NE(start, nullptr);
    const std::int64_t start_x = start->x.raw;

    sim.enqueue(Order{tank, protekt::sim::orders::Recycle{recycler}});
    ASSERT_EQ(sim.command_log().size(), 1U) << "the order was accepted";

    // The ship GOES TO the recycler: four tiles west of it, it must actually close the distance
    // before it is consumed -- a recycle that killed it where it stood would pass the ledger
    // checks below without ever driving.
    bool moved = false;
    for (int i = 0; i < 3000; ++i) {
        const Unit* now = unit_of(sim, tank);
        ASSERT_NE(now, nullptr);
        if (!now->alive) {
            break;
        }
        if (now->x.raw < start_x) {
            moved = true;
        }
        sim.step();
    }
    EXPECT_TRUE(moved) << "the tank never moved toward its recycler";

    ASSERT_TRUE(step_until(sim, 3000, [tank](const Sim& s) {
        const Unit* unit = unit_of(s, tank);
        return unit != nullptr && !unit->alive;
    })) << "the tank never reached its recycler";

    EXPECT_EQ(sim.ore(Side::Player), protekt::sim::kRecycleOre)
        << "one recycled unit, one delivered load";
    EXPECT_EQ(sim.ore(Side::Enemy), 0) << "the enemy's ledger is untouched";
    EXPECT_EQ(sim.scraps().size(), 0U)
        << "a recycled unit leaves no wreckage: its value goes to the ledger instead";
    const Unit* recycler_now = unit_of(sim, recycler);
    ASSERT_NE(recycler_now, nullptr);
    EXPECT_TRUE(recycler_now->alive) << "the recycler itself survives the recycle";
}

TEST(Recycle, ARecycleIntoTheEnemyRecyclerIsRefused) {
    // The base belongs to the side: recycling into the enemy's would hand it the scrap. The sim
    // refuses the order whole, so nothing is logged and the tank keeps whatever it was doing.
    Scenario s = recycle_battle();
    s.spawns.push_back(Spawn{UnitKind::Recycler, Side::Enemy, 108, 108});
    Sim sim = Sim::create(s);
    const UnitId tank = find_unit(sim, UnitKind::Tank, Side::Player);
    const UnitId enemy_recycler = find_unit(sim, UnitKind::Recycler, Side::Enemy);
    ASSERT_TRUE(tank.valid());
    ASSERT_TRUE(enemy_recycler.valid());
    const std::size_t commands = sim.command_log().size();

    sim.enqueue(Order{tank, protekt::sim::orders::Recycle{enemy_recycler}});
    EXPECT_EQ(sim.command_log().size(), commands) << "an invalid order is never logged";
    for (int i = 0; i < 200; ++i) {
        sim.step();
    }
    const Unit* unit = unit_of(sim, tank);
    ASSERT_NE(unit, nullptr);
    EXPECT_TRUE(unit->alive) << "the tank was not consumed into the enemy's ledger";
    EXPECT_EQ(sim.ore(Side::Enemy), 0);
}

TEST(Recycle, ARecyclerMayNotRecycleItself) {
    Sim sim = Sim::create(recycle_battle());
    const UnitId recycler = find_unit(sim, UnitKind::Recycler, Side::Player);
    ASSERT_TRUE(recycler.valid());
    const std::size_t commands = sim.command_log().size();

    sim.enqueue(Order{recycler, protekt::sim::orders::Recycle{recycler}});
    EXPECT_EQ(sim.command_log().size(), commands) << "a unit may not name itself";
    for (int i = 0; i < 200; ++i) {
        sim.step();
    }
    const Unit* unit = unit_of(sim, recycler);
    ASSERT_NE(unit, nullptr);
    EXPECT_TRUE(unit->alive);
    EXPECT_EQ(sim.ore(Side::Player), 0);
}

TEST(Recycle, TheSameRecycleTwiceBanksTheSameLoadAndHashesTheSame) {
    // Determinism: the drive, the arrival and the ledger write are all in the hashed state.
    auto run = [] {
        Sim sim = Sim::create(recycle_battle());
        const UnitId tank = find_unit(sim, UnitKind::Tank, Side::Player);
        const UnitId recycler = find_unit(sim, UnitKind::Recycler, Side::Player);
        sim.enqueue(Order{tank, protekt::sim::orders::Recycle{recycler}});
        for (int i = 0; i < 3000; ++i) {
            sim.step();
        }
        return sim;
    };
    const Sim first = run();
    const Sim second = run();
    EXPECT_EQ(first.ore(Side::Player), second.ore(Side::Player));
    EXPECT_EQ(first.ore(Side::Player), protekt::sim::kRecycleOre);
    EXPECT_EQ(first.state_hash(), second.state_hash());
}

} // namespace
