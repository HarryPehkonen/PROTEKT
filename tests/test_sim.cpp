// The simulation's contract. Everything asserted here is a property of the SPEC (sim.hpp and
// the balance constants in units.hpp) -- never of whatever the implementation chose.
#include <gtest/gtest.h>

#include <cstdlib>
#include <utility>
#include <vector>

#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
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
    EXPECT_TRUE(profile(UnitKind::Turret).mobile) << "turrets crawl";
    EXPECT_EQ(profile(UnitKind::Turret).max_speed.raw, kTankSpeedRaw / 4);
    EXPECT_EQ(profile(UnitKind::Turret).max_speed.raw, kTurretSpeedRaw);
    EXPECT_FALSE(profile(UnitKind::Factory).mobile);
    EXPECT_EQ(profile(UnitKind::Factory).max_speed.raw, 0) << "a factory stays put";
    EXPECT_EQ(profile(UnitKind::Factory).hp, kFactoryHp);
    EXPECT_EQ(profile(UnitKind::Factory).weapon_range.raw, 0) << "a factory has no gun";
}

TEST(Sim, TheScavengerIsUnarmedAndMobile) {
    // The kind that exists to haul (C2). Unarmed is the load-bearing half: `range == 0` is what
    // keeps it out of targeting, out of the AI's threat list, and firing nothing at anyone.
    const UnitProfile scav = profile(UnitKind::Scavenger);
    EXPECT_EQ(scav.weapon_range.raw, 0)
        << "unarmed: it never fires, and nothing it does is a threat";
    EXPECT_EQ(scav.damage, 0);
    EXPECT_EQ(scav.hp, kScavengerHp);
    EXPECT_EQ(scav.max_speed.raw, kScavengerSpeedRaw);
    EXPECT_TRUE(scav.mobile) << "it hauls: it has to reach the wreckage";
    EXPECT_LT(scav.max_speed.raw, profile(UnitKind::Tank).max_speed.raw) << "slower than a tank";
    EXPECT_GT(scav.hp, 0) << "light, but not a one-shot";
    // Appended last, so no existing kind's index -- nor any hashed kind value -- moved.
    EXPECT_EQ(static_cast<int>(UnitKind::Scavenger), static_cast<int>(UnitKind::Recycler) - 1);
    EXPECT_TRUE(is_drivable(UnitKind::Scavenger));
    EXPECT_FALSE(is_tank(UnitKind::Scavenger)) << "the tank rules skip a hauler";
}

TEST(Sim, TheRecyclerKeepsIndexSevenAndIsDrivableButNotATank) {
    // Appended, never inserted: the kind is hashed, packed and replayed, so every earlier index
    // has to stay where it was -- its own included, now that the deployed recycler follows it.
    EXPECT_EQ(static_cast<int>(UnitKind::Recycler), 7);
    EXPECT_TRUE(is_drivable(UnitKind::Recycler)) << "it moves: only the factory does not";
    EXPECT_FALSE(is_tank(UnitKind::Recycler)) << "the tank rules assume a gun, and it has none";
}

TEST(Sim, TheRecyclerNeverFiresAndIsNothingToFear) {
    const UnitSpec& spec = spec_of(UnitKind::Recycler);
    EXPECT_EQ(spec.range.raw, 0) << "range zero is what makes a kind unarmed everywhere";
    EXPECT_EQ(spec.damage, 0);
    EXPECT_EQ(spec.cooldown, 0);
    EXPECT_GT(spec.max_speed.raw, 0) << "it has to be able to reach a deploy site";
}

TEST(Sim, TheScoutIsAppendedAfterTheDeployedRecyclerAndNoEarlierIndexMoved) {
    // DEPLOY, then VIS. Every value below is hashed, packed into observations and written into
    // replays: a kind inserted anywhere but the end would move all of them.
    EXPECT_EQ(static_cast<int>(UnitKind::Tank), 0);
    EXPECT_EQ(static_cast<int>(UnitKind::Turret), 1);
    EXPECT_EQ(static_cast<int>(UnitKind::Factory), 2);
    EXPECT_EQ(static_cast<int>(UnitKind::HoverTank), 3);
    EXPECT_EQ(static_cast<int>(UnitKind::MortarBike), 4);
    EXPECT_EQ(static_cast<int>(UnitKind::RocketTank), 5);
    EXPECT_EQ(static_cast<int>(UnitKind::Scavenger), 6);
    EXPECT_EQ(static_cast<int>(UnitKind::Recycler), 7);
    EXPECT_EQ(static_cast<int>(UnitKind::DeployedRecycler), 8);
    EXPECT_EQ(static_cast<int>(UnitKind::Scout), 9);
    // The shotgun was appended here as kind 10 and has been REMOVED again: it is a carried
    // WEAPON (`kShotgunSpec`, fired by `orders::FireFan`), not a hull. Nothing earlier moved
    // either way, which is the property this test exists to pin.
    // The four factory kinds were appended after it, again without moving anything.
    EXPECT_EQ(static_cast<int>(UnitKind::AssaultTank), 10);
    EXPECT_EQ(static_cast<int>(UnitKind::Walker), 11);
    EXPECT_EQ(static_cast<int>(UnitKind::Apc), 12);
    EXPECT_EQ(static_cast<int>(UnitKind::Bomber), 13);
    EXPECT_EQ(kUnitKindCount, 14);
    EXPECT_EQ(static_cast<int>(UnitKind::Bomber), kUnitKindCount - 1) << "the bomber stays last";
}

TEST(Sim, TheScoutIsALightFastArmedVehicleThatCanBeDriven) {
    const UnitSpec& scout = spec_of(UnitKind::Scout);
    const UnitSpec& tank = spec_of(UnitKind::Tank);
    EXPECT_GT(scout.range.raw, 0) << "armed: it can shoot back";
    EXPECT_GT(scout.damage, 0);
    EXPECT_GT(scout.cooldown, 0);
    EXPECT_GT(scout.max_speed.raw, 0) << "mobile";
    EXPECT_LT(scout.hp, tank.hp) << "light armour";
    EXPECT_GT(scout.max_speed.raw, tank.max_speed.raw) << "it has to scout";
    EXPECT_TRUE(is_drivable(UnitKind::Scout));
    EXPECT_FALSE(scout.buildable) << "the build menu (RB3) fills that flag, not this slice";
}

TEST(Sim, TheDeployedRecyclerIsAStillUnarmedBuildingWithTheRecyclersHull) {
    const UnitSpec& spec = spec_of(UnitKind::DeployedRecycler);
    EXPECT_EQ(spec.max_speed.raw, 0) << "a building never moves";
    EXPECT_EQ(spec.range.raw, 0) << "unarmed";
    EXPECT_EQ(spec.damage, 0);
    EXPECT_EQ(spec.cooldown, 0);
    EXPECT_EQ(spec.hp, spec_of(UnitKind::Recycler).hp) << "the recycler's hull, by default";
    EXPECT_EQ(kDeployedRecyclerHp, kRecyclerHp);
    EXPECT_FALSE(profile(UnitKind::DeployedRecycler).mobile);
    EXPECT_FALSE(is_drivable(UnitKind::DeployedRecycler)) << "nothing to drive";
    EXPECT_TRUE(has_footprint(UnitKind::DeployedRecycler));
    EXPECT_FALSE(has_footprint(UnitKind::Recycler)) << "a vehicle has no footprint";
    EXPECT_FALSE(has_footprint(UnitKind::Factory)) << "DEPLOY gives the factory none";
}

TEST(Sim, TheHoverTankHasTheTanksProfileExactly) {
    EXPECT_EQ(kUnitKindCount, 14);
    EXPECT_EQ(static_cast<int>(UnitKind::HoverTank), 3);
    const UnitProfile tank = profile(UnitKind::Tank);
    const UnitProfile hover = profile(UnitKind::HoverTank);
    EXPECT_EQ(hover.hp, tank.hp);
    EXPECT_EQ(hover.max_speed.raw, tank.max_speed.raw);
    EXPECT_EQ(hover.weapon_range.raw, tank.weapon_range.raw);
    EXPECT_EQ(hover.damage, tank.damage);
    EXPECT_EQ(hover.cooldown_ticks, tank.cooldown_ticks);
    EXPECT_EQ(hover.mobile, tank.mobile);
    EXPECT_TRUE(is_tank(UnitKind::Tank));
    EXPECT_TRUE(is_tank(UnitKind::HoverTank));
    EXPECT_FALSE(is_tank(UnitKind::Turret));
    EXPECT_FALSE(is_tank(UnitKind::Factory));
}

TEST(Sim, AHoverTankMovesUnderOrdersAtTankSpeed) {
    constexpr std::uint64_t kSeed = 3;
    int x0 = 0;
    int y0 = 0;
    ASSERT_TRUE(find_flat_run(kSeed, 40, &x0, &y0));
    const Fixed goal_x = Fixed::from_int(x0 + 32);
    const auto drive = [&](UnitKind kind) {
        Scenario s;
        s.seed = kSeed;
        s.spawns = {{kind, Side::Player, x0 + 2, y0}};
        Sim sim = Sim::create(s);
        sim.enqueue(Order{sim.units()[0].id, orders::MoveTo{goal_x, Fixed::from_int(y0)}});
        for (int t = 0; t < 100; ++t) {
            sim.step();
        }
        return sim.units()[0];
    };
    const Unit tank = drive(UnitKind::Tank);
    const Unit hover = drive(UnitKind::HoverTank);
    EXPECT_EQ(hover.x.raw, tank.x.raw) << "same speed, same path: the same position";
    EXPECT_EQ(hover.vx.raw, tank.vx.raw);
    EXPECT_GT(hover.x.raw, Fixed::from_int(x0 + 2).raw) << "it moved";
}

TEST(Sim, AHoverTankAutoFiresInRangeLikeAnyArmedUnit) {
    Scenario s;
    s.seed = 5;
    s.spawns
        = {{UnitKind::HoverTank, Side::Player, 100, 100}, {UnitKind::Tank, Side::Enemy, 108, 100}};
    Sim sim = Sim::create(s);
    const UnitId enemy = sim.units()[1].id;
    for (int t = 0; t < kTankCooldownTicks + 30; ++t) {
        sim.step();
    }
    EXPECT_LT(sim.find(enemy)->hp, kTankHp) << "the hover tank shot the tank in range";
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

TEST(Sim, ATurretMovesToATileAtCrawlSpeed) {
    constexpr std::uint64_t kSeed = 3;
    int x0 = 0;
    int y0 = 0;
    ASSERT_TRUE(find_flat_run(kSeed, 40, &x0, &y0));
    Scenario s;
    s.seed = kSeed;
    s.spawns = {{UnitKind::Turret, Side::Player, x0 + 2, y0}};
    Sim sim = Sim::create(s);
    const Fixed goal = Fixed::from_int(x0 + 32);
    const std::int32_t start = sim.units()[0].x.raw;
    sim.enqueue(Order{sim.units()[0].id, orders::MoveTo{goal, Fixed::from_int(y0)}});
    for (int t = 0; t < kUnmountTicks; ++t) {
        sim.step(); // a mounted turret unmounts first, without moving
    }
    EXPECT_EQ(sim.units()[0].x.raw, start);
    for (int t = 0; t < 100; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.units()[0].x.raw - start, 100 * kTurretSpeedRaw) << "a steady crawl";
    EXPECT_GT(fixed_abs(fixed_sub(sim.units()[0].x, goal)), Fixed::from_raw(kArriveRadiusRaw));
    for (int t = 0; t < 600; ++t) {
        sim.step();
    }
    EXPECT_LT(fixed_abs(fixed_sub(sim.units()[0].x, goal)), Fixed::from_raw(kArriveRadiusRaw))
        << "never arrived";
}

TEST(Sim, AttackDoesNotMakeATurretChase) {
    Scenario s;
    s.seed = 3;
    s.spawns
        = {{UnitKind::Turret, Side::Player, 100, 100}, {UnitKind::Tank, Side::Enemy, 140, 100}};
    Sim sim = Sim::create(s);
    const Fixed x = sim.units()[0].x;
    const Fixed y = sim.units()[0].y;
    sim.enqueue(Order{sim.units()[0].id, orders::Attack{sim.units()[1].id}});
    for (int t = 0; t < 200; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.units()[0].x.raw, x.raw) << "a turret shoots in place, it never closes";
    EXPECT_EQ(sim.units()[0].y.raw, y.raw);
    EXPECT_EQ(sim.units()[0].vx.raw, 0);
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

/// A player tank whose gun has finished its opening reload, with no enemy anywhere near.
Sim reloaded_lone_tank() {
    Sim sim = Sim::create(two_tanks(7));
    for (int t = 0; t < profile(UnitKind::Tank).cooldown_ticks; ++t) {
        sim.step();
    }
    return sim;
}

TEST(Sim, AFireTowardsOrderLaunchesOneBolt) {
    Sim sim = reloaded_lone_tank();
    ASSERT_TRUE(sim.projectiles().empty());
    const UnitId mine = id_of(sim, Side::Player, 0);
    sim.enqueue(Order{mine, orders::FireTowards{Fixed::zero(), Fixed::from_int(1)}});
    sim.step();
    ASSERT_EQ(sim.projectiles().size(), 1U);
    const Projectile& bolt = sim.projectiles().front();
    EXPECT_EQ(bolt.shooter, mine);
    EXPECT_FALSE(bolt.aimed_at.valid());
    EXPECT_EQ(bolt.damage, profile(UnitKind::Tank).damage);
    EXPECT_EQ(bolt.vx.raw, 0) << "asked for due south";
    EXPECT_GT(bolt.vy.raw, 0) << "asked for due south";
    EXPECT_EQ(sim.find(mine)->stance, Stance::Idle) << "a shot is not a movement order";
}

TEST(Sim, FireTowardsObeysTheWeaponCooldown) {
    Sim sim = reloaded_lone_tank();
    const UnitId mine = id_of(sim, Side::Player, 0);
    sim.enqueue(Order{mine, orders::FireTowards{Fixed::from_int(1), Fixed::zero()}});
    sim.enqueue(Order{mine, orders::FireTowards{Fixed::zero(), Fixed::from_int(1)}});
    sim.step();
    EXPECT_EQ(sim.projectiles().size(), 1U) << "two orders in one tick, one gun, one bolt";

    // And it reloads like any shot: once the cooldown has run out the gun answers again.
    for (int t = 0; t < profile(UnitKind::Tank).cooldown_ticks; ++t) {
        sim.step();
    }
    sim.enqueue(Order{mine, orders::FireTowards{Fixed::from_int(1), Fixed::zero()}});
    const std::size_t before = sim.projectiles().size();
    sim.step();
    EXPECT_EQ(sim.projectiles().size(), before + 1);
}

// ---- repair near a living friendly factory -------------------------------------------------
// A damaged unit cannot be built from a scenario, so each case shoots the victim with a distant
// enemy tank and compares against the same fight with no factory: any hp the factory gave shows
// up as a difference, and nothing has to reach into the sim.

constexpr int kRepairTicks = 120; // 20 repair beats; the enemy tank loses ~64 hp of shots at 8/15

/// The player tank's hp after `ticks` of being shot by an enemy tank 10 tiles away, with the
/// given extra spawns (factories) added to the fight.
std::uint16_t victim_hp_after(std::vector<Spawn> extra, int ticks) {
    Scenario s;
    s.seed = 7;
    s.spawns = {{UnitKind::Tank, Side::Player, 100, 100}, {UnitKind::Tank, Side::Enemy, 110, 100}};
    s.spawns.insert(s.spawns.end(), extra.begin(), extra.end());
    Sim sim = Sim::create(s);
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
    return sim.units()[0].hp;
}

TEST(SimRepair, ADamagedTankNextToItsOwnFactoryGainsHp) {
    const std::uint16_t alone = victim_hp_after({}, kRepairTicks);
    const std::uint16_t repaired
        = victim_hp_after({{UnitKind::Factory, Side::Player, 100, 96}}, kRepairTicks);
    ASSERT_LT(alone, kTankHp) << "the fixture must actually hurt the tank";
    EXPECT_GT(repaired, alone);
}

TEST(SimRepair, RepairIsOneHpEverySixthTick) {
    // The first bolt lands on step 32, so the beats at ticks 0..30 (six of them) find a full-hp
    // tank and heal nothing; every later beat heals exactly 1.
    constexpr int kBeatsAtFullHp = 6;
    const int beats = kRepairTicks / static_cast<int>(kRepairTickInterval) - kBeatsAtFullHp;
    const std::uint16_t alone = victim_hp_after({}, kRepairTicks);
    const std::uint16_t repaired
        = victim_hp_after({{UnitKind::Factory, Side::Player, 100, 96}}, kRepairTicks);
    EXPECT_EQ(repaired - alone, beats);
}

TEST(SimRepair, HpNeverExceedsTheKindMaximum) {
    Scenario s;
    s.seed = 7;
    s.spawns = {{UnitKind::Factory, Side::Player, 100, 96},
                {UnitKind::Tank, Side::Player, 100, 100},
                {UnitKind::Turret, Side::Player, 103, 100}};
    Sim sim = Sim::create(s);
    for (int t = 0; t < 60; ++t) {
        sim.step();
    }
    EXPECT_EQ(by_side(sim, Side::Player, 1)->hp, kTankHp);
    EXPECT_EQ(by_side(sim, Side::Player, 2)->hp, kTurretHp);
}

TEST(SimRepair, ATankJustOutsideTheRadiusGainsNothing) {
    const std::uint16_t alone = victim_hp_after({}, kRepairTicks);
    const std::uint16_t outside = victim_hp_after(
        {{UnitKind::Factory, Side::Player, 100, 100 - kRepairRadiusTiles - 1}}, kRepairTicks);
    EXPECT_EQ(outside, alone);
    const std::uint16_t edge = victim_hp_after(
        {{UnitKind::Factory, Side::Player, 100, 100 - kRepairRadiusTiles}}, kRepairTicks);
    EXPECT_GT(edge, alone) << "exactly on the radius is inside it";
}

TEST(SimRepair, AnEnemyFactoryRepairsNothing) {
    const std::uint16_t alone = victim_hp_after({}, kRepairTicks);
    const std::uint16_t near_enemy_factory
        = victim_hp_after({{UnitKind::Factory, Side::Enemy, 100, 96}}, kRepairTicks);
    EXPECT_EQ(near_enemy_factory, alone);
}

TEST(SimRepair, ADestroyedFactoryRepairsNothing) {
    // Six enemy tanks beside the factory kill it, then turn on the tank 6 tiles away. After the
    // factory is gone the tank's hp may only fall.
    Scenario s;
    s.seed = 7;
    s.spawns
        = {{UnitKind::Tank, Side::Player, 100, 106}, {UnitKind::Factory, Side::Player, 100, 100}};
    for (int i = 0; i < 6; ++i) {
        s.spawns.push_back({UnitKind::Tank, Side::Enemy, 96 + i, 96});
    }
    Sim sim = Sim::create(s);
    const UnitId factory = id_of(sim, Side::Player, 1);
    bool factory_dead = false;
    std::uint16_t last = sim.units()[0].hp;
    for (int t = 0; t < 600 && sim.units()[0].alive; ++t) {
        sim.step();
        factory_dead = factory_dead || !sim.find(factory)->alive;
        if (factory_dead) {
            EXPECT_LE(sim.units()[0].hp, last) << "tick " << t;
        }
        last = sim.units()[0].hp;
    }
    EXPECT_TRUE(factory_dead) << "the fixture must kill the factory";
}

TEST(SimRepair, TheFactoryDoesNotRepairItself) {
    // Wounded factory: an enemy tank shoots it from 10 tiles; a second friendly factory beside it
    // would heal it, so there is none -- a factory's own radius must not count itself.
    Scenario s;
    s.seed = 7;
    s.spawns
        = {{UnitKind::Factory, Side::Player, 100, 100}, {UnitKind::Tank, Side::Enemy, 110, 100}};
    Sim sim = Sim::create(s);
    std::uint16_t last = sim.units()[0].hp;
    for (int t = 0; t < kRepairTicks; ++t) {
        sim.step();
        EXPECT_LE(sim.units()[0].hp, last) << "tick " << t;
        last = sim.units()[0].hp;
    }
    EXPECT_LT(last, kFactoryHp);
}

TEST(SimRepair, AUnitKilledThisTickIsNotHealed) {
    // Repair runs after fire, and skips the dead: a unit whose last hp was shot away stays at 0
    // hp rather than being healed to 1 on the same tick.
    Scenario s;
    s.seed = 7;
    s.spawns
        = {{UnitKind::Factory, Side::Player, 100, 100}, {UnitKind::Tank, Side::Player, 100, 104}};
    for (int i = 0; i < 12; ++i) {
        s.spawns.push_back({UnitKind::Tank, Side::Enemy, 96 + i, 112});
    }
    Sim sim = Sim::create(s);
    bool saw_death = false;
    for (int t = 0; t < 300; ++t) {
        sim.step();
        for (const Unit& u : sim.units()) {
            if (!u.alive) {
                EXPECT_EQ(u.hp, 0) << "tick " << t;
                saw_death = true;
            }
        }
    }
    EXPECT_TRUE(saw_death);
}

TEST(SimRepair, TheSameScenarioHashesTheSame) {
    const auto run = [] {
        Scenario s;
        s.seed = 7;
        s.spawns = {{UnitKind::Factory, Side::Player, 100, 96},
                    {UnitKind::Tank, Side::Player, 100, 100},
                    {UnitKind::Tank, Side::Enemy, 110, 100}};
        Sim sim = Sim::create(s);
        for (int t = 0; t < 300; ++t) {
            sim.step();
        }
        return sim.state_hash();
    };
    EXPECT_EQ(run(), run());
}

// DEPLOY: a recycler drives to a site, stands there kDeployTicks, and becomes a DeployedRecycler in
// place -- same id, same hit points -- on a 2x2 footprint nothing may enter or be spawned inside.

constexpr std::uint64_t kDeploySeed = 42;

/// The north-west tile of a 2x2 site with `margin` tiles of passable ground all round it, the first
/// found scanning rows from (`from`, `from`). Every deploy test stands on one, so a failure is the
/// rule's and never the terrain's.
Footprint open_site(std::uint64_t seed, int from, int margin) {
    for (int y = from; y < kMapTiles - from; ++y) {
        for (int x = from; x < kMapTiles - from; ++x) {
            bool open = true;
            for (int ty = y - margin; ty < y + kBuildingFootprintTiles + margin && open; ++ty) {
                for (int tx = x - margin; tx < x + kBuildingFootprintTiles + margin && open; ++tx) {
                    open = tile_passable(seed, tx, ty);
                }
            }
            if (open) {
                return Footprint{x, y};
            }
        }
    }
    return Footprint{-1, -1};
}

/// A sim of `spawns` on the deploy seed.
Sim deploy_sim(std::vector<Spawn> spawns) {
    return Sim::create(Scenario{kDeploySeed, std::move(spawns)});
}

/// The first unit of `kind`, whatever it is now.
UnitId first_of_kind(const Sim& sim, UnitKind kind) {
    for (const Unit& u : sim.units()) {
        if (u.kind == kind) {
            return u.id;
        }
    }
    return UnitId{};
}

void step_n(Sim& sim, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
}

/// His two numeric rulings, pinned to the NUMBERS and not to a name: the recycler takes ten
/// seconds to deploy (30 ticks a second) and a building stands on a 2x2 square. Every other deploy
/// test is written against `kDeployTicks` and `kBuildingFootprintTiles`, so without this test
/// either value could be changed to anything and the whole suite would still pass.
TEST(Deploy, TheRuledNumbersAreTenSecondsAndATwoByTwoBuilding) {
    EXPECT_EQ(kDeployTicks, 10 * kTicksPerSecond) << "ten seconds at 30 ticks a second";
    EXPECT_EQ(kBuildingFootprintTiles, 2) << "2x2 tiles, his ruling 2026-10-26";
}

TEST(Deploy, CompletesAtExactlyDeployTicksNotOneTickEarlierOrLater) {
    const Footprint site = open_site(kDeploySeed, 40, 2);
    ASSERT_GE(site.x, 0);
    // Standing on the site's centre already, so the count starts on the very first tick.
    Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()}});
    const UnitId id = first_of_kind(sim, UnitKind::Recycler);
    ASSERT_EQ(sim.find(id)->x, Fixed::from_int(site.centre_x()));
    sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
    ASSERT_EQ(sim.command_log().size(), 1U) << "a good site is accepted";

    step_n(sim, kDeployTicks - 1);
    const Unit before = *sim.find(id);
    EXPECT_EQ(before.kind, UnitKind::Recycler) << "one tick early, it is still a vehicle";
    EXPECT_EQ(before.stance, Stance::Deploy);
    EXPECT_EQ(before.deploy_ticks, kDeployTicks - 1);

    sim.step();
    const Unit* after = sim.find(id);
    ASSERT_NE(after, nullptr) << "the same id still names it";
    EXPECT_EQ(after->kind, UnitKind::DeployedRecycler) << "on exactly the kDeployTicks-th tick";
    EXPECT_EQ(sim.tick(), kDeployTicks);
    EXPECT_EQ(after->stance, Stance::Idle);
    EXPECT_EQ(after->deploy_ticks, 0) << "no countdown is left behind";
    EXPECT_EQ(after->x, Fixed::from_int(site.centre_x())) << "it stands on its site's centre";
    EXPECT_EQ(after->y, Fixed::from_int(site.centre_y()));
    EXPECT_EQ(sim.units().size(), 1U) << "in place: no unit was added, none removed";

    step_n(sim, 30);
    EXPECT_EQ(sim.find(id)->kind, UnitKind::DeployedRecycler) << "and it stays one";
    EXPECT_EQ(sim.find(id)->x, after->x) << "a building never moves";
}

TEST(Deploy, TheCountdownStartsOnlyOnceTheRecyclerHasReachedItsSite) {
    const Footprint site = open_site(kDeploySeed, 40, 8);
    ASSERT_GE(site.x, 0);
    Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.x - 6, site.centre_y()}});
    const UnitId id = first_of_kind(sim, UnitKind::Recycler);
    sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
    int driving = 0;
    while (sim.find(id)->deploy_ticks == 0 && driving < 600) {
        sim.step();
        ++driving;
    }
    ASSERT_GT(driving, 1) << "it had to drive there first";
    ASSERT_EQ(sim.find(id)->deploy_ticks, 1);
    step_n(sim, kDeployTicks - 2);
    EXPECT_EQ(sim.find(id)->kind, UnitKind::Recycler) << "the drive does not count";
    sim.step();
    EXPECT_EQ(sim.find(id)->kind, UnitKind::DeployedRecycler);
    EXPECT_EQ(sim.tick(), static_cast<Tick>(driving - 1 + kDeployTicks));
}

TEST(Deploy, HitPointsSurviveTheTransformationWhetherWholeOrDamaged) {
    const Footprint site = open_site(kDeploySeed, 40, 4);
    ASSERT_GE(site.x, 0);
    {
        Sim sim
            = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()}});
        const UnitId id = first_of_kind(sim, UnitKind::Recycler);
        sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
        step_n(sim, kDeployTicks);
        ASSERT_EQ(sim.find(id)->kind, UnitKind::DeployedRecycler);
        EXPECT_EQ(sim.find(id)->hp, kRecyclerHp) << "whole in, whole out";
    }
    // Wounded first, by its own side's gun (friendly fire is a FireTowards that names it), with no
    // factory anywhere to mend it: the deploy must neither heal nor wound it.
    Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()},
                          {UnitKind::Tank, Side::Player, site.centre_x() - 3, site.centre_y()}});
    const UnitId id = first_of_kind(sim, UnitKind::Recycler);
    const UnitId gun = first_of_kind(sim, UnitKind::Tank);
    step_n(sim, kTankCooldownTicks); // a fresh gun starts reloading
    sim.enqueue(
        Order{gun, orders::FireTowards{Fixed::from_int(1), Fixed::zero(), id, Fixed::zero()}});
    for (int t = 0; t < 60 && sim.find(id)->hp == kRecyclerHp; ++t) {
        sim.step();
    }
    const std::uint16_t wounded = sim.find(id)->hp;
    ASSERT_LT(wounded, kRecyclerHp) << "the shot must land, or this proves nothing";
    ASSERT_GT(wounded, 0);
    sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
    step_n(sim, kDeployTicks);
    ASSERT_EQ(sim.find(id)->kind, UnitKind::DeployedRecycler);
    EXPECT_EQ(sim.find(id)->hp, wounded) << "deploying does not heal it";
}

TEST(Deploy, TheFootprintIsImpassableToMovementAndTheTilesJustOutsideAreNot) {
    const Footprint site = open_site(kDeploySeed, 40, 6);
    ASSERT_GE(site.x, 0);
    Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()},
                          {UnitKind::Tank, Side::Player, site.x - 4, site.y}});
    const UnitId recycler = first_of_kind(sim, UnitKind::Recycler);
    const UnitId tank = first_of_kind(sim, UnitKind::Tank);
    sim.enqueue(Order{recycler, orders::Deploy{site.x, site.y}});
    step_n(sim, kDeployTicks);
    ASSERT_EQ(sim.find(recycler)->kind, UnitKind::DeployedRecycler);

    for (int ty = site.y - 1; ty <= site.y + kBuildingFootprintTiles; ++ty) {
        for (int tx = site.x - 1; tx <= site.x + kBuildingFootprintTiles; ++tx) {
            const bool inside = site.covers(tx, ty);
            EXPECT_EQ(building_blocks(sim.units(), tx, ty), inside) << tx << "," << ty;
            EXPECT_TRUE(tile_passable(kDeploySeed, tx, ty))
                << "the terrain answer is unchanged by a building: " << tx << "," << ty;
        }
    }

    // Straight through the building's row, then into the building itself: never inside it.
    const auto drive = [&](int goal_x, int goal_y, int ticks) {
        sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(goal_x), Fixed::from_int(goal_y)}});
        for (int t = 0; t < ticks; ++t) {
            sim.step();
            const Unit* u = sim.find(tank);
            ASSERT_FALSE(site.covers(u->x.floor_int(), u->y.floor_int()))
                << "tick " << sim.tick() << " at " << u->x.floor_int() << "," << u->y.floor_int();
        }
    };
    drive(site.x + 5, site.y, 200);
    const Unit* past = sim.find(tank);
    EXPECT_LE(std::abs(past->x.floor_int() - (site.x + 5)), 1) << "it went round, and arrived";
    EXPECT_LE(std::abs(past->y.floor_int() - site.y), 1);
    drive(site.centre_x(), site.centre_y(), 200);
}

TEST(Deploy, NoSpawnIsPlacedInsideAFootprintAndTheTilesJustOutsideStillHostOne) {
    const Footprint site = open_site(kDeploySeed, 40, 4);
    ASSERT_GE(site.x, 0);
    // The building first, then a tank wished onto each of its tiles and onto the tile east of it.
    std::vector<Spawn> spawns{{UnitKind::DeployedRecycler, Side::Player, site.x, site.y}};
    for (int dy = 0; dy < kBuildingFootprintTiles; ++dy) {
        for (int dx = 0; dx < kBuildingFootprintTiles; ++dx) {
            spawns.push_back({UnitKind::Tank, Side::Player, site.x + dx, site.y + dy});
        }
    }
    spawns.push_back({UnitKind::Tank, Side::Enemy, site.x + kBuildingFootprintTiles, site.y});
    const Sim sim = deploy_sim(spawns);
    const Unit& building = sim.units().front();
    EXPECT_EQ(building.x, Fixed::from_int(site.centre_x())) << "a building stands at its centre";
    EXPECT_EQ(building.y, Fixed::from_int(site.centre_y()));
    for (const Unit& u : sim.units()) {
        if (u.kind == UnitKind::Tank) {
            EXPECT_FALSE(site.covers(u.x.floor_int(), u.y.floor_int()))
                << "unit " << u.id.value << " was placed inside the building";
        }
    }
    const Unit& outside = sim.units().back();
    EXPECT_EQ(outside.x.floor_int(), site.x + kBuildingFootprintTiles) << "just outside: exactly";
    EXPECT_EQ(outside.y.floor_int(), site.y);

    // The other way round: a building is never placed over a unit already standing there.
    const Sim later = deploy_sim({{UnitKind::Tank, Side::Player, site.x + 1, site.y + 1},
                                  {UnitKind::DeployedRecycler, Side::Player, site.x, site.y}});
    const Unit& tank = later.units().front();
    EXPECT_FALSE(footprint_around(later.units().back().x, later.units().back().y)
                     .covers(tank.x.floor_int(), tank.y.floor_int()));
}

TEST(Deploy, AnEdgeTileIsClippedSoTheWholeSquareIsOnTheMap) {
    EXPECT_EQ(footprint_at(10, 20).x, 10) << "an inland tile is the north-west corner itself";
    EXPECT_EQ(footprint_at(10, 20).y, 20);
    EXPECT_EQ(footprint_at(kMapTiles - 1, 7).x, kMapTiles - kBuildingFootprintTiles);
    EXPECT_EQ(footprint_at(7, kMapTiles - 1).y, kMapTiles - kBuildingFootprintTiles);
    EXPECT_EQ(footprint_at(kMapTiles - 1, kMapTiles - 1).centre_x(), kMapTiles - 1)
        << "the centre of the last square is still on the map";
}

TEST(Deploy, AimedOffTheMapIsRefusedAndLeavesAWorkingRecycler) {
    const Footprint site = open_site(kDeploySeed, 40, 4);
    Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()}});
    const UnitId id = first_of_kind(sim, UnitKind::Recycler);
    for (const auto& [x, y] : std::vector<std::pair<int, int>>{
             {-1, site.y}, {site.x, -1}, {kMapTiles, site.y}, {site.x, kMapTiles}}) {
        EXPECT_FALSE(deploy_site_ok(kDeploySeed, sim.units(), x, y)) << x << "," << y;
        sim.enqueue(Order{id, orders::Deploy{x, y}});
    }
    EXPECT_TRUE(sim.command_log().empty()) << "every one refused";
    step_n(sim, kDeployTicks);
    EXPECT_EQ(sim.find(id)->kind, UnitKind::Recycler);
    EXPECT_EQ(sim.find(id)->stance, Stance::Idle);
    // Still a working vehicle: it drives, and it can still deploy where there is room.
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(site.x - 2), Fixed::from_int(site.y)}});
    step_n(sim, 30);
    EXPECT_NE(sim.find(id)->x, Fixed::from_int(site.centre_x())) << "it moved";
    sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
    step_n(sim, 120 + kDeployTicks);
    EXPECT_EQ(sim.find(id)->kind, UnitKind::DeployedRecycler);
}

TEST(Deploy, OntoImpassableGroundIsRefusedAndLeavesAWorkingRecycler) {
    // A crater, and a tile whose square has a crater in it though the tile itself is fine.
    int crater_x = -1;
    int crater_y = -1;
    for (int y = 40; y < 200 && crater_x < 0; ++y) {
        for (int x = 40; x < 200 && crater_x < 0; ++x) {
            if (!tile_passable(kDeploySeed, x, y) && tile_passable(kDeploySeed, x - 1, y - 1)) {
                crater_x = x;
                crater_y = y;
            }
        }
    }
    ASSERT_GE(crater_x, 0) << "the seed has a crater with open ground to its north-west";
    const Footprint site = open_site(kDeploySeed, 40, 4);
    Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()}});
    const UnitId id = first_of_kind(sim, UnitKind::Recycler);
    sim.enqueue(Order{id, orders::Deploy{crater_x, crater_y}});
    sim.enqueue(Order{id, orders::Deploy{crater_x - 1, crater_y - 1}});
    EXPECT_TRUE(sim.command_log().empty()) << "no square with a crater in it is a site";
    step_n(sim, kDeployTicks);
    EXPECT_EQ(sim.find(id)->kind, UnitKind::Recycler);
    sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
    step_n(sim, kDeployTicks);
    EXPECT_EQ(sim.find(id)->kind, UnitKind::DeployedRecycler) << "and it still deploys";
}

TEST(Deploy, IntoAnotherBuildingsFootprintIsRefusedAndLeavesAWorkingRecycler) {
    const Footprint site = open_site(kDeploySeed, 40, 6);
    Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()},
                          {UnitKind::Recycler, Side::Enemy, site.x + 4, site.centre_y()}});
    const UnitId first = sim.units().front().id;
    const UnitId second = sim.units().back().id;
    sim.enqueue(Order{first, orders::Deploy{site.x, site.y}});
    step_n(sim, kDeployTicks);
    ASSERT_EQ(sim.find(first)->kind, UnitKind::DeployedRecycler);
    const std::size_t logged = sim.command_log().size();
    // Every square that shares a tile with the building.
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            sim.enqueue(Order{second, orders::Deploy{site.x + dx, site.y + dy}});
        }
    }
    EXPECT_EQ(sim.command_log().size(), logged) << "an overlapping site is refused";
    sim.enqueue(Order{second, orders::Deploy{site.x + kBuildingFootprintTiles, site.y}});
    EXPECT_EQ(sim.command_log().size(), logged + 1) << "the square right beside it is not";
    step_n(sim, 60 + kDeployTicks);
    EXPECT_EQ(sim.find(second)->kind, UnitKind::DeployedRecycler);
}

TEST(Deploy, ASiteTakenDuringTheCountdownFailsCleanlyAndLeavesAWorkingRecycler) {
    // Two recyclers count down on squares that share a column. The lower id deploys first, on the
    // same tick, and the other's site is then inside its footprint.
    const Footprint site = open_site(kDeploySeed, 40, 6);
    const Footprint overlap{site.x + 1, site.y};
    Sim sim
        = deploy_sim({{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()},
                      {UnitKind::Recycler, Side::Enemy, overlap.centre_x(), overlap.centre_y()}});
    const UnitId first = sim.units().front().id;
    const UnitId second = sim.units().back().id;
    sim.enqueue(Order{first, orders::Deploy{site.x, site.y}});
    sim.enqueue(Order{second, orders::Deploy{overlap.x, overlap.y}});
    ASSERT_EQ(sim.command_log().size(), 2U) << "both sites are free when the orders are given";
    step_n(sim, kDeployTicks - 1);
    ASSERT_EQ(sim.find(second)->deploy_ticks, kDeployTicks - 1) << "both were counting";
    sim.step();
    EXPECT_EQ(sim.find(first)->kind, UnitKind::DeployedRecycler);
    const Unit* loser = sim.find(second);
    EXPECT_EQ(loser->kind, UnitKind::Recycler) << "never half-deployed";
    EXPECT_EQ(loser->stance, Stance::Idle) << "the order failed whole";
    EXPECT_EQ(loser->deploy_ticks, 0);
    step_n(sim, 30);
    EXPECT_EQ(sim.find(second)->kind, UnitKind::Recycler);
    // And it still works: it deploys on the next square along.
    sim.enqueue(Order{second, orders::Deploy{site.x + kBuildingFootprintTiles, site.y}});
    step_n(sim, 60 + kDeployTicks);
    EXPECT_EQ(sim.find(second)->kind, UnitKind::DeployedRecycler);
}

TEST(Deploy, IsRefusedASecondTimeAndForEveryOtherKind) {
    const Footprint site = open_site(kDeploySeed, 40, 6);
    std::vector<Spawn> spawns{{UnitKind::Recycler, Side::Player, site.centre_x(), site.centre_y()}};
    for (int kind = 0; kind < kUnitKindCount; ++kind) {
        if (static_cast<UnitKind>(kind) != UnitKind::Recycler) {
            spawns.push_back(
                {static_cast<UnitKind>(kind), Side::Player, site.x - 4, site.y + kind});
        }
    }
    Sim sim = deploy_sim(spawns);
    for (const Unit& u : sim.units()) {
        if (u.kind != UnitKind::Recycler) {
            sim.enqueue(Order{u.id, orders::Deploy{site.x + 4, site.y}});
        }
    }
    EXPECT_TRUE(sim.command_log().empty()) << "only a recycler deploys";
    const UnitId id = first_of_kind(sim, UnitKind::Recycler);
    sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
    step_n(sim, kDeployTicks);
    ASSERT_EQ(sim.find(id)->kind, UnitKind::DeployedRecycler);
    const std::size_t logged = sim.command_log().size();
    sim.enqueue(Order{id, orders::Deploy{site.x, site.y}});
    sim.enqueue(Order{id, orders::Deploy{site.x + 4, site.y}});
    EXPECT_EQ(sim.command_log().size(), logged) << "a deployed recycler does not deploy again";
}

TEST(Deploy, ABattleThatDeploysIsDeterministic) {
    const auto run = [] {
        const Footprint site = open_site(kDeploySeed, 40, 8);
        Sim sim = deploy_sim({{UnitKind::Recycler, Side::Player, site.x - 6, site.y},
                              {UnitKind::Tank, Side::Player, site.x - 5, site.y + 3}});
        sim.enqueue(Order{sim.units().front().id, orders::Deploy{site.x, site.y}});
        step_n(sim, 150);
        const std::uint64_t counting = sim.state_hash();
        step_n(sim, kDeployTicks);
        return std::pair{counting, sim.state_hash()};
    };
    EXPECT_EQ(run(), run());
}

TEST(Deploy, ADeployedRecyclerStillDecidesTheWar) {
    // Deploying changes the kind of the same unit: the war must still know it is the recycler.
    const Footprint site = open_site(kDeploySeed, 40, 6);
    Sim sim = deploy_sim({{UnitKind::DeployedRecycler, Side::Player, site.x, site.y},
                          {UnitKind::Recycler, Side::Enemy, site.x + 120, site.y + 120},
                          {UnitKind::Tank, Side::Enemy, site.x + 5, site.y},
                          {UnitKind::Tank, Side::Enemy, site.x - 4, site.y + 1},
                          {UnitKind::Tank, Side::Enemy, site.x + 1, site.y + 5}});
    EXPECT_EQ(sim.war_outcome(), WarOutcome::Undecided) << "a living deployed recycler";
    for (int t = 0; t < 3000 && sim.war_outcome() == WarOutcome::Undecided; ++t) {
        sim.step();
    }
    EXPECT_FALSE(sim.units().front().alive);
    EXPECT_EQ(sim.war_outcome(), WarOutcome::PlayerLost);
}

} // namespace
