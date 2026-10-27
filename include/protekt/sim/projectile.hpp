#pragma once

#include <cstdint>
#include <vector>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// Forward-declared: a launch may take a WEAPON's spec row (`spec.hpp`) instead of its shooter's
/// kind's. Declared here as an incomplete type so this header does not have to include the spec
/// table, which includes this header.
struct UnitSpec;

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
    /// The unit the gun was aimed at when it fired, whose miss it measures. A bolt hurts any living
    /// unit it meets, its own side included (friendly fire, Harri's ruling); this does not name
    /// who it may hurt, only whose approach to track. It never bends the path.
    UnitId aimed_at{};
    /// Height units per tick squared that `vz` loses every tick, Q16.16. Zero is a straight flight;
    /// a mortar shell's is not, and its launch solved `vz` so the parabola ends at the aim point.
    Fixed gravity{};
    /// Tiles around the end of the flight in which every living unit, either side, takes `damage`;
    /// zero is single-target.
    Fixed splash_radius{};
    /// How far the bolt flies before it fizzles: the distance flown for a straight bolt, the
    /// distance flown ACROSS THE MAP for one with gravity (its speed is not constant).
    Fixed max_travel{kProjectileRange};
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
    /// The bouncer's state (M3). All zero for a contact shell, and hashed only when `round` is not
    /// Contact. `damage` is a bouncer's pulse damage at the centre.
    Round round{Round::Contact};
    std::uint32_t bounces{}; ///< contacts it has bounced off
    bool resting{};          ///< it has come to rest and spins, pulsing
    std::uint32_t rested{};  ///< ticks it has spent at rest
    UnitId bounced_off{};    ///< the unit it last bounced off, which it does not bounce off twice
};
[[nodiscard]] bool operator==(const Projectile& a, const Projectile& b) noexcept;

/// A bolt that landed during the last step: the raw material for a HitTaken event. A splash victim
/// is reported like a direct hit.
struct Impact {
    UnitId shooter{};
    UnitId victim{};
    std::uint16_t victim_hp{}; ///< after the hit
};

/// A shell that came down during the last step, and where: the tile it touched down on, whether a
/// unit was under it, and whether it was a lob. OBSERVATION OUTPUT, NOT SIMULATION STATE -- it is
/// rebuilt by every step, never hashed, and nothing in the sim reads it. It exists for the shell
/// that lands on EMPTY GROUND, which makes no `Impact` and so would otherwise leave no trace.
struct Landing {
    UnitId shooter{};
    std::int32_t tile_x{};
    std::int32_t tile_y{};
    bool hit{}; ///< it burst on a unit rather than on bare ground
    bool lob{}; ///< it had gravity (a mortar shell), not a straight bolt
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
/// shooter's position, age zero; `aimed_at` names whose miss it measures.
///
/// It leaves the gun at kMuzzleHeight above the ground under it and is aimed vertically at the
/// middle of a body standing on the ground under the aim point: the target's REAL height. A
/// target's height does not evade, so there is no vertical estimate -- it follows from the
/// horizontal one -- and a gun on level ground still hits a target in a crater or on a ridge. The
/// horizontal speed is exactly what it was before bolts had a height; the vertical speed is added
/// to it, so on a slope the bolt is a little faster than kProjectileSpeed in total.
[[nodiscard]] Projectile launch(const Unit& shooter, const Unit& target, std::uint16_t damage,
                                std::uint64_t seed) noexcept;

/// Can a gun at (`from_x`, `from_y`) see some part of a body of `kind` standing at (`at_x`,
/// `at_y`)? The very test `launch` runs to choose its aim point: a line from the muzzle to the
/// middle of the body, or failing that a little higher, or higher still, that stays clear of the
/// ground. No line to any of them means a bolt fired there lands in the hill. Pure in its
/// arguments; no tank, no range, only the ground.
[[nodiscard]] bool can_see(std::uint64_t seed, Fixed from_x, Fixed from_y, Fixed at_x, Fixed at_y,
                           UnitKind kind) noexcept;

/// `can_see` for two real units, at the point `launch` would aim at: the target's position plus the
/// shooter's aim estimate. A gun that would fire into a hill has no line.
[[nodiscard]] bool has_line(const Unit& shooter, const Unit& target, std::uint64_t seed) noexcept;

/// The bolt `shooter` fires along (`dir_x`, `dir_y`, `dir_z`) and nowhere else: the direction
/// normalised, times kProjectileSpeed, PLUS the shooter's own velocity (map plane only) exactly as
/// `launch` does. `dir_z` is up: it is the vertical component of the same world-space direction
/// (map x, map y, height), so a pitch of p is (sin yaw cos p, cos yaw cos p, sin p) and the total
/// speed does not depend on it. A zero direction has none to normalise and goes due east, level, as
/// `launch` does. The bolt leaves at kMuzzleHeight above the ground under the shooter. There is no
/// lead and no centring: `aimed_at` is carried onto the bolt as INTENT and never bends the path,
/// so a bolt aimed at a unit it is not pointing at misses it. Friendly fire is the flight's rule,
/// not the intent's: the bolt hurts its own side's units in its way, named or not. It never follows
/// anything.
[[nodiscard]] Projectile launch_towards(const Unit& shooter, Fixed dir_x, Fixed dir_y, Fixed dir_z,
                                        UnitId aimed_at, std::uint16_t damage,
                                        std::uint64_t seed) noexcept;

/// The same launch with the WEAPON's spec instead of the shooter's kind's: the ballistics, gravity,
/// splash and travel all come from `spec`, so a carried weapon's shot does not inherit the hull's
/// numbers. This is what `orders::FireFan` fires with (`kShotgunSpec`); the overload above is the
/// old path, `spec_of(shooter.kind)`.
[[nodiscard]] Projectile launch_towards(const Unit& shooter, Fixed dir_x, Fixed dir_y, Fixed dir_z,
                                        UnitId aimed_at, std::uint16_t damage, std::uint64_t seed,
                                        const UnitSpec& spec) noexcept;

/// Appends the pellets of one shot to `into`: when `pellets` is more than one, the aimed `bolt` is
/// replaced by that many copies of it, each turned about the map plane to its place in the cone
/// (`pellet_fan`) at the same horizontal speed, with the same climb, damage and range; otherwise
/// `bolt` itself, untouched. The numbers are passed rather than looked up by kind, because a
/// carried weapon's fan belongs to the WEAPON (`kShotgunSpec`), not to the hull that carries it.
/// A pure function of the bolt and those two numbers -- no RNG, no state -- so a replay fires the
/// same pellets byte for byte. Pellets do not `learn`: simultaneous misses would teach the
/// shooter's aim estimate one lesson each, and a scatter gun is not aimed with lead. Order is the
/// fan's, left to right of the aim.
void fan_out(const Projectile& bolt, std::uint8_t pellets, Fixed cone_half_tan,
             std::vector<Projectile>& into) noexcept;

/// THE MORTAR'S CLIMB, the one place it is computed: the vertical speed (height units a tick) of a
/// mortar shell that rises `rise` over a horizontal run of `run` (both Q16.16, any common scale;
/// `run` > 0), leaving at the mortar spec's `bolt_speed` across the map -- `tan(pitch)` tiles of
/// climb per tile of run -- and capped at the spec's `arc`. Both `launch_lob` and the player's
/// range ladder (`mortar_landing_tiles`) call it, so the sight cannot disagree with the shot.
[[nodiscard]] std::int64_t mortar_climb(std::int64_t run, std::int64_t rise) noexcept;

/// The mortar shell `shooter` lobs along (`dir_x`, `dir_y`) with `dir_z` of climb per unit of
/// horizontal run (the same world-space direction as `launch_towards`; it need not be normalised):
/// the MORTAR's spec ballistics whatever `shooter` is, from the muzzle, no aim solve, no lead and
/// no shooter velocity. A zero horizontal direction goes due east at the arc cap up or down.
[[nodiscard]] Projectile launch_lob(const Unit& shooter, Fixed dir_x, Fixed dir_y, Fixed dir_z,
                                    UnitId aimed_at, std::uint16_t damage, std::uint64_t seed,
                                    Round round = Round::Contact) noexcept;

/// One tick of flight for every projectile, in index order. Each moves by its velocity; the first
/// living unit on the other side (or the one it was aimed at) whose centre the path passes within
/// kProjectileHitRadius of in the map plane AND whose body (`body_bottom`..`body_top` above the
/// ground it stands on) the bolt is level with at that moment -- nearest, ties to the lowest id --
/// takes its damage and the bolt is removed. A shot aimed over a unit's head passes over it. A bolt
/// that meets the ground is removed where it meets it, with no damage and no mark; one that has
/// flown its max_travel, or left the map, is removed with no effect. A bolt with gravity loses
/// `gravity` of vertical speed after every step. When a bolt with a splash radius ends on the
/// ground or on a unit, every other living unit of EITHER side whose centre is within the radius of
/// the impact point takes its damage too, in ascending id (the shooter's side included: friendly
/// fire is real); each is reported as an Impact, after the direct hit. While a sim-aimed bolt
/// (`learns`) flies it tracks its closest approach to its `aimed_at` unit; the moment it has PASSED
/// that unit -- the distance has stopped falling -- its shooter adds `target - bolt` at that
/// closest moment to its aim estimate (clamped to kMaxAimLeadRaw), once, without waiting for the
/// bolt to expire -- unless the shooter is dead or has since moved on to another target, in which
/// case the lesson is dropped. A bolt that expires un-passed (its target died, say) still reports
/// at the end if it has not already; a bolt that lands, or stops at the ground before it has
/// passed, corrects nothing at all. `units` is mutated (hit points, death); each landing is
/// appended to `impacts`. `seed` is the world's: it says where the ground is.
void advance_projectiles(std::vector<Projectile>& projectiles, std::vector<Unit>& units,
                         std::vector<Impact>& impacts, std::uint64_t seed) noexcept;

/// The same flight, and every shell that comes down -- on bare ground or on a unit -- is also
/// appended to `landings`. Observation output: it changes nothing about the flight.
void advance_projectiles(std::vector<Projectile>& projectiles, std::vector<Unit>& units,
                         std::vector<Impact>& impacts, std::vector<Landing>& landings,
                         std::uint64_t seed) noexcept;

} // namespace protekt::sim
