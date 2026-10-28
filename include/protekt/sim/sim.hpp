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
};

/// A battle is a seed plus its starting units. Everything else is derived.
struct Scenario {
    std::uint64_t seed{};
    std::vector<Spawn> spawns{};
    /// The litter the battle OPENS with (C0), placed by whoever built the scenario -- `skirmish`
    /// lays a field near each base. Empty for a battle assembled by hand, so a rig starts on clean
    /// ground exactly as it did before there was a field at all.
    std::vector<Scrap> scrap{};
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

/// Whether a deploy aimed at tile (`tile_x`, `tile_y`) has somewhere to stand: the tile is on the
/// map, and every tile of `footprint_at(tile_x, tile_y)` is ground a unit may enter
/// (`tile_passable`) and inside no other building's footprint (`building_blocks`). Asked when the
/// order is enqueued, when it is applied, and on every tick of its countdown.
[[nodiscard]] bool deploy_site_ok(std::uint64_t seed, std::span<const Unit> units, int tile_x,
                                  int tile_y) noexcept;

/// The simulation. Deterministic by construction: no floating point, no unseeded randomness,
/// no time, no iteration over unordered storage.
///
/// A tick runs in THIS order (fixed, because the outcome depends on it):
///   1. every order enqueued since the last step is applied in the order it was enqueued;
///      an order naming a unit that does not exist, is dead, or belongs to the other side is
///      dropped without effect;
///   2. units act in ascending unit-id order: a scavenger fetches (it is sent to the nearest
///      wreckage, or home with a load aboard, and picks up or hands over anything it is standing
///      next to -- C2; an order outranks it, always); a recycler under a Deploy order whose site
///      has stopped being usable (`deploy_site_ok`) drops the order and stands, an idle recycler,
///      and one that is standing at its site counts one tick and, on the `kDeployTicks`th, becomes
///      a `DeployedRecycler` where the site's centre is -- same id, same hit points -- and does not
///      move; then a mobile unit with a MoveTo/GuardAt/Patrol/Fetch/Deploy stance steers towards
///      its goal, then every unit's position moves by its velocity; map bounds clamp, and a unit
///      may not enter an impassable tile -- a crater or a building's footprint;
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
///   6. every unit this tick's damage killed leaves its wreckage on the ground (C1): scrap pieces
///      around the tile it stood on, in ascending victim id, so the litter is a function of the
///      battle and not of the order the bolts happened to fly in;
///   7. the tick counter advances.
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
    /// currency, spent by a factory that builds (C4). A count, so repair stays free.
    [[nodiscard]] std::uint16_t ore(Side side) const noexcept;
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
    /// Delivered loads per side, indexed by `Side` (C3). Hashed only when either side has
    /// delivered anything, so a battle with no scavenger hashes exactly as before.
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
    std::uint32_t tick_{};
    std::uint32_t next_id_{1};
    std::uint32_t route_plans_{};
};

} // namespace protekt::sim
