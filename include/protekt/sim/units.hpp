#pragma once

#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/world/world.hpp"

namespace protekt::sim {

inline constexpr int kMapTiles = 256; ///< the playable world is [0, kMapTiles) tiles square
/// A tick counter. Named once so the sim, orders, replays and observers cannot drift apart on it.
using Tick = std::uint32_t;

inline constexpr int kTicksPerSecond = 30;

enum class UnitKind : std::uint8_t { Tank = 0, Turret = 1, Factory = 2 };
inline constexpr int kUnitKindCount = 3;

enum class Side : std::uint8_t { Player = 0, Enemy = 1 };

/// What a unit is currently doing. Orders set a stance; the sim advances it.
enum class Stance : std::uint8_t {
    Idle = 0,
    MoveTo = 1,
    GuardAt = 2,
    Patrol = 3,
    Attack = 4,
    Dead = 5
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
    UnitId target{}; ///< Attack: the unit being hunted
    Fixed guard_x{}; ///< GuardAt: the post to hold
    Fixed guard_y{};
    Fixed patrol_x{}; ///< Patrol: the far end of the beat (the near end is the post)
    Fixed patrol_y{};
    bool alive{true};
};

/// Frozen balance (M2). These live in the contract on purpose: a test asserts "a tank shot
/// costs a factory 8 hit points", and that must be a property of the game, not of whatever the
/// implementation happened to pick.
inline constexpr std::uint16_t kTankHp = 100;
inline constexpr std::uint16_t kTurretHp = 150;
inline constexpr std::uint16_t kFactoryHp = 400;
inline constexpr std::uint16_t kTankDamage = 8;
inline constexpr std::uint16_t kTurretDamage = 12;
inline constexpr std::uint16_t kTankCooldownTicks = 15;
inline constexpr std::uint16_t kTurretCooldownTicks = 20;
inline constexpr std::int32_t kTankSpeedRaw = 13107;    ///< 0.2 tiles/tick = 6 tiles/second
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
