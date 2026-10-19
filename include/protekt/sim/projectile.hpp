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

/// The longest aim estimate, in raw tiles: five. Full-speed tanks cross at 0.2 tiles a tick, so a
/// bolt over a twelve-tile tank shot (0.75 s) needs 4.5 tiles of lead to hit one: a quarter of
/// kProjectileRange (4) would leave that target permanently missed by half a tile, which is the hit
/// radius -- the gun would never converge on the fastest thing it can face at the range it is
/// allowed to fire from. Five covers it with a little margin, and stays well short of the 6 tiles a
/// full-speed target at a turret's 16 tiles would need: that one stays evasive, which is intended.
/// It also bounds a pathological history: one wild miss cannot send the aim point across the map.
inline constexpr std::int32_t kMaxAimLeadRaw = 5 * Fixed::kOne;

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
    /// True for a bolt the sim aimed (`launch`): if it misses, it reports where back to its
    /// shooter. A player's manual shot (`launch_towards`) is false, and teaches nobody anything.
    bool learns{};
    /// Closest approach to the unit named by `aimed_at` so far: the squared distance, and the
    /// vector from the bolt to that unit then. Meaningful once `closest_d2` is below its start.
    std::int64_t closest_d2{INT64_MAX};
    Fixed miss_x{};
    Fixed miss_y{};
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

/// Makes `target` the one the shooter's aim estimate is about. A different unit (or none) throws
/// the estimate away: the shooter holds one belief at a time, so switching targets costs a miss, on
/// purpose. The same target changes nothing.
void retarget_aim(Unit& shooter, UnitId target) noexcept;

/// The bolt `shooter` fires at `target` now, aimed at the target's position PLUS the shooter's aim
/// estimate (`aim_x`, `aim_y`) -- not at where the target is going. A fresh gun, whose estimate is
/// zero, aims where the target IS and misses anything that moves; the estimate is learned
/// afterwards from where the bolt missed. It takes the shooter's estimate rather than an aim point
/// so that there is one place the lead comes from; the caller calls `retarget_aim` first, so the
/// estimate is about this target. Plus the shooter's own velocity, so a moving gun sends its shot
/// sideways as a real one goes. The bolt then free-flies: it does not track. Starts at the
/// shooter's position, age zero; `aimed_at` names who it may hurt and whose miss it measures.
[[nodiscard]] Projectile launch(const Unit& shooter, const Unit& target,
                                std::uint16_t damage) noexcept;

/// The bolt `shooter` fires along (`dir_x`, `dir_y`) and nowhere else: the direction normalised,
/// times kProjectileSpeed, PLUS the shooter's own velocity exactly as `launch` does. A zero
/// direction has none to normalise and goes due east, as `launch` does. There is no lead and no
/// centring: `aimed_at` is carried onto the bolt as INTENT -- it lets the bolt hurt that unit even
/// on the shooter's own side -- and never bends the path, so a bolt aimed at a unit it is not
/// pointing at misses it. An invalid `aimed_at` passes through its own side. It never follows
/// anything.
[[nodiscard]] Projectile launch_towards(const Unit& shooter, Fixed dir_x, Fixed dir_y,
                                        UnitId aimed_at, std::uint16_t damage) noexcept;

/// One tick of flight for every projectile, in index order. Each moves by its velocity; the first
/// living unit on the other side (or the one it was aimed at) whose centre the path passes within
/// kProjectileHitRadius of -- nearest, ties to the lowest id -- takes its damage and the bolt is
/// removed. A bolt that has flown kProjectileRange, or left the map, is removed with no effect.
/// While a sim-aimed bolt (`learns`) flies it tracks its closest approach to its `aimed_at` unit;
/// when it fizzles, its shooter adds `target - bolt` at that closest moment to its aim estimate
/// (clamped to kMaxAimLeadRaw) -- unless the shooter is dead or has since moved on to another
/// target, in which case the lesson is dropped. A bolt that lands corrects nothing at all: it was
/// within the hit radius, and the segment's end is not a miss, so a hit leaves the estimate exactly
/// as it was.
/// `units` is mutated (hit points, death); each landing is appended to `impacts`.
void advance_projectiles(std::vector<Projectile>& projectiles, std::vector<Unit>& units,
                         std::vector<Impact>& impacts) noexcept;

} // namespace protekt::sim
