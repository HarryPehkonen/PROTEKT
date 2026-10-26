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

/// Keep within kFollowRadiusTiles of `target`, wherever it goes. A standing order: it closes when
/// outside the radius, holds inside it, fires at what is in range on the way but never leaves the
/// radius to chase. It names the unit, not whoever drives it; if the target dies the order lapses.
struct Follow {
    UnitId target{};
};

/// Shoot THIS unit, and change nothing else. Deliberately does NOT set a stance or a
/// destination: a unit the player is driving is steered by the player's keys every tick, so an
/// order that also moved it would fight the keyboard. This is the trigger, as an order.
struct FocusFire {
    UnitId target{};
};

/// Fire one bolt along a direction, at whatever is in the way. The direction (map x, map y, up)
/// need not be normalised: a shot at the ground or the sky is still a shot. `dir_z` is the pitch:
/// zero is level, and it comes LAST so that a level shot is written as it always was. Like
/// FocusFire it sets no stance and no destination, and the weapon's cooldown applies to it.
///
/// `aimed_at` is INTENT, not a path: the bolt flies along `dir_x`/`dir_y` either way. It names who
/// the shot is for and so who it may hurt on its own side (friendly fire); invalid is a shot at
/// nothing.
struct FireTowards {
    Fixed dir_x{};
    Fixed dir_y{};
    UnitId aimed_at{};
    Fixed dir_z{};
};

/// Lob one MORTAR shell along a barrel direction, whatever the shooter is: the player's manual
/// mortar (M2). The same payload as FireTowards, a different flight: the shell is the mortar's own
/// (its bolt speed across the map, its gravity, arc cap, splash and travel, its damage and its
/// cooldown), leaves the muzzle climbing `dir_z` per unit of horizontal run, and goes where that
/// parabola goes. No aim solve, no lead, no shooter velocity. `aimed_at` is intent, as in
/// FireTowards. A zero horizontal run (straight up or down) climbs or falls at the arc cap.
struct FireLob {
    Fixed dir_x{};
    Fixed dir_y{};
    UnitId aimed_at{};
    Fixed dir_z{};
    /// Contact is the order as it always was; Bounce is the bouncer's (M3). Last, and defaulted,
    /// so a four-field order keeps its meaning.
    Round round{Round::Contact};
};

/// One tick of sideways drift, as an INPUT: the direction (map x, map y; need not be normalised,
/// zero is no strafe at all) a hover tank slides this tick. The sim scales it to a fixed fraction
/// of the unit's speed, so the commander says where and the sim says how fast. It sets no stance
/// and no destination and nothing is remembered: the next tick without a Strafe order has no
/// strafe, which is what keeps it an input like a throttle and not a state on the unit.
struct Strafe {
    Fixed dx{};
    Fixed dy{};
};

/// DEPLOY a recycler at a site: drive to it, stand there `kDeployTicks`, and become a
/// `DeployedRecycler` in place (see `Sim`'s tick order). The tile is the footprint's NORTH-WEST
/// corner as aimed (`footprint_at` clips it so the whole square is on the map). Refused for any
/// kind but a `Recycler`, which is also what refuses deploying twice, and for a site that is off
/// the map, on ground no unit may enter, or inside another building's footprint (`deploy_site_ok`).
struct Deploy {
    std::int32_t tile_x{};
    std::int32_t tile_y{};
};

} // namespace orders

using OrderPayload = std::variant<orders::MoveTo, orders::GuardAt, orders::Patrol, orders::Attack,
                                  orders::Stop, orders::FocusFire, orders::FireTowards,
                                  orders::Strafe, orders::Follow, orders::FireLob, orders::Deploy>;

/// How each payload alternative is named and reported. This is the payload's index, in the
/// payload's order, and the assertions below make that true rather than intended.
enum class OrderKind : std::uint8_t {
    MoveTo = 0,
    GuardAt = 1,
    Patrol = 2,
    Attack = 3,
    Stop = 4,
    FocusFire = 5,
    FireTowards = 6,
    Strafe = 7,
    Follow = 8,
    FireLob = 9,
    Deploy = 10,
};

inline constexpr std::size_t kOrderKindCount = static_cast<std::size_t>(OrderKind::Deploy) + 1;

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
static_assert(std::is_same_v<PayloadOf<OrderKind::FireTowards>, orders::FireTowards>);
static_assert(std::is_same_v<PayloadOf<OrderKind::Strafe>, orders::Strafe>);
static_assert(std::is_same_v<PayloadOf<OrderKind::Follow>, orders::Follow>);
static_assert(std::is_same_v<PayloadOf<OrderKind::FireLob>, orders::FireLob>);
static_assert(std::is_same_v<PayloadOf<OrderKind::Deploy>, orders::Deploy>);

/// The kind is the payload's own index. There is no second field that can disagree with it, which
/// is precisely what the flat record could not promise.
[[nodiscard]] constexpr OrderKind kind_of(const OrderPayload& payload) noexcept {
    return static_cast<OrderKind>(payload.index());
}

/// What a payload carries, in the canonical order: up to four positions and an optional target,
/// zero where the kind has none. This is the ONE definition of a payload's contents, and its two
/// callers need it for different reasons -- the replay writer has to know how many numbers a verb
/// writes, and the state hash has to feed the same fixed-shape stream it always has (kind, unit, x,
/// y, x2, y2, target) so that recorded hashes do not move.
struct OrderFields {
    std::array<Fixed, 4> positions{};
    std::size_t position_count = 0;
    bool has_target = false;
    UnitId target{};
};

/// Reads a payload's contents. NON-THROWING on purpose: it is called from `noexcept` functions, and
/// `std::visit` and `std::get` may throw `bad_variant_access`. That cannot happen here -- no
/// payload holds a type whose move can throw, so the variant can never be valueless -- but the
/// compiler cannot know that, and a `std::terminate` is not a failure mode worth admitting into the
/// tick loop to save an if-chain.
[[nodiscard]] inline OrderFields fields_of(const OrderPayload& payload) noexcept {
    OrderFields out{};
    if (const auto* p = std::get_if<orders::MoveTo>(&payload)) {
        out.positions[0] = p->x;
        out.positions[1] = p->y;
        out.position_count = 2;
    } else if (const auto* p = std::get_if<orders::GuardAt>(&payload)) {
        out.positions[0] = p->x;
        out.positions[1] = p->y;
        out.position_count = 2;
    } else if (const auto* p = std::get_if<orders::Patrol>(&payload)) {
        out.positions[0] = p->x;
        out.positions[1] = p->y;
        out.positions[2] = p->x2;
        out.positions[3] = p->y2;
        out.position_count = 4;
    } else if (const auto* p = std::get_if<orders::Attack>(&payload)) {
        out.has_target = true;
        out.target = p->target;
    } else if (const auto* p = std::get_if<orders::Follow>(&payload)) {
        out.has_target = true;
        out.target = p->target;
    } else if (const auto* p = std::get_if<orders::FocusFire>(&payload)) {
        out.has_target = true;
        out.target = p->target;
    } else if (const auto* p = std::get_if<orders::FireTowards>(&payload)) {
        out.positions[0] = p->dir_x;
        out.positions[1] = p->dir_y;
        out.positions[2] = p->dir_z;
        out.position_count = 3;
        out.has_target = true; // the intent: replays and the hash must keep it
        out.target = p->aimed_at;
    } else if (const auto* p = std::get_if<orders::FireLob>(&payload)) {
        out.positions[0] = p->dir_x;
        out.positions[1] = p->dir_y;
        out.positions[2] = p->dir_z;
        // The round rides in the fourth slot as its raw number: zero for Contact, so the hashed
        // stream (which always walks four slots) of every existing lob is unchanged.
        out.positions[3] = Fixed::from_raw(static_cast<std::int32_t>(p->round));
        out.position_count = 4;
        out.has_target = true;
        out.target = p->aimed_at;
    } else if (const auto* p = std::get_if<orders::Strafe>(&payload)) {
        out.positions[0] = p->dx;
        out.positions[1] = p->dy;
        out.position_count = 2;
    } else if (const auto* p = std::get_if<orders::Deploy>(&payload)) {
        // The tile as a position, a whole tile: a replay reads it back with `floor_int`.
        out.positions[0] = Fixed::from_int(p->tile_x);
        out.positions[1] = Fixed::from_int(p->tile_y);
        out.position_count = 2;
    }
    // Stop carries nothing, and neither does anything else that reaches here.
    return out;
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
