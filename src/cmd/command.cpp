#include "protekt/cmd/command.hpp"

#include <algorithm>

namespace protekt::cmd {

namespace {

/// A position the sim could stand on: inside [0, kMapTiles) tiles on both axes.
[[nodiscard]] bool on_map(sim::Fixed x, sim::Fixed y) noexcept {
    constexpr std::int32_t kLimit = sim::kMapTiles * sim::Fixed::kOne;
    return x.raw >= 0 && x.raw < kLimit && y.raw >= 0 && y.raw < kLimit;
}

/// Ascending, unique, and without "no unit": the one place the roster is normalised.
[[nodiscard]] std::vector<sim::UnitId> ascending(std::span<const sim::UnitId> roster) {
    std::vector<sim::UnitId> ids(roster.begin(), roster.end());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.erase(ids.begin(),
              std::find_if(ids.begin(), ids.end(), [](sim::UnitId id) { return id.valid(); }));
    return ids;
}

[[nodiscard]] std::vector<sim::Order> make(std::span<const sim::UnitId> roster,
                                           const sim::Order& proto) {
    const std::vector<sim::UnitId> ids = ascending(roster);
    std::vector<sim::Order> orders;
    orders.reserve(ids.size());
    for (const sim::UnitId id : ids) {
        sim::Order order = proto;
        order.unit = id;
        orders.push_back(order);
    }
    return orders;
}

[[nodiscard]] std::vector<sim::Order> at_point(std::span<const sim::UnitId> roster,
                                               sim::OrderKind kind, sim::Fixed x, sim::Fixed y) {
    if (!on_map(x, y)) {
        return {};
    }
    sim::Order proto;
    proto.kind = kind;
    proto.x = x;
    proto.y = y;
    return make(roster, proto);
}

} // namespace

std::vector<sim::Order> move_to(std::span<const sim::UnitId> roster, sim::Fixed x, sim::Fixed y) {
    return at_point(roster, sim::OrderKind::MoveTo, x, y);
}

std::vector<sim::Order> guard_at(std::span<const sim::UnitId> roster, sim::Fixed x, sim::Fixed y) {
    return at_point(roster, sim::OrderKind::GuardAt, x, y);
}

std::vector<sim::Order> patrol(std::span<const sim::UnitId> roster, sim::Fixed x, sim::Fixed y,
                               sim::Fixed x2, sim::Fixed y2) {
    if (!on_map(x, y) || !on_map(x2, y2)) {
        return {};
    }
    sim::Order proto;
    proto.kind = sim::OrderKind::Patrol;
    proto.x = x;
    proto.y = y;
    proto.x2 = x2;
    proto.y2 = y2;
    return make(roster, proto);
}

std::vector<sim::Order> attack(std::span<const sim::UnitId> roster, sim::UnitId target) {
    if (!target.valid()) {
        return {};
    }
    sim::Order proto;
    proto.kind = sim::OrderKind::Attack;
    proto.target = target;
    return make(roster, proto);
}

std::vector<sim::Order> focus_fire(sim::UnitId shooter, sim::UnitId target) {
    if (!shooter.valid() || !target.valid() || shooter == target) {
        return {};
    }
    sim::Order order{};
    order.kind = sim::OrderKind::FocusFire;
    order.unit = shooter;
    order.target = target;
    return {order};
}

std::vector<sim::Order> stop(std::span<const sim::UnitId> roster) {
    sim::Order proto;
    proto.kind = sim::OrderKind::Stop;
    return make(roster, proto);
}

} // namespace protekt::cmd
