#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protekt/sim/orders.hpp"
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

/// The simulation. Deterministic by construction: no floating point, no unseeded randomness,
/// no time, no iteration over unordered storage.
///
/// A tick runs in THIS order (fixed, because the outcome depends on it):
///   1. every order enqueued since the last step is applied in the order it was enqueued;
///      an order naming a unit that does not exist, is dead, or belongs to the other side is
///      dropped without effect;
///   2. units act in ascending unit-id order: a mobile unit with a MoveTo/GuardAt/Patrol
///      stance steers towards its goal, then every unit's position moves by its velocity;
///      map bounds clamp, and a unit may not enter an impassable tile;
///   3. units fire in ascending unit-id order: a unit under an Attack order fires at its named
///      target while that target is alive and in range -- an explicit order outranks
///      auto-engagement -- and otherwise at the nearest hostile in range (distance is
///      squared fixed-point; ties go to the lowest id). Damage is exact: there is no
///      spread, so the sim needs no RNG;
///   4. the tick counter advances.
class Sim final : public OrderSink {
public:
    [[nodiscard]] static Sim create(const Scenario& scenario);

    void enqueue(const Order& order) override;
    void step();

    [[nodiscard]] std::uint32_t tick() const noexcept;
    [[nodiscard]] const std::vector<Unit>& units() const noexcept;
    [[nodiscard]] const Unit* find(UnitId id) const noexcept;

    /// Every command that was enqueued and accepted, in order. A replay is this list.
    [[nodiscard]] const std::vector<Command>& command_log() const noexcept;

    /// A hash of the ENTIRE simulation state that can be replayed (units and tick). The
    /// determinism oracle: a replay must reproduce it exactly, on any machine.
    [[nodiscard]] std::uint64_t state_hash() const noexcept;

private:
    Scenario scenario_{};
    std::vector<Unit> units_{};
    std::vector<Order> pending_{};
    std::vector<Command> log_{};
    std::uint32_t tick_{};
    std::uint32_t next_id_{1};
};

} // namespace protekt::sim
