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
};

/// Every armed kind today shoots at kProjectileSpeedRaw with a climb ceiling of 2.0.
inline constexpr std::int32_t kBoltArcRaw = 2 * Fixed::kOne;

/// Indexed by the value of `UnitKind`, which stays the index type; the order below is the enum's.
inline constexpr std::array<UnitSpec, static_cast<std::size_t>(kUnitKindCount)> kUnitSpecs{{
    {"tank", kTankHp, Fixed::from_raw(kTankSpeedRaw), Fixed::from_raw(kTankRangeRaw), kTankDamage,
     kTankCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw), Fixed::from_raw(kBoltArcRaw)},
    {"turret", kTurretHp, Fixed::from_raw(kTurretSpeedRaw), Fixed::from_raw(kTurretRangeRaw),
     kTurretDamage, kTurretCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw)},
    {"factory", kFactoryHp, Fixed::zero(), Fixed::zero(), 0, 0, Fixed::zero(), Fixed::zero()},
    // The hover tank has the tank's numbers: the look and the controls differ, not the stats.
    {"hovertank", kTankHp, Fixed::from_raw(kTankSpeedRaw), Fixed::from_raw(kTankRangeRaw),
     kTankDamage, kTankCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw)},
}};

[[nodiscard]] constexpr const UnitSpec& spec_of(UnitKind kind) noexcept {
    return kUnitSpecs.at(static_cast<std::size_t>(kind));
}

} // namespace protekt::sim
