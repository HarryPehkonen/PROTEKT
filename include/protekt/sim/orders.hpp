#pragma once

#include <cstdint>
#include <vector>

#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// An order is plain data with no behaviour. The human commander and the AI commander produce
/// the SAME values through the same path -- that is what makes the AI replaceable, and what
/// makes a commander testable with no renderer and no window.
enum class OrderKind : std::uint8_t {
    MoveTo = 0,  ///< x, y
    GuardAt = 1, ///< x, y -- hold this post, engage what comes into range
    Patrol = 2,  ///< x, y .. x2, y2
    Attack = 3,  ///< target
    Stop = 4,    ///< no more moving, keep firing if armed
    /// Shoot THIS unit, and change nothing else. Deliberately does NOT set a stance or a
    /// destination: a unit the player is driving is steered by the player's keys every tick, so an
    /// order that also moved it would fight the keyboard. This is the trigger, as an order.
    FocusFire = 5, ///< target
};
inline constexpr int kOrderKindCount = 6;

struct Order {
    OrderKind kind{};
    UnitId unit{}; ///< must exist, be alive, and belong to the side issuing it
    Fixed x{};     ///< MoveTo / GuardAt / Patrol (near end)
    Fixed y{};
    Fixed x2{}; ///< Patrol (far end)
    Fixed y2{};
    UnitId target{}; ///< Attack
};

/// An order stamped with the tick it takes effect on. A replay is a seed plus these.
struct Command {
    Tick tick{};
    Order order{};
};

} // namespace protekt::sim
