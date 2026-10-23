#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protekt/sim/orders.hpp"
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

/// True when a unit may stand on the tile: inside the map and not a crater. The question the mover
/// asks, answered for a caller that is not the mover.
[[nodiscard]] bool tile_passable(std::uint64_t seed, int tile_x, int tile_y) noexcept;

/// The simulation. Deterministic by construction: no floating point, no unseeded randomness,
/// no time, no iteration over unordered storage.
///
/// A tick runs in THIS order (fixed, because the outcome depends on it):
///   1. every order enqueued since the last step is applied in the order it was enqueued;
///      an order naming a unit that does not exist, is dead, or belongs to the other side is
///      dropped without effect;
///   2. units act in ascending unit-id order: a scavenger fetches (it is sent to the nearest
///      wreckage, or home with a load aboard, and picks up or hands over anything it is standing
///      next to -- C2; an order outranks it, always), then a mobile unit with a MoveTo/GuardAt/
///      Patrol/Fetch stance steers towards its goal, then every unit's position moves by its
///      velocity; map bounds clamp, and a unit may not enter an impassable tile;
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
    std::uint32_t tick_{};
    std::uint32_t next_id_{1};
    std::uint32_t route_plans_{};
};

} // namespace protekt::sim
