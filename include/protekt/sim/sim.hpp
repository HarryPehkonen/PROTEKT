#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "protekt/sim/objective.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/outcome.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// One unit to place at the start of a battle.
struct Spawn {
    UnitKind kind{};
    Side side{};
    int tile_x{}; ///< whole tiles; converted internally
    int tile_y{};
    /// The hit points it starts with; zero (the default) is the kind's full complement, and a
    /// larger number than that is clamped to it. A battle that begins with a wounded unit is the
    /// only way a repair can be told apart from a unit that was never hurt.
    std::uint16_t hp{};
    /// A SERVICE TRUCK's facing at the start (AJ): a direction, in whole numbers and any length
    /// (only the direction counts). Zero in both (the default) is +Y, which is how a hull that has
    /// never moved is drawn. Like `hp`, a fixture: the replay format does not carry it.
    int face_x{};
    int face_y{};
};

/// A battle is a seed plus its starting units. Everything else is derived.
struct Scenario {
    std::uint64_t seed{};
    std::vector<Spawn> spawns{};
    /// The litter the battle OPENS with (C0), placed by whoever built the scenario -- `skirmish`
    /// lays a field near each base. Empty for a battle assembled by hand, so a rig starts on clean
    /// ground exactly as it did before there was a field at all.
    std::vector<Scrap> scrap{};
    /// The biometal pools the map has: fixed places, laid by whoever built the scenario (`skirmish`
    /// lays them from the seed). Empty for a battle assembled by hand, which then has nowhere for a
    /// scavenger to deploy. Scenario data, never hashed.
    std::vector<Pool> pools{};
    /// What each side's scrap store holds before any scavenger has deployed
    /// (`kStoreStartCapacity`).
    std::uint16_t store_capacity{kStoreStartCapacity};
    /// What the PLAYER's bank holds at tick 0. Zero for every real battle (the economy starts dry);
    /// a rig that tests a press against the bank sets it, so it need not farm scrap first. Never
    /// above `store_capacity`: the store clamps the bank to that each tick.
    std::uint16_t start_ore{0};
    /// The conditions the war is decided by (OBJ), in the order they are asked. Defaulted to the
    /// built-in rule so that a battle assembled by hand is the war the game ships with, and a
    /// mission is a battle that names its own.
    std::vector<Objective> objectives = built_in_objectives();
};

/// The channel every commander writes into: the human's input layer, the scripted opponent, an AI,
/// and a replay being replayed. Orders are stamped with the tick they take effect on.
///
/// The sim CANNOT tell who issued an order, so it does not filter by side: it accepts any order
/// that is valid in itself (a living unit; for Attack, a living hostile target) whatever side that
/// unit is on. A commander is responsible for ordering only the units it owns.
class OrderSink {
public:
    virtual ~OrderSink() = default;
    virtual void enqueue(const Order& order) = 0;

protected:
    // Copying or moving through the interface would slice the derived commander or sim, so only
    // the derived classes may copy and move it.
    OrderSink() = default;
    OrderSink(const OrderSink&) = default;
    OrderSink& operator=(const OrderSink&) = default;
    OrderSink(OrderSink&&) = default;
    OrderSink& operator=(OrderSink&&) = default;
};

/// True when the TERRAIN lets a unit stand on the tile: inside the map and not a crater. The
/// ground's half of the question the mover asks, answered for a caller that is not the mover; a
/// building's footprint is the other half (`building_blocks`), and it is never folded in here.
[[nodiscard]] bool tile_passable(std::uint64_t seed, int tile_x, int tile_y) noexcept;

/// True when a living deployed building of either side stands on the tile (its `Footprint` covers
/// it). THE one answer to "is a building in the way": the mover, the route planner, every spawn and
/// the deploy site's own check all ask it, so no unit walks through, or is placed inside, one.
[[nodiscard]] bool building_blocks(std::span<const Unit> units, int tile_x, int tile_y) noexcept;

/// The living Constructor of `site`'s own side that is within `kBuildReachTiles` of its footprint
/// (the nearest point of the rectangle), lowest id first; null when there is none. THE one answer
/// to "is this site being built": the sim counts the build down while it is non-null, and the
/// scene draws the beam from the Constructor it names -- the drawing reads the sim's rule and
/// never keeps one of its own.
[[nodiscard]] const Unit* site_builder(std::span<const Unit> units, const Unit& site) noexcept;

/// SETTLES WRECKAGE (see sim.cpp): moves each of `scraps[first..]` onto a tile a hauler can stand
/// on and, when `home_x >= 0`, onto one a hauler could DRIVE to from the wreck's tile (`home_x/y`)
/// -- a bounded flood fill, so a piece is never left on a tile ringed by craters. `home_x < 0` is a
/// scenario's own pile: placed on standable ground, with no connectivity check.
void settle_litter(std::vector<Scrap>& scraps, std::size_t first, std::uint64_t seed,
                   std::span<const Unit> units, int home_x, int home_y) noexcept;

/// Where a relocating unit (`scoots`) goes after a shot at `target` fired on `tick`.
struct ScootSpot {
    Fixed x{};
    Fixed y{};
};

/// THE SPOT a mortar bike scoots to: a point on the circle round `target` that is `kScootMarginRaw`
/// inside the weapon's range, one or two sectors (`kScootSectorSteps`) round from where the bike
/// stands. Which of the four hops is the first tried comes from a hash of (seed, the bike's id,
/// `tick`) -- no runtime randomness, so the same battle scoots the same way and two bikes do not
/// choose alike. A hop onto ground nobody can stand on is skipped for the next; empty only when
/// all four are, and then the bike stays. Never another unit's tile: two bikes that pick one spot
/// shove each other apart, like every other pair of vehicles.
[[nodiscard]] std::optional<ScootSpot> scoot_spot(std::uint64_t seed, std::span<const Unit> units,
                                                  const Unit& bike, const Unit& target,
                                                  Tick tick) noexcept;

/// THE SPOT an attacker with no line goes to: the tile centre nearest the unit, within
/// `kFiringSpotReachTiles` of it, that can be stood on, is inside the weapon's range of `target`
/// and from which `has_line_from` holds. Nearest by squared distance from where the unit stands;
/// a tie goes to the lowest row and then the lowest column. Integer only and a pure function of the
/// state, so the same battle chooses the same spot. Empty when the unit has no such tile within
/// reach (it then stays), and for a kind that cannot move.
[[nodiscard]] std::optional<ScootSpot> firing_spot(std::uint64_t seed, std::span<const Unit> units,
                                                   const Unit& shooter,
                                                   const Unit& target) noexcept;

/// Whether a deploy aimed at tile (`tile_x`, `tile_y`) has somewhere to stand: the tile is on the
/// map, and every tile of `footprint_at(tile_x, tile_y)` is ground a unit may enter
/// (`tile_passable`) and inside no other building's footprint (`building_blocks`). Asked when the
/// order is enqueued, when it is applied, and on every tick of its countdown.
[[nodiscard]] bool deploy_site_ok(std::uint64_t seed, std::span<const Unit> units, int tile_x,
                                  int tile_y) noexcept;

/// The same rules for a footprint of any `size` (a rectangle: width and height are separate):
/// the aimed tile is on the map, and every tile of `footprint_at(size, tile_x, tile_y)` is
/// standable. `deploy_site_ok` is this with the deployed recycler's size.
[[nodiscard]] bool site_ok(std::uint64_t seed, std::span<const Unit> units, FootprintSize size,
                           int tile_x, int tile_y) noexcept;

/// Whether a Constructor may place a building of `kind`, `turned` or not, with its footprint's
/// north-west tile at (`tile_x`, `tile_y`): the kind is placeable (`is_placeable`), `turned` swaps
/// the footprint's width and height, and the site is `site_ok` for that rectangle -- on the map, on
/// ground a unit may enter, inside no other building's footprint -- with no living unit standing
/// on any of its tiles. The ONE answer: the sim asks it when the order is enqueued and applied, and
/// the game's preview draws from it, so green means SPACE will place.
[[nodiscard]] bool place_ok(std::uint64_t seed, std::span<const Unit> units, UnitKind kind,
                            bool turned, int tile_x, int tile_y) noexcept;

/// The simulation. Deterministic by construction: no floating point, no unseeded randomness,
/// no time, no iteration over unordered storage.
///
/// A tick runs in THIS order (fixed, because the outcome depends on it):
///   1. every order enqueued since the last step is applied in the order it was enqueued;
///      an order naming a unit that does not exist, is dead, or belongs to the other side is
///      dropped without effect;
///   2. units act in ascending unit-id order: an idle scavenger takes the nearest scrap inside
///      its radar, once a `kScavengeIntervalTicks`, and the bank is credited in the same tick --
///      it neither walks to the scrap nor carries it anywhere (C2; any order outranks it); a
///      recycler under a Deploy order whose site has stopped being usable (`deploy_site_ok`) drops
///      the order and stands, an idle recycler, and one that is standing at its site counts one
///      tick and, on the `kDeployTicks`th, becomes a `DeployedRecycler` where the site's centre is
///      -- same id, same hit points -- and does not move; a turret under a Deploy or Defend order
///      drives to its spot (or to within weapon range of its enemy) and on the
///      `kTurretDeployTicks`th tick there becomes a `DeployedTurret`, and a deployed turret told to
///      leave (an Undeploy, or any order that goes somewhere) counts `kTurretUndeployTicks` and
///      becomes a plain `Turret` again -- the one kind that changes both ways
///      (`turret_deploy_step`); then a mobile unit with a MoveTo/GuardAt/Patrol/Deploy stance
///      steers towards its goal, then every unit's position moves by its velocity; map bounds
///      clamp, and a unit may not enter an impassable tile -- a crater or a building's footprint;
///   2b. every SITE still being built (`Unit::build_left`) with a friendly Constructor within reach
///      has one tick less to build (K2); it stands on the tick the count reaches zero;
///   3. every projectile already in the air advances, in spawn order; one that reaches a living
///      hostile lands its damage there (see advance_projectiles), so a unit killed here does not
///      fire this tick;
///   4. units fire in ascending unit-id order: a unit under an Attack order fires at its named
///      target while that target is alive and in range -- an explicit order outranks
///      auto-engagement -- and otherwise at the nearest hostile in range (distance is
///      squared fixed-point; ties go to the lowest id). A target the gun has no line to (see
///      `has_line`) is not a target: a gun does not shoot a hill, costs no bolt and is not
///      reported as having fired -- except the one the trigger named, which is the player's call.
///      Firing spawns a projectile aimed at the
///      target's position NOW plus the gun's learned aim estimate (see `launch`); the damage
///      arrives when the projectile does. Damage is exact:
///      there is no spread, so the sim needs no RNG;
///   5. every living unit near a living friendly factory mends 1 hp on the global repair beat
///      (`tick % kRepairTickInterval`), after the shooting, so repair never undoes a kill;
///   5b. every friendly SERVICE TRUCK mends the hurt units within `kServiceRepairRadiusTiles`, each
///      by `max_hp / kServiceRepairTicks` a tick at the truck's own spot, falling linearly in the
///      squared distance to half that at the radius, the trucks adding (`service_repair_step`);
///   6. every unit this tick's damage killed leaves its wreckage on the ground (C1): scrap pieces
///      around the tile it stood on, in ascending victim id, so the litter is a function of the
///      battle and not of the order the bolts happened to fly in;
///   6b. THE STORE (POOLS): each side's bank is clamped to its capacity (`capacity`: the scenario's
///      store plus 20 per living deployed scavenger), so a deployed scavenger killed this tick has
///      already taken its 20 and the scrap above the new capacity; then each side holding a
///      deployed scavenger extracts ONE unit on the `kPoolFastIntervalTicks` beat, however many
///      pools it holds, into whatever room is left. (In step 2 a scavenger under a `Deploy` order
///      drives to its pool, counts `scavenger_deploy_ticks` and becomes a `DeployedScavenger`; a
///      full store stops its collecting of litter.)
///   7. every PRODUCER's build queue runs, in ascending id (BUILD-QUEUE): the front entry waits for
///      its scrap -- the one `ore` bank, paused and never cancelled while it is short -- pays its
///      whole cost on the tick it starts, counts its build time down, and on the last tick its
///      unit appears beside the producer, never inside a building's footprint;
///   8. the tick counter advances.
class Sim final : public OrderSink {
public:
    [[nodiscard]] static Sim create(const Scenario& scenario);

    void enqueue(const Order& order) override;
    void step();

    [[nodiscard]] std::uint32_t tick() const noexcept;
    /// The seed the ground comes from.
    [[nodiscard]] std::uint64_t seed() const noexcept;
    [[nodiscard]] const std::vector<Unit>& units() const noexcept;
    [[nodiscard]] const Unit* find(UnitId id) const noexcept;

    /// The shots in the air, in the order they were fired.
    [[nodiscard]] const std::vector<Projectile>& projectiles() const noexcept;

    /// The wreckage on the ground, in the order it was made (C1). Litter, not an actor: a scavenger
    /// walks to it and carries it home (C2), and its own state is hashed with the rest.
    [[nodiscard]] const std::vector<Scrap>& scraps() const noexcept;

    /// How many loads a side has delivered to its own factory (C3). The war's ledger: the only
    /// currency, spent by a producer's queue. A count, so repair stays free.
    [[nodiscard]] std::uint16_t ore(Side side) const noexcept;
    /// How many pieces of biometal a side's store has TAKEN IN over the battle: litter a scavenger
    /// picked up and the pool extraction a deployed scavenger banks. A tally for the statistics
    /// screen -- output, not state: never spent, never hashed, and nothing in the sim reads it. A
    /// recycle's refund and a cancelled build's refund are not collecting and are not counted.
    [[nodiscard]] std::uint32_t collected(Side side) const noexcept;
    /// How much a side's store can hold now: the scenario's `store_capacity` plus 20 per living
    /// DEPLOYED scavenger (`store_capacity`). The bank never exceeds it.
    [[nodiscard]] std::uint16_t capacity(Side side) const noexcept;
    /// The GOLD band of that store: the room the side's deployed scavengers added (`capacity` minus
    /// the scenario's base). The bank fills at the fast pool rate while its level is inside it.
    [[nodiscard]] std::uint16_t fast_band(Side side) const noexcept;
    /// The biometal pools of this battle, in the order the scenario listed them.
    [[nodiscard]] const std::vector<Pool>& pools() const noexcept;
    /// The bolts that landed during the most recent step. Output, not state: it is rebuilt by every
    /// step and is not part of the hash.
    [[nodiscard]] const std::vector<Impact>& impacts() const noexcept;

    /// The shots this step made: the firing unit and the unit it was aimed at, or an invalid target
    /// for a shot fired at nothing. Cleared at the start of every step, like `impacts()`, and NOT
    /// part of `state_hash()`: it describes a step, not a state.
    struct Shot {
        UnitId shooter{};
        UnitId target{}; ///< invalid for a shot at nothing (`orders::FireTowards`)
    };
    [[nodiscard]] const std::vector<Shot>& shots() const noexcept;

    /// The shells that came down during the most recent step (see `Landing`). Observation output,
    /// not state: cleared at the start of every step like `impacts()` and `shots()`, and NOT part
    /// of `state_hash()`.
    [[nodiscard]] const std::vector<Landing>& landings() const noexcept;

    /// Every command that was enqueued and accepted, in order. A replay is this list.
    [[nodiscard]] const std::vector<Command>& command_log() const noexcept;

    /// How many A* route plans this battle has run. Output, not state: a cost meter, not hashed.
    [[nodiscard]] std::uint32_t route_plans() const noexcept;

    /// HAS THIS SIDE LOST EVERY UNIT OF THIS KIND? True when the side was given at least one `kind`
    /// and none of them is alive now. The one predicate the war's conditions are written in:
    /// `war_outcome` asks the same question about each side's recycler -- counting the vehicle and
    /// the building it deploys into as one, because deploying changes the kind of the same unit --
    /// and a mission's own conditions (an escort's APC, say) are meant to ask it about theirs
    /// rather than restate it. DERIVED, never
    /// stored and never hashed: a read over the unit table, in which a dead unit stays as an entry.
    ///
    /// A side that NEVER HAD a unit of the kind has not lost one, so this is false for it. That
    /// guard is what keeps a test rig whose scenario spawns no recycler from reading as a defeat on
    /// tick zero.
    [[nodiscard]] bool kind_wiped_out(Side side, UnitKind kind) const noexcept;

    /// HOW THE WAR STANDS. The player WINS when the enemy's recycler (the alien team's matriarch),
    /// deployed or not, is dead, whatever else the enemy has left, and LOSES when the player's own
    /// is dead, whatever
    /// else the player has left: instantly and completely. If both are dead the player has lost.
    /// Anything else, including a battle in which no recycler was ever spawned, is Undecided.
    /// DERIVED, never stored and never hashed: a battle that nobody asks produces exactly the
    /// state it did before this existed. This REPLACES C8's attrition verdict (no gun and no
    /// factory able to build one), which Harri retired on 2026-10-24. Whether the game shows the
    /// outcome, and how, is the game layer's business: the sim only answers the question.
    [[nodiscard]] WarOutcome war_outcome() const noexcept;

    /// A hash of the ENTIRE simulation state that can be replayed (units, projectiles and tick).
    /// The determinism oracle: a replay must reproduce it exactly, on any machine.
    [[nodiscard]] std::uint64_t state_hash() const noexcept;

private:
    Scenario scenario_{};
    std::vector<Unit> units_{};
    std::vector<Projectile> projectiles_{};
    std::vector<Scrap> scraps_{};
    /// The scrap banked per side, indexed by `Side` (C3): credited the tick a scavenger takes a
    /// piece, or a unit is recycled. Hashed only when either side has any, so a battle with no
    /// scavenger hashes exactly as before.
    std::array<std::uint16_t, 2> ore_{};
    std::vector<Impact> impacts_{};
    std::vector<Shot> shots_{};
    std::vector<Landing> landings_{};
    std::vector<Order> pending_{};
    std::vector<Command> log_{};
    /// THE ONE UNIT THE PLAYER IS DRIVING, learned from a `TakeControl` order and invalid until
    /// one arrives. The sim carries it because step 4's auto-engagement skips exactly this unit: a
    /// hull under the player's hand fires when the trigger says so and not before. An order rather
    /// than a call into the sim, so a replay's recorded order stream is the whole input and a
    /// replayed battle cannot diverge from the live one. Never cleared: unit ids are not reused,
    /// and the skip tests `unit.id == driven_` only for a living unit, so a dead helm excludes no
    /// one.
    UnitId driven_{};
    /// How many lone builds have been given a batch id (BUILD-QUEUE): the next is
    /// `kFirstLoneBatch + lone_batches_`. Hashed only once non-zero.
    std::uint32_t lone_batches_{};
    std::uint32_t tick_{};
    std::uint32_t next_id_{1};
    std::uint32_t route_plans_{};
    /// `collected`'s tally, per `Side`. Not hashed.
    std::array<std::uint32_t, 2> collected_{};
};

} // namespace protekt::sim
