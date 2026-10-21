#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

/// The Follow verb: a standing order to keep within kFollowRadiusTiles of ANOTHER unit's current
/// position. It closes when outside, holds when inside, fires at what is in range on the way and
/// while holding, and never leaves the radius to chase anything.
namespace protekt::sim {
namespace {

struct Spot {
    Side side;
    UnitKind kind;
    int x;
    int y;
};

struct Field {
    Sim sim;
    std::vector<UnitId> ids; ///< in spawn order
};

[[nodiscard]] Field field(const std::vector<Spot>& spots) {
    Scenario scenario{};
    scenario.seed = 42;
    for (const Spot& s : spots) {
        Spawn spawn{};
        spawn.side = s.side;
        spawn.kind = s.kind;
        spawn.tile_x = s.x;
        spawn.tile_y = s.y;
        scenario.spawns.push_back(spawn);
    }
    Field out{};
    out.sim = Sim::create(scenario);
    for (const Unit& unit : out.sim.units()) {
        out.ids.push_back(unit.id);
    }
    return out;
}

void run(Sim& sim, int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
    }
}

/// Tiles between two units, as a double: test arithmetic only, the sim never sees it.
double tiles_between(const Sim& sim, UnitId a, UnitId b) {
    const Unit* ua = sim.find(a);
    const Unit* ub = sim.find(b);
    const double dx = (ub->x.raw - ua->x.raw) / 65536.0;
    const double dy = (ub->y.raw - ua->y.raw) / 65536.0;
    return std::sqrt(dx * dx + dy * dy);
}

Fixed tile(int v) { return Fixed::from_int(v); }

constexpr double kRadius = static_cast<double>(kFollowRadiusTiles);

TEST(Follow, ClosesFromOutsideTheRadiusAndHoldsInside) {
    Field f = field({{Side::Player, UnitKind::Tank, 100, 100},   // follower
                     {Side::Player, UnitKind::Tank, 112, 100}}); // leader, idle
    const UnitId me = f.ids[0];
    const UnitId lead = f.ids[1];
    f.sim.enqueue(Order{me, orders::Follow{lead}});
    f.sim.step();
    EXPECT_EQ(f.sim.find(me)->stance, Stance::Follow);
    EXPECT_EQ(f.sim.find(me)->target, lead);

    run(f.sim, 9);
    EXPECT_GT(tiles_between(f.sim, me, lead), kRadius) << "still closing at tick 10";
    EXPECT_GT(f.sim.find(me)->x.raw, tile(100).raw) << "and it is moving towards the leader";

    run(f.sim, 90);
    const double held = tiles_between(f.sim, me, lead);
    EXPECT_LT(held, kRadius) << "inside the radius by tick 100";
    EXPECT_GT(held, kRadius - 1.0) << "it stops at the edge, it does not run up to the leader";

    const Fixed hold_x = f.sim.find(me)->x;
    const Fixed hold_y = f.sim.find(me)->y;
    run(f.sim, 60);
    EXPECT_EQ(f.sim.find(me)->x, hold_x) << "holding means standing still, not orbiting";
    EXPECT_EQ(f.sim.find(me)->y, hold_y);
}

TEST(Follow, ShadowsALeaderThatMovesOff) {
    Field f = field(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Player, UnitKind::Tank, 103, 100}});
    const UnitId me = f.ids[0];
    const UnitId lead = f.ids[1];
    f.sim.enqueue(Order{me, orders::Follow{lead}});
    f.sim.enqueue(Order{lead, orders::MoveTo{tile(140), tile(100)}});
    run(f.sim, 300);
    EXPECT_GT(f.sim.find(lead)->x.raw, tile(130).raw) << "the leader got there";
    EXPECT_LT(tiles_between(f.sim, me, lead), kRadius) << "and the follower kept up";
    EXPECT_GT(f.sim.find(me)->x.raw, tile(120).raw);
}

TEST(Follow, ShootsAHostileInRangeWhileHoldingAndStaysPut) {
    Field f = field({{Side::Player, UnitKind::Tank, 100, 100},
                     {Side::Player, UnitKind::Tank, 103, 100},
                     {Side::Enemy, UnitKind::Factory, 100, 108}}); // 8 tiles: in range
    const UnitId me = f.ids[0];
    f.sim.enqueue(Order{me, orders::Follow{f.ids[1]}});
    const Fixed x0 = f.sim.find(me)->x;
    const Fixed y0 = f.sim.find(me)->y;
    run(f.sim, 120);
    EXPECT_LT(f.sim.find(f.ids[2])->hp, kFactoryHp) << "the follower did not fire";
    EXPECT_EQ(f.sim.find(me)->x, x0) << "it was already inside the radius: nothing to do but shoot";
    EXPECT_EQ(f.sim.find(me)->y, y0);
}

TEST(Follow, FiresWhileClosing) {
    Field f = field({{Side::Player, UnitKind::Tank, 100, 100},
                     {Side::Player, UnitKind::Tank, 118, 100},
                     {Side::Enemy, UnitKind::Factory, 102, 104}});
    f.sim.enqueue(Order{f.ids[0], orders::Follow{f.ids[1]}});
    int shots = 0;
    for (int tick = 0; tick < 60; ++tick) {
        f.sim.step();
        for (const Sim::Shot& shot : f.sim.shots()) {
            shots += shot.shooter == f.ids[0] && shot.target == f.ids[2] ? 1 : 0;
        }
    }
    EXPECT_GT(tiles_between(f.sim, f.ids[0], f.ids[1]), kRadius) << "still on the way";
    EXPECT_GE(shots, 2) << "moving did not stop the gun";
}

TEST(Follow, NeverPursuesAHostileOutOfRange) {
    Field f = field({{Side::Player, UnitKind::Tank, 100, 100},
                     {Side::Player, UnitKind::Tank, 103, 100},
                     {Side::Enemy, UnitKind::Factory, 100, 130}}); // 30 tiles: out of range
    const UnitId me = f.ids[0];
    f.sim.enqueue(Order{me, orders::Follow{f.ids[1]}});
    const Fixed x0 = f.sim.find(me)->x;
    const Fixed y0 = f.sim.find(me)->y;
    run(f.sim, 200);
    EXPECT_EQ(f.sim.find(me)->x, x0);
    EXPECT_EQ(f.sim.find(me)->y, y0) << "it walked off to chase something";
    EXPECT_EQ(f.sim.find(f.ids[2])->hp, kFactoryHp);
}

TEST(Follow, OfADeadOrInvalidUnitIsDropped) {
    // The victim stands 14 tiles from a turret and is shot dead; the follower starts far enough
    // west to be out of the turret's reach.
    Field f = field({{Side::Player, UnitKind::Tank, 60, 100},
                     {Side::Player, UnitKind::Tank, 100, 100},
                     {Side::Enemy, UnitKind::Turret, 100, 114}});
    const UnitId me = f.ids[0];
    const UnitId victim = f.ids[1];
    for (int i = 0; i < 600 && f.sim.find(victim)->alive; ++i) {
        f.sim.step();
    }
    ASSERT_FALSE(f.sim.find(victim)->alive);

    const std::size_t logged = f.sim.command_log().size();
    f.sim.enqueue(Order{me, orders::Follow{victim}});
    f.sim.enqueue(Order{me, orders::Follow{UnitId{}}});
    f.sim.enqueue(Order{me, orders::Follow{UnitId{999}}});
    f.sim.enqueue(Order{me, orders::Follow{me}});
    EXPECT_EQ(f.sim.command_log().size(), logged) << "an invalid follow must not be accepted";
    f.sim.step();
    EXPECT_EQ(f.sim.find(me)->stance, Stance::Idle);
}

TEST(Follow, AnyLivingUnitOnEitherSideCanBeFollowed) {
    Field f = field(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 130, 100}});
    f.sim.enqueue(Order{f.ids[0], orders::Follow{f.ids[1]}});
    f.sim.step();
    EXPECT_EQ(f.sim.find(f.ids[0])->stance, Stance::Follow);
}

TEST(Follow, TargetDyingMakesTheOrderMootAndTheFollowerDefendsInPlace) {
    Field f = field({{Side::Player, UnitKind::Tank, 90, 100},
                     {Side::Player, UnitKind::Tank, 100, 100},
                     {Side::Enemy, UnitKind::Turret, 100, 114}});
    const UnitId me = f.ids[0];
    const UnitId victim = f.ids[1];
    f.sim.enqueue(Order{me, orders::Follow{victim}});
    for (int i = 0; i < 600 && f.sim.find(victim)->alive; ++i) {
        f.sim.step();
    }
    ASSERT_FALSE(f.sim.find(victim)->alive);
    f.sim.step();
    EXPECT_EQ(f.sim.find(me)->stance, Stance::Idle) << "the order outlived its target";
    EXPECT_FALSE(f.sim.find(me)->target.valid());
    const Fixed x = f.sim.find(me)->x;
    run(f.sim, 5);
    EXPECT_EQ(f.sim.find(me)->x, x) << "with nobody to follow it stands and defends";
}

TEST(Follow, ANewStandingOrderReplacesIt) {
    for (int which = 0; which < 5; ++which) {
        Field f = field(
            {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Player, UnitKind::Tank, 130, 100}});
        const UnitId me = f.ids[0];
        f.sim.enqueue(Order{me, orders::Follow{f.ids[1]}});
        f.sim.step();
        ASSERT_EQ(f.sim.find(me)->stance, Stance::Follow);
        switch (which) {
        case 0:
            f.sim.enqueue(Order{me, orders::GuardAt{tile(100), tile(100)}});
            break;
        case 1:
            f.sim.enqueue(Order{me, orders::MoveTo{tile(100), tile(100)}});
            break;
        case 2:
            f.sim.enqueue(Order{me, orders::Patrol{tile(100), tile(100), tile(100), tile(95)}});
            break;
        case 3:
            f.sim.enqueue(Order{me, orders::Attack{f.ids[1]}});
            break;
        default:
            f.sim.enqueue(Order{me, orders::Stop{}});
            break;
        }
        f.sim.step();
        EXPECT_NE(f.sim.find(me)->stance, Stance::Follow) << "replacement #" << which;
    }
    // And the replaced follow really stops: a guard at home keeps the unit home while the leader
    // is 30 tiles off.
    Field f = field(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Player, UnitKind::Tank, 130, 100}});
    f.sim.enqueue(Order{f.ids[0], orders::Follow{f.ids[1]}});
    run(f.sim, 10);
    f.sim.enqueue(Order{f.ids[0], orders::GuardAt{tile(100), tile(100)}});
    run(f.sim, 200);
    EXPECT_LT(f.sim.find(f.ids[0])->x.raw, tile(102).raw);
    EXPECT_FALSE(f.sim.find(f.ids[0])->target.valid());
}

TEST(Follow, FollowIsBoundToTheUnitNotToAnyDriver) {
    // The leader changes hands (an X-swap is a change of who steers, not of who the unit is): the
    // order names an id and nothing else, so a leader that is merely ordered elsewhere is still
    // the one followed.
    Field f = field(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Player, UnitKind::Tank, 103, 100}});
    f.sim.enqueue(Order{f.ids[0], orders::Follow{f.ids[1]}});
    f.sim.step();
    f.sim.enqueue(Order{f.ids[1], orders::Stop{}});
    f.sim.enqueue(Order{f.ids[1], orders::MoveTo{tile(120), tile(100)}});
    run(f.sim, 200);
    EXPECT_EQ(f.sim.find(f.ids[0])->stance, Stance::Follow);
    EXPECT_LT(tiles_between(f.sim, f.ids[0], f.ids[1]), kRadius);
}

TEST(Follow, TwoRunsWithTheSameSeedAreByteIdentical) {
    const auto run_once = [] {
        Field f = field({{Side::Player, UnitKind::Tank, 100, 100},
                         {Side::Player, UnitKind::Tank, 112, 100},
                         {Side::Enemy, UnitKind::Tank, 100, 108},
                         {Side::Enemy, UnitKind::Factory, 120, 108}});
        f.sim.enqueue(Order{f.ids[0], orders::Follow{f.ids[1]}});
        f.sim.enqueue(Order{f.ids[1], orders::MoveTo{tile(140), tile(100)}});
        std::vector<std::uint64_t> hashes;
        for (int i = 0; i < 400; ++i) {
            f.sim.step();
            hashes.push_back(f.sim.state_hash());
        }
        return hashes;
    };
    EXPECT_EQ(run_once(), run_once());
}

} // namespace
} // namespace protekt::sim
