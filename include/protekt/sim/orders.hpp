#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// An order is plain data with no behaviour. The human commander and the AI commander produce
/// the SAME values through the same path -- that is what makes the AI replaceable, and what
/// makes a commander testable with no renderer and no window.
///
/// Each kind's payload holds that kind's fields and NOTHING else. An order used to be a flat record
/// carrying `x, y, x2, y2, target` for every kind at once, with a comment naming which kind read
/// which -- so a "shoot this unit" order could carry a destination, and the replay format's idea of
/// how many numbers a verb writes was a hand-written count kept in step with the struct by memory.
/// It came apart exactly that way: the trigger was added as a kind, the verb table was not, and
/// recorded replays silently dropped every shot the player took.
///
/// Now the kind IS the variant's index, and everything that has to know what a kind contains visits
/// the payload: a kind that no visitor handles does not compile.

namespace orders {

/// Move to a point and hold there.
struct MoveTo {
    Fixed x{};
    Fixed y{};
};

/// Hold this post, engage what comes into range.
struct GuardAt {
    Fixed x{};
    Fixed y{};
};

/// Walk between two points, forever.
struct Patrol {
    Fixed x{};
    Fixed y{};
    Fixed x2{};
    Fixed y2{};
};

/// Close with `target` and engage it.
struct Attack {
    UnitId target{};
};

/// Stop where you are, keep firing if armed.
struct Stop {};

/// Shoot THIS unit, and change nothing else. Deliberately does NOT set a stance or a
/// destination: a unit the player is driving is steered by the player's keys every tick, so an
/// order that also moved it would fight the keyboard. This is the trigger, as an order.
struct FocusFire {
    UnitId target{};
};

} // namespace orders

using OrderPayload = std::variant<orders::MoveTo, orders::GuardAt, orders::Patrol, orders::Attack,
                                  orders::Stop, orders::FocusFire>;

/// How each payload alternative is named and reported. This is the payload's index, in the
/// payload's order, and the assertions below make that true rather than intended.
enum class OrderKind : std::uint8_t {
    MoveTo = 0,
    GuardAt = 1,
    Patrol = 2,
    Attack = 3,
    Stop = 4,
    FocusFire = 5,
};

inline constexpr std::size_t kOrderKindCount = static_cast<std::size_t>(OrderKind::FocusFire) + 1;

static_assert(std::variant_size_v<OrderPayload> == kOrderKindCount,
              "every alternative needs a kind, and every kind an alternative");

/// The two directions of one fact, checked by the compiler: the alternative at a kind's index IS
/// that kind's payload type. Reordering `OrderPayload` without reordering `OrderKind` fails to
/// compile.
template <OrderKind K>
using PayloadOf = std::variant_alternative_t<static_cast<std::size_t>(K), OrderPayload>;

static_assert(std::is_same_v<PayloadOf<OrderKind::MoveTo>, orders::MoveTo>);
static_assert(std::is_same_v<PayloadOf<OrderKind::GuardAt>, orders::GuardAt>);
static_assert(std::is_same_v<PayloadOf<OrderKind::Patrol>, orders::Patrol>);
static_assert(std::is_same_v<PayloadOf<OrderKind::Attack>, orders::Attack>);
static_assert(std::is_same_v<PayloadOf<OrderKind::Stop>, orders::Stop>);
static_assert(std::is_same_v<PayloadOf<OrderKind::FocusFire>, orders::FocusFire>);

/// The kind is the payload's own index. There is no second field that can disagree with it, which
/// is precisely what the flat record could not promise.
[[nodiscard]] constexpr OrderKind kind_of(const OrderPayload& payload) noexcept {
    return static_cast<OrderKind>(payload.index());
}

/// An order, addressed to one unit.
struct Order {
    UnitId unit{}; ///< must exist, be alive, and belong to the side issuing it
    OrderPayload payload{};

    [[nodiscard]] constexpr OrderKind kind() const noexcept { return kind_of(payload); }
};

/// An order stamped with the tick it takes effect on. A replay is a seed plus these.
struct Command {
    Tick tick{};
    Order order{};
};

} // namespace protekt::sim
