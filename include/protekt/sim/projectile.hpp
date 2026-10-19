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

/// World heights are whole units of a 64th of a tile (`world::Tile::height`); a bolt's height and
/// vertical speed are in the same units, in Q16.16.
inline constexpr std::int32_t kHeightUnitsPerTile = 64;
/// How high above the ground under it a unit's body starts (its hover) and how tall it is, in
/// height units: 0.25 m and 1.8 m (4.5 m for a factory) at 32 units to the metre, which is how the
/// scene draws them. A bolt hurts a unit only while it is between the two.
inline constexpr Fixed kRideHeight = Fixed::from_raw(8 * Fixed::kOne);
inline constexpr Fixed kBodyHeight = Fixed::from_raw(576 * Fixed::kOne / 10); ///< 57.6 units
inline constexpr Fixed kFactoryBodyHeight = Fixed::from_raw(144 * Fixed::kOne);
/// Where a gun's bolt leaves it, above the ground under the gun: the middle of a tank's body, so a
/// tank shooting a tank on level ground shoots level.
inline constexpr Fixed kMuzzleHeight = Fixed::from_raw(kRideHeight.raw + kBodyHeight.raw / 2);

/// A shot in the air. Not a unit: it has no id in the unit table, cannot be ordered or targeted,
/// and blocks neither movement nor sight. The sim owns it and moves it in a straight line in the
/// straight line through space -- no arc, no gravity. It has its OWN height: it does not follow the
/// ground it flies over, and it ends where it meets the ground.
struct Projectile {
    Fixed x{};
    Fixed y{};
    Fixed z{};  ///< height above the terrain datum, in height units (see kHeightUnitsPerTile)
    Fixed vx{}; ///< tiles per tick
    Fixed vy{};
    Fixed vz{}; ///< height units per tick
    UnitId shooter{};
    Side side{};
    /// The unit the gun was aimed at when it fired. A bolt passes through its own side, EXCEPT this
    /// one: an explicit order to shoot a friendly (friendly fire) has to be able to land. The bolt
    /// does not follow it -- this names who it may hurt, not where it flies.
    UnitId aimed_at{};
    std::uint16_t damage{};
    std::uint32_t age{}; ///< ticks in the air
    /// True while a bolt the sim aimed (`launch`) still has a lesson to report: the moment it has
    /// PASSED its target it reports where it missed back to its shooter, and this goes false, so a
    /// bolt teaches once. A player's manual shot (`launch_towards`) is never true, and teaches
    /// nobody anything.
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

/// The height of the ground at a point, in height units, Q16.16: the SAME surface the scene draws
/// (two planes per tile, split along the p00-p11 diagonal), so a bolt that meets the ground in the
/// sim meets it on screen. Integer arithmetic only. Pure in `(seed, x, y)`.
[[nodiscard]] Fixed ground_height(std::uint64_t seed, Fixed x, Fixed y) noexcept;

/// The top of a unit's body above the ground under it, and the bottom: a unit stands on the ground
/// plus its ride height, and is as tall as its kind.
[[nodiscard]] Fixed body_bottom(UnitKind kind) noexcept;
[[nodiscard]] Fixed body_top(UnitKind kind) noexcept;

/// The bolt `shooter` fires at `target` now, aimed at the target's position PLUS the shooter's aim
/// estimate (`aim_x`, `aim_y`) -- not at where the target is going. A fresh gun, whose estimate is
/// zero, aims where the target IS and misses anything that moves; the estimate is learned
/// afterwards from where the bolt missed. It takes the shooter's estimate rather than an aim point
/// so that there is one place the lead comes from; the caller calls `retarget_aim` first, so the
/// estimate is about this target. Plus the shooter's own velocity, so a moving gun sends its shot
/// sideways as a real one goes. The bolt then free-flies: it does not track. Starts at the
/// shooter's position, age zero; `aimed_at` names who it may hurt and whose miss it measures.
///
/// It leaves the gun at kMuzzleHeight above the ground under it and is aimed vertically at the
/// middle of a body standing on the ground under the aim point: the target's REAL height. A
/// target's height does not evade, so there is no vertical estimate -- it follows from the
/// horizontal one -- and a gun on level ground still hits a target in a crater or on a ridge. The
/// horizontal speed is exactly what it was before bolts had a height; the vertical speed is added
/// to it, so on a slope the bolt is a little faster than kProjectileSpeed in total.
[[nodiscard]] Projectile launch(const Unit& shooter, const Unit& target, std::uint16_t damage,
                                std::uint64_t seed) noexcept;

/// The bolt `shooter` fires along (`dir_x`, `dir_y`, `dir_z`) and nowhere else: the direction
/// normalised, times kProjectileSpeed, PLUS the shooter's own velocity (map plane only) exactly as
/// `launch` does. `dir_z` is up: it is the vertical component of the same world-space direction
/// (map x, map y, height), so a pitch of p is (sin yaw cos p, cos yaw cos p, sin p) and the total
/// speed does not depend on it. A zero direction has none to normalise and goes due east, level, as
/// `launch` does. The bolt leaves at kMuzzleHeight above the ground under the shooter. There is no
/// lead and no centring: `aimed_at` is carried onto the bolt as INTENT -- it lets the bolt hurt
/// that unit even on the shooter's own side -- and never bends the path, so a bolt aimed at a unit
/// it is not pointing at misses it. An invalid `aimed_at` passes through its own side. It never
/// follows anything.
[[nodiscard]] Projectile launch_towards(const Unit& shooter, Fixed dir_x, Fixed dir_y, Fixed dir_z,
                                        UnitId aimed_at, std::uint16_t damage,
                                        std::uint64_t seed) noexcept;

/// One tick of flight for every projectile, in index order. Each moves by its velocity; the first
/// living unit on the other side (or the one it was aimed at) whose centre the path passes within
/// kProjectileHitRadius of in the map plane AND whose body (`body_bottom`..`body_top` above the
/// ground it stands on) the bolt is level with at that moment -- nearest, ties to the lowest id --
/// takes its damage and the bolt is removed. A shot aimed over a unit's head passes over it. A bolt
/// that meets the ground is removed where it meets it, with no damage and no mark; one that has
/// flown kProjectileRange, or left the map, is removed with no effect.
/// While a sim-aimed bolt (`learns`) flies it tracks its closest approach to its `aimed_at` unit;
/// the moment it has PASSED that unit -- the distance has stopped falling -- its shooter adds
/// `target - bolt` at that closest moment to its aim estimate (clamped to kMaxAimLeadRaw), once,
/// without waiting for the bolt to expire -- unless the shooter is dead or has since moved on to
/// another target, in which case the lesson is dropped. A bolt that expires un-passed (its target
/// died, say) still reports at the end if it has not already; a bolt that lands, or stops at the
/// ground before it has passed, corrects nothing at all.
/// `units` is mutated (hit points, death); each landing is appended to `impacts`. `seed` is the
/// world's: it says where the ground is.
void advance_projectiles(std::vector<Projectile>& projectiles, std::vector<Unit>& units,
                         std::vector<Impact>& impacts, std::uint64_t seed) noexcept;

} // namespace protekt::sim
