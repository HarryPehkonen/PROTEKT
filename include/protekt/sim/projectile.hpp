#pragma once

#include <cstdint>
#include <vector>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// Whole tiles per second. Chosen so that a shot at the outer edge of the longest weapon range (a
/// turret's 16 tiles) takes one second to arrive; a tank's 12 tiles takes three quarters of one.
inline constexpr std::int32_t kProjectileSpeed = 16;
/// The same speed per tick, in raw Q16.16: the unit the sim actually moves a bolt in.
inline constexpr std::int32_t kProjectileSpeedRaw
    = kProjectileSpeed * Fixed::kOne / kTicksPerSecond;
/// How far a bolt flies before it fizzles. Equal to the longest `weapon_range` of any kind (the
/// turret's), so a bolt always outlives the reach of the gun that fired it: a shot at the edge of a
/// tank's 12 tiles that misses flies on four tiles further, rather than vanishing where it was
/// aimed. It is a distance flown, not a distance from the shooter.
inline constexpr Fixed kProjectileRange = Fixed::from_raw(kTurretRangeRaw);
/// A bolt hits a unit whose centre its path passes within this many tiles of.
inline constexpr Fixed kProjectileHitRadius = Fixed::from_raw(Fixed::kOne / 2);

/// A shot in the air. Not a unit: it has no id in the unit table, cannot be ordered or targeted,
/// and blocks neither movement nor sight. The sim owns it and moves it in a straight line in the
/// map plane -- no arc, no gravity.
struct Projectile {
    Fixed x{};
    Fixed y{};
    Fixed vx{}; ///< tiles per tick
    Fixed vy{};
    UnitId shooter{};
    Side side{};
    /// The unit the gun was aimed at when it fired. A bolt passes through its own side, EXCEPT this
    /// one: an explicit order to shoot a friendly (friendly fire) has to be able to land. The bolt
    /// does not follow it -- this names who it may hurt, not where it flies.
    UnitId aimed_at{};
    std::uint16_t damage{};
    std::uint32_t age{}; ///< ticks in the air
};
[[nodiscard]] bool operator==(const Projectile& a, const Projectile& b) noexcept;

/// A bolt that landed during the last step: the raw material for a HitTaken event.
struct Impact {
    UnitId shooter{};
    UnitId victim{};
    std::uint16_t victim_hp{}; ///< after the hit
};

/// floor(sqrt(n)) for n >= 0, by the bitwise method: no floating point, no loop-count surprises.
[[nodiscard]] std::int64_t isqrt(std::int64_t n) noexcept;

/// The bolt `shooter` fires at `target` now: aimed at where the target WILL be, times
/// kProjectileSpeed, PLUS the shooter's own velocity -- so a moving gun sends its shot sideways as
/// a real one goes. The prediction is made once: flight time is the present distance over the
/// bolt's speed (at least one tick, at most one second), and the aim point is the target's position
/// plus its velocity times that. Only the target's motion is led, never the shooter's. The bolt is
/// then free-flying: it does not track, so a target that changes course is missed. Starts at the
/// shooter's position, age zero; `aimed_at` names who it may hurt, not where it points.
[[nodiscard]] Projectile launch(const Unit& shooter, const Unit& target,
                                std::uint16_t damage) noexcept;

/// One tick of flight for every projectile, in index order. Each moves by its velocity; the first
/// living unit on the other side (or the one it was aimed at) whose centre the path passes within
/// kProjectileHitRadius of -- nearest, ties to the lowest id -- takes its damage and the bolt is
/// removed. A bolt that has flown kProjectileRange, or left the map, is removed with no effect.
/// `units` is mutated (hit points, death); each landing is appended to `impacts`.
void advance_projectiles(std::vector<Projectile>& projectiles, std::vector<Unit>& units,
                         std::vector<Impact>& impacts) noexcept;

} // namespace protekt::sim
