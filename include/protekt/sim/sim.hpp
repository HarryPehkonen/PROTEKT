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

/// Where a commander's orders are recorded, stamped with the tick they take effect on.
/// Sim implements this; a commander never touches a Sim member directly.
class OrderSink {
public:
    virtual ~OrderSink() = default;
    virtual void enqueue(const Order& order) = 0;
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
///   3. units fire in ascending unit-id order: an armed unit whose cooldown has expired fires
///      at the nearest hostile in range (distance is squared fixed-point; ties go to the
///      lowest id). Damage is exact -- there is no spread, so the sim needs no RNG;
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
