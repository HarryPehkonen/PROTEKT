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
    // Round 19, L2: the factory is 8 x 8 (10..17), so the tanks stand two tiles clear of it at 19
    // and 21; they stood at 12 and 14, two and four tiles from the point.
    add(Side::Player, UnitKind::Factory, 10, 90);
    add(Side::Player, UnitKind::Tank, 19, 90);
    add(Side::Player, UnitKind::Tank, 21, 90);
    add(Side::Enemy, UnitKind::Factory, 140, 90);
    return scenario;
}

constexpr int kSettleTicks = 200;

struct Trip {
    double worst_gap{};
    int final_gap{};
    int longest_stand_behind{}; ///< most consecutive ticks the follower did not move, 8+ tiles back
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
    int stand = 0;
    for (int tick = 0; tick < ticks; ++tick) {
        const Fixed was_x = sim.find(follower)->x;
        const Fixed was_y = sim.find(follower)->y;
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
        const bool still = a->x.raw == was_x.raw && a->y.raw == was_y.raw;
        stand = still && gap > 8.0 ? stand + 1 : 0;
        trip.longest_stand_behind = std::max(trip.longest_stand_behind, stand);
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
    // OLD bound 8.0 (measured, with room). M1: the leader is faster downhill (up to 3/2 of a tank's
    // step) and the follower, capped at its own speed, falls back on the descents: the worst gap is
    // 9.46 now. The wedge this test is about FROZE the follower 14 tiles off, so 12 still says
    // "shadowed", and the end of the trip says it came back (below).
    // RESTATED, G6 (the press): the slope is one force now, from a 60 percent steepest grade, and
    // on the 10-to-25 slopes of this trip it is about twice what the old grade was (at 20 a tile
    // the old grade took 31% of a step; the press takes 58%) -- so while the leader runs down a
    // hill the follower, a few tiles behind, is still climbing it, and the gap opens wider before
    // it closes: 12.27 here (worst on tick 428), measured. The gap alone no longer separates a
    // follower that lags from one that FROZE, so the freeze is now measured as what it is: the
    // follower standing still, well behind, tick after tick (the wedge held it for good). It
    // never stands for more than a few ticks, and the bound on the gap is the measured worst with
    // room.
    EXPECT_LE(trip.worst_gap, 16.0) << "the follower must shadow the leader the whole trip";
    EXPECT_LE(trip.longest_stand_behind, 20) << "the follower froze behind its leader";
    EXPECT_LE(trip.final_gap, 8) << "and it was alongside again at the end, not stuck behind";
}

TEST(Routing, ThePinnedFollowWedgeOnSeedTenIsRoutedAround) {
    EXPECT_NE(world::sample(10, 124, 87).terrain, world::Terrain::Crater);
    const Trip trip = run_probe(10, 5000);
    EXPECT_TRUE(trip.both_alive);
    // OLD bound 8.0; 10.87 under M1, for the reason given above. See the seed-eight test.
    // RESTATED, G6: 20.30 (worst on tick 1066, both tanks on the same row, the leader down a
    // descent while the follower climbs the one before it), measured; see the seed-eight test.
    EXPECT_LE(trip.worst_gap, 24.0) << "the follower must shadow the leader the whole trip";
    EXPECT_LE(trip.longest_stand_behind, 20) << "the follower froze behind its leader";
    EXPECT_LE(trip.final_gap, 8) << "and it was alongside again at the end, not stuck behind";
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
    // Round 19, L2: the goal is a factory, 8 x 8 now, whose centre is inside a reserved footprint
    // no route can end in; the leader is sent to the open ground six tiles west of it.
    sim.enqueue(
        Order{leader, orders::MoveTo{Fixed::from_raw(goal->x.raw - 6 * Fixed::kOne), goal->y}});
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

TEST(Routing, AGoalOnACraterIsApproachedAndTheDriveEndsBesideIt) {
    // RE-WRITTEN, G1 (go around). This test used to be "an unreachable goal falls back to steering
    // at it and still moves": a goal on a crater tile can never be planned to, so the unit
    // steered at it in a straight line and pressed on the rim for ever -- which is the silent
    // stall, pinned as a feature. The route now ends on the nearest ground the unit CAN stand on,
    // and a unit going to a fixed spot has arrived when it has driven to the end of it.
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
    for (int tick = 0; tick < 1500; ++tick) {
        sim.step();
    }
    const Unit* unit = sim.find(tank);
    EXPECT_TRUE(unit->route.has_goal) << "it tried to plan";
    EXPECT_EQ(unit->route.failure, RouteFailure::None) << "and the plan found ground beside it";
    EXPECT_GT(unit->route.count, 0);
    EXPECT_TRUE(unit->route.ring_goal) << "the route says the goal itself cannot be entered";
    EXPECT_GT(std::abs((static_cast<double>(unit->x.raw) / Fixed::kOne)
                       - (static_cast<double>(start_x.raw) / Fixed::kOne)),
              5.0)
        << "it moved";
    const double gap = std::hypot((static_cast<double>(unit->x.raw) / Fixed::kOne) - goal_x,
                                  (static_cast<double>(unit->y.raw) / Fixed::kOne) - goal_y);
    EXPECT_LT(gap, 4.0) << "and it came to the crater's rim: " << gap << " tiles off";
    EXPECT_EQ(unit->vx.raw, 0) << "and it is standing there, not pressing on the rim";
    EXPECT_EQ(unit->vy.raw, 0);
}

TEST(Routing, PlanningIsBoundedNotPerTick) {
    const Trip trip = run_probe(8, 2400);
    EXPECT_LT(trip.plans, 2400 / 10) << "plans over a 2400-tick trip: " << trip.plans;
}

} // namespace protekt::sim
