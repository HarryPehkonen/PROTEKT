#include "protekt/sim/projectile.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

/// The spec table is the one place a kind's numbers live. Every stat of every kind is pinned here
/// to the constant the game has always used, so a wrong entry names itself.
namespace protekt::sim {
namespace {

TEST(Spec, TableCoversEveryKind) {
    EXPECT_EQ(kUnitSpecs.size(), static_cast<std::size_t>(kUnitKindCount));
}

TEST(Spec, EveryKindHasAName) {
    for (int i = 0; i < kUnitKindCount; ++i) {
        const UnitSpec& spec = spec_of(static_cast<UnitKind>(i));
        ASSERT_NE(spec.name, nullptr) << "kind " << i;
        EXPECT_GT(std::strlen(spec.name), 0U) << "kind " << i;
    }
}

TEST(Spec, TankStats) {
    const UnitSpec& s = spec_of(UnitKind::Tank);
    EXPECT_STREQ(s.name, "tank");
    EXPECT_EQ(s.hp, kTankHp);
    EXPECT_EQ(s.max_speed.raw, kTankSpeedRaw);
    EXPECT_EQ(s.range.raw, kTankRangeRaw);
    EXPECT_EQ(s.damage, kTankDamage);
    EXPECT_EQ(s.cooldown, kTankCooldownTicks);
}

TEST(Spec, HoverTankHasTheTanksNumbers) {
    const UnitSpec& s = spec_of(UnitKind::HoverTank);
    EXPECT_STREQ(s.name, "hovertank");
    EXPECT_EQ(s.hp, kTankHp);
    EXPECT_EQ(s.max_speed.raw, kTankSpeedRaw);
    EXPECT_EQ(s.range.raw, kTankRangeRaw);
    EXPECT_EQ(s.damage, kTankDamage);
    EXPECT_EQ(s.cooldown, kTankCooldownTicks);
}

TEST(Spec, TurretStats) {
    const UnitSpec& s = spec_of(UnitKind::Turret);
    EXPECT_STREQ(s.name, "turret");
    EXPECT_EQ(s.hp, kTurretHp);
    EXPECT_EQ(s.max_speed.raw, kTurretSpeedRaw);
    EXPECT_EQ(s.range.raw, kTurretRangeRaw);
    EXPECT_EQ(s.damage, kTurretDamage);
    EXPECT_EQ(s.cooldown, kTurretCooldownTicks);
}

TEST(Spec, FactoryStats) {
    const UnitSpec& s = spec_of(UnitKind::Factory);
    EXPECT_STREQ(s.name, "factory");
    EXPECT_EQ(s.hp, kFactoryHp);
    EXPECT_EQ(s.max_speed.raw, 0);
    EXPECT_EQ(s.range.raw, 0);
    EXPECT_EQ(s.damage, 0);
    EXPECT_EQ(s.cooldown, 0);
}

TEST(Spec, IndexedByTheUnchangedEnumValues) {
    EXPECT_EQ(static_cast<int>(UnitKind::Tank), 0);
    EXPECT_EQ(static_cast<int>(UnitKind::Turret), 1);
    EXPECT_EQ(static_cast<int>(UnitKind::Factory), 2);
    EXPECT_EQ(static_cast<int>(UnitKind::HoverTank), 3);
    EXPECT_STREQ(kUnitSpecs.at(1).name, "turret");
}

TEST(Spec, ProfileIsTheTableRead) {
    for (int i = 0; i < kUnitKindCount; ++i) {
        const UnitKind kind = static_cast<UnitKind>(i);
        const UnitSpec& s = spec_of(kind);
        const UnitProfile p = profile(kind);
        EXPECT_EQ(p.hp, s.hp) << s.name;
        EXPECT_EQ(p.max_speed.raw, s.max_speed.raw) << s.name;
        EXPECT_EQ(p.weapon_range.raw, s.range.raw) << s.name;
        EXPECT_EQ(p.damage, s.damage) << s.name;
        EXPECT_EQ(p.cooldown_ticks, s.cooldown) << s.name;
        EXPECT_EQ(p.mobile, s.max_speed.raw > 0) << s.name;
    }
}

// The weapon block. `bolt_speed` is the bolt's speed in raw tiles per tick; `arc` is the steepest
// climb the launch allows, in tiles of rise per tile of run, Q16.16 (today: 2.0 for every gun).
constexpr std::int32_t kTodaysArcRaw = 2 * Fixed::kOne;

TEST(SpecWeapon, EveryArmedKindKeepsTodaysBoltSpeedAndArc) {
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Turret, UnitKind::HoverTank}) {
        const UnitSpec& s = spec_of(kind);
        EXPECT_EQ(s.bolt_speed.raw, kProjectileSpeedRaw) << s.name;
        EXPECT_EQ(s.arc.raw, kTodaysArcRaw) << s.name;
    }
}

TEST(SpecWeapon, FactoryWeaponColumnsAreAllZero) {
    const UnitSpec& s = spec_of(UnitKind::Factory);
    EXPECT_EQ(s.bolt_speed.raw, 0);
    EXPECT_EQ(s.arc.raw, 0);
    EXPECT_EQ(s.range.raw, 0);
    EXPECT_EQ(s.damage, 0);
    EXPECT_EQ(s.cooldown, 0);
}

namespace {
Unit gun_of(UnitKind kind) {
    Unit unit{};
    unit.id = UnitId{1};
    unit.kind = kind;
    unit.side = Side::Player;
    unit.x = Fixed::from_int(100);
    unit.y = Fixed::from_int(100);
    unit.hp = kTankHp;
    return unit;
}
constexpr std::uint64_t kSeed = 7;
} // namespace

TEST(SpecWeapon, LaunchSpeedComesFromTheShootersSpec) {
    Unit target = gun_of(UnitKind::Tank);
    target.id = UnitId{2};
    target.side = Side::Enemy;
    target.x = Fixed::from_int(112);
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Turret, UnitKind::HoverTank}) {
        const Projectile bolt = launch(gun_of(kind), target, 8, kSeed);
        EXPECT_EQ(bolt.vx.raw, spec_of(kind).bolt_speed.raw) << spec_of(kind).name;
    }
}

TEST(SpecWeapon, LaunchTowardsStraightUpIsTheSpecSpeedInHeightUnits) {
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Turret, UnitKind::HoverTank}) {
        const Projectile bolt = launch_towards(gun_of(kind), Fixed::zero(), Fixed::zero(),
                                               Fixed::from_int(1), UnitId{}, 8, kSeed);
        EXPECT_EQ(bolt.vz.raw, spec_of(kind).bolt_speed.raw * kHeightUnitsPerTile)
            << spec_of(kind).name;
    }
}

TEST(SpecWeapon, LaunchClampsTheClimbToTheSpecArc) {
    // A factory one raw unit away and much taller than the muzzle: the unclamped climb is huge.
    Unit target = gun_of(UnitKind::Factory);
    target.id = UnitId{2};
    target.side = Side::Enemy;
    target.x = Fixed::from_raw(Fixed::from_int(100).raw + 1);
    const UnitSpec& s = spec_of(UnitKind::Tank);
    const Projectile bolt = launch(gun_of(UnitKind::Tank), target, 8, kSeed);
    const std::int64_t steepest = static_cast<std::int64_t>(s.arc.raw) * s.bolt_speed.raw
                                  / Fixed::kOne * kHeightUnitsPerTile;
    EXPECT_EQ(bolt.vz.raw, steepest);
}

// THE FACTORY'S FOUR NEW KINDS (2026-10-30). Every number is PROVISIONAL -- Harri's to change once
// he has played it -- and pinned as a VALUE (not through the constant that builds it), so a later
// edit to the table has to be deliberate. The tank's: hp 100, damage 8, cooldown 15, speed 13107
// raw (0.2 tiles/tick), range 12.
TEST(SpecFactoryKinds, TheAssaultTankIsTheTanksGunOnThreeHalvesTheHull) {
    const UnitSpec& s = spec_of(UnitKind::AssaultTank);
    EXPECT_EQ(s.hp, 150);
    EXPECT_EQ(s.max_speed.raw, 13107);
    EXPECT_EQ(s.range.raw, 12 << 16);
    EXPECT_EQ(s.damage, 12);
    EXPECT_EQ(s.cooldown, 15);
    EXPECT_EQ(s.splash_radius.raw, 0);
    EXPECT_EQ(s.gravity.raw, 0) << "a straight bolt";
    EXPECT_EQ(s.cost, 70);
    EXPECT_EQ(s.build_ticks, 1400);
}

TEST(SpecFactoryKinds, TheWalkerIsSlowHeavyAndTwiceTheTanksHitAndReload) {
    const UnitSpec& s = spec_of(UnitKind::Walker);
    EXPECT_EQ(s.hp, 200);
    EXPECT_EQ(s.max_speed.raw, 13107 / 2);
    EXPECT_EQ(s.range.raw, 12 << 16);
    EXPECT_EQ(s.damage, 16);
    EXPECT_EQ(s.cooldown, 30);
    EXPECT_EQ(s.splash_radius.raw, 0);
    EXPECT_EQ(s.gravity.raw, 0) << "a straight bolt";
    EXPECT_EQ(s.cost, 100);
    EXPECT_EQ(s.build_ticks, 2000);
}

TEST(SpecFactoryKinds, TheApcIsTheFastestGroundVehicleWithALightGun) {
    const UnitSpec& s = spec_of(UnitKind::Apc);
    EXPECT_EQ(s.hp, 100);
    EXPECT_EQ(s.max_speed.raw, 13107 * 3 / 2);
    EXPECT_EQ(s.range.raw, spec_of(UnitKind::Scout).range.raw) << "the scout's 10 tiles";
    EXPECT_EQ(s.damage, 4);
    EXPECT_EQ(s.cooldown, 7);
    EXPECT_EQ(s.splash_radius.raw, 0);
    EXPECT_EQ(s.gravity.raw, 0) << "a straight bolt";
    EXPECT_EQ(s.cost, 50);
    EXPECT_EQ(s.build_ticks, 1000);
    EXPECT_GT(s.range.raw, 0) << "armed: it has no troops to carry yet, so it must fight";
}

TEST(SpecFactoryKinds, TheBomberDropsTheMortarsLobbedShellWithAOneAndAHalfTileSplash) {
    const UnitSpec& s = spec_of(UnitKind::Bomber);
    const UnitSpec& mortar = spec_of(UnitKind::MortarBike);
    EXPECT_EQ(s.hp, 75);
    EXPECT_EQ(s.max_speed.raw, 13107 * 3 / 4);
    EXPECT_EQ(s.range.raw, 14 << 16);
    EXPECT_EQ(s.damage, 16);
    EXPECT_EQ(s.cooldown, 45);
    EXPECT_EQ(s.splash_radius.raw, 3 * Fixed::kOne / 2) << "1.5 tiles";
    EXPECT_EQ(s.cost, 75);
    EXPECT_EQ(s.build_ticks, 1500);
    // The bomb IS the existing lobbed shell: no ballistics of its own.
    EXPECT_EQ(s.gravity.raw, mortar.gravity.raw);
    EXPECT_EQ(s.arc.raw, mortar.arc.raw);
    EXPECT_EQ(s.bolt_speed.raw, mortar.bolt_speed.raw);
    EXPECT_GT(s.gravity.raw, 0);
}

TEST(SpecFactoryKinds, TheFourOldKindsNowCarryTheirPrices) {
    EXPECT_EQ(spec_of(UnitKind::MortarBike).cost, 45);
    EXPECT_EQ(spec_of(UnitKind::MortarBike).build_ticks, 900);
    EXPECT_EQ(spec_of(UnitKind::Scout).cost, 45);
    EXPECT_EQ(spec_of(UnitKind::Scout).build_ticks, 900);
    EXPECT_EQ(spec_of(UnitKind::Tank).cost, 55) << "Harri's cost; it was 40";
    EXPECT_EQ(spec_of(UnitKind::Tank).build_ticks, 900) << "the one exception to 20 ticks a scrap";
    EXPECT_EQ(spec_of(UnitKind::RocketTank).cost, 65);
    EXPECT_EQ(spec_of(UnitKind::RocketTank).build_ticks, 1300);
}

TEST(SpecFactoryKinds, EveryFactoryKindIsQueueableAndTheUnpricedOnesAreNot) {
    for (const UnitKind kind :
         {UnitKind::MortarBike, UnitKind::Scout, UnitKind::Tank, UnitKind::RocketTank,
          UnitKind::AssaultTank, UnitKind::Walker, UnitKind::Apc, UnitKind::Bomber}) {
        EXPECT_TRUE(is_queueable(kind)) << spec_of(kind).name;
    }
    // The scavenger keeps its own 20/300 row; the rest have no price and are not buildable.
    EXPECT_TRUE(is_queueable(UnitKind::Scavenger));
    for (const UnitKind kind : {UnitKind::Factory, UnitKind::DeployedRecycler, UnitKind::Recycler,
                                UnitKind::Turret, UnitKind::HoverTank}) {
        EXPECT_FALSE(is_queueable(kind)) << spec_of(kind).name;
    }
}

TEST(SpecFactoryKinds, EveryNewKindIsDrivableAndNotATank) {
    for (const UnitKind kind :
         {UnitKind::AssaultTank, UnitKind::Walker, UnitKind::Apc, UnitKind::Bomber}) {
        EXPECT_TRUE(is_drivable(kind)) << spec_of(kind).name;
        EXPECT_FALSE(is_tank(kind)) << spec_of(kind).name;
    }
}

TEST(SpecScavenger, ItHasTheTanksOwnHullAndTakesTwentyFiveScoutShotsToKill) {
    EXPECT_EQ(spec_of(UnitKind::Scavenger).hp, 100) << "thick armour: the tank's hull, was 60";
    EXPECT_EQ(spec_of(UnitKind::Scavenger).hp, kTankHp);
    const int scout_damage = spec_of(UnitKind::Scout).damage;
    ASSERT_EQ(scout_damage, 4);
    // Ceiling division: the shots it takes to bring 100 hp to zero or below.
    const int shots = (spec_of(UnitKind::Scavenger).hp + scout_damage - 1) / scout_damage;
    EXPECT_EQ(shots, 25) << "it was 15 at 60 hp";
    // Everything else about it is as it was.
    EXPECT_EQ(spec_of(UnitKind::Scavenger).range.raw, 0) << "still unarmed";
    EXPECT_EQ(spec_of(UnitKind::Scavenger).cost, 20);
    EXPECT_EQ(spec_of(UnitKind::Scavenger).build_ticks, 300);
}

} // namespace
} // namespace protekt::sim
