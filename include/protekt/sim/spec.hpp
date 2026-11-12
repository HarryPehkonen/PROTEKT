#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// THE WEAPON AXIS: what a unit shoots with. RANGE BELONGS TO THE WEAPON, not to the hull that
/// carries it (Harri, 2026-11-03: "'Rocket tank' has a long range because it has ROCKETS, not
/// because it's a rocket tank. Other vehicles that use rockets also get the same range."). A kind's
/// row names its weapon and nothing else about reach; `UnitSpec::range()` reads the weapon, so two
/// kinds carrying one weapon cannot disagree.
enum class WeaponId : std::uint8_t {
    None,      ///< unarmed: range zero is what makes a kind unarmed everywhere in the sim
    TankGun,   ///< the tank's cannon: tank, hover tank, assault tank
    TurretGun, ///< the turret's cannon: the turret, deployed or not
    Minigun,   ///< the light gun: scout, APC
    Mortar,    ///< the lobbed shell: the mortar bike, the bomber's bomb
    Rocket,    ///< the rocket: the rocket tank, the missile scout, and a rocket carried by any hull
    Shotgun,   ///< the carried shotgun
    WalkerGun, ///< the Walker's heavy cannon: the longest reach of any vehicle (APPENDED: no id
               ///< moved)
};
inline constexpr std::size_t kWeaponCount = 8;

/// THE RANGES, each authored once and nowhere else. Tiles, raw Q16.16.
inline constexpr std::int32_t kMortarRangeRaw = 14 << 16;
/// The scout's minigun: a reach of 10 tiles, below the tank's 12 (`kTankRangeRaw`).
inline constexpr std::int32_t kMinigunRangeRaw = kTankRangeRaw * 5 / 6;
/// The rocket: 18 tiles, the longest reach in the game, past the turret's 16 (Harri, 2026-10-27:
/// "can the rocket have more reach? I know that's cheating, but I want to try that"). Every
/// vehicle that fires a rocket reaches this far.
inline constexpr std::int32_t kRocketRangeRaw = 18 << 16;
/// The shotgun's 6 tiles, where the fan is still a few tiles wide.
inline constexpr std::int32_t kShotgunRangeRaw = 6 << 16;

/// THE WALKER'S GUN: 20 tiles, past the rocket's 18, the mortar's 14 and the tank's 12 -- "a very
/// powerful and far-reaching weapon" (Harri, 2026-11-10). Its own weapon, not the tank's: the
/// tank's gun is untouched and every tank reaches 12 as before. PROVISIONAL, a balance number.
inline constexpr std::int32_t kWalkerGunRangeRaw = 20 << 16;
/// How far the Walker's bolt flies: two tiles past its reach, so it does not fizzle short of it.
inline constexpr std::int32_t kWalkerGunTravelRaw = 22 << 16;

/// One weapon's reach. Indexed by the value of `WeaponId`.
struct WeaponSpec {
    const char* name{};
    Fixed range{}; ///< tiles; zero means unarmed
};
inline constexpr std::array<WeaponSpec, kWeaponCount> kWeaponSpecs{{
    {"none", Fixed::zero()},
    {"tankgun", Fixed::from_raw(kTankRangeRaw)},
    {"turretgun", Fixed::from_raw(kTurretRangeRaw)},
    {"minigun", Fixed::from_raw(kMinigunRangeRaw)},
    {"mortar", Fixed::from_raw(kMortarRangeRaw)},
    {"rocket", Fixed::from_raw(kRocketRangeRaw)},
    {"shotgun", Fixed::from_raw(kShotgunRangeRaw)},
    {"walkergun", Fixed::from_raw(kWalkerGunRangeRaw)},
}};

[[nodiscard]] constexpr const WeaponSpec& weapon_spec(WeaponId weapon) noexcept {
    return kWeaponSpecs.at(static_cast<std::size_t>(weapon));
}

/// One unit kind's stats, as data. Q16.16 and integers only: this table is read by the tick loop.
struct UnitSpec {
    const char* name{};
    std::uint16_t hp{};
    Fixed max_speed{}; ///< tiles per tick; zero means it never moves
    WeaponId weapon{}; ///< what it shoots with; its reach is the WEAPON's (`range()`)
    std::uint16_t damage{};
    std::uint16_t cooldown{}; ///< ticks between shots
    // The weapon block's ballistics. Zero for an unarmed kind.
    /// Raw tiles per tick. For a flat gun it is the speed in the map plane's direction of aim. For
    /// the MORTAR it is the MUZZLE speed, along the barrel: the aim's elevation splits it between
    /// reach across the map and climb (`mortar_climb`), so the horizontal speed is not constant.
    Fixed bolt_speed{};
    /// The steepest climb a bolt may be launched at: tiles of rise per tile of run (a slope),
    /// Q16.16. The launch aims the bolt straight at the aim point; this only caps how steeply it
    /// may climb. For a flat gun that is `vz` raw <= arc * bolt_speed * kHeightUnitsPerTile, so 2.0
    /// is a 63 degree ceiling. For the mortar the cap is on the ELEVATION, taken as the aim's slope
    /// clamped to `arc` before the muzzle speed is split: the shell leaves at most atan(arc) above
    /// the horizon, so 4.0 is a 76 degree ceiling. (The bolt's `vz` can then never exceed
    /// bolt_speed * kHeightUnitsPerTile, the whole muzzle speed pointed straight up.)
    Fixed arc{};
    /// How far a bolt flies before it fizzles, tiles (see Projectile::max_travel).
    Fixed max_travel{};
    /// Tiles around the end of a flight in which every unit takes the bolt's damage; zero is
    /// single-target.
    Fixed splash_radius{};
    /// Height units per tick squared, Q16.16; zero is a straight flight, a mortar's is a lob.
    Fixed gravity{};
    /// May a side build this kind in play. False for every kind today; the build menu fills it,
    /// and the build queue reads it.
    bool buildable{};
    /// Scrap cost, in the loads C3 counts.
    std::uint16_t cost{};
    /// Ticks spent building.
    std::uint16_t build_ticks{};
    /// Pellets in one shot (the shotgun's shape, with `cone_half_tan` and `damage`, which is
    /// PER PELLET). Zero or one is an ordinary single bolt. At most `kMaxPellets`.
    std::uint8_t pellets{};
    /// The cone's half-angle as a TANGENT, Q16.16 (no trigonometry in the sim): the outermost
    /// pellet leaves at this many tiles of sideways drift per tile flown. `pellet_fan` derives
    /// every pellet from it, so no pellet can leave the cone.
    Fixed cone_half_tan{};
    /// HOW FAST IT TURNS, in tenths of a degree per tick (30 ticks a second). ZERO IS TODAY'S RATE:
    /// no limit, the hull points wherever it is sent at once. Only the Walker is limited (15 = 1.5
    /// degrees a tick, 45 a second, a half turn in four seconds): see `steer`, the one place a
    /// machine's heading is turned. "It moves slowly. That includes turning."
    std::uint16_t turn_decidegrees{};

    /// How far it shoots, tiles; zero means unarmed. Read from the weapon, never authored here.
    [[nodiscard]] constexpr Fixed range() const noexcept { return weapon_spec(weapon).range; }
};

/// Every armed kind today shoots at kProjectileSpeedRaw with a climb ceiling of 2.0.
inline constexpr std::int32_t kBoltArcRaw = 2 * Fixed::kOne;
/// Every kind keeps the engine's old bolt range: the turret's 16 tiles.
inline constexpr Fixed kBoltTravel = kProjectileRange;

/// The mortar bike's mortar: a lob (gravity 2.25 height units per tick squared; peak ~3 tiles over
/// a 7-tile shot, ~12 over its 14), a 1.5-tile splash, two fifths of the tank's hp and 5/4 of its
/// speed (fast and thin, Harri 2026-11-03), twice its damage, three times its cooldown; range 14
/// sits between the tank's 12 and the turret's 16.
inline constexpr std::int32_t kMortarGravityRaw = 9 * Fixed::kOne / 4;
inline constexpr std::int32_t kMortarArcRaw = 4 * Fixed::kOne;
inline constexpr std::int32_t kMortarSplashRaw = 3 * Fixed::kOne / 2;

/// The floor of the square root of `n`, usable in a constant expression (the sim's `isqrt` is not).
[[nodiscard]] constexpr std::int64_t constexpr_isqrt(std::int64_t n) noexcept {
    std::int64_t lo = 0;
    std::int64_t hi = std::int64_t{1} << 31;
    while (lo < hi) {
        const std::int64_t mid = (lo + hi + 1) / 2;
        if (mid * mid <= n) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}
/// THE MORTAR'S MUZZLE SPEED, raw tiles per tick: the shell leaves the barrel at one speed whatever
/// the elevation, and the elevation splits it between reach and climb. Calibrated so a 45 degree
/// shot peaks at the mortar's own range: on level ground the range is v^2/g, so v = sqrt(range *
/// gravity), with gravity in tiles per tick squared (`kMortarGravityRaw` is height units, 64 to the
/// tile). In raw Q16.16 that is sqrt(range_tiles * gravity_raw * 65536 / 64) = sqrt(14 * 147456 *
/// 1024) = 45977.49, floored to 45977: about 21.05 tiles a second.
inline constexpr std::int32_t kMortarMuzzleSpeedRaw = static_cast<std::int32_t>(constexpr_isqrt(
    static_cast<std::int64_t>(kMortarRangeRaw >> 16) * kMortarGravityRaw * Fixed::kOne / 64));
/// THE BOUNCER'S MAX TRAVEL, tiles across the map from the barrel to where it may rest: 18, against
/// the shell's 16 (`kBoltTravel`), which its bounces used to carry it past (~30). Harri's number
/// (2026-10-31: "Give it a separate max travel. Maybe 18 tiles for now."), so PROVISIONAL. The
/// shell that has bounced and would fly further comes to rest where the cap is spent.
inline constexpr Fixed kBouncerTravel = Fixed::from_raw(18 << 16);
/// THE BOUNCER ROUND (M3), all of it data. It leaves the barrel with the mortar's own ballistics,
/// so its FIRST landing is the mortar's; then it bounces, rests, spins and pulses.
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

/// The rocket TANK (`UnitKind::RocketTank`): the tank's hp and speed, the ROCKET's range, three
/// times the damage, five times the cooldown, three quarters of the bolt speed, a straight flight,
/// no splash.

/// THE SHOTGUN (Harri's ruling, 2026-10-27: "the amount of damage depends on vicinity and aim"),
/// a CARRIED WEAPON's numbers rather than a unit kind's (A): it rides the driven craft like the
/// rocket and the mortar, fired through `orders::FireFan`. Seven pellets in a cone whose
/// half-angle has tangent 1/4 (~14 degrees), so neighbouring pellets drift 1/12 of a tile apart
/// per tile flown. A tank is struck by a pellet passing within half a tile of its centre: all
/// seven at 2 tiles, three at 6, one at 8. Vicinity IS the pellet count, with no falloff curve to
/// double-count it. Range 6 is where the fan is still a few tiles wide; travel 8 is the
/// "shortness of range", not a new mechanic. Per-pellet damage 3 (21 point-blank against a tank
/// bolt's 8). FLAT (gravity zero) so a hill eats its pellets. The reload is tripled because a shot
/// is seven bolts. All provisional: its own numbers are Harri's to set in a balance pass. B turns
/// these constants into a weapon table keyed by a weapon id.
inline constexpr std::int32_t kShotgunTravelRaw = 8 << 16;
inline constexpr std::int32_t kShotgunConeRaw = Fixed::kOne / 4;
inline constexpr std::uint16_t kShotgunPellets = 7;
inline constexpr std::uint16_t kShotgunPelletDamage = 3;
/// Ticks the shotgun reloads for: a shot is seven bolts, so three times the tank's cooldown.
inline constexpr std::uint16_t kShotgunReloadTicks = kTankCooldownTicks * 3;

/// THE SHOTGUN AS DATA (A): a weapon's spec, not a unit kind's. It is NOT a row in `kUnitSpecs`
/// and has no `UnitKind`: it belongs to the carried weapon axis, and an order that fires it
/// (`orders::FireFan`) reads this row. Its `range` is 6 tiles for the AI's engagement test; the
/// pellets themselves fly `kShotgunTravelRaw` (8) before they fizzle. Hull and speed are the
/// tank's because the SHOTGUN does not decide them -- the vehicle carrying it does; B moves these
/// rows into a per-vehicle weapon table.
inline constexpr UnitSpec kShotgunSpec{"shotgun",
                                       kTankHp,
                                       Fixed::from_raw(kTankSpeedRaw),
                                       WeaponId::Shotgun,
                                       kShotgunPelletDamage,
                                       kShotgunReloadTicks,
                                       Fixed::from_raw(kProjectileSpeedRaw),
                                       Fixed::from_raw(kBoltArcRaw),
                                       Fixed::from_raw(kShotgunTravelRaw),
                                       Fixed::zero(),
                                       Fixed::zero(),
                                       false,
                                       0,
                                       0,
                                       kShotgunPellets,
                                       Fixed::from_raw(kShotgunConeRaw)};

/// THE ROCKET AS A CARRIED WEAPON (B), the second row on the weapon axis after the shotgun: the
/// numbers a rocket fired from ANY hull uses, read by `orders::FireRocket`. Harri asked for it to
/// reach further than the gun he is firing it from (2026-10-27: "can the rocket have more reach? I
/// know that's cheating, but I want to try that"), so its range (`kRocketRangeRaw`, 18 tiles) is
/// the longest reach in the game, past the turret's 16, and EVERY kind that fires a rocket has it:
/// the rocket tank and the missile scout read it from this weapon too. Its other
/// numbers are the rocket tank's own, because the round IS that rocket: three times the tank's
/// damage, five times its reload, three quarters of the bolt speed, a straight flight, no splash.
/// `max_travel` must be at least `range` or the bolt fizzles before it reaches the distance the
/// engagement test allows, which is why travel is 20 and not the engine's `kBoltTravel` (16); a
/// kind that fires the rocket flies it that far too.
/// All provisional: these are Harri's to set in a balance pass.
inline constexpr std::int32_t kRocketCarriedTravelRaw = 20 << 16;
inline constexpr std::uint16_t kRocketCarriedDamage = kTankDamage * 3;
inline constexpr std::uint16_t kRocketCarriedReloadTicks = kTankCooldownTicks * 5;

/// THE ROCKET'S SPEC ROW. Like `kShotgunSpec` it is NOT a row in `kUnitSpecs` and has no
/// `UnitKind`: it belongs to the carried weapon axis, and an order that fires it
/// (`orders::FireRocket`) reads this row. Hull and speed are the tank's because the rocket does not
/// decide them -- the vehicle carrying it does.
inline constexpr UnitSpec kRocketSpec{"rocket",
                                      kTankHp,
                                      Fixed::from_raw(kTankSpeedRaw),
                                      WeaponId::Rocket,
                                      kRocketCarriedDamage,
                                      kRocketCarriedReloadTicks,
                                      Fixed::from_raw(kProjectileSpeedRaw * 3 / 4),
                                      Fixed::from_raw(kBoltArcRaw),
                                      Fixed::from_raw(kRocketCarriedTravelRaw),
                                      Fixed::zero(),
                                      Fixed::zero(),
                                      false,
                                      0,
                                      0,
                                      0,
                                      Fixed::zero()};

static_assert(kRocketSpec.max_travel.raw >= kRocketSpec.range().raw,
              "a carried rocket must fly at least as far as its own range, or it fizzles short");

/// Indexed by the value of `UnitKind`, which stays the index type; the order below is the enum's.
inline constexpr std::array<UnitSpec, static_cast<std::size_t>(kUnitKindCount)> kUnitSpecs{{
    {"tank", kTankHp, Fixed::from_raw(kTankSpeedRaw), WeaponId::TankGun, kTankDamage,
     kTankCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw), Fixed::from_raw(kBoltArcRaw),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false, 55, 900, 0, Fixed::zero()},
    // The turret is a vehicle until it deploys, so the recycler's row 3 queues THIS kind: cost 40
    // (Harri's recycler list) and the 20 ticks a scrap the other rows use.
    {"turret", kTurretHp, Fixed::from_raw(kTurretSpeedRaw), WeaponId::TurretGun, kTurretDamage,
     kTurretCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw), Fixed::from_raw(kBoltArcRaw),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false, 40, 800, 0, Fixed::zero()},
    {"factory", kFactoryHp, Fixed::zero(), WeaponId::None, 0, 0, Fixed::zero(), Fixed::zero(),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false, 0, 0, 0, Fixed::zero()},
    // The hover tank has the tank's numbers: the look and the controls differ, not the stats.
    {"hovertank", kTankHp, Fixed::from_raw(kTankSpeedRaw), WeaponId::TankGun, kTankDamage,
     kTankCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw), Fixed::from_raw(kBoltArcRaw),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false, 0, 0, 0, Fixed::zero()},
    // Harri's ruling (2026-11-03): "quite fast", with "next to no shields", so it has to be fast
    // and nimble not to be hit. Second only to the bomber in speed among the named four.
    {"mortarbike", kTankHp * 2 / 5, Fixed::from_raw(kTankSpeedRaw * 5 / 4), WeaponId::Mortar,
     kTankDamage * 2, kTankCooldownTicks * 3, Fixed::from_raw(kMortarMuzzleSpeedRaw),
     Fixed::from_raw(kMortarArcRaw), kBoltTravel, Fixed::from_raw(kMortarSplashRaw),
     Fixed::from_raw(kMortarGravityRaw), false, 45, 900, 0, Fixed::zero()},
    // Harri's ruling (2026-11-03): moves "fairly slowly, but not as slowly as walkers" -- 3/4 of
    // the tank's speed, between the mortar bike's 5/4 and the walker's 1/2.
    {"rockettank", kTankHp, Fixed::from_raw(kTankSpeedRaw * 3 / 4), WeaponId::Rocket,
     kTankDamage * 3, kTankCooldownTicks * 5, Fixed::from_raw(kProjectileSpeedRaw * 3 / 4),
     Fixed::from_raw(kBoltArcRaw), Fixed::from_raw(kRocketCarriedTravelRaw), Fixed::zero(),
     Fixed::zero(), false, 65, 1300, 0, Fixed::zero()},
    // The scavenger (C2): no gun, no range, no damage -- `range` zero is what makes it unarmed
    // everywhere in the sim (fire, targeting, the AI's threat list). It is fast ENOUGH to reach
    // wreckage, not fast enough to outrun what killed it.
    {"scavenger", kScavengerHp, Fixed::from_raw(kScavengerSpeedRaw), WeaponId::None, 0, 0,
     Fixed::zero(), Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 20, 300, 0,
     Fixed::zero()},
    // The recycler: unarmed like the scavenger (`range` zero, and the ballistics block with it).
    // Its hp and speed are BORROWED, not designed: the tank's hull (`kRecyclerHp`), so a base does
    // not die too easily, and the hauler's speed, so it can reach a deploy site. Its own numbers
    // are Harri's to set in a balance pass.
    {"recycler", kRecyclerHp, Fixed::from_raw(kScavengerSpeedRaw), WeaponId::None, 0, 0,
     Fixed::zero(), Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 0, 0, 0,
     Fixed::zero()},
    // The deployed recycler (DEPLOY): a building. Speed zero (it never moves), unarmed (no range,
    // damage or cooldown), and the recycler's hull, `kDeployedRecyclerHp`, a default of his.
    {"deployedrecycler", kDeployedRecyclerHp, Fixed::zero(), WeaponId::None, 0, 0, Fixed::zero(),
     Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 0, 0, 0, Fixed::zero()},
    // The scout (VIS): light armour and fast, with the tank's bolt as a minigun -- half the tank's
    // hp, half again its speed (the fastest vehicle here: it has to scout), a 10-tile reach, half
    // the damage rounded up, a third of the cooldown, and the tank's straight single-target bolt.
    // Outgunned by any tank, but it shoots back. Every number is DERIVED from the tank's and is
    // provisional: its own numbers are Harri's to set in a balance pass. Not buildable: the build
    // menu (RB3) is what fills that flag, and its display-only `SCOUT` row does not.
    {"scout", kTankHp / 2, Fixed::from_raw(kTankSpeedRaw * 3 / 2), WeaponId::Minigun,
     (kTankDamage + 1) / 2, kTankCooldownTicks / 3, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 50, 900, 0,
     Fixed::zero()},
    // The four kinds Harri's factory menu adds (2026-10-30). EVERY number below is PROVISIONAL,
    // derived from the tank's (hp 100, damage 8, cooldown 15, speed 0.2 tiles/tick, range 12) so
    // the ladder stays readable; costs are from his own menu screenshot, and build times follow one
    // rule, 20 ticks per scrap (the scout's 45 -> 900), as the four rows above do bar the tank's
    // 900. The assault tank: the tank's gun at 3/2 the hull and 3/2 the damage, a straight bolt.
    {"assaulttank", kTankHp * 3 / 2, Fixed::from_raw(kTankSpeedRaw), WeaponId::TankGun,
     kTankDamage * 3 / 2, kTankCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 70, 1400, 0,
     Fixed::zero()},
    // The walker: twice the tank's hull and damage and twice its cooldown, at half its speed (it
    // walks) and a turn rate to match (15 tenths of a degree a tick); its OWN gun, reaching 20
    // tiles, and a straight bolt.
    {"walker", kTankHp * 2, Fixed::from_raw(kTankSpeedRaw / 2), WeaponId::WalkerGun,
     kTankDamage * 2, kTankCooldownTicks * 2, Fixed::from_raw(kProjectileSpeedRaw),
     Fixed::from_raw(kBoltArcRaw), Fixed::from_raw(kWalkerGunTravelRaw), Fixed::zero(),
     Fixed::zero(), false, 100, 2000, 0, Fixed::zero(), 15},
    // The APC: the tank's hull, 3/2 its speed (the fastest ground vehicle, level with the scout),
    // the scout's 10-tile range. It has no troop-carrying role yet, so it carries a light gun --
    // half the tank's damage and half its cooldown -- rather than being a unit that cannot fight at
    // all. A straight bolt.
    {"apc", kTankHp, Fixed::from_raw(kTankSpeedRaw * 3 / 2), WeaponId::Minigun, kTankDamage / 2,
     kTankCooldownTicks / 2, Fixed::from_raw(kProjectileSpeedRaw), Fixed::from_raw(kBoltArcRaw),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false, 50, 1000, 0, Fixed::zero()},
    // The bomber: 3/4 the tank's hull and TWICE its speed (Harri, 2026-11-03: "the fastest-moving
    // vehicle in the game", so past the scout's and the APC's 3/2), the mortar's 14-tile range,
    // twice the tank's damage and three times its cooldown. The bomb IS the existing lobbed shell
    // -- the mortar's gravity, arc, muzzle speed and 1.5-tile splash -- so the bomber needs no new
    // ballistics. It is an AIRCRAFT dropping the mortar's shell, which is why its `bolt_speed` is
    // the mortar's `kMortarMuzzleSpeedRaw` and not a number of its own: one shell, one flight
    // model, whoever lets it go. (The bomber's own flight speed is the `speed` field above and is
    // not this.)
    {"bomber", kTankHp * 3 / 4, Fixed::from_raw(kTankSpeedRaw * 2), WeaponId::Mortar,
     kTankDamage * 2, kTankCooldownTicks * 3, Fixed::from_raw(kMortarMuzzleSpeedRaw),
     Fixed::from_raw(kMortarArcRaw), kBoltTravel, Fixed::from_raw(kMortarSplashRaw),
     Fixed::from_raw(kMortarGravityRaw), false, 75, 1500, 0, Fixed::zero()},
    // The missile scout: the scout's own hull and speed carrying the rocket tank's rocket (its
    // range, damage, cooldown and bolt speed, a straight flight). EVERY number here is PROVISIONAL
    // and Harri's to set: he plays it and corrects it. The cost, 45, is his factory menu's; the
    // build time follows the same 20 ticks a scrap as the rows above (45 -> 900).
    {"mslscout", kTankHp / 2, Fixed::from_raw(kTankSpeedRaw * 3 / 2), WeaponId::Rocket,
     kTankDamage * 3, kTankCooldownTicks * 5, Fixed::from_raw(kProjectileSpeedRaw * 3 / 4),
     Fixed::from_raw(kBoltArcRaw), Fixed::from_raw(kRocketCarriedTravelRaw), Fixed::zero(),
     Fixed::zero(), false, 45, 900, 0, Fixed::zero()},
    // The deployed scavenger: a fixture on a biometal pool. Speed zero, unarmed, and the
    // scavenger's own hull -- deploying neither heals nor wounds it. Not buildable: it is a
    // scavenger that has finished a deploy, never a row.
    {"deployedscavenger", kScavengerHp, Fixed::zero(), WeaponId::None, 0, 0, Fixed::zero(),
     Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 0, 0, 0, Fixed::zero()},
    // The deployed turret (TURRET-CMD): the turret's gun and hull, fixed to its spot. Speed zero is
    // what makes it immobile everywhere in the sim; every other number is the turret's own, so
    // deploying neither heals, wounds nor re-arms it. Not buildable: it is a turret that has
    // finished a deploy, never a row.
    {"deployedturret", kTurretHp, Fixed::zero(), WeaponId::TurretGun, kTurretDamage,
     kTurretCooldownTicks, Fixed::from_raw(kProjectileSpeedRaw), Fixed::from_raw(kBoltArcRaw),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false, 0, 0, 0, Fixed::zero()},
    // The four support vehicles of the human recycler's list. All are unarmed (`range` zero, and
    // the ballistics block with it) and EVERY number is PROVISIONAL and Harri's to set; the costs
    // are his recycler list's (Tug 45, Constructor 40, Service Truck 50, Service Pod 2) and the
    // build time follows the 20 ticks a scrap of the rows above. The tug, constructor and truck
    // have the tank's hull and three quarters of its speed (the scavenger's); the pod, a small
    // craft, has half the hull and the tank's speed.
    {"tug", kTankHp, Fixed::from_raw(kTankSpeedRaw * 3 / 4), WeaponId::None, 0, 0, Fixed::zero(),
     Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 45, 900, 0, Fixed::zero()},
    {"constructor", kTankHp, Fixed::from_raw(kTankSpeedRaw * 3 / 4), WeaponId::None, 0, 0,
     Fixed::zero(), Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 40, 800, 0,
     Fixed::zero()},
    {"servicetruck", kTankHp, Fixed::from_raw(kTankSpeedRaw * 3 / 4), WeaponId::None, 0, 0,
     Fixed::zero(), Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 50, 1000, 0,
     Fixed::zero()},
    {"servicepod", kTankHp / 2, Fixed::from_raw(kTankSpeedRaw), WeaponId::None, 0, 0, Fixed::zero(),
     Fixed::zero(), kBoltTravel, Fixed::zero(), Fixed::zero(), false, 2, 40, 0, Fixed::zero()},
    // POWER (round 17, J3), the Constructor's first building: unarmed, never moves, the tank's
    // hull as a default. Cost 30 is the owner's (confirmed off his capture, 2026-11-11). Its
    // `build_ticks` is the 20-ticks-a-scrap the other rows use (30 -> 600) and is the length of
    // a building's construction (K2): PROVISIONAL, the owner has given no number. A placed site
    // counts it down while a Constructor is near (`orders::Place`, `build_site_step`). It is not
    // `buildable` and `is_queueable` refuses it, so no producer's queue can make it as a unit.
    {"power", kPowerHp, Fixed::zero(), WeaponId::None, 0, 0, Fixed::zero(), Fixed::zero(),
     kBoltTravel, Fixed::zero(), Fixed::zero(), false, 30, 600, 0, Fixed::zero()},
}};

/// No armed kind's bolt may fizzle short of the reach its weapon lets it engage at.
[[nodiscard]] constexpr bool every_bolt_outflies_its_range() noexcept {
    for (const UnitSpec& row : kUnitSpecs) {
        if (row.max_travel.raw < row.range().raw) {
            return false;
        }
    }
    return true;
}
static_assert(every_bolt_outflies_its_range(),
              "a kind's bolt must fly at least as far as its weapon's range, or it fizzles short");

[[nodiscard]] constexpr const UnitSpec& spec_of(UnitKind kind) noexcept {
    return kUnitSpecs.at(static_cast<std::size_t>(kind));
}

/// Whether a producer's queue may take this kind: it has a scrap cost AND a build time. Zero in
/// either means not buildable -- the signal the queue reads, so a kind with no ruled price is never
/// queued for nothing. A value that is not a kind at all (an order off the wire) is not queueable
/// either, rather than an out-of-range read of the table. A BUILDING (a kind with a footprint) is
/// never queueable even though its row carries a price and a time: a producer's queue makes units
/// that drive out beside it, and a building is placed by a Constructor (`orders::Place`).
[[nodiscard]] constexpr bool is_queueable(UnitKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kUnitSpecs.size() && kUnitSpecs.at(index).cost != 0
           && kUnitSpecs.at(index).build_ticks != 0 && !has_footprint(kind);
}

} // namespace protekt::sim
