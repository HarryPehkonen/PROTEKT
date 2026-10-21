#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// One unit kind's stats, as data. Q16.16 and integers only: this table is read by the tick loop.
struct UnitSpec {
    const char* name{};
    std::uint16_t hp{};
    Fixed max_speed{}; ///< tiles per tick; zero means it never moves
    Fixed range{};     ///< tiles; zero means unarmed
    std::uint16_t damage{};
    std::uint16_t cooldown{}; ///< ticks between shots
    // The weapon block's ballistics. Zero for an unarmed kind.
    Fixed bolt_speed{}; ///< raw tiles per tick, in the map plane's direction of aim
    /// The steepest climb a bolt may be launched at: tiles of rise per tile of run (a slope),
    /// Q16.16. The launch aims the bolt straight at the aim point; this only caps how steeply it
    /// may climb
    /// (`vz` raw <= arc * bolt_speed * kHeightUnitsPerTile), so 2.0 is a 63 degree ceiling.
    Fixed arc{};
    /// How far a bolt flies before it fizzles, tiles (see Projectile::max_travel).
    Fixed max_travel{};
    /// Tiles around the end of a flight in which every unit takes the bolt's damage; zero is
    /// single-target.
    Fixed splash_radius{};
    /// Height units per tick squared, Q16.16; zero is a straight flight, a mortar's is a lob.
    Fixed gravity{};
    /// May a side build this kind in play. False for every kind today; the build menu fills it.
    bool buildable{};
};

/// Every armed kind today shoots at kProjectileSpeedRaw with a climb ceiling of 2.0.
inline constexpr std::int32_t kBoltArcRaw = 2 * Fixed::kOne;
/// Every kind keeps the engine's old bolt range: the turret's 16 tiles.
inline constexpr Fixed kBoltTravel = kProjectileRange;

/// The mortar: a lob (gravity 2.25 height units per tick squared; peak ~3 tiles over a 7-tile shot,
/// ~12 over its 14), a 1.5-tile splash, 3/4 the tank's hp, half its speed, twice its damage, three
/// times its cooldown; range 14 sits between the tank's 12 and the turret's 16.
inline constexpr std::int32_t kMortarRangeRaw = 14 << 16;
inline constexpr std::int32_t kMortarGravityRaw = 9 * Fixed::kOne / 4;
inline constexpr std::int32_t kMortarArcRaw = 4 * Fixed::kOne;
inline constexpr std::int32_t kMortarSplashRaw = 3 * Fixed::kOne / 2;
/// The rocket: the tank's hp and speed, range 15, three times the damage, five times the cooldown,
/// three quarters of the bolt speed, a straight flight, no splash.
inline constexpr std::int32_t kRocketRangeRaw = 15 << 16;

/// Indexed by the value of `UnitKind`, which stays the index type; the order below is the enum's.
inline constexpr std::array<UnitSpec, static_cast<std::size_t>(kUnitKindCount)> kUnitSpecs{{
    {"tank", kTankHp, Fixed::from_raw(kTankSpeedRaw), Fixed::from_raw(kTankRangeRaw), kTankDamage,
     kTankCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw), Fixed::from_raw(kBoltArcRaw),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false},
    {"turret", kTurretHp, Fixed::from_raw(kTurretSpeedRaw), Fixed::from_raw(kTurretRangeRaw),
     kTurretDamage, kTurretCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw), kBoltTravel, Fixed::zero(), Fixed::zero(), false},
    {"factory", kFactoryHp, Fixed::zero(), Fixed::zero(), 0, 0, Fixed::zero(), Fixed::zero(),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false},
    // The hover tank has the tank's numbers: the look and the controls differ, not the stats.
    {"hovertank", kTankHp, Fixed::from_raw(kTankSpeedRaw), Fixed::from_raw(kTankRangeRaw),
     kTankDamage, kTankCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw), kBoltTravel, Fixed::zero(), Fixed::zero(), false},
    {"mortar", kTankHp * 3 / 4, Fixed::from_raw(kTankSpeedRaw / 2),
     Fixed::from_raw(kMortarRangeRaw), kTankDamage * 2, kTankCooldownTicks * 3,
     Fixed::from_raw(kProjectileSpeedRaw / 2), Fixed::from_raw(kMortarArcRaw), kBoltTravel,
     Fixed::from_raw(kMortarSplashRaw), Fixed::from_raw(kMortarGravityRaw), false},
    {"rocket", kTankHp, Fixed::from_raw(kTankSpeedRaw), Fixed::from_raw(kRocketRangeRaw),
     kTankDamage * 3, kTankCooldownTicks * 5, Fixed::from_raw(kProjectileSpeedRaw * 3 / 4),
     Fixed::from_raw(kBoltArcRaw), kBoltTravel, Fixed::zero(), Fixed::zero(), false},
}};

[[nodiscard]] constexpr const UnitSpec& spec_of(UnitKind kind) noexcept {
    return kUnitSpecs.at(static_cast<std::size_t>(kind));
}

} // namespace protekt::sim
