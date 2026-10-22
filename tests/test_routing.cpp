#include "protekt/sim/route.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

/// Route planning: a deterministic A* over the same tiles the mover can enter, handed to the unit
/// as tile-center waypoints. The failure it exists for was silent -- a follower that stopped
/// dead, order and target both alive, 14 tiles from the unit it was meant to shadow.
namespace protekt::sim {
namespace {

/// A drawn map: '#' cannot be entered, anything else can. Anything off the drawing is open.
struct DrawnGround {
    std::vector<std::string> rows;
    [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
        if (y < 0 || y >= static_cast<int>(rows.size()) || x < 0
            || x >= static_cast<int>(rows[static_cast<std::size_t>(y)].size())) {
            return Fixed::kOne;
        }
        return rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == '#' ? 0
                                                                                     : Fixed::kOne;
    }
};

/// The follow probe's world: a home factory, a leader and a follower, and an unarmed enemy factory
/// across the map for the leader to drive at.
[[nodiscard]] Scenario probe_scenario(std::uint64_t seed) {
    Scenario scenario{};
    scenario.seed = seed;
    const auto add = [&](Side side, UnitKind kind, int x, int y) {
        Spawn spawn{};
        spawn.side = side;
        spawn.kind = kind;
        spawn.tile_x = x;
        spawn.tile_y = y;
        scenario.spawns.push_back(spawn);
    };
    add(Side::Player, UnitKind::Factory, 10, 90);
    add(Side::Player, UnitKind::Tank, 12, 90);
    add(Side::Player, UnitKind::Tank, 14, 90);
    add(Side::Enemy, UnitKind::Factory, 140, 90);
    return scenario;
}

constexpr int kSettleTicks = 200;

struct Trip {
    double worst_gap{};
    int final_gap{};
    int plans{};
    bool both_alive{true};
};

/// The probe, ported: the leader is re-ordered onto the far factory every 90 ticks like a driver
/// steering, the follower is ordered to Follow it, and the gap is watched on every tick.
[[nodiscard]] Trip run_probe(std::uint64_t seed, int ticks) {
    Sim sim = Sim::create(probe_scenario(seed));
    const UnitId leader{2};
    const UnitId follower{3};
    const UnitId goal{4};
    sim.enqueue(Order{follower, orders::Follow{leader}});
    Trip trip{};
    for (int tick = 0; tick < ticks; ++tick) {
        if (tick % 90 == 0) {
            sim.enqueue(Order{leader, orders::MoveTo{sim.find(goal)->x, sim.find(goal)->y}});
        }
        sim.step();
        const Unit* a = sim.find(follower);
        const Unit* b = sim.find(leader);
        if (!a->alive || !b->alive) {
            trip.both_alive = false;
            break;
        }
        if (tick < kSettleTicks) {
            continue; // the leader's spawn may be pushed off a crater tile; give the follower time
        }
        const double gap = std::hypot((static_cast<double>(a->x.raw) / Fixed::kOne)
                                          - (static_cast<double>(b->x.raw) / Fixed::kOne),
                                      (static_cast<double>(a->y.raw) / Fixed::kOne)
                                          - (static_cast<double>(b->y.raw) / Fixed::kOne));
        trip.worst_gap = std::max(trip.worst_gap, gap);
        trip.final_gap = static_cast<int>(gap);
    }
    trip.plans = static_cast<int>(sim.route_plans());
    return trip;
}

} // namespace

TEST(Routing, ThePinnedFollowWedgeOnSeedEightIsRoutedAround) {
    // The probe pinned the follower frozen at (22,94) on this seed, 14 tiles from its leader.
    EXPECT_EQ(world::sample(8, 22, 94).terrain != world::Terrain::Crater, true)
        << "the probe's freeze tile is ground, not a crater: the trap is the geometry around it";
    const Trip trip = run_probe(8, 5000);
    EXPECT_TRUE(trip.both_alive);
    EXPECT_LE(trip.worst_gap, 8.0) << "the follower must shadow the leader the whole trip";
}

TEST(Routing, ThePinnedFollowWedgeOnSeedTenIsRoutedAround) {
    EXPECT_NE(world::sample(10, 124, 87).terrain, world::Terrain::Crater);
    const Trip trip = run_probe(10, 5000);
    EXPECT_TRUE(trip.both_alive);
    EXPECT_LE(trip.worst_gap, 8.0) << "the follower must shadow the leader the whole trip";
}

TEST(Routing, ARouteGoesRoundACraterBandThroughItsGap) {
    // A band across the way with one gap, the column at x=10. The straight line from (2,2) to
    // (2,16) is blocked; the route must reach the goal, pass through the gap, and visit its corners
    // in order.
    DrawnGround ground{};
    for (int y = 0; y < 20; ++y) {
        const bool band = y >= 6 && y <= 12;
        ground.rows.push_back(band ? "##########.#" : "............");
    }
    Route route{};
    ASSERT_TRUE(plan_route(ground, TileXY{2, 2}, TileXY{2, 16}, route));
    ASSERT_GT(route.count, 0);
    EXPECT_EQ(route.points.at(route.count - 1), (TileXY{2, 16})) << "the goal is the last waypoint";
    bool through_gap = false;
    for (int i = 0; i < route.count; ++i) {
        const TileXY p = route.points.at(static_cast<std::size_t>(i));
        EXPECT_NE(ground.factor(p.x, p.y), 0) << "waypoint " << i << " is enterable";
        if (p.x == 10 && p.y >= 5 && p.y <= 13) {
            through_gap = true;
        }
    }
    EXPECT_TRUE(through_gap) << "the only way across is the gap column";
}

TEST(Routing, AUnitInTheSimPopsItsWaypointsInOrder) {
    // The leader's trip on seed 8 is long and turns. While the plan is unchanged, `next` only
    // ever goes up, and it ends past at least one waypoint.
    Sim sim = Sim::create(probe_scenario(8));
    const UnitId leader{2};
    const Unit* goal = sim.find(UnitId{4});
    sim.enqueue(Order{leader, orders::MoveTo{goal->x, goal->y}});
    int last_next = 0;
    int popped = 0;
    TileXY last_goal{};
    for (int tick = 0; tick < 3000; ++tick) {
        sim.step();
        const Route& route = sim.find(leader)->route;
        if (route.has_goal && route.goal == last_goal && route.count > 0) {
            EXPECT_GE(route.next, last_next) << "tick " << tick;
            popped = std::max<int>(popped, route.next);
        }
        last_goal = route.goal;
        last_next = route.next;
    }
    EXPECT_GT(popped, 0) << "a long trip pops waypoints";
}

TEST(Routing, TheSameSeedPlansTheSameRoute) {
    const auto digest = [](std::uint64_t seed) {
        Sim sim = Sim::create(probe_scenario(seed));
        sim.enqueue(Order{UnitId{3}, orders::Follow{UnitId{2}}});
        sim.enqueue(
            Order{UnitId{2}, orders::MoveTo{sim.find(UnitId{4})->x, sim.find(UnitId{4})->y}});
        std::uint64_t acc = 1469598103934665603ULL;
        for (int tick = 0; tick < 1500; ++tick) {
            sim.step();
            for (const UnitId id : {UnitId{2}, UnitId{3}}) {
                const Route& r = sim.find(id)->route;
                for (int i = 0; i < r.count; ++i) {
                    acc = (acc ^ r.points.at(static_cast<std::size_t>(i)).x) * 1099511628211ULL;
                    acc = (acc ^ r.points.at(static_cast<std::size_t>(i)).y) * 1099511628211ULL;
                }
            }
        }
        return std::make_pair(acc, sim.state_hash());
    };
    EXPECT_EQ(digest(8), digest(8));
    EXPECT_NE(digest(8).second, digest(10).second);
}

TEST(Routing, AnEnclosedGoalHasNoRoute) {
    DrawnGround ground{};
    for (int y = 0; y < 9; ++y) {
        std::string row(9, '.');
        if (y >= 3 && y <= 5) {
            row = "...###...";
        }
        if (y == 4) {
            row = "...#.#...";
        }
        ground.rows.push_back(row);
    }
    Route route{};
    EXPECT_FALSE(plan_route(ground, TileXY{0, 0}, TileXY{4, 4}, route));
    EXPECT_EQ(route.count, 0);
    EXPECT_TRUE(route.has_goal)
        << "the failed attempt is remembered, so it is not retried per tick";
}

TEST(Routing, AnUnreachableGoalFallsBackToSteeringAtItAndStillMoves) {
    // A goal on a crater tile can never be planned to. The unit must degrade to steer-at-goal --
    // moving, not freezing -- and its route must say "tried, none".
    int goal_x = -1;
    int goal_y = -1;
    for (int y = 90; y < 140 && goal_x < 0; ++y) {
        for (int x = 40; x < 140; ++x) {
            if (world::sample(42, x, y).terrain == world::Terrain::Crater) {
                goal_x = x;
                goal_y = y;
                break;
            }
        }
    }
    ASSERT_GE(goal_x, 0) << "seed 42 has craters in this window";
    Sim sim = Sim::create(probe_scenario(42));
    const UnitId tank{2};
    const Fixed start_x = sim.find(tank)->x;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(goal_x), Fixed::from_int(goal_y)}});
    for (int tick = 0; tick < 300; ++tick) {
        sim.step();
    }
    const Unit* unit = sim.find(tank);
    EXPECT_TRUE(unit->route.has_goal) << "it tried to plan";
    EXPECT_EQ(unit->route.count, 0) << "and reports that there is no route";
    EXPECT_GT(std::abs((static_cast<double>(unit->x.raw) / Fixed::kOne)
                       - (static_cast<double>(start_x.raw) / Fixed::kOne)),
              5.0)
        << "but it still moves";
}

TEST(Routing, PlanningIsBoundedNotPerTick) {
    const Trip trip = run_probe(8, 2400);
    EXPECT_LT(trip.plans, 2400 / 10) << "plans over a 2400-tick trip: " << trip.plans;
}

} // namespace protekt::sim
