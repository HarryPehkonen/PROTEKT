#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protekt/sim/fixed.hpp"
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
};

/// Indexed by the value of `UnitKind`, which stays the index type; the order below is the enum's.
inline constexpr std::array<UnitSpec, static_cast<std::size_t>(kUnitKindCount)> kUnitSpecs{{
    {"tank", kTankHp, Fixed::from_raw(kTankSpeedRaw), Fixed::from_raw(kTankRangeRaw), kTankDamage,
     kTankCooldownTicks},
    {"turret", kTurretHp, Fixed::from_raw(kTurretSpeedRaw), Fixed::from_raw(kTurretRangeRaw),
     kTurretDamage, kTurretCooldownTicks},
    {"factory", kFactoryHp, Fixed::zero(), Fixed::zero(), 0, 0},
    // The hover tank has the tank's numbers: the look and the controls differ, not the stats.
    {"hovertank", kTankHp, Fixed::from_raw(kTankSpeedRaw), Fixed::from_raw(kTankRangeRaw),
     kTankDamage, kTankCooldownTicks},
}};

[[nodiscard]] constexpr const UnitSpec& spec_of(UnitKind kind) noexcept {
    return kUnitSpecs.at(static_cast<std::size_t>(kind));
}

} // namespace protekt::sim
