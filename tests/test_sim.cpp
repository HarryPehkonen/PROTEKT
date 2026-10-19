// The simulation's contract. Everything asserted here is a property of the SPEC (sim.hpp and
// the balance constants in units.hpp) -- never of whatever the implementation chose.
#include <gtest/gtest.h>

#include <vector>

#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace {

using namespace protekt::sim;
namespace world = protekt::world;

Scenario two_tanks(std::uint64_t seed) {
    Scenario s;
    s.seed = seed;
    s.spawns = {{UnitKind::Tank, Side::Player, 20, 20}, {UnitKind::Tank, Side::Enemy, 200, 200}};
    return s;
}

UnitId id_of(const Sim& sim, Side side, int index) {
    int seen = 0;
    for (const Unit& u : sim.units()) {
        if (u.side == side) {
            if (seen == index) {
                return u.id;
            }
            ++seen;
        }
    }
    return UnitId{};
}

const Unit* by_side(const Sim& sim, Side side, int index) {
    const UnitId id = id_of(sim, side, index);
    return sim.find(id);
}

/// A stretch of passable ground to drive on. Scans the map rather than assuming one, because
/// where the flat land is depends on the seed.
bool find_flat_run(std::uint64_t seed, int length, int* out_x, int* out_y) {
    for (int y = 4; y < kMapTiles - 4; ++y) {
        int run = 0;
        for (int x = 4; x < kMapTiles - 4; ++x) {
            const bool flat = world::sample(seed, x, y).terrain == world::Terrain::Flat;
            run = flat ? run + 1 : 0;
            if (run >= length) {
                *out_x = x - length + 1;
                *out_y = y;
                return true;
            }
        }
    }
    return false;
}

TEST(Sim, ProfilesMatchTheFrozenBalance) {
    EXPECT_EQ(profile(UnitKind::Tank).hp, kTankHp);
    EXPECT_EQ(profile(UnitKind::Tank).damage, kTankDamage);
    EXPECT_EQ(profile(UnitKind::Tank).cooldown_ticks, kTankCooldownTicks);
    EXPECT_EQ(profile(UnitKind::Tank).max_speed.raw, kTankSpeedRaw);
    EXPECT_TRUE(profile(UnitKind::Tank).mobile);
    EXPECT_EQ(profile(UnitKind::Turret).hp, kTurretHp);
    EXPECT_FALSE(profile(UnitKind::Turret).mobile);
    EXPECT_EQ(profile(UnitKind::Factory).hp, kFactoryHp);
    EXPECT_EQ(profile(UnitKind::Factory).weapon_range.raw, 0) << "a factory has no gun";
}

TEST(Sim, ScenarioPlacesUnitsWithUniqueIds) {
    Sim sim = Sim::create(two_tanks(42));
    ASSERT_EQ(sim.units().size(), 2U);
    EXPECT_EQ(sim.units()[0].kind, UnitKind::Tank);
    EXPECT_EQ(sim.units()[0].x, Fixed::from_int(20));
    EXPECT_EQ(sim.units()[0].y, Fixed::from_int(20));
    EXPECT_EQ(sim.units()[0].hp, kTankHp);
    EXPECT_EQ(sim.units()[0].side, Side::Player);
    EXPECT_EQ(sim.units()[1].side, Side::Enemy);
    EXPECT_NE(sim.units()[0].id, sim.units()[1].id);
    EXPECT_TRUE(sim.units()[0].id.valid());
    EXPECT_EQ(sim.tick(), 0U);
}

TEST(Sim, IdenticalRunsHashIdentically) {
    Sim a = Sim::create(two_tanks(42));
    Sim b = Sim::create(two_tanks(42));
    EXPECT_EQ(a.state_hash(), b.state_hash());
    for (int i = 0; i < 300; ++i) {
        a.step();
        b.step();
    }
    EXPECT_EQ(a.state_hash(), b.state_hash()) << "the same scenario must replay identically";
}

TEST(Sim, DifferentSeedsDiverge) {
    Sim a = Sim::create(two_tanks(42));
    Sim b = Sim::create(two_tanks(43));
    EXPECT_NE(a.state_hash(), b.state_hash());
}

TEST(Sim, NoOrdersMeansNoMovement) {
    Sim sim = Sim::create(two_tanks(42));
    for (int i = 0; i < 300; ++i) {
        sim.step();
    }
    for (const Unit& u : sim.units()) {
        EXPECT_EQ(u.x, u.kind == UnitKind::Tank ? Fixed::from_int(u.side == Side::Player ? 20 : 200)
                                                : u.x);
        EXPECT_EQ(u.vx, Fixed::zero());
    }
    EXPECT_EQ(sim.tick(), 300U);
}

TEST(Sim, MoveToMovesTheUnitAndChangesTheHash) {
    Sim still = Sim::create(two_tanks(42));
    Sim moving = Sim::create(two_tanks(42));
    const Fixed before = moving.units()[0].x;
    moving.enqueue(
        Order{moving.units()[0].id, orders::MoveTo{Fixed::from_int(60), Fixed::from_int(20)}});
    EXPECT_NE(still.state_hash(), moving.state_hash()) << "a queued order changes the state";
    for (int i = 0; i < 30; ++i) {
        still.step();
        moving.step();
    }
    EXPECT_GT(moving.units()[0].x, before) << "the tank drove towards the order";
    EXPECT_LT(still.units()[0].x, moving.units()[0].x);
}

TEST(Sim, ACommandIsLoggedWithTheTickItTakesEffectOn) {
    Sim sim = Sim::create(two_tanks(42));
    const UnitId id = sim.units()[0].id;
    sim.enqueue(Order{id, orders::Stop{}});
    sim.step();
    sim.enqueue(Order{id, orders::Stop{}});
    ASSERT_EQ(sim.command_log().size(), 2U);
    EXPECT_EQ(sim.command_log()[0].tick, 0U);
    EXPECT_EQ(sim.command_log()[1].tick, 1U);
    EXPECT_EQ(sim.command_log()[0].order.unit, id);
}

TEST(Sim, GenuinelyInvalidOrdersAreDroppedWithoutEffect) {
    Sim clean = Sim::create(two_tanks(42));
    Sim polluted = Sim::create(two_tanks(42));
    const UnitId mine = polluted.units()[0].id;
    // No such unit.
    polluted.enqueue(Order{UnitId{9999}, orders::MoveTo{Fixed::from_int(60), Fixed::from_int(60)}});
    // No such target.
    polluted.enqueue(Order{mine, orders::Attack{UnitId{4242}}});
    for (int i = 0; i < 100; ++i) {
        clean.step();
        polluted.step();
    }
    EXPECT_EQ(clean.state_hash(), polluted.state_hash()) << "no order should have been accepted";
    EXPECT_TRUE(polluted.command_log().empty()) << "a dropped order is not a command";
}

TEST(Sim, AnOrderForTheOtherSidesUnitIsAccepted) {
    // The sim cannot know who ISSUED an order, so ownership is not its business to enforce: the
    // enemy commander's orders arrive through this same channel, and a check here silently kills
    // it.
    Sim with_order = Sim::create(two_tanks(42));
    Sim without = Sim::create(two_tanks(42));
    const UnitId theirs = with_order.units()[1].id;
    ASSERT_EQ(with_order.units()[1].side, Side::Enemy);
    with_order.enqueue(Order{theirs, orders::MoveTo{Fixed::from_int(100), Fixed::from_int(100)}});
    ASSERT_EQ(with_order.command_log().size(), 1U)
        << "a valid order for an enemy unit is a command";
    with_order.step();
    without.step();
    EXPECT_EQ(with_order.units()[1].stance, Stance::MoveTo) << "the order must have been applied";
    EXPECT_NE(with_order.state_hash(), without.state_hash());
}

TEST(Sim, UnitsStayInsideTheMapAndOutOfCraters) {
    constexpr std::uint64_t kSeed = 42;
    Scenario s;
    s.seed = kSeed;
    s.spawns = {{UnitKind::Tank, Side::Player, 10, 128},
                {UnitKind::Tank, Side::Player, 128, 10},
                {UnitKind::Tank, Side::Player, 128, 128}};
    Sim sim = Sim::create(s);
    std::vector<UnitId> ids;
    for (const Unit& u : sim.units()) {
        ids.push_back(u.id);
    }
    sim.enqueue(Order{ids[0], orders::MoveTo{Fixed::from_int(1000), Fixed::from_int(-40)}});
    sim.enqueue(Order{ids[1], orders::MoveTo{Fixed::from_int(-500), Fixed::from_int(900)}});
    sim.enqueue(Order{ids[2], orders::MoveTo{Fixed::from_int(255), Fixed::from_int(255)}});
    for (int t = 0; t < 600; ++t) {
        sim.step();
        if (t % 10 != 0) {
            continue;
        }
        for (const Unit& u : sim.units()) {
            EXPECT_GE(u.x.floor_int(), 0);
            EXPECT_GE(u.y.floor_int(), 0);
            EXPECT_LT(u.x.floor_int(), kMapTiles);
            EXPECT_LT(u.y.floor_int(), kMapTiles);
            const world::Terrain ground
                = world::sample(kSeed, u.x.floor_int(), u.y.floor_int()).terrain;
            EXPECT_NE(ground, world::Terrain::Crater) << "a unit drove into an impassable tile";
        }
    }
}

TEST(Sim, DrivesAcrossFlatGroundAndArrives) {
    constexpr std::uint64_t kSeed = 3;
    int x0 = 0;
    int y0 = 0;
    ASSERT_TRUE(find_flat_run(kSeed, 40, &x0, &y0)) << "seed 3 must offer 40 flat tiles in a row";
    Scenario s;
    s.seed = kSeed;
    s.spawns = {{UnitKind::Tank, Side::Player, x0 + 2, y0}};
    Sim sim = Sim::create(s);
    const Fixed goal = Fixed::from_int(x0 + 32);
    sim.enqueue(Order{sim.units()[0].id, orders::MoveTo{goal, Fixed::from_int(y0)}});
    for (int t = 0; t < 400; ++t) {
        sim.step();
    }
    const Unit& tank = sim.units()[0];
    EXPECT_LT(fixed_abs(fixed_sub(tank.x, goal)), Fixed::from_raw(kArriveRadiusRaw))
        << "never arrived";
    EXPECT_EQ(tank.stance, Stance::MoveTo);
}

TEST(Sim, GuardAtHoldsThePost) {
    constexpr std::uint64_t kSeed = 3;
    int x0 = 0;
    int y0 = 0;
    ASSERT_TRUE(find_flat_run(kSeed, 40, &x0, &y0));
    Scenario s;
    s.seed = kSeed;
    s.spawns = {{UnitKind::Tank, Side::Player, x0 + 2, y0}};
    Sim sim = Sim::create(s);
    const Fixed post = Fixed::from_int(x0 + 30);
    sim.enqueue(Order{sim.units()[0].id, orders::GuardAt{post, Fixed::from_int(y0)}});
    for (int t = 0; t < 400; ++t) {
        sim.step();
    }
    for (int t = 0; t < 200; ++t) {
        sim.step();
        EXPECT_LT(fixed_abs(fixed_sub(sim.units()[0].x, post)),
                  Fixed::from_raw(kArriveRadiusRaw * 3 / 2))
            << "a guard drifted off its post";
    }
}

TEST(Sim, TerrainSpeedFactorIsOrdered) {
    using world::Terrain;
    EXPECT_EQ(terrain_speed_factor(Terrain::Flat), Fixed::from_int(1));
    EXPECT_GT(terrain_speed_factor(Terrain::Rough), Fixed::zero());
    EXPECT_LT(terrain_speed_factor(Terrain::Rough), terrain_speed_factor(Terrain::Flat));
    EXPECT_LT(terrain_speed_factor(Terrain::Ridge), terrain_speed_factor(Terrain::Rough));
    EXPECT_EQ(terrain_speed_factor(Terrain::Crater), Fixed::zero()) << "craters are impassable";
}

TEST(Sim, ATurretFiresAtTheNearestHostileInRange) {
    Scenario s;
    s.seed = 5;
    s.spawns = {{UnitKind::Turret, Side::Enemy, 100, 100},
                {UnitKind::Tank, Side::Player, 110, 100},
                {UnitKind::Tank, Side::Player, 114, 100}};
    Sim sim = Sim::create(s);
    const Unit* near = by_side(sim, Side::Player, 0);
    const Unit* far = by_side(sim, Side::Player, 1);
    ASSERT_NE(near, nullptr);
    ASSERT_NE(far, nullptr);
    const UnitId near_id = near->id;
    const UnitId far_id = far->id;
    // The first bolt leaves at the end of the reload and takes about 19 ticks to cross ten tiles;
    // the second leaves a reload later and has not arrived.
    for (int t = 0; t < kTurretCooldownTicks + 25; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.find(near_id)->hp, kTankHp - kTurretDamage)
        << "the nearer tank should have been hit";
    EXPECT_EQ(sim.find(far_id)->hp, kTankHp) << "the farther tank is behind the nearest one";
}

TEST(Sim, AnUnarmedFactoryDoesNotShootBack) {
    // The first version of this test asserted the FACTORY's hp was untouched, which cannot hold:
    // the tank beside it is armed and shoots it. What is under test is that the factory has no
    // gun -- so the tank must come through unharmed while the factory takes fire.
    Scenario s;
    s.seed = 5;
    s.spawns
        = {{UnitKind::Factory, Side::Enemy, 100, 100}, {UnitKind::Tank, Side::Player, 104, 100}};
    Sim sim = Sim::create(s);
    const UnitId tank = by_side(sim, Side::Player, 0)->id;
    const UnitId factory = by_side(sim, Side::Enemy, 0)->id;
    for (int t = 0; t < 300; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.find(tank)->hp, kTankHp) << "the factory has no weapon to answer with";
    EXPECT_LT(sim.find(factory)->hp, kFactoryHp) << "the tank did fire, so the phase ran";
}

TEST(Sim, OutOfRangeUnitsDoNotFire) {
    Scenario s;
    s.seed = 5;
    s.spawns = {{UnitKind::Tank, Side::Player, 10, 10}, {UnitKind::Factory, Side::Enemy, 220, 220}};
    Sim sim = Sim::create(s);
    for (int t = 0; t < 400; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.find(by_side(sim, Side::Enemy, 0)->id)->hp, kFactoryHp)
        << "210 tiles is out of range";
}

TEST(Sim, ShotsDoDamageAndDeadUnitsStopBeingTargets) {
    Scenario s;
    s.seed = 5;
    s.spawns
        = {{UnitKind::Tank, Side::Player, 100, 100}, {UnitKind::Factory, Side::Enemy, 104, 100}};
    Sim sim = Sim::create(s);
    const UnitId factory = by_side(sim, Side::Enemy, 0)->id;
    const int shots_needed = kFactoryHp / kTankDamage;
    for (int t = 0; t < shots_needed * kTankCooldownTicks + 60; ++t) {
        sim.step();
    }
    const Unit* dead = sim.find(factory);
    ASSERT_NE(dead, nullptr) << "a dead unit stays in the roster, marked not alive";
    EXPECT_FALSE(dead->alive);
    EXPECT_EQ(dead->hp, 0);
    EXPECT_EQ(dead->stance, Stance::Dead);
    const std::uint16_t hp = dead->hp;
    const std::uint64_t hash = sim.state_hash();
    for (int t = 0; t < 300; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.find(factory)->hp, hp) << "a dead unit takes no more damage";
    EXPECT_NE(sim.state_hash(), hash) << "the tick still advances";
}

TEST(Sim, LongScriptedRunIsReproducible) {
    // 5000 ticks of orders, deaths and patrols. The property is that two runs agree -- and I check
    // the same script by hand across -O0/-O1/-O2/-O3/-ffast-math and under ASan+UBSan, where a
    // single floating-point operation or a signed overflow would make the hashes diverge.
    const auto run = []() -> std::uint64_t {
        Scenario s;
        s.seed = 12345;
        s.spawns = {{UnitKind::Tank, Side::Player, 20, 128},
                    {UnitKind::Tank, Side::Player, 40, 128},
                    {UnitKind::Factory, Side::Player, 30, 140},
                    {UnitKind::Turret, Side::Enemy, 180, 128},
                    {UnitKind::Factory, Side::Enemy, 200, 132}};
        Sim sim = Sim::create(s);
        std::vector<UnitId> ids;
        for (const Unit& u : sim.units()) {
            ids.push_back(u.id);
        }
        for (std::uint32_t t = 0; t < 5000; ++t) {
            if (t % 37 == 0) {
                sim.enqueue(
                    Order{ids[t % ids.size()],
                          orders::MoveTo{Fixed::from_int(static_cast<int>(t % 250)),
                                         Fixed::from_int(static_cast<int>((t * 7) % 250))}});
            }
            if (t % 101 == 0) {
                sim.enqueue(Order{ids[0], orders::Attack{ids[3]}});
            }
            sim.step();
        }
        return sim.state_hash();
    };
    EXPECT_EQ(run(), run());
    EXPECT_NE(run(), 0U) << "a hash of zero means the hash covers nothing";
}

TEST(Sim, TimeToKillMatchesTheArithmetic) {
    Scenario s;
    s.seed = 5;
    s.spawns
        = {{UnitKind::Tank, Side::Player, 100, 100}, {UnitKind::Factory, Side::Enemy, 104, 100}};
    Sim sim = Sim::create(s);
    const UnitId factory = by_side(sim, Side::Enemy, 0)->id;
    const int ticks = (kFactoryHp / kTankDamage - 1) * kTankCooldownTicks + 2;
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
    EXPECT_GT(sim.find(factory)->hp, 0) << "one shot short: the factory should still stand";
}

TEST(Sim, PatrolWalksBetweenItsTwoEnds) {
    // Patrol was implemented and completely unverified: nothing in the suite would have noticed
    // if the two ends never swapped.
    constexpr std::uint64_t kSeed = 3;
    int x0 = 0;
    int y0 = 0;
    ASSERT_TRUE(find_flat_run(kSeed, 60, &x0, &y0));
    Scenario s;
    s.seed = kSeed;
    s.spawns = {{UnitKind::Tank, Side::Player, x0 + 2, y0}};
    Sim sim = Sim::create(s);
    const Fixed left = Fixed::from_int(x0 + 2);
    const Fixed right = Fixed::from_int(x0 + 50);
    sim.enqueue(Order{sim.units()[0].id,
                      orders::Patrol{left, Fixed::from_int(y0), right, Fixed::from_int(y0)}});
    bool reached_left = false;
    bool reached_right = false;
    const Fixed r = Fixed::from_raw(kArriveRadiusRaw);
    for (int t = 0; t < 2000; ++t) {
        sim.step();
        const Fixed x = sim.units()[0].x;
        reached_left = reached_left || fixed_abs(fixed_sub(x, left)) < r;
        reached_right = reached_right || fixed_abs(fixed_sub(x, right)) < r;
        if (reached_left && reached_right) {
            break;
        }
    }
    EXPECT_TRUE(reached_right) << "a patrol must reach its far end";
    EXPECT_TRUE(reached_left) << "and must come back -- otherwise the ends never swap";
    EXPECT_EQ(sim.units()[0].stance, Stance::Patrol);
}

TEST(Sim, StopHaltsAMovingUnit) {
    constexpr std::uint64_t kSeed = 3;
    int x0 = 0;
    int y0 = 0;
    ASSERT_TRUE(find_flat_run(kSeed, 60, &x0, &y0));
    Scenario s;
    s.seed = kSeed;
    s.spawns = {{UnitKind::Tank, Side::Player, x0 + 2, y0}};
    Sim sim = Sim::create(s);
    const UnitId id = sim.units()[0].id;
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(x0 + 50), Fixed::from_int(y0)}});
    for (int t = 0; t < 60; ++t) {
        sim.step();
    }
    ASSERT_GT(sim.units()[0].x, Fixed::from_int(x0 + 2)) << "the tank should be under way";
    sim.enqueue(Order{id, orders::Stop{}});
    sim.step();
    const Unit stopped = *sim.find(id);
    EXPECT_EQ(stopped.stance, Stance::Idle);
    EXPECT_EQ(stopped.vx, Fixed::zero());
    EXPECT_EQ(stopped.vy, Fixed::zero());
    for (int t = 0; t < 300; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.find(id)->x, stopped.x) << "a stopped unit must not drift";
}

TEST(Sim, AnAttackOrderEngagesItsNamedTargetNotTheNearest) {
    // Approved semantics: an explicit target outranks auto-engagement while it lives and is in
    // range. The nearer factory is inside tank range too, and must be ignored.
    Scenario s;
    s.seed = 5;
    s.spawns = {{UnitKind::Tank, Side::Player, 100, 100},
                {UnitKind::Factory, Side::Enemy, 105, 104},
                {UnitKind::Factory, Side::Enemy, 110, 100}};
    Sim sim = Sim::create(s);
    const UnitId near_factory = by_side(sim, Side::Enemy, 0)->id;
    const UnitId far_factory = by_side(sim, Side::Enemy, 1)->id;
    sim.enqueue(Order{by_side(sim, Side::Player, 0)->id, orders::Attack{far_factory}});
    // The nearer factory is off the line of fire (a bolt hits the first hostile on its path). The
    // first bolt takes about 19 ticks to cross ten tiles; the second has not arrived.
    for (int t = 0; t < kTankCooldownTicks + 25; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.find(far_factory)->hp, kFactoryHp - kTankDamage) << "the named target was hit";
    EXPECT_EQ(sim.find(near_factory)->hp, kFactoryHp) << "the nearer factory must be left alone";
}

TEST(Sim, AnAttackOrderFallsBackToTheNearestWhenItsTargetDies) {
    Scenario s;
    s.seed = 5;
    s.spawns = {{UnitKind::Tank, Side::Player, 100, 100},
                {UnitKind::Factory, Side::Enemy, 104, 100},
                {UnitKind::Factory, Side::Enemy, 108, 100}};
    Sim sim = Sim::create(s);
    const UnitId near_factory = by_side(sim, Side::Enemy, 0)->id;
    const UnitId far_factory = by_side(sim, Side::Enemy, 1)->id;
    sim.enqueue(Order{by_side(sim, Side::Player, 0)->id, orders::Attack{far_factory}});
    // Step to the exact tick the named target dies. Overshooting lets the fallback fire at the
    // near factory inside the same window, which would make the next assertion wrong.
    int t = 0;
    while (sim.find(far_factory)->alive && t < 2000) {
        sim.step();
        ++t;
    }
    ASSERT_FALSE(sim.find(far_factory)->alive) << "50 shots at one per 15 ticks should kill it";
    EXPECT_EQ(sim.find(near_factory)->hp, kFactoryHp) << "the nearer one is still queued behind";
    const std::uint16_t before = sim.find(near_factory)->hp;
    for (int t = 0; t < 60; ++t) {
        sim.step();
    }
    EXPECT_LT(sim.find(near_factory)->hp, before) << "with the target dead, auto-engage resumes";
}

} // namespace
