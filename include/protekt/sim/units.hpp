#pragma once

#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/route.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/world/world.hpp"

namespace protekt::sim {

/// Which round a lob fires: a Contact shell bursts where it lands (the mortar's), a Bounce shell
/// bounces, comes to rest and pulses shrapnel (the bouncer's, `kBouncer`). A round, not a unit.
enum class Round : std::uint8_t { Contact = 0, Bounce = 1 };

inline constexpr int kMapTiles = 256; ///< the playable world is [0, kMapTiles) tiles square
static_assert(kMapTiles == kRouteTiles, "a tile coordinate in a Route is one byte");
/// A tick counter. Named once so the sim, orders, replays and observers cannot drift apart on it.
using Tick = std::uint32_t;

inline constexpr int kTicksPerSecond = 30;

enum class UnitKind : std::uint8_t {
    Tank = 0,
    Turret = 1,
    Factory = 2,
    HoverTank = 3,
    Mortar = 4, ///< lobs a splashing shell over hills; outranged by a turret
    Rocket = 5, ///< slow, heavy, single-target
    /// Appended after Rocket so every kind's index -- and every hash, replay and observation that
    /// carries one -- is unchanged by its arrival. Unarmed: it hauls wreckage, and never shoots.
    Scavenger = 6,
    /// Appended after Scavenger for the same reason: the kind is hashed, packed into observations
    /// and serialised in replays, so inserting it anywhere else would move every recorded value.
    /// Unarmed, and it MOVES: each side's one recycler, the base on wheels, before it deploys.
    Recycler = 7,
    /// The recycler once a `Deploy` order has run its countdown (DEPLOY): the SAME unit -- the same
    /// `UnitId`, the same entry in the unit table, the hit points it had -- turned into a building.
    /// Appended after Recycler, never inserted, for the reason every kind above was. It never moves
    /// and never fires, and it stands on a `kBuildingFootprintTiles` square no unit may enter.
    DeployedRecycler = 8,
    /// The fast, lightly armoured recon vehicle (VIS), with a short-range rapid-fire gun. Appended
    /// after DeployedRecycler, never inserted, for the reason every kind above was: the kind is
    /// hashed, packed into observations and serialised in replays. What it is FOR is the game
    /// layer's: its radar reaches twice as far as any other kind's (`viewer_radar_radius`).
    Scout = 9
};
inline constexpr int kUnitKindCount = 10;

/// Tanks and hover tanks: every rule written "for a tank" (orders, retreat, defend, swap, the AI's
/// hunters) takes both. The hover tank has the tank's profile and, until it is drivable, its
/// behaviour.
[[nodiscard]] constexpr bool is_tank(UnitKind kind) noexcept {
    return kind == UnitKind::Tank || kind == UnitKind::HoverTank;
}

/// Every ARMED kind can be driven (swapped into): the tanks, the mortar, the rocket and the
/// turret. The factory is unarmed and cannot move, so there is nothing to drive. The scavenger is
/// unarmed too, but it DOES move, so it can be driven -- an order from the player outranks the
/// fetch it would otherwise run (see `Unit::fetching`). The recycler is the same case: unarmed, and
/// it moves (it has to reach the site it deploys at), so it is driven like the hauler. Once
/// deployed it is a building, like the factory, and there is nothing to drive.
[[nodiscard]] constexpr bool is_drivable(UnitKind kind) noexcept {
    return kind != UnitKind::Factory && kind != UnitKind::DeployedRecycler;
}

/// A kind that stands on a FOOTPRINT (see `Footprint`): only the deployed recycler today. The
/// factory is a building in the game's eyes but has never had one, and DEPLOY does not give it one.
[[nodiscard]] constexpr bool has_footprint(UnitKind kind) noexcept {
    return kind == UnitKind::DeployedRecycler;
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
    Follow = 6, ///< appended after Dead, so every recorded stance value (and hash) is unchanged
    /// The fetch (C2), appended for the same reason. Not an order: nothing a commander can issue
    /// sets it -- the sim sets it on an unarmed scavenger with no orders of its own, and any order
    /// the player or an AI gives replaces it (see `fetch_step`).
    Fetch = 7,
    /// A recycler under a `Deploy` order (DEPLOY), appended for the same reason: it drives to the
    /// site's centre (`guard_x`/`guard_y`) and, once there, counts `Unit::deploy_ticks` up to
    /// `kDeployTicks`. Any other order replaces it, and the count with it.
    Deploy = 8
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
    /// A mounted turret that has been told to move spends kUnmountTicks unmounting first, and this
    /// counts them (0 = not unmounting). Hashed only when non-zero.
    std::uint16_t unmount_ticks{};
    /// The planned route to this unit's goal (see sim/route.hpp). Hashed: it decides every step.
    Route route{};
    /// How many pieces of wreckage this unit is carrying (C2): 0 or 1 today, a count so a
    /// bigger hold is a number and not a new field. Hashed only when non-zero, so a battle with
    /// no scavenger anywhere in it hashes exactly as it did before the fetch existed.
    std::uint16_t cargo{};
    /// DEPLOY: ticks this recycler has stood at its site under a `Deploy` order, counting up to
    /// `kDeployTicks`; zero whenever it is not standing there under one. Hashed only when non-zero,
    /// like `cargo`, so a battle in which nothing deploys hashes exactly as it did before.
    std::uint16_t deploy_ticks{};
    bool alive{true};
};

/// A turret is a DEPLOYABLE weapon: it crawls unmounted and cannot fire, and it mounts after this
/// many ticks (3 s) of unbroken stillness. Moving unmounts it (see kUnmountTicks).
inline constexpr std::uint16_t kMountTicks = 90;

/// A MOUNTED turret told to move first spends this many ticks (3 s, as long as mounting takes)
/// unmounting, during which it neither moves nor fires; only then does it crawl.
inline constexpr std::uint16_t kUnmountTicks = 90;

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
/// THE BUILD (C4). A factory replaces a lost gun with a tank for `kTankOreCost` delivered loads --
/// one wreck's worth, so one kill pays for one tank -- on a global production beat, the way repair
/// runs on one. The spot it appears on is its own tile or the nearest free one within
/// `kBuildSpawnRadiusTiles`; a yard with no room builds nothing and spends nothing.
inline constexpr std::uint16_t kTankOreCost = 4;
inline constexpr int kBuildIntervalTicks = 150; ///< 5 s: an unhurried production line
inline constexpr int kBuildSpawnRadiusTiles = 3;
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

/// The scavenger (C2). Unarmed and lightly built -- three fifths of a tank's hit points, three
/// quarters of its speed -- because a hauler is meant to be worth escorting, not worth arming.
inline constexpr std::uint16_t kScavengerHp = kTankHp * 3 / 5;
inline constexpr std::int32_t kScavengerSpeedRaw = kTankSpeedRaw * 3 / 4;
/// How close (tiles) a scavenger must be to a piece of wreckage to pick it up, and to its own
/// factory to hand the load over. Both are radii, not tiles: the mover stops inside them.
inline constexpr int kFetchRadiusTiles = 1;
inline constexpr int kDeliverRadiusTiles = 2;

/// The recycler's hull, and the deployed recycler's. The recycler's is BORROWED from the tank (see
/// its spec entry). The deployed recycler's equals it -- a building that heals or wounds itself by
/// deploying would make the countdown a repair bay -- and it is a DEFAULT, Harri's to change: it is
/// named here, beside the recycler's, so that change is this one line and nothing else.
inline constexpr std::uint16_t kRecyclerHp = kTankHp;
inline constexpr std::uint16_t kDeployedRecyclerHp = kRecyclerHp;

/// DEPLOY: how many ticks a recycler stands at its site before it becomes a `DeployedRecycler` --
/// 300, ten seconds at `kTicksPerSecond`. The one number to change for a faster or slower deploy.
inline constexpr std::uint16_t kDeployTicks = 300;

/// A building's footprint is a square this many tiles on a side (his ruling, 2026-10-26: "2x2
/// tiles -- a building should not be the same size as a tank").
inline constexpr int kBuildingFootprintTiles = 2;

/// The tiles a building stands on: the square of `kBuildingFootprintTiles` whose NORTH-WEST (lowest
/// x, lowest y) tile is (`x`, `y`). The building itself stands at the square's centre, which for an
/// even side is the corner its middle tiles share -- a whole-tile position, like a spawn's.
struct Footprint {
    int x{};
    int y{};
    [[nodiscard]] constexpr bool covers(int tile_x, int tile_y) const noexcept {
        return tile_x >= x && tile_x < x + kBuildingFootprintTiles && tile_y >= y
               && tile_y < y + kBuildingFootprintTiles;
    }
    /// The building's position, in whole tiles: the square's centre.
    [[nodiscard]] constexpr int centre_x() const noexcept {
        return x + kBuildingFootprintTiles / 2;
    }
    [[nodiscard]] constexpr int centre_y() const noexcept {
        return y + kBuildingFootprintTiles / 2;
    }
};

/// The footprint a deploy aimed at tile (`tile_x`, `tile_y`) builds: that tile is its north-west
/// corner, moved in from the east and south edges just far enough that the whole square is on the
/// map. Only a tile ON the map is clipped: a deploy aimed off it is refused (`deploy_site_ok`)
/// before this is asked, never pulled back onto the map.
[[nodiscard]] constexpr Footprint footprint_at(int tile_x, int tile_y) noexcept {
    constexpr int kLast = kMapTiles - kBuildingFootprintTiles; ///< the last north-west tile
    return Footprint{tile_x > kLast ? kLast : tile_x, tile_y > kLast ? kLast : tile_y};
}

/// The footprint of a building standing at (`x`, `y`): the square centred there (the inverse of
/// `Footprint::centre_x`/`centre_y`).
[[nodiscard]] constexpr Footprint footprint_around(Fixed x, Fixed y) noexcept {
    return Footprint{x.floor_int() - kBuildingFootprintTiles / 2,
                     y.floor_int() - kBuildingFootprintTiles / 2};
}

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
