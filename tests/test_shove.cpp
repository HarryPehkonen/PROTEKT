#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

/// Vehicles and each other: a unit cannot be driven through, but it can be SHOVED -- the pair
/// moves, at a cost to the pusher -- and no arrangement of vehicles can hold a unit for good.
///
/// The ground is asserted open rather than described, so a world change cannot quietly turn these
/// into a record of different terrain. The enemy base is unarmed and far away: nothing here is
/// shot.
namespace protekt::sim {
namespace {

constexpr int kRowY = 22;
constexpr int kFirstX = 102;
constexpr int kLastX = 142;

[[nodiscard]] bool open_row() {
    for (int y = kRowY - 3; y <= kRowY + 3; ++y) {
        for (int x = kFirstX; x <= kLastX; ++x) {
            if (world::sample(42, x, y).terrain != world::Terrain::Flat) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] Scenario scenario_of(const std::vector<std::pair<int, int>>& tiles) {
    Scenario scenario{};
    scenario.seed = 42;
    for (const auto& tile : tiles) {
        Spawn tank{};
        tank.side = Side::Player;
        tank.kind = UnitKind::Tank;
        tank.tile_x = tile.first;
        tank.tile_y = tile.second;
        scenario.spawns.push_back(tank);
    }
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 200;
    base.tile_y = 200;
    scenario.spawns.push_back(base);
    return scenario;
}

[[nodiscard]] std::vector<UnitId> player_ids(const Sim& sim) {
    std::vector<UnitId> ids;
    for (const Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            ids.push_back(unit.id);
        }
    }
    return ids;
}

void go(Sim& sim, UnitId id, int x, int y) {
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(x), Fixed::from_int(y)}});
}

[[nodiscard]] std::int64_t gap2(const Sim& sim, UnitId a, UnitId b) {
    const Unit* ua = sim.find(a);
    const Unit* ub = sim.find(b);
    const std::int64_t dx = ua->x.raw - ub->x.raw;
    const std::int64_t dy = ua->y.raw - ub->y.raw;
    return (dx * dx) + (dy * dy);
}

} // namespace

TEST(Shove, TheGroundIsOpen) {
    ASSERT_TRUE(open_row()) << "the rows these tests drive on must be flat";
}

TEST(Shove, AVehicleCannotBeDrivenThroughAnother) {
    ASSERT_TRUE(open_row());
    Sim sim = Sim::create(scenario_of({{105, kRowY}, {108, kRowY}}));
    const auto ids = player_ids(sim);
    go(sim, ids[0], 130, kRowY); // straight through the second tank, which holds still
    std::int64_t closest = INT64_MAX;
    for (int tick = 0; tick < 40; ++tick) {
        sim.step();
        closest = std::min(closest, gap2(sim, ids[0], ids[1]));
    }
    const std::int64_t radius = kBodyRadiusRaw;
    EXPECT_GE(closest, radius * radius) << "the pusher got inside the other vehicle's body";
}

TEST(Shove, AVehicleDrivenIntoIsPushedAside) {
    ASSERT_TRUE(open_row());
    Sim sim = Sim::create(scenario_of({{105, kRowY}, {108, kRowY}}));
    const auto ids = player_ids(sim);
    const Unit start = *sim.find(ids[1]);
    go(sim, ids[0], 130, kRowY);
    for (int tick = 0; tick < 60; ++tick) {
        sim.step();
    }
    const Unit* shoved = sim.find(ids[1]);
    const std::int64_t dx = shoved->x.raw - start.x.raw;
    const std::int64_t dy = shoved->y.raw - start.y.raw;
    EXPECT_GT((dx * dx) + (dy * dy), static_cast<std::int64_t>(Fixed::kOne) * Fixed::kOne)
        << "the shoved vehicle should have been carried more than a tile";
    EXPECT_GT(sim.find(ids[0])->x.raw, start.x.raw) << "and the pusher is past where it stood";
}

TEST(Shove, ShovingCostsThePusherAThirdOfItsSpeed) {
    // A fact about the world, with its numbers written out and none read from the code under test:
    // a tank drives 0.2 tiles a tick, so in ten ticks it covers two tiles when nothing is in its
    // way, and on a tick that it spends pushing another it covers two thirds of that, 2/15 of a
    // tile (Harri's ruling). Contact is not unbroken -- the shoved vehicle is carried sideways and
    // can slip free for a tick -- so only the ticks on which `shove_ticks` rose are counted, and
    // the pusher's advance over them is compared to the literal. The slack is a hundredth of a
    // tile, far narrower than the gap to a half (0.2 tile over twelve ticks) or three quarters.
    ASSERT_TRUE(open_row());
    Sim free_run = Sim::create(scenario_of({{105, kRowY}}));
    Sim shoving = Sim::create(scenario_of({{105, kRowY}, {106, kRowY}}));
    const auto free_ids = player_ids(free_run);
    const auto shove_ids = player_ids(shoving);
    go(free_run, free_ids[0], 130, kRowY);
    go(shoving, shove_ids[0], 130, kRowY);
    free_run.step(); // the first tick: the pusher closes the gap and touches
    shoving.step();
    ASSERT_EQ(shoving.find(shove_ids[0])->shove_ticks, 0) << "not yet in contact";
    const std::int64_t free_from = free_run.find(free_ids[0])->x.raw;
    for (int tick = 0; tick < 10; ++tick) {
        free_run.step();
    }
    int shoving_ticks = 0;
    std::int64_t shove_advance = 0;
    for (int tick = 0; tick < 30; ++tick) {
        const Unit before = *shoving.find(shove_ids[0]);
        shoving.step();
        const Unit* after = shoving.find(shove_ids[0]);
        if (after->shove_ticks > before.shove_ticks) {
            ++shoving_ticks;
            shove_advance += after->x.raw - before.x.raw;
        }
    }
    ASSERT_GE(shoving_ticks, 12) << "enough shoving ticks to measure";
    EXPECT_GT(shoving.find(shove_ids[1])->x.raw, 106 * Fixed::kOne) << "and the other was carried";

    const std::int64_t free_advance = free_run.find(free_ids[0])->x.raw - free_from;
    const double slack = Fixed::kOne / 100.0; // rounding, not a rule
    EXPECT_NEAR(static_cast<double>(free_advance), 2.0 * Fixed::kOne, slack)
        << "unobstructed, a tank covers two tiles in ten ticks";
    EXPECT_NEAR(static_cast<double>(shove_advance), shoving_ticks * 2.0 * Fixed::kOne / 15.0, slack)
        << "shoving, it covers 2/15 of a tile a tick: two thirds of its speed";
}

TEST(Shove, ABystanderThatIsNotInTheWayIsUntouched) {
    ASSERT_TRUE(open_row());
    Sim sim = Sim::create(scenario_of({{105, kRowY}, {108, kRowY + 3}}));
    const auto ids = player_ids(sim);
    const Fixed bx = sim.find(ids[1])->x;
    const Fixed by = sim.find(ids[1])->y;
    go(sim, ids[0], 130, kRowY);
    for (int tick = 0; tick < 200; ++tick) {
        sim.step();
    }
    EXPECT_EQ(sim.find(ids[1])->x.raw, bx.raw);
    EXPECT_EQ(sim.find(ids[1])->y.raw, by.raw);
}

TEST(Shove, TwoVehiclesDrivenHeadOnPassEachOther) {
    ASSERT_TRUE(open_row());
    for (int offset = -1; offset <= 1; ++offset) {
        Sim sim = Sim::create(scenario_of({{105, kRowY}, {125, kRowY + offset}}));
        const auto ids = player_ids(sim);
        go(sim, ids[0], 125, kRowY);
        go(sim, ids[1], 105, kRowY);
        for (int tick = 0; tick < 900; ++tick) {
            sim.step();
        }
        const Unit* a = sim.find(ids[0]);
        const Unit* b = sim.find(ids[1]);
        EXPECT_GT(a->x.raw, 123 * Fixed::kOne) << "offset " << offset << ": A never got across";
        EXPECT_LT(b->x.raw, 107 * Fixed::kOne) << "offset " << offset << ": B never got across";
    }
}

TEST(Shove, AVehiclePinnedInTheMapCornerDoesNotHoldItsPusherForever) {
    // B sits in the corner of the map: it can be shoved nowhere. A drives into it. A shove that
    // became a wall here would be a permanent trap; the pusher must get through.
    Sim sim = Sim::create(scenario_of({{4, 4}, {0, 0}}));
    const auto ids = player_ids(sim);
    go(sim, ids[0], 0, 0);
    for (int tick = 0; tick < 600; ++tick) {
        sim.step();
    }
    const Unit* a = sim.find(ids[0]);
    EXPECT_LT(a->x.raw, 2 * Fixed::kOne);
    EXPECT_LT(a->y.raw, 2 * Fixed::kOne);
}

TEST(Shove, NoCrowdOfVehiclesCanHoldAnyOfThemForever) {
    // A block of six, each ordered to the spot another one occupies: every one of them must
    // arrive. This is the property the design exists for; the sim has no pathfinding to rescue it.
    ASSERT_TRUE(open_row());
    const std::vector<std::pair<int, int>> tiles{{105, kRowY},     {106, kRowY},
                                                 {107, kRowY},     {105, kRowY + 1},
                                                 {106, kRowY + 1}, {107, kRowY + 1}};
    Sim sim = Sim::create(scenario_of(tiles));
    const auto ids = player_ids(sim);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& goal = tiles[ids.size() - 1 - i];
        go(sim, ids[i], goal.first + 20, goal.second);
    }
    for (int tick = 0; tick < 1500; ++tick) {
        sim.step();
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& goal = tiles[ids.size() - 1 - i];
        const Unit* unit = sim.find(ids[i]);
        EXPECT_NEAR(unit->x.raw, (goal.first + 20) * Fixed::kOne, 3 * Fixed::kOne) << "unit " << i;
        EXPECT_NEAR(unit->y.raw, goal.second * Fixed::kOne, 3 * Fixed::kOne) << "unit " << i;
    }
}

TEST(Shove, ADenseBlockSentToShuffledGoalsLeavesNobodyStuck) {
    // Twenty-four vehicles packed a tile apart, each sent to the post of another, by a fixed
    // shuffle. Crossing streams in a crush is the nearest thing to a trap a battle makes.
    ASSERT_TRUE(open_row());
    std::vector<std::pair<int, int>> tiles;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 6; ++col) {
            tiles.emplace_back(105 + col, kRowY - 2 + row);
        }
    }
    Sim sim = Sim::create(scenario_of(tiles));
    const auto ids = player_ids(sim);
    ASSERT_EQ(ids.size(), tiles.size());
    constexpr std::size_t kStride = 7; // coprime with 24, so the map is a permutation
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& goal = tiles[(i * kStride + 5) % ids.size()];
        go(sim, ids[i], goal.first + 12, goal.second);
    }
    for (int tick = 0; tick < 3000; ++tick) {
        sim.step();
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& goal = tiles[(i * kStride + 5) % ids.size()];
        const Unit* unit = sim.find(ids[i]);
        EXPECT_NEAR(unit->x.raw, (goal.first + 12) * Fixed::kOne, 3 * Fixed::kOne) << "unit " << i;
        EXPECT_NEAR(unit->y.raw, goal.second * Fixed::kOne, 3 * Fixed::kOne) << "unit " << i;
    }
}

} // namespace protekt::sim
