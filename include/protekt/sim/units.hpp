#pragma once

#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/world/world.hpp"

namespace protekt::sim {

inline constexpr int kMapTiles = 256; ///< the playable world is [0, kMapTiles) tiles square
/// A tick counter. Named once so the sim, orders, replays and observers cannot drift apart on it.
using Tick = std::uint32_t;

inline constexpr int kTicksPerSecond = 30;

enum class UnitKind : std::uint8_t {
    Tank = 0,
    Turret = 1,
    Factory = 2,
    HoverTank = 3,
    Mortar = 4, ///< lobs a splashing shell over hills; outranged by a turret
    Rocket = 5  ///< slow, heavy, single-target
};
inline constexpr int kUnitKindCount = 6;

/// Tanks and hover tanks: every rule written "for a tank" (orders, retreat, defend, swap, the AI's
/// hunters) takes both. The hover tank has the tank's profile and, until it is drivable, its
/// behaviour.
[[nodiscard]] constexpr bool is_tank(UnitKind kind) noexcept {
    return kind == UnitKind::Tank || kind == UnitKind::HoverTank;
}

enum class Side : std::uint8_t { Player = 0, Enemy = 1 };

/// What a unit is currently doing. Orders set a stance; the sim advances it.
enum class Stance : std::uint8_t {
    Idle = 0,
    MoveTo = 1,
    GuardAt = 2,
    Patrol = 3,
    Attack = 4,
    Dead = 5,
    Follow = 6 ///< appended after Dead, so every recorded stance value (and hash) is unchanged
};

struct UnitId {
    std::uint32_t value{}; ///< 0 means "none"
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};
[[nodiscard]] constexpr bool operator==(UnitId a, UnitId b) noexcept { return a.value == b.value; }
[[nodiscard]] constexpr bool operator!=(UnitId a, UnitId b) noexcept { return a.value != b.value; }
[[nodiscard]] constexpr bool operator<(UnitId a, UnitId b) noexcept { return a.value < b.value; }

struct Unit {
    UnitId id{};
    UnitKind kind{};
    Side side{};
    Fixed x{};
    Fixed y{};
    Fixed vx{};
    Fixed vy{};
    std::uint16_t hp{};
    std::uint16_t cooldown{}; ///< ticks until this unit may fire again
    Stance stance{};
    UnitId target{}; ///< Attack: the unit being hunted; Follow: the unit being shadowed
    /// FocusFire: what the trigger named, if anything. Preferred over both the Attack stance and
    /// auto-engagement while it is alive and in range.
    UnitId focus_target{};
    /// The obstacle this unit is working around, if any. State, not a decision: a one-tick escape
    /// cancels itself out, because steering re-aims at the goal every tick.
    Detour detour{};
    Fixed guard_x{}; ///< GuardAt: the post to hold
    Fixed guard_y{};
    Fixed patrol_x{}; ///< Patrol: the far end of the beat (the near end is the post)
    Fixed patrol_y{};
    /// The aim estimate: how far ahead of its target this unit's gun believes it should aim,
    /// learned from where its own earlier bolts missed (see `launch` and `advance_projectiles`).
    /// Zero means "aim where the target is". It is the SHOOTER's one belief, held about
    /// `aim_target` alone, and it is hashed: it changes what the next bolt does.
    Fixed aim_x{};
    Fixed aim_y{};
    UnitId aim_target{}; ///< whom the estimate was learned against; none when it is zero
    /// Ticks since this unit last moved, saturating at kMountTicks. Only a Turret's is ever
    /// advanced (see `is_mounted`); it is hashed because it decides whether the gun may fire.
    std::uint16_t still_ticks{};
    bool alive{true};
};

/// A turret is a DEPLOYABLE weapon: it crawls unmounted and cannot fire, and it mounts after this
/// many ticks (3 s) of unbroken stillness. Moving unmounts it at once.
inline constexpr std::uint16_t kMountTicks = 90;

/// True when the gun may fire. Every other kind fires on the move, so it is always "mounted".
[[nodiscard]] constexpr bool is_mounted(const Unit& unit) noexcept {
    return unit.kind != UnitKind::Turret || unit.still_ticks >= kMountTicks;
}

/// Frozen balance (M2). These live in the contract on purpose: a test asserts "a tank shot
/// costs a factory 8 hit points", and that must be a property of the game, not of whatever the
/// implementation happened to pick.
inline constexpr std::uint16_t kTankHp = 100;
inline constexpr std::uint16_t kTurretHp = 150;
inline constexpr std::uint16_t kFactoryHp = 400;
/// A follower holds within this many tiles of the unit it follows, and closes in when outside it.
inline constexpr int kFollowRadiusTiles = 4;
inline constexpr int kRepairRadiusTiles = 6;  ///< a living friendly factory mends units this close
inline constexpr int kRepairTickInterval = 6; ///< ...by 1 hp every this many ticks
inline constexpr std::uint16_t kTankDamage = 8;
inline constexpr std::uint16_t kTurretDamage = 12;
inline constexpr std::uint16_t kTankCooldownTicks = 15;
inline constexpr std::uint16_t kTurretCooldownTicks = 20;
inline constexpr std::int32_t kTankSpeedRaw = 13107; ///< 0.2 tiles/tick = 6 tiles/second
/// A turret crawls at a quarter of tank speed (0.05 tiles/tick = 1.5 tiles/second): a defense that
/// can be repositioned, not a rolling wall of guns.
inline constexpr std::int32_t kTurretSpeedRaw = kTankSpeedRaw / 4;
/// A strafe slides at this fraction of the unit's speed (3/4), in integer parts.
inline constexpr std::int64_t kStrafeSpeedNumerator = 3;
inline constexpr std::int64_t kStrafeSpeedDenominator = 4;
inline constexpr std::int32_t kTankRangeRaw = 12 << 16; ///< 12 tiles
inline constexpr std::int32_t kTurretRangeRaw = 16 << 16;
inline constexpr std::int32_t kArriveRadiusRaw = 1 << 16; ///< 1 tile

/// Per-kind constants, exposed so that the tests (and later the balance pass) can assert on
/// them instead of restating them.
struct UnitProfile {
    std::uint16_t hp{};
    Fixed max_speed{};    ///< tiles per tick
    Fixed weapon_range{}; ///< tiles; zero means unarmed
    std::uint16_t damage{};
    std::uint16_t cooldown_ticks{};
    bool mobile{};
};
[[nodiscard]] UnitProfile profile(UnitKind kind) noexcept;

/// 1 = full speed on Flat, 0 = cannot enter. Craters are impassable, ridges are slow.
[[nodiscard]] Fixed terrain_speed_factor(world::Terrain terrain) noexcept;

} // namespace protekt::sim
