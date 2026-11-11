#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

/// THE WALKER FIGHTS LIKE THE HEAVY IT LOOKS LIKE (round 15, H2). Harri: "It moves very slowly, has
/// a ton of armour, and a very powerful and far-reaching weapon. Some of its downsides reloading
/// takes a long time, and it moves slowly. That includes turning." Armour, damage, reload and speed
/// were already so; REACH and TURNING are what this file adds, and the first test pins the four
/// that were not touched.
namespace protekt::sim {
namespace {

constexpr double kPi = 3.14159265358979323846;

Sim lone(UnitKind kind) {
    Scenario scenario{};
    scenario.seed = 7;
    Spawn spawn{};
    spawn.side = Side::Player;
    spawn.kind = kind;
    spawn.tile_x = 100;
    spawn.tile_y = 100;
    scenario.spawns.push_back(spawn);
    return Sim::create(scenario);
}

/// Sends the lone unit straight to -Y: 180 degrees from the +Y a hull that has never moved points
/// along. Returns the unit's id.
UnitId send_behind(Sim& sim) {
    const UnitId id = sim.units().front().id;
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(100), Fixed::from_int(70)}});
    return id;
}

/// Degrees from +Y (the way a hull that has never moved points) to the unit's velocity.
double velocity_degrees(const Unit& unit) {
    return std::abs(std::atan2(static_cast<double>(unit.vx.raw), static_cast<double>(unit.vy.raw)))
           * 180.0 / kPi;
}

TEST(WalkerFights, ArmourDamageReloadAndSpeedAreUnchanged) {
    const UnitSpec& s = spec_of(UnitKind::Walker);
    EXPECT_EQ(s.hp, 200);
    EXPECT_EQ(s.damage, 16);
    EXPECT_EQ(s.cooldown, 30);
    EXPECT_EQ(s.max_speed.raw, 6553);
}

TEST(WalkerFights, TheWalkersReachIsLongerThanTheTanksAndEveryOtherVehicles) {
    const std::int32_t walker = spec_of(UnitKind::Walker).range().raw;
    for (int i = 0; i < kUnitKindCount; ++i) {
        const UnitSpec& other = spec_of(static_cast<UnitKind>(i));
        if (static_cast<UnitKind>(i) == UnitKind::Walker || other.max_speed.raw == 0) {
            continue;
        }
        EXPECT_GT(walker, other.range().raw) << other.name;
    }
}

TEST(WalkerFights, AWalkerFacingTheWrongWayHasNotMovedInTwentyTicks) {
    Sim sim = lone(UnitKind::Walker);
    const UnitId id = send_behind(sim);
    for (int i = 0; i < 20; ++i) {
        sim.step();
    }
    EXPECT_EQ(sim.find(id)->y, Fixed::from_int(100)) << "still turning, so still standing";
}

TEST(WalkerFights, ATankReversesAtOnce) {
    Sim sim = lone(UnitKind::Tank);
    const UnitId id = send_behind(sim);
    sim.step();
    EXPECT_NEAR(velocity_degrees(*sim.find(id)), 180.0, 0.5);
}

// ---- the symbols this slice adds: the Walker's OWN weapon, and a per-kind turn rate ----

TEST(WalkerFights, TheWalkerCarriesItsOwnGunAndNobodyElseDoes) {
    EXPECT_EQ(spec_of(UnitKind::Walker).weapon, WeaponId::WalkerGun);
    for (int i = 0; i < kUnitKindCount; ++i) {
        if (static_cast<UnitKind>(i) != UnitKind::Walker) {
            EXPECT_NE(spec_of(static_cast<UnitKind>(i)).weapon, WeaponId::WalkerGun)
                << spec_of(static_cast<UnitKind>(i)).name;
        }
    }
}

TEST(WalkerFights, TheRangesInTilesAreTankTwelveMortarFourteenRocketEighteenWalkerTwenty) {
    EXPECT_EQ(weapon_spec(WeaponId::TankGun).range.raw, 12 << 16) << "the tank's gun is untouched";
    EXPECT_EQ(weapon_spec(WeaponId::Mortar).range.raw, 14 << 16);
    EXPECT_EQ(weapon_spec(WeaponId::Rocket).range.raw, 18 << 16);
    EXPECT_EQ(weapon_spec(WeaponId::WalkerGun).range.raw, 20 << 16);
    EXPECT_EQ(spec_of(UnitKind::Walker).range().raw, 20 << 16);
    EXPECT_EQ(spec_of(UnitKind::Tank).range().raw, 12 << 16);
}

TEST(WalkerFights, TheWalkersBoltFliesAsFarAsItsReach) {
    const UnitSpec& s = spec_of(UnitKind::Walker);
    EXPECT_GE(s.max_travel.raw, s.range().raw) << "or the bolt fizzles short of its own range";
}

TEST(WalkerFights, OnlyTheWalkerTurnsAtALimitedRateEveryOtherKindTurnsAtOnce) {
    for (int i = 0; i < kUnitKindCount; ++i) {
        const UnitKind kind = static_cast<UnitKind>(i);
        EXPECT_EQ(spec_of(kind).turn_decidegrees, kind == UnitKind::Walker ? 15 : 0)
            << spec_of(kind).name << " (0 is today's rate: no limit)";
    }
}

/// Degrees the unit's HEADING has turned from +Y.
double heading_degrees(const Unit& unit) {
    return std::abs(
               std::atan2(static_cast<double>(unit.heading_x), static_cast<double>(unit.heading_y)))
           * 180.0 / kPi;
}

TEST(WalkerFights, InTwentyTicksTheTankHasTurnedOneEightyAndTheWalkerThirty) {
    Sim tank = lone(UnitKind::Tank);
    const UnitId tank_id = send_behind(tank);
    Sim walker = lone(UnitKind::Walker);
    const UnitId walker_id = send_behind(walker);
    for (int i = 0; i < 20; ++i) {
        tank.step();
        walker.step();
    }
    EXPECT_NEAR(velocity_degrees(*tank.find(tank_id)), 180.0, 0.5);
    // 1.5 degrees a tick, 45 a second at 30 ticks a second.
    EXPECT_NEAR(heading_degrees(*walker.find(walker_id)), 30.0, 0.1);
}

TEST(WalkerFights, AWalkerSetsOffOnceItFacesTheGoalAndTakesAHundredAndTwentyTicksToDoIt) {
    Sim sim = lone(UnitKind::Walker);
    const UnitId id = send_behind(sim);
    for (int i = 0; i < 119; ++i) {
        sim.step();
    }
    EXPECT_EQ(sim.find(id)->y, Fixed::from_int(100));
    for (int i = 0; i < 3; ++i) {
        sim.step();
    }
    EXPECT_LT(sim.find(id)->y, Fixed::from_int(100));
}

} // namespace
} // namespace protekt::sim
