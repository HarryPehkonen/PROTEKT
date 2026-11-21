#pragma once

#include <array>
#include <cstddef>
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
    /// The mortar BIKE: a motorcycle that carries mortars (and can carry bouncers). "Mortar" is a
    /// weapon, never a vehicle (Harri, 2026-10-29), so the kind is named for what it is. Renamed
    /// from `Mortar` in place: the value 4 is hashed, packed and replayed, and did not move.
    /// Lobs a splashing shell over hills; outranged by a turret.
    MortarBike = 4,
    /// The rocket TANK, renamed from `Rocket` for the same reason; the value 5 did not move.
    /// Slow, heavy, single-target.
    RocketTank = 5,
    /// Appended after RocketTank so every kind's index -- and every hash, replay and observation
    /// that carries one -- is unchanged by its arrival. Unarmed: it hauls wreckage, and never
    /// shoots.
    Scavenger = 6,
    /// Appended after Scavenger for the same reason: the kind is hashed, packed into observations
    /// and serialised in replays, so inserting it anywhere else would move every recorded value.
    /// Unarmed, and it MOVES: each side's one recycler, the base on wheels, before it deploys.
    Recycler = 7,
    /// The recycler once a `Deploy` order has run its countdown (DEPLOY): the SAME unit -- the same
    /// `UnitId`, the same entry in the unit table, the hit points it had -- turned into a building.
    /// Appended after Recycler, never inserted, for the reason every kind above was. It never moves
    /// and never fires, and it stands on a `footprint_of` rectangle no unit may enter.
    DeployedRecycler = 8,
    /// The fast, lightly armoured recon vehicle (VIS), with a short-range rapid-fire gun. Appended
    /// after DeployedRecycler, never inserted, for the reason every kind above was: the kind is
    /// hashed, packed into observations and serialised in replays. What it is FOR is the game
    /// layer's: its radar reaches twice as far as any other kind's (`viewer_radar_radius`).
    Scout = 9,
    /// The heavier tank (the factory's build row 5): the tank's gun at 3/2 the hull and the damage.
    /// Appended after Scout, never inserted, for the reason every kind above was: it moves no
    /// recorded hash, observation or replay.
    AssaultTank = 10,
    /// The slow, heavy walker (build row 6): twice the tank's hull and damage at half its speed.
    /// Appended after AssaultTank so that no recorded value moves.
    Walker = 11,
    /// The armoured personnel carrier (build row 7): the fastest ground vehicle. It has no
    /// troop-carrying role yet, so it carries a light gun. Appended after Walker so that no
    /// recorded value moves.
    Apc = 12,
    /// The bomber (build row 8): a lobbed-shell carrier, the mortar's bomb on a wide flat hull.
    /// Appended after Apc so that no recorded value moves.
    Bomber = 13,
    /// The scout carrying missiles (the factory's row 2, `MSL Scout`): a vehicle of its own, NOT
    /// the scout under a second price -- in Battlezone 2 the Scout and the Scout with missiles are
    /// two vehicles, and one kind may carry only one price. The scout's hull, the rocket tank's
    /// rocket. Appended after Bomber so that no recorded value moves.
    MslScout = 14,
    /// The scavenger once it has finished deploying on a biometal pool: the SAME unit, turned into
    /// a fixture that extracts there and holds `kStoreCapacityPerDeployed` of store. It never
    /// moves, never fires, takes no order and can never be un-deployed. Appended after MslScout so
    /// that no recorded value moves.
    DeployedScavenger = 15,
    /// The turret once a deploy has run its countdown (TURRET-CMD): the SAME unit -- the same
    /// `UnitId`, the same F key, the hit points it had -- standing on its spot with its gun up. It
    /// fires, never moves, and UNLIKE the two kinds above it goes back: an un-deploy (an
    /// `Undeploy` order, or any order that would move it) counts `kTurretUndeployTicks` and returns
    /// it to a plain `Turret`. The scavenger's permanence exists for the capacity it grants; a
    /// turret grants none, so the two rules are different on purpose. Appended after
    /// DeployedScavenger so that no recorded value moves.
    DeployedTurret = 16,
    /// The four support vehicles the human recycler's list names (build rows 4 to 7). Each is
    /// appended after DeployedTurret, in the list's key order and never inserted, for the reason
    /// every kind above was: the kind is hashed, packed into observations and serialised in
    /// replays, so a value moved would move every recorded number. All four are UNARMED (`range`
    /// zero) and move, like the scavenger, so each is selectable, takes an F key and can be driven.
    /// The Tug hauls; its towing is not built yet.
    Tug = 17,
    /// Raises the buildings a recycler cannot. It is buildable and driveable and does NOT place
    /// buildings: that needs a placement feature that does not exist yet.
    Constructor = 18,
    /// The repair vehicle: mends every friendly unit within `kServiceRepairRadiusTiles` on its own.
    ServiceTruck = 19,
    /// The small, cheap craft. It does not repair: only the truck does.
    ServicePod = 20,
    /// The first building a Constructor places (round 17, J3): POWER. Appended after ServicePod,
    /// never inserted, for the reason every kind above was. A building: it never moves, never
    /// fires, and stands on a `footprint_of` rectangle no unit may enter. It supplies NOTHING yet:
    /// the power grid is the next slice.
    Power = 21,
    /// The other seven buildings of a Constructor's panel (round 18, K3), appended in the panel's
    /// own order after Power, never inserted. (The panel's FACTORY row is the `Factory` above: that
    /// kind already existed and is wired, not duplicated.) Each is a building that never moves and
    /// never moves, and stands on a `footprint_of` rectangle. THEIR FUNCTIONS ARE NOT BUILT (but
    /// the gun tower's, K5, which fires): the relay bunker relays nothing, the armory unlocks
    /// nothing, the service bay's AMMUNITION half is not built (N1: weapons have cooldowns, not
    /// rounds) though its platform repairs, the training centre and the tech centre do nothing
    /// beyond the prerequisites the panel already models, and the bomber bay holds no bomber.
    RelayBunker = 22,
    Armory = 23,
    ServiceBay = 24,
    GunTower = 25,
    TrainingCentre = 26,
    TechCentre = 27,
    BomberBay = 28,
    /// The ALIEN Builder's six buildings (round 18, K6), appended after BomberBay in the panel's
    /// own order, never inserted. Faction-less like every kind: the Builder's panel names them, and
    /// a human side never lists them. Buildings that never move, on a `footprint_of` rectangle (see
    /// the table: the owner gave the Kiln, Antenna and Stronghold). THEIR FUNCTIONS ARE NOT BUILT:
    /// the Kiln produces nothing, the Gun Spire does not shoot, the Antenna detects nothing, the
    /// Dower's platform repairs (N1) and it does nothing else, the Jammer jams nothing and the
    /// Stronghold constructs no weapons and no shields. There is NO power grid on the alien side
    /// (his ruling), and the Power Lungs are not built.
    Kiln = 29,
    GunSpire = 30,
    Antenna = 31,
    Dower = 32,
    Stronghold = 33,
    Jammer = 34,
    /// THE POWER LUNG (N3), appended after the Jammer, never inserted: a COMPONENT of an alien
    /// structure, built into it (`needs_lung`), standing just outside its footprint's east edge.
    /// Never moves, never fires, and is not a structure in its own right: it owns no footprint.
    /// It is a unit so a gun can be told to shoot it -- the pick path finds it, the `3 Attack`
    /// list does not (`Unit::host` links it to the structure it powers). Killing it DISABLES the
    /// host (`structure_disabled`); it never destroys it. A Builder restores it cheaply
    /// (`kLungRestoreCost`, `kLungRestoreTicks`).
    PowerLung = 35
};
inline constexpr int kUnitKindCount = 36;

/// Tanks and hover tanks: every rule written "for a tank" (orders, retreat, defend, swap, the AI's
/// hunters) takes both. The hover tank has the tank's profile and, until it is drivable, its
/// behaviour.
[[nodiscard]] constexpr bool is_tank(UnitKind kind) noexcept {
    return kind == UnitKind::Tank || kind == UnitKind::HoverTank;
}

/// A turret, standing or crawling: the plain `Turret` and the `DeployedTurret` it becomes. One
/// thing that changes state both ways, so a rule written "for a turret" (its look, its bar, its
/// map mark) takes both.
[[nodiscard]] constexpr bool is_turret(UnitKind kind) noexcept {
    return kind == UnitKind::Turret || kind == UnitKind::DeployedTurret;
}

/// The kinds that build: the factory, and the recycler once it has deployed. Chosen BY KIND, never
/// as "the one building" -- a side may have several, and each has a queue of its own.
[[nodiscard]] constexpr bool is_producer(UnitKind kind) noexcept {
    return kind == UnitKind::Factory || kind == UnitKind::DeployedRecycler;
}

/// Every ARMED kind can be driven (swapped into): the tanks, the mortar bike, the rocket tank and
/// the turret. The factory is unarmed and cannot move, so there is nothing to drive. The scavenger
/// is unarmed too, but it DOES move, so it can be driven -- an order from the player outranks the
/// fetch it would otherwise run (see `Unit::fetching`). The recycler is the same case: unarmed, and
/// it moves (it has to reach the site it deploys at), so it is driven like the hauler. Once
/// deployed it is a building, like the factory, and there is nothing to drive.
[[nodiscard]] constexpr bool is_drivable(UnitKind kind) noexcept {
    return kind != UnitKind::Factory && kind != UnitKind::DeployedRecycler
           && kind != UnitKind::DeployedScavenger;
}

/// A kind that ALWAYS stands on a FOOTPRINT (see `Footprint`): the deployed recycler, the factory,
/// POWER and the buildings of round 18's K3. ROUND 19, L2: the FACTORY is here now, whoever raised
/// it. It used to be a point unit when a scenario spawned it (and only a Constructor's stood on a
/// footprint), so every battle recorded before spawned it with none; the owner ruled it scales
/// ("we also need to scale the factory and recycler"), so the kind has one in BOTH states and the
/// battles it stands in are re-recorded (each named in its test).
[[nodiscard]] constexpr bool has_footprint(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::DeployedRecycler:
    case UnitKind::Factory:
    case UnitKind::Power:
    case UnitKind::RelayBunker:
    case UnitKind::Armory:
    case UnitKind::ServiceBay:
    case UnitKind::GunTower:
    case UnitKind::TrainingCentre:
    case UnitKind::TechCentre:
    case UnitKind::BomberBay:
    case UnitKind::Kiln:
    case UnitKind::GunSpire:
    case UnitKind::Antenna:
    case UnitKind::Dower:
    case UnitKind::Stronghold:
    case UnitKind::Jammer:
        return true;
    default:
        return false;
    }
}

/// WHAT MAY PASS A BUILDING'S DRAWN BODY (N5, and N1 before it): a per-kind property, so no call
/// site names a kind. `Blocks` is the default -- a unit cannot enter the building's drawn box
/// (`drawn_box`). `Drivable` is the PLATFORM: the Service Bay and its alien twin the Dower, whose
/// box a unit may drive onto. Either way the box is the hit shape that stops a bolt, and the
/// footprint around it is where no other building may be placed.
enum class Passage : std::uint8_t { Blocks = 0, Drivable = 1 };

[[nodiscard]] constexpr Passage passage_of(UnitKind kind) noexcept {
    return kind == UnitKind::ServiceBay || kind == UnitKind::Dower ? Passage::Drivable
                                                                   : Passage::Blocks;
}

/// A kind whose FOOTPRINT IS A PLATFORM (N1): the SERVICE BAY and its alien twin the DOWER. Units
/// may drive onto it; the footprint is otherwise a building's in every respect -- no other building
/// may be placed inside it, it is the hit shape that stops a bolt, and the build site and the
/// connection rule are unchanged. A unit of the building's own side standing inside is mended at
/// the Service Truck's own rate (`service_repair_step`). Every other kind's body blocks units, and
/// that stays the default. Since N5 this is the kind's `passage_of`, read, not a second flag.
///
/// NOT BUILT, and said so: the Service Bay's AMMUNITION half. PROTEKT's weapons have cooldowns,
/// not rounds, so there is nothing to refill; no resource is faked.
[[nodiscard]] constexpr bool has_drivable_footprint(UnitKind kind) noexcept {
    return passage_of(kind) == Passage::Drivable;
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
    /// A scavenger on a trip to a pile of litter (AR): `scavenge_step` sets it with the pile's spot
    /// in `guard_x/y`, `steer` drives it there like a MoveTo, and the pickup on arrival ends it.
    /// Any order the player gives replaces it. (It was retired for a while, when scavengers took
    /// scrap from where they stood; the value never changed, so a recorded one still names it.)
    Fetch = 7,
    /// A recycler under a `Deploy` order (DEPLOY), appended for the same reason: it drives to the
    /// site's centre (`guard_x`/`guard_y`) and, once there, counts `Unit::deploy_ticks` up to
    /// `kDeployTicks`. Any other order replaces it, and the count with it.
    Deploy = 8,
    /// A RECYCLE order (RECYCLE): drive to the named recycler and, on arrival, be consumed there
    /// into the side's scrap reserve. Appended for the same reason as every value above.
    Recycle = 9,
    /// An UNDEPLOY order (TURRET-CMD) on a `DeployedTurret`: the gun comes off its mount over
    /// `kTurretUndeployTicks` and the unit is a plain `Turret` again, idle. Appended for the same
    /// reason as every value above.
    Undeploy = 10,
    /// A DEFEND order (TURRET-CMD): drive at the named enemy until within the turret's weapon
    /// range of it, then count `kTurretDeployTicks` standing there and become a `DeployedTurret`.
    Defend = 11,
    /// A REPAIR order (D1b) on a service truck: drive at the named friendly until within
    /// `kServiceRepairApproachTiles` of it and hold, until it is whole or gone. Appended for the
    /// same reason as every value above.
    Repair = 12,
    /// A CONSTRUCTOR under a `Place` order (round 18, K2), appended for the same reason: `target`
    /// is the site it raised, and it drives there like a MoveTo (the site's centre, rewritten into
    /// `guard_x/y`) until it is within `kBuildReachTiles` of the footprint, where it stands until
    /// the site is built (or gone) and then goes idle. Any other order replaces it.
    Build = 13
};

struct UnitId {
    std::uint32_t value{}; ///< 0 means "none"
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};
[[nodiscard]] constexpr bool operator==(UnitId a, UnitId b) noexcept { return a.value == b.value; }
[[nodiscard]] constexpr bool operator!=(UnitId a, UnitId b) noexcept { return a.value != b.value; }
[[nodiscard]] constexpr bool operator<(UnitId a, UnitId b) noexcept { return a.value < b.value; }

/// THE BUILD QUEUE (BUILD-QUEUE). A producer builds one unit at a time, in the order they were
/// asked for, and holds at most this many entries -- one per shift-press.
inline constexpr std::size_t kBuildQueueDepth = 10;

/// The largest batch token an ORDER can carry. A token rides in an order as a whole-number
/// position (`Fixed::from_int`), so it is bounded by what that holds. The sim's own batch ids, the
/// ones it gives a lone build, are all ABOVE this, so a token and an id can never be the same
/// number.
inline constexpr std::uint32_t kMaxBatchToken = 32767;

/// The first id the sim gives a LONE build's batch; each further lone build takes the next. Above
/// `kMaxBatchToken`, so it can never be a token an order carried.
inline constexpr std::uint32_t kFirstLoneBatch = kMaxBatchToken + 1;

/// What one entry of a queue names: the kind to build and the batch it will arrive in.
struct BuildEntry {
    UnitKind kind{};
    std::uint32_t batch{};
};

/// A producer's queue: the entries in order, and the front entry's countdown. `left` is zero while
/// the front entry has not started (it is waiting for its scrap) and the ticks still to run once it
/// has; the entry pays its cost on the tick it starts, so a non-zero `left` IS "paid for". A plain
/// count of ticks, never a time: the sim has no clock.
struct BuildQueue {
    std::array<BuildEntry, kBuildQueueDepth> entries{};
    std::uint8_t count{}; ///< entries held, 0..kBuildQueueDepth
    std::uint16_t left{}; ///< ticks until the front entry's unit arrives; 0 = not started
};

/// A scavenger GIVES UP A PILE it cannot reach (the stalled-fetch fix). Two ways to find out, both
/// deterministic: the route planner says the pile's tile is not joined to the scavenger's (so no
/// drive can end there) and it gives up on the next beat; or, whatever the reason -- a wedge the
/// planner did not foresee -- it has spent `kFetchGiveUpBeats` beats on the one trip. 60 beats is a
/// minute: a 40-tile radar over the roughest ground at the slowest speed is a trip of well under
/// that. It then goes IDLE, which is what lets it look for another pile, and a pile it has given
/// up is not chosen again (the last `kFetchShunned` of them are remembered).
inline constexpr std::uint8_t kFetchGiveUpBeats = 60;
inline constexpr std::size_t kFetchShunned = 4;

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
    /// N9: whom this gun chose on its last beat (`kThinkBeatTicks`) and keeps between beats; none
    /// when it found nothing. Dropped when the target is dead, out of reach or shielded by a
    /// friend. Hashed only while it holds one, so a battle in which no gun has a target hashes as
    /// before.
    UnitId fire_target{};
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
    /// DEPLOY: ticks this recycler has stood at its site under a `Deploy` order, counting up to
    /// `kDeployTicks`; zero whenever it is not standing there under one. Hashed only when non-zero,
    /// so a battle in which nothing deploys hashes exactly as it did before.
    std::uint16_t deploy_ticks{};
    /// Consecutive ticks this unit has spent shoving another vehicle (`kShoveYieldTicks`); zero
    /// whenever it is clear of every vehicle. Hashed only when non-zero, so a battle in which
    /// nothing is ever shoved hashes as it did before.
    std::uint16_t shove_ticks{};
    /// A SCAVENGER'S TRIP IN HAND: how many beats (`kScavengeIntervalTicks`) it has spent under
    /// `Fetch` for the pile in `guard_x/y`; `kFetchGiveUpBeats` of them and the pile is given up.
    /// Zero whenever it is not fetching. Hashed only when non-zero, so a battle with no scavenger
    /// on a trip hashes as it did before.
    std::uint8_t fetch_beats{};
    /// The tiles of the piles this scavenger GAVE UP ON (a ring of `kFetchShunned`, oldest
    /// overwritten first), and how many it has ever given up, which also says where the ring is
    /// full. It does not choose these piles again. Hashed only when `shunned_count` is non-zero.
    std::array<TileXY, kFetchShunned> shunned{};
    std::uint8_t shunned_count{};
    /// The batch this unit was BUILT in (BUILD-QUEUE): the id its queue entry carried. Zero for a
    /// unit the scenario placed, which belongs to no batch; a lone build gets an id of its own, so
    /// a built unit's is never zero. The row groups by it. Hashed only when non-zero,
    /// so a battle that builds nothing hashes exactly as it did before there was a queue.
    std::uint32_t batch{};
    /// SERVICE TRUCKS: the part of a hit point the trucks have mended so far, in
    /// `kServiceRepairHitPoint`ths -- always less than one hit point, because the whole ones are
    /// paid out as they complete. Zero whenever the unit is whole. Hashed only when non-zero, so
    /// a battle no truck ever mends in hashes exactly as it did before there were trucks.
    std::uint32_t repair_frac{};
    /// SERVICE TRUCKS (AJ): which way the truck points -- the axis of its repair cone -- as the
    /// direction it last moved, scaled so the larger of the two is `kServiceFacingScale`. Both zero
    /// means it has never moved and points +Y, as the presentation draws a hull that has not
    /// (`Headings`). Only a truck's is ever set, and it is hashed only when it is, so a battle
    /// with no moving truck in it hashes exactly as it did before the cone.
    std::int16_t face_x{};
    std::int16_t face_y{};
    /// A TURN-LIMITED MACHINE's heading (only the Walker today: `UnitSpec::turn_decidegrees`
    /// non-zero), a direction scaled so its larger component is `kHeadingScale`; both zero means it
    /// has never turned and points +Y (yaw 0, as `Headings` draws a hull that has not moved). Every
    /// other kind turns at once and has no heading in the sim, so theirs stay zero. Hashed only
    /// when set, so a battle with no Walker that has turned hashes as before.
    std::int32_t heading_x{};
    std::int32_t heading_y{};
    /// This unit's build queue, if it is a producer (see `is_producer`). Hashed only when it holds
    /// an entry, for the same reason.
    BuildQueue queue{};
    /// A PLACED BUILDING turned a quarter (`~ Rotate Buildings`): its footprint's width and height
    /// are swapped. Only a placed building's is ever set; hashed only when set.
    bool turned{};
    bool alive{true};
    /// A POWER LUNG's structure (N3): the unit this component is built into; none for every other
    /// kind. Hashed only when set.
    UnitId host{};
    /// A DEAD Power Lung being restored: the ticks of the restoration still to run, counted down
    /// only while a friendly Constructor is within reach of the host. Non-zero IS "paid for"
    /// (`kLungRestoreCost` was taken on the tick it started). Hashed only when non-zero.
    std::uint16_t restore_left{};
    /// A BUILDING STILL BEING BUILT (round 18, K2): the ticks of construction it has LEFT. Set to
    /// the kind's `build_ticks` when a Constructor places it, counted down only while a friendly
    /// Constructor is within `kBuildReachTiles` of its footprint, and zero for a building that
    /// stands -- which is every building a scenario deploys or spawns. Hashed only when non-zero,
    /// so a battle with no site under construction hashes exactly as it did before there was one.
    std::uint16_t build_left{};
};

/// A Constructor must be within this many TILES of a site's footprint (the nearest point of its
/// rectangle) for the site to progress. The owner's "within one block" -- his block is this tree's
/// tile.
inline constexpr int kBuildReachTiles = 1;

/// THE POWER LUNG (N3). A Lung's hit points: a fraction of a building's (`kPlacedBuildingHp`), so
/// a gun that points at it kills it long before it could kill the structure. PROVISIONAL.
inline constexpr std::uint16_t kLungHp = 30;
/// RESTORING A DEAD LUNG IS CHEAP, deliberately (his rule 4: "They are easy to repair" -- the
/// counter-play is a raid, not a demolition): a few scrap and a few seconds, against the Gun
/// Spire's 75 scrap and 1500 ticks. A test pins both as small fractions of the structure's.
/// PROVISIONAL: he has given no figure.
inline constexpr std::uint16_t kLungRestoreCost = 5;
inline constexpr std::uint16_t kLungRestoreTicks = 90;

/// True while `unit` is a site still being built: solid like any building, but not yet standing.
[[nodiscard]] constexpr bool under_construction(const Unit& unit) noexcept {
    return unit.build_left != 0;
}

/// A turret is a DEPLOYABLE weapon: it crawls unmounted and cannot fire, and it mounts after this
/// many ticks (3 s) of unbroken stillness. Moving unmounts it (see kUnmountTicks).
inline constexpr std::uint16_t kMountTicks = 90;

/// A MOUNTED turret told to move first spends this many ticks (3 s, as long as mounting takes)
/// unmounting, during which it neither moves nor fires; only then does it crawl.
inline constexpr std::uint16_t kUnmountTicks = 90;

/// True when the gun may fire. Every other kind fires on the move, so it is always "mounted".
///
/// A DEPLOYED turret fires from the tick it exists until an un-deploy begins: `deploy_ticks` is the
/// un-deploy's count on that kind, so a non-zero count is a gun coming off its mount.
[[nodiscard]] constexpr bool is_mounted(const Unit& unit) noexcept {
    if (unit.kind == UnitKind::DeployedTurret) {
        return unit.deploy_ticks == 0;
    }
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
inline constexpr int kRepairRadiusTiles = 6; ///< a living friendly factory mends units this close
/// N9: THE THINK BEAT, 15 ticks (half a second). The cadence the AI's controllers think on and the
/// idle tanks' defence uses (`game::kDefendCadence` is this), and now the cadence on which a gun
/// chooses its target and proves a line of sight to it. Between beats a gun keeps its choice.
inline constexpr int kThinkBeatTicks = 15;
inline constexpr int kRepairTickInterval = 6; ///< ...by 1 hp every this many ticks
/// SERVICE TRUCKS. A truck mends a friendly unit by `max_hp / kServiceRepairTicks` a tick at its
/// closest, whatever the unit is, so the time to mend scales with max hit points exactly as the
/// time to destroy it does: 20 s for a tank (Harri's figure), 80 s for a factory. This is the ONE
/// rate; no kind has a number of its own.
inline constexpr int kServiceRepairTicks = 20 * kTicksPerSecond;
inline constexpr int kServiceRepairRadiusTiles = 6; ///< a truck mends only what is this close
/// How near a truck SENT to a patient (`orders::Repair`) comes before it holds: well inside the
/// radius, so it mends at over four fifths of the full rate, and clear of a 2x2 building's edge.
inline constexpr int kServiceRepairApproachTiles = 3;
/// How hard one truck mends at distance `d`, in 65536ths of the full rate: the full rate at
/// d = 0, falling LINEARLY IN d^2 (integers all the way; no square root) to `kServiceRepairEdge`
/// at the radius. Closer is faster, and a truck on the very edge still mends, half as quickly.
inline constexpr std::int64_t kServiceRepairFull = 1 << 16;
inline constexpr std::int64_t kServiceRepairEdge = 1 << 15;
/// THE SHOTGUN BEAM (AJ). A truck's mending of a patient is the distance weight above times a SHARE
/// set by how near the truck's facing the patient lies: cos^2 of the angle between them, which is
/// 1.00 dead ahead, 0.75 at 30 degrees, 0.50 at 45, 0.25 at 60 and 0.00 at 90 -- Harri's "shotgun
/// microphone" -- and ZERO for anything behind. In integers it is
/// `dot^2 / (|to patient|^2 * |facing|^2)`, no square root. Everything in reach and ahead of the
/// truck's side-line is mended; the centre simply gets the most.
/// THE KNOB: the share is raised to this power. 1 is cos^2, the wide shotgun; 2 is cos^4, a tighter
/// one (0.56 at 30 degrees, 0.25 at 45). It is the one number that sets the angle and the drop-off.
inline constexpr int kServiceRepairConePower = 1;
/// A turn-limited machine's stored heading is scaled so its larger component is this
/// (`Unit::heading_x`).
inline constexpr std::int32_t kHeadingScale = 1 << 16;
/// A truck's stored facing is scaled so its larger component is this (`Unit::face_x`).
inline constexpr int kServiceFacingScale = 1024;
/// The vector to the patient is read in this many raw Q16.16 units at a time (1/256 of a tile), so
/// that the squared products below stay inside 64 bits at the reach of `kServiceRepairRadiusTiles`.
inline constexpr std::int64_t kServiceConeStep = 256;
/// What truck `truck` gives patient `patient` per tick, in 65536ths of the full rate: the distance
/// weight times the cone's share, or zero when the patient is out of reach, behind the truck, or
/// abeam of it. `service_repair_step` sums it over trucks; the scene asks it to draw only the beams
/// that mend.
[[nodiscard]] std::int64_t service_repair_weight(const Unit& truck, const Unit& patient) noexcept;
/// The unit `Unit::repair_frac` counts in: a hit point is this many `max_hp * weight` units, so
/// that a full-rate truck adds `max_hp * kServiceRepairFull` of them a tick and the hit point
/// completes after `kServiceRepairTicks` of that for a unit with one hit point to its name.
inline constexpr std::int64_t kServiceRepairHitPoint = kServiceRepairTicks * kServiceRepairFull;
/// RECYCLE. What one recycled unit returns to its side's scrap reserve, in the same delivered loads
/// the ledger counts. PROVISIONAL: one flat number is refunded for every kind, chosen to be tunable
/// in one place, not a designed economy; a refund that follows each kind's own `spec_of(kind).cost`
/// would replace it. A recycled unit leaves NO wreckage -- its value goes to the ledger instead of
/// onto the ground -- so a recycle is not a second harvest of the same hull.
inline constexpr std::uint16_t kRecycleOre = 2;
/// How far from a producer's centre a built unit may be put: the half of the 8 x 8 footprint every
/// producer stands on (4 tiles, round 19, L2) and the three tiles of yard it had beyond a point.
inline constexpr int kBuildSpawnRadiusTiles = 7;
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

// VEHICLES AND EACH OTHER. A vehicle cannot be driven through another, but it can be SHOVED: the
// mover pushes what is in its way along with it, and pays for it. No wall -- see `shove_step`.
/// How close two vehicle centres may come before one is "in" the other: three quarters of a tile.
/// Spawn placement keeps units a whole tile apart, so a fresh battle starts with nobody touching.
inline constexpr std::int32_t kBodyRadiusRaw = (3 << 16) / 4;
/// THE COST OF A SHOVE: a unit pushing another moves at this fraction of its speed (2/3) -- the
/// number "with difficulty" is. The shoved vehicle is carried the pusher's step, so the pair moves
/// at two thirds of the pusher's free speed.
inline constexpr std::int64_t kShoveSpeedNumerator = 2;
inline constexpr std::int64_t kShoveSpeedDenominator = 3;
/// The deadlock breaker: after this many consecutive ticks of shoving (1.5 s) a unit stops
/// colliding until it has been clear of every vehicle for a tick. Two vehicles each shoving the
/// other cancel out; without this they could hold one another for ever.
inline constexpr std::uint16_t kShoveYieldTicks = 45;

// THE MORTAR BIKE'S SHOOT-AND-SCOOT (Harri, 2026-11-03: "they go to a spot, fire their mortar, and
// then move to a different spot immediately"). It is the VEHICLE's doctrine, not its weapon's: the
// bomber shares the mortar and does not do it, and the weapon still owns the range. A bike that is
// ENGAGING (an Attack stance) fires, picks a new spot at once and drives to it while it reloads; a
// bike told to move, guard or patrol does what it was told. The relocation is a step round the
// circle that keeps the target in range (`scoot_spot`), so the next shot still reaches it.
/// Whether this kind relocates after every shot it fires from an Attack stance.
[[nodiscard]] constexpr bool scoots(UnitKind kind) noexcept { return kind == UnitKind::MortarBike; }
/// The circle it scoots round the target is this far inside the weapon's range, so the spot, which
/// is snapped to a tile centre, is never past it. One tile.
inline constexpr std::int32_t kScootMarginRaw = Fixed::kOne;
/// How far, in tiles each way from where it stands, a unit with an Attack order and no line to its
/// target looks for a tile that has one (`firing_spot`). A square of 13 x 13 tiles: it bounds the
/// search, and a hill wider than that leaves the unit where it is.
inline constexpr int kFiringSpotReachTiles = 6;
/// The circle is cut into this many equal sectors; a spot is a sector's point on the circle.
inline constexpr int kScootSectors = 16;
/// A relocation moves one or two sectors, either way round -- on the 13-tile circle that is a
/// 5-tile or a 10-tile hop, which the bike's speed covers inside its 45-tick reload. The SPOT is
/// the hop; the INTERVAL is the reload itself (`UnitSpec::cooldown`), because a new spot is
/// chosen exactly when a shot is fired and not otherwise.
inline constexpr std::array<int, 4> kScootSectorSteps{-2, -1, 1, 2};

/// The scavenger (C2). Unarmed, and thickly armoured: the tank's own hull, 100 (Harri, 2026-10-30:
/// "they should take a beating -- they have thick armour"; it WAS 3/5 of that, 60). The consequence
/// is the reason to stop there: a scout deals 4 damage a shot, so killing one now takes 25 shots
/// where it took 15. Above 100 a 20-scrap unarmed vehicle becomes unraidable. Its speed (three
/// quarters of the tank's), its unarmed state and its 20/300 build row are unchanged.
inline constexpr std::uint16_t kScavengerHp = kTankHp;
inline constexpr std::int32_t kScavengerSpeedRaw = kTankSpeedRaw * 3 / 4;
/// THE RADAR'S REACH, in tiles: 40 for every kind but the scout, 80 for the scout (VIS, twice as
/// far). It lives in the sim because the sim reads it too -- a scavenger's collection range IS its
/// radar's range (Harri: "they look for scrap in their radar") -- and the game layer's radar takes
/// its numbers from here, so the two cannot drift apart.
inline constexpr int kRadarRadiusTiles = 40;
inline constexpr int kScoutRadarRadiusTiles = 80;
[[nodiscard]] constexpr int radar_radius_tiles(UnitKind kind) noexcept {
    return kind == UnitKind::Scout ? kScoutRadarRadiusTiles : kRadarRadiusTiles;
}

/// How often one idle scavenger takes a piece of scrap from inside its radar, in ticks: once a
/// second. A dial, Harri's to set; several scavengers over ample scrap multiply it.
inline constexpr int kScavengeIntervalTicks = kTicksPerSecond;

/// HOW CLOSE A SCAVENGER MUST BE TO A PILE OF LITTER TO TAKE IT (AR): one tile, the very radius a
/// MoveTo stops in (`kArriveRadiusRaw`). That is the point: a scavenger sent to a pile is, on
/// arrival, by construction inside the radius that collects it, so there is no gap in which it
/// stands "arrived" yet cannot take the pile; and it is tight enough that a pile two tiles off is
/// a drive, not a reach. Litter is collected by a trip (Harri: "it has to actually go to the spot
/// where the scraps are"); the deployed scavenger's pool is a different thing and has no radius.
inline constexpr std::int32_t kScavengeCollectRadiusRaw = kArriveRadiusRaw;

/// The recycler's hull, and the deployed recycler's. The recycler's is BORROWED from the tank (see
/// its spec entry). The deployed recycler's equals it -- a building that heals or wounds itself by
/// deploying would make the countdown a repair bay -- and it is a DEFAULT, Harri's to change: it is
/// named here, beside the recycler's, so that change is this one line and nothing else.
inline constexpr std::uint16_t kRecyclerHp = kTankHp;
inline constexpr std::uint16_t kDeployedRecyclerHp = kRecyclerHp;

/// DEPLOY: how many ticks a recycler stands at its site before it becomes a `DeployedRecycler` --
/// 300, ten seconds at `kTicksPerSecond`. The one number to change for a faster or slower deploy.
inline constexpr std::uint16_t kDeployTicks = 300;

/// THE SCRAP STORE (Harri, 2026-10-31). A side's store holds this much at the start of the game
/// (Instant Action is always 40; a scenario may supply its own, `Scenario::store_capacity`) and
/// each COMPLETED deployed scavenger adds `kStoreCapacityPerDeployed` -- 40, 60, 80. Space only,
/// never scrap. Losing a deployed scavenger takes the 20 away again and the bank is clamped to what
/// is left.
inline constexpr std::uint16_t kStoreStartCapacity = 40;
inline constexpr std::uint16_t kStoreCapacityPerDeployed = 20;

/// THE TWO POOL RATES (Harri, 2026-11-01: "When the store is in the green section, it accumulates
/// very slowly. When it's in the yellow section, which is what a deployed scavenger gives, it
/// accumulates faster."). A side that holds a deployed scavenger extracts ONE unit per beat, and
/// the beat depends on where the bank's level sits in the store: inside the GOLD band (the bottom
/// `kStoreCapacityPerDeployed` for each deployed scavenger) it is `kPoolFastIntervalTicks`, once
/// the level climbs into the GREEN (the base `kStoreStartCapacity`) it is `kPoolSlowIntervalTicks`.
/// Both are dials, Harri's to set; the fast one keeps the number the single pool rate had, one a
/// second, and the slow one is five times slower. Litter keeps its own, `kScavengeIntervalTicks`.
///
/// THIS DOES NOT CONTRADICT "the rate does not scale with more pools or scavengers". There are
/// exactly these two rates and neither depends on how many scavengers a side has. What a deployed
/// scavenger changes is the SIZE OF THE FAST REGION: the gold band is `kStoreCapacityPerDeployed`
/// tall per deployed scavenger. Three scavengers do not fill faster than one; they give a 60-unit
/// band at the fast rate instead of a 20-unit band. More room AT the fast rate, never a faster
/// rate. The beat is the global tick, not a per-unit timer.
inline constexpr int kPoolFastIntervalTicks = kTicksPerSecond;
inline constexpr int kPoolSlowIntervalTicks = 5 * kTicksPerSecond;

/// THE TURRET'S DEPLOY AND UN-DEPLOY (Harri, 2026-11-01: "Deploying takes maybe five seconds" and
/// "I can un-deploy them at will, but it takes maybe three seconds"). Ticks, at `kTicksPerSecond`:
/// 150 is five seconds and 90 is three. Per-entity, like the recycler's and the scavenger's: a
/// turret's deploy is its own number, and nothing else reads these.
inline constexpr std::uint16_t kTurretDeployTicks = 150;
inline constexpr std::uint16_t kTurretUndeployTicks = 90;

/// How long a scavenger stands on its pool before it is deployed, per side: the human and the alien
/// harvester follow identical rules and only this constant may differ (Harri, 2026-10-31). Five
/// seconds, 150 ticks (his, 2026-11-01); it was a DEFAULT borrowed from the recycler's
/// `kDeployTicks`, which keeps its own 300.
inline constexpr std::array<std::uint16_t, 2> kScavengerDeployTicks{150, 150}; ///< by `Side`
[[nodiscard]] constexpr std::uint16_t scavenger_deploy_ticks(Side side) noexcept {
    return kScavengerDeployTicks.at(static_cast<std::size_t>(side));
}

/// "POINTING AT": a vehicle points at a place when the angle between its facing and the line to the
/// place is no more than this -- the same cone the player's assist uses (`kAssistConeDegrees`, 4.0,
/// pinned equal by a test). The sim tests it in integers, as the tangent in millionths:
/// tan(4 degrees) = 0.0699268.
inline constexpr int kPointingConeDegrees = 4;
inline constexpr std::int64_t kPointingConeTanMillionths = 69927;

/// How many tiles a building covers: `width` along x, `height` along y. A footprint is a RECTANGLE
/// (his correction, 2026-11-11: "they are not all completely square? Some are rectangular?"), so
/// the two numbers are separate everywhere they are carried and nothing may reuse one for both.
struct FootprintSize {
    int width{};
    int height{};
    friend constexpr bool operator==(FootprintSize a, FootprintSize b) noexcept {
        return a.width == b.width && a.height == b.height;
    }
};

/// POWER's hull: the tank's, a DEFAULT borrowed like the recycler's (`kRecyclerHp`) -- the owner's
/// to set in a balance pass.
inline constexpr std::uint16_t kPowerHp = kTankHp;

/// The hull of each of the seven buildings K3 adds: the tank's, a DEFAULT -- the owner's to set.
inline constexpr std::uint16_t kPlacedBuildingHp = kTankHp;

/// ROUND 19, L1: EVERY PLACEABLE BUILDING IS FOUR TIMES AS WIDE AND LONG AS IT WAS. The owner,
/// 2026-11-13: "a 'tile' is actually about four tank lengths. Can we make all buildings four times
/// as wide, long, and tall? ... The buildings seem... puny.  So, 1 x 1 becomes 4 x 4.  2 x 1
/// becomes 8 x 4."  The factor is written ONCE, here, and the table below is the figure the owner
/// reads (1x1 -> 4x4) times it, so a number in the table is still the number he said.
///
/// ROUND 19, L2 (the owner: "I'm afraid we also need to scale the factory and recycler, including a
/// deployed recycler"): the DEPLOYED RECYCLER (`kDeployedRecyclerFootprint`) and the FACTORY in
/// BOTH states -- raised by a Constructor or spawned by a scenario -- scale with the rest. They
/// exist in recorded battles, so a bigger footprint changes what those battles do (a unit that
/// walks round the recycler walks round a bigger one): a stated RE-RECORD, each one named in its
/// test with the number it had.
inline constexpr int kBuildingFootprintScale = 4;

/// A footprint as the owner reads it off the game (in the old, small tiles) made
/// `kBuildingFootprintScale` times as wide and long.
[[nodiscard]] constexpr FootprintSize scaled_footprint(int width, int height) noexcept {
    return FootprintSize{width * kBuildingFootprintScale, height * kBuildingFootprintScale};
}

/// POWER's footprint: ONE tile, 1x1 (the owner's ruling, round 18, K1: "I think Power should take
/// up only 1 x 1.  At least that's what their footprint seems to be in BattleZone 2."), now four
/// times that: 4x4. A footprint is measured in TILES (his word for it was "block"). An EVEN side
/// has a corner for a middle, so a building stands at its rectangle's centre (`place_building`).
inline constexpr FootprintSize kPowerFootprint = scaled_footprint(1, 1);

/// The nine buildings' footprints (the owner read them off the game, 2026-11-12), in tiles, times
/// four (L1). RELAY BUNKER and GUN TOWER are PROVISIONAL 1x1 (so 4x4): he did not name them.
/// TRAINING CTR. and TECH CENTRE are the two RECTANGLES, 2x1 (so 8x4).
inline constexpr FootprintSize kFactoryFootprint = scaled_footprint(2, 2);
inline constexpr FootprintSize kRelayBunkerFootprint = scaled_footprint(1, 1); // PROVISIONAL
inline constexpr FootprintSize kArmoryFootprint = scaled_footprint(1, 1);
inline constexpr FootprintSize kServiceBayFootprint = scaled_footprint(2, 2);
inline constexpr FootprintSize kGunTowerFootprint = scaled_footprint(1, 1); // PROVISIONAL
inline constexpr FootprintSize kTrainingCentreFootprint = scaled_footprint(2, 1);
inline constexpr FootprintSize kTechCentreFootprint = scaled_footprint(2, 1);
inline constexpr FootprintSize kBomberBayFootprint = scaled_footprint(2, 2);

/// The ALIEN Builder's six footprints. THE OWNER'S (2026-11-13), no longer provisional for the
/// three he corrected: STRONGHOLD 4x4, KILN 8x8, ANTENNA 8x8. The GUN SPIRE, DOWER and JAMMER
/// stay as chosen by the job each does on the human side (gun tower 1x1, service bay 2x2, a small
/// 1x1) -- still the assistant's, not his. AN UPGRADE DOES NOT CHANGE THE FOOTPRINT (his
/// correction): the Forge is the Kiln at 8x8 and the Overseer Array is the Antenna at 8x8, so a
/// footprint belongs to the BUILDING and an upgrade replaces it with one of the same size. ALL SIX
/// ARE SQUARE (his belief; that side has no overhead view to measure from), so turning changes
/// nothing. (Each times `kBuildingFootprintScale`.)
inline constexpr FootprintSize kKilnFootprint = scaled_footprint(2, 2);
inline constexpr FootprintSize kGunSpireFootprint = scaled_footprint(1, 1);
inline constexpr FootprintSize kAntennaFootprint = scaled_footprint(2, 2);
inline constexpr FootprintSize kDowerFootprint = scaled_footprint(2, 2);
inline constexpr FootprintSize kStrongholdFootprint = scaled_footprint(1, 1);
inline constexpr FootprintSize kJammerFootprint = scaled_footprint(1, 1);

/// A kind that is BUILT WITH A POWER LUNG (N3): the six alien buildings. Completing one completes
/// its Lung -- no separate placement, no extra cost -- and losing the Lung disables it. (The
/// Matriarch is not one of these: it is a deployed unit, not a Builder's building. The Forge's and
/// the Overseer Array's SECOND lung belongs to the upgrade, which is not built.)
[[nodiscard]] constexpr bool needs_lung(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::Kiln:
    case UnitKind::GunSpire:
    case UnitKind::Antenna:
    case UnitKind::Dower:
    case UnitKind::Stronghold:
    case UnitKind::Jammer:
        return true;
    default:
        return false;
    }
}

/// A kind a CONSTRUCTOR places (`orders::Place`): the nine buildings of its panel, the Factory
/// included. The deployed recycler has a footprint too but is made by a deploy, never placed.
[[nodiscard]] constexpr bool is_placeable(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::Power:
    case UnitKind::Factory:
    case UnitKind::RelayBunker:
    case UnitKind::Armory:
    case UnitKind::ServiceBay:
    case UnitKind::GunTower:
    case UnitKind::TrainingCentre:
    case UnitKind::TechCentre:
    case UnitKind::BomberBay:
    case UnitKind::Kiln:
    case UnitKind::GunSpire:
    case UnitKind::Antenna:
    case UnitKind::Dower:
    case UnitKind::Stronghold:
    case UnitKind::Jammer:
        return true;
    default:
        return false;
    }
}

/// Whether THIS unit is one of the SCALED buildings: a kind that stands on a footprint (since round
/// 19, L2 that is every placeable kind and the deployed recycler too).
[[nodiscard]] constexpr bool is_scaled_building(const Unit& unit) noexcept {
    return has_footprint(unit.kind);
}

/// Whether THIS unit's MODEL is drawn at `kBuildingFootprintScale` times its authored size: a
/// scaled building, and the wheeled RECYCLER -- a vehicle with no footprint, but the same machine
/// the deployed one is (L2). Everything that draws or picks at the larger size asks this.
[[nodiscard]] constexpr bool is_scaled_model(const Unit& unit) noexcept {
    return is_scaled_building(unit) || unit.kind == UnitKind::Recycler;
}

/// The deployed recycler's footprint: 2x2 tiles (his ruling, 2026-10-26: "2x2 tiles -- a building
/// should not be the same size as a tank"), four times that since round 19, L2: 8x8.
inline constexpr FootprintSize kDeployedRecyclerFootprint = scaled_footprint(2, 2);

/// THE TABLE: the size of the footprint a kind stands on, 0x0 for a kind that has none (it covers
/// no tile). The Factory's is the same whoever raised it. `test_footprint.cpp` pins the whole table
/// so an edit is deliberate, never a drift.
[[nodiscard]] constexpr FootprintSize footprint_of(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::DeployedRecycler:
        return kDeployedRecyclerFootprint;
    case UnitKind::Power:
        return kPowerFootprint;
    case UnitKind::Factory:
        return kFactoryFootprint;
    case UnitKind::RelayBunker:
        return kRelayBunkerFootprint;
    case UnitKind::Armory:
        return kArmoryFootprint;
    case UnitKind::ServiceBay:
        return kServiceBayFootprint;
    case UnitKind::GunTower:
        return kGunTowerFootprint;
    case UnitKind::TrainingCentre:
        return kTrainingCentreFootprint;
    case UnitKind::TechCentre:
        return kTechCentreFootprint;
    case UnitKind::BomberBay:
        return kBomberBayFootprint;
    case UnitKind::Kiln:
        return kKilnFootprint;
    case UnitKind::GunSpire:
        return kGunSpireFootprint;
    case UnitKind::Antenna:
        return kAntennaFootprint;
    case UnitKind::Dower:
        return kDowerFootprint;
    case UnitKind::Stronghold:
        return kStrongholdFootprint;
    case UnitKind::Jammer:
        return kJammerFootprint;
    default:
        return FootprintSize{};
    }
}

/// The tiles a building stands on: the `width` x `height` rectangle whose NORTH-WEST (lowest x,
/// lowest y) tile is (`x`, `y`). The building itself stands at the rectangle's centre, which for an
/// even side is the corner its middle tiles share -- a whole-tile position, like a spawn's.
struct Footprint {
    int x{};
    int y{};
    int width{};
    int height{};
    [[nodiscard]] constexpr bool covers(int tile_x, int tile_y) const noexcept {
        return tile_x >= x && tile_x < x + width && tile_y >= y && tile_y < y + height;
    }
    /// The one place that walks "which tiles does this building stand on": true when `ok(tx, ty)`
    /// holds for every tile of the rectangle.
    template <typename Predicate>
    [[nodiscard]] constexpr bool all_tiles(const Predicate& ok) const {
        for (int dy = 0; dy < height; ++dy) {
            for (int dx = 0; dx < width; ++dx) {
                if (!ok(x + dx, y + dy)) {
                    return false;
                }
            }
        }
        return true;
    }
    /// The building's position, in whole tiles: the rectangle's centre.
    [[nodiscard]] constexpr int centre_x() const noexcept { return x + width / 2; }
    [[nodiscard]] constexpr int centre_y() const noexcept { return y + height / 2; }
};

/// The footprint a placed building stands on: its kind's rectangle, with width and height swapped
/// when it was turned a quarter.
[[nodiscard]] constexpr FootprintSize footprint_of(const Unit& unit) noexcept {
    const FootprintSize size = footprint_of(unit.kind);
    return unit.turned ? FootprintSize{size.height, size.width} : size;
}

/// The footprint of `size` that a placement aimed at tile (`tile_x`, `tile_y`) builds: that tile is
/// its north-west corner, moved in from the east and south edges just far enough that the whole
/// rectangle is on the map. Only a tile ON the map is clipped: a deploy aimed off it is refused
/// (`site_ok`) before this is asked, never pulled back onto the map.
[[nodiscard]] constexpr Footprint footprint_at(FootprintSize size, int tile_x,
                                               int tile_y) noexcept {
    const int last_x = kMapTiles - size.width; ///< the last north-west tile, per axis
    const int last_y = kMapTiles - size.height;
    return Footprint{tile_x > last_x ? last_x : tile_x, tile_y > last_y ? last_y : tile_y,
                     size.width, size.height};
}

/// The deploy order's footprint: the deployed recycler's, at the aimed tile.
[[nodiscard]] constexpr Footprint footprint_at(int tile_x, int tile_y) noexcept {
    return footprint_at(kDeployedRecyclerFootprint, tile_x, tile_y);
}

/// The footprint of a building of `size` standing at (`x`, `y`): the rectangle centred there (the
/// inverse of `Footprint::centre_x`/`centre_y`).
[[nodiscard]] constexpr Footprint footprint_around(FootprintSize size, Fixed x, Fixed y) noexcept {
    return Footprint{x.floor_int() - size.width / 2, y.floor_int() - size.height / 2, size.width,
                     size.height};
}

/// The footprint of the placed building `unit`: its kind's rectangle, turned if it was.
[[nodiscard]] constexpr Footprint footprint_around(const Unit& unit) noexcept {
    return footprint_around(footprint_of(unit), unit.x, unit.y);
}

/// The footprint of the building `kind` standing at (`x`, `y`).
[[nodiscard]] constexpr Footprint footprint_around(UnitKind kind, Fixed x, Fixed y) noexcept {
    return footprint_around(footprint_of(kind), x, y);
}

/// THE DRAWN BOX (N5). A building's body -- what blocks a unit, and what stops a bolt -- is the box
/// it is DRAWN as, which is the box the pick ray tests (`game::pick`), and the game reads it from
/// here so the picture, the pick and the sim cannot drift. The box is as far from the building's
/// centre as the pick reaches: `kBoxReachTiles` (the pick radius, three quarters of a tile, times
/// the model scale), or the footprint's own half-extent where that is shorter. So a four-wide
/// building is drawn to its whole footprint, and an eight-wide one is six wide: the outer tile on
/// each side is a RING a unit may drive on, and two eight-wide buildings that touch leave a
/// two-tile corridor between their boxes. It is a BOX; a kind that declares a `SolidMask` (M4, the
/// Gun Spire) is solid on the mask instead (`building_solid_at`), and the box is what a kind
/// with no mask stays.
inline constexpr int kBoxReachTiles = 3;

/// The extent of the box along a footprint side of `side` tiles.
[[nodiscard]] constexpr int drawn_extent(int side) noexcept {
    return side < 2 * kBoxReachTiles ? side : 2 * kBoxReachTiles;
}

/// The tiles the box covers: centred on the footprint, `drawn_extent` along each side (the
/// footprint sides are multiples of four, so the margin is a whole number of tiles).
[[nodiscard]] constexpr Footprint drawn_box(const Footprint& whole) noexcept {
    const int w = drawn_extent(whole.width);
    const int h = drawn_extent(whole.height);
    return Footprint{whole.x + (whole.width - w) / 2, whole.y + (whole.height - h) / 2, w, h};
}

/// The box of the placed building `unit` (turned if it was).
[[nodiscard]] constexpr Footprint drawn_box(const Unit& unit) noexcept {
    return drawn_box(footprint_around(unit));
}

/// THE SOLID MASK (M4): a bitmap of IMPASSABLE tiles in the frame of whatever carries it -- one
/// concept, two frames. A BUILDING carries one in its OWN frame (`solid_mask_of`: tile (0, 0) is
/// the mask's north-west corner, whichever way the building is turned), so the shape travels with
/// the building when it is placed, turned or deployed. A WORLD-anchored shape is the same type
/// placed at a world tile (`solid_placed` with `turned` false and the origin a map tile): that is
/// the room author-placed impassable ground will use. (The map file that would carry it is a later
/// slice; today the only world-frame impassable ground is the Crater, `tile_passable`.)
///
/// `bits` is row-major, bit `ly * width + lx`; at most 64 tiles (8 x 8).
struct SolidMask {
    int width{};
    int height{};
    std::uint64_t bits{};

    /// A mask with no tiles declared: its owner keeps the old rule (the drawn box).
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

    /// Whether mask tile (`lx`, `ly`) is solid; false outside the mask.
    [[nodiscard]] constexpr bool solid(int lx, int ly) const noexcept {
        return lx >= 0 && ly >= 0 && lx < width && ly < height
               && ((bits >> static_cast<unsigned>(ly * width + lx)) & 1U) != 0;
    }

    /// Whether WORLD tile (`tx`, `ty`) is solid when the mask stands with its north-west corner on
    /// (`x`, `y`). `turned` is a quarter turn the way the picture turns a building (what faced
    /// south faces east: local (dx, dy) lands on world (dy, -dx)), so the placed rectangle is
    /// `height` wide and `width` tall.
    [[nodiscard]] constexpr bool solid_placed(int x, int y, bool turned, int tx,
                                              int ty) const noexcept {
        const int wx = tx - x;
        const int wy = ty - y;
        return turned ? solid(width - 1 - wy, wx) : solid(wx, wy);
    }
};

/// Rows of a mask, top (north) row first, one bit per tile with bit 0 the west-most:
/// `rows<4>({...})`.
template <std::size_t N>
[[nodiscard]] constexpr SolidMask mask_of_rows(int width, const std::array<unsigned, N>& rows) {
    std::uint64_t bits = 0;
    for (std::size_t ly = 0; ly < N; ++ly) {
        bits |= static_cast<std::uint64_t>(rows.at(ly)) << (ly * static_cast<std::size_t>(width));
    }
    return SolidMask{width, static_cast<int>(N), bits};
}

/// The GUN SPIRE's mask: two PRONGS, the outer columns of its 4 x 4 footprint, standing the whole
/// depth, with a two-tile gap between them that a unit may drive through. AUTHORED, not derived
/// from the model: the model's prongs are blades in the air (2.2 to 3.5 m up) and its ground
/// silhouette is one root disc (3.2 tiles across), so reading the ground shape off the model would
/// give back a block. The footprint is four tiles wide, which holds two prongs and a gap; three
/// prongs with a gap between each would need five. Symmetric under a half turn, so an enemy's
/// model (drawn turned a half turn further) and a player's are one shape.
inline constexpr SolidMask kGunSpireMask = mask_of_rows<4>(4, {0b1001U, 0b1001U, 0b1001U, 0b1001U});

/// The solid mask of `kind`, in the building's own frame; EMPTY for a kind that declares none,
/// which is then as solid as its drawn box (`drawn_box`), exactly as before M4. A declared mask is
/// the size of the kind's footprint (a test pins it).
[[nodiscard]] constexpr SolidMask solid_mask_of(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::GunSpire:
        return kGunSpireMask;
    default:
        return SolidMask{};
    }
}

/// THE ONE GEOMETRY (M4): whether the placed building `unit` is solid on tile (`tile_x`, `tile_y`).
/// What stops a unit (`building_stops_units`) and what stops a bolt (`for_each_solid_rect`) both
/// read it, so what may pass and what is hit are the same shape. A kind with no mask is solid on
/// its drawn box; one with a mask, on the mask's solid tiles, placed on the footprint and turned
/// with the building. Never outside the footprint.
[[nodiscard]] constexpr bool building_solid_at(const Unit& unit, int tile_x, int tile_y) noexcept {
    const SolidMask mask = solid_mask_of(unit.kind);
    if (mask.empty()) {
        return drawn_box(unit).covers(tile_x, tile_y);
    }
    const Footprint whole = footprint_around(unit);
    return whole.covers(tile_x, tile_y)
           && mask.solid_placed(whole.x, whole.y, unit.turned, tile_x, tile_y);
}

/// Calls `visit(Footprint)` for each rectangle that together make up the building's solid shape:
/// the one drawn box for a kind with no mask, else one rectangle per solid tile. The very tiles
/// `building_solid_at` answers for.
template <typename Visitor>
constexpr void for_each_solid_rect(const Unit& unit, const Visitor& visit) {
    if (solid_mask_of(unit.kind).empty()) {
        visit(drawn_box(unit));
        return;
    }
    const Footprint whole = footprint_around(unit);
    for (int ty = whole.y; ty < whole.y + whole.height; ++ty) {
        for (int tx = whole.x; tx < whole.x + whole.width; ++tx) {
            if (building_solid_at(unit, tx, ty)) {
                visit(Footprint{tx, ty, 1, 1});
            }
        }
    }
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
    std::uint16_t turn_decidegrees{}; ///< tenths of a degree a tick; zero turns at once
};
[[nodiscard]] UnitProfile profile(UnitKind kind) noexcept;

/// 1 = full speed on Flat, 0 = cannot enter. Craters are impassable, ridges are slow.
[[nodiscard]] Fixed terrain_speed_factor(world::Terrain terrain) noexcept;

} // namespace protekt::sim
