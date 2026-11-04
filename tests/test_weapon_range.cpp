// RANGE BELONGS TO THE WEAPON, not to the vehicle (Harri, 2026-11-03: "'Rocket tank' has a long
// range because it has ROCKETS, not because it's a rocket tank. Other vehicles that use rockets
// also get the same range."). These tests hold the RULE, not the numbers: a kind's reach is a
// function of the weapon it mounts and of nothing else on its row.

#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace {

using protekt::sim::kRocketSpec;
using protekt::sim::kShotgunSpec;
using protekt::sim::kUnitSpecs;
using protekt::sim::kWeaponCount;
using protekt::sim::kWeaponSpecs;
using protekt::sim::spec_of;
using protekt::sim::UnitKind;
using protekt::sim::UnitSpec;
using protekt::sim::weapon_spec;
using protekt::sim::WeaponId;

// The reach is not a field of the kind's row: `row.range` names no data member, only the weapon's
// `range()` accessor exists. (A data member would make `row.range.raw` well-formed.)
template <typename T>
concept HasRangeField = requires(T row) { row.range.raw; };
static_assert(!HasRangeField<UnitSpec>, "a kind's row must not carry its own range");

TEST(WeaponRange, EveryKindsRangeIsItsWeaponsRange) {
    for (const UnitSpec& row : kUnitSpecs) {
        EXPECT_EQ(row.range().raw, weapon_spec(row.weapon).range.raw) << row.name;
    }
}

TEST(WeaponRange, TwoKindsMountingTheSameWeaponReportTheSameRange) {
    // Group every kind by weapon: within a group the range is one value, by construction.
    for (std::size_t w = 0; w < kWeaponCount; ++w) {
        const auto weapon = static_cast<WeaponId>(w);
        for (const UnitSpec& row : kUnitSpecs) {
            if (row.weapon == weapon) {
                EXPECT_EQ(row.range().raw, kWeaponSpecs.at(w).range.raw)
                    << row.name << " mounts " << kWeaponSpecs.at(w).name;
            }
        }
    }
    // The ruling's own example: every vehicle that fires a rocket reaches as far as the rocket.
    EXPECT_EQ(spec_of(UnitKind::RocketTank).weapon, WeaponId::Rocket);
    EXPECT_EQ(spec_of(UnitKind::MslScout).weapon, WeaponId::Rocket);
    EXPECT_EQ(spec_of(UnitKind::RocketTank).range().raw, spec_of(UnitKind::MslScout).range().raw);
    EXPECT_EQ(spec_of(UnitKind::RocketTank).range().raw, kRocketSpec.range().raw)
        << "the rocket carried by any hull is the same rocket";
    // The turret and its deployed form carry one gun; the bike and the bomber one shell.
    EXPECT_EQ(spec_of(UnitKind::Turret).range().raw, spec_of(UnitKind::DeployedTurret).range().raw);
    EXPECT_EQ(spec_of(UnitKind::MortarBike).range().raw, spec_of(UnitKind::Bomber).range().raw);
    EXPECT_EQ(kShotgunSpec.weapon, WeaponId::Shotgun);
}

TEST(WeaponRange, TheHullDecidesNothingAboutReach) {
    // Take the tank's whole row, change every number that is a hull's or a gun's own, keep the
    // weapon: the range does not move. Change only the weapon: it follows.
    UnitSpec row = spec_of(UnitKind::Tank);
    const std::int32_t before = row.range().raw;
    row.hp = 1;
    row.max_speed = protekt::sim::Fixed::from_raw(1);
    row.damage = 250;
    row.cooldown = 1;
    row.max_travel = protekt::sim::Fixed::from_raw(1);
    EXPECT_EQ(row.range().raw, before);
    row.weapon = WeaponId::Rocket;
    EXPECT_EQ(row.range().raw, weapon_spec(WeaponId::Rocket).range.raw);
    row.weapon = WeaponId::None;
    EXPECT_EQ(row.range().raw, 0) << "no weapon, unarmed";
}

TEST(WeaponRange, TheTurretsReachIsItsWeaponsToo) {
    EXPECT_EQ(spec_of(UnitKind::Turret).weapon, WeaponId::TurretGun);
    EXPECT_EQ(weapon_spec(WeaponId::TurretGun).range.raw, protekt::sim::kTurretRangeRaw);
    EXPECT_NE(spec_of(UnitKind::Turret).weapon, spec_of(UnitKind::Tank).weapon);
}

TEST(WeaponRange, UnarmedKindsMountNoWeapon) {
    for (const UnitKind kind : {UnitKind::Factory, UnitKind::Scavenger, UnitKind::Recycler,
                                UnitKind::Tug, UnitKind::ServiceTruck}) {
        EXPECT_EQ(spec_of(kind).weapon, WeaponId::None) << spec_of(kind).name;
        EXPECT_EQ(spec_of(kind).range().raw, 0) << spec_of(kind).name;
    }
}

TEST(WeaponRange, ABoltFliesAtLeastAsFarAsItsWeaponReaches) {
    for (const UnitSpec& row : kUnitSpecs) {
        EXPECT_GE(row.max_travel.raw, row.range().raw) << row.name;
    }
}

} // namespace
