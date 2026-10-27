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
/// THE BOUNCER ROUND (M3), all of it data. It leaves the barrel with the mortar's own ballistics,
/// so the range ladder is honest for its FIRST landing; then it bounces, rests, spins and pulses.
struct BouncerSpec {
    Fixed restitution{};         ///< fraction of the fall speed kept as the rebound
    Fixed horizontal_keep{};     ///< fraction of the horizontal speed kept by each bounce
    std::uint32_t max_bounces{}; ///< contacts it bounces off; the next one brings it to rest
    Fixed min_bounce_vz{}; ///< a rebound slower than this (height units a tick) is not a bounce
    std::uint32_t rest_ticks{};     ///< how long it spins at rest, then vanishes
    std::uint32_t pulse_interval{}; ///< ticks between shrapnel pulses; the last ends the rest
    Fixed pulse_radius{};         ///< tiles; every living unit of either side inside takes a pulse
    std::uint16_t pulse_damage{}; ///< at the centre; falls off linearly to zero at the radius
};
inline constexpr BouncerSpec kBouncer{Fixed::from_raw(Fixed::kOne / 2),
                                      Fixed::from_raw(Fixed::kOne * 3 / 4),
                                      3,
                                      Fixed::from_raw(6 * Fixed::kOne),
                                      90,
                                      15,
                                      Fixed::from_raw(3 * Fixed::kOne),
                                      12};

/// The rocket: the tank's hp and speed, range 15, three times the damage, five times the cooldown,
/// three quarters of the bolt speed, a straight flight, no splash.
inline constexpr std::int32_t kRocketRangeRaw = 15 << 16;

/// The scout: a minigun's reach, 10 tiles, below the tank's 12 (`kTankRangeRaw`).
inline constexpr std::int32_t kScoutRangeRaw = kTankRangeRaw * 5 / 6;

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
    // The scavenger (C2): no gun, no range, no damage -- `range` zero is what makes it unarmed
    // everywhere in the sim (fire, targeting, the AI's threat list). It is fast ENOUGH to reach
    // wreckage, not fast enough to outrun what killed it.
    {"scavenger", kScavengerHp, Fixed::from_raw(kScavengerSpeedRaw), Fixed::zero(), 0, 0,
     Fixed::zero(), Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false},
    // The recycler: unarmed like the scavenger (`range` zero, and the ballistics block with it).
    // Its hp and speed are BORROWED, not designed: the tank's hull (`kRecyclerHp`), so a base does
    // not die too easily, and the hauler's speed, so it can reach a deploy site. Its own numbers
    // are Harri's to set in a balance pass.
    {"recycler", kRecyclerHp, Fixed::from_raw(kScavengerSpeedRaw), Fixed::zero(), 0, 0,
     Fixed::zero(), Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false},
    // The deployed recycler (DEPLOY): a building. Speed zero (it never moves), unarmed (no range,
    // damage or cooldown), and the recycler's hull, `kDeployedRecyclerHp`, a default of his.
    {"deployedrecycler", kDeployedRecyclerHp, Fixed::zero(), Fixed::zero(), 0, 0, Fixed::zero(),
     Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false},
    // The scout (VIS): light armour and fast, with the tank's bolt as a minigun -- half the tank's
    // hp, half again its speed (the fastest vehicle here: it has to scout), a 10-tile reach, half
    // the damage rounded up, a third of the cooldown, and the tank's straight single-target bolt.
    // Outgunned by any tank, but it shoots back. Every number is DERIVED from the tank's and is
    // provisional: its own numbers are Harri's to set in a balance pass. Not buildable: the build
    // menu (RB3) is what fills that flag, and its display-only `SCOUT` row does not.
    {"scout", kTankHp / 2, Fixed::from_raw(kTankSpeedRaw * 3 / 2), Fixed::from_raw(kScoutRangeRaw),
     (kTankDamage + 1) / 2, kTankCooldownTicks / 3, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw), kBoltTravel, Fixed::zero(), Fixed::zero(), false},
}};

[[nodiscard]] constexpr const UnitSpec& spec_of(UnitKind kind) noexcept {
    return kUnitSpecs.at(static_cast<std::size_t>(kind));
}

} // namespace protekt::sim
