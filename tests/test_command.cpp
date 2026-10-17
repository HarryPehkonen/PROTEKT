// The player's verbs: intent in, orders out.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "protekt/cmd/command.hpp"
#include "protekt/sim/fixed.hpp"

namespace {

using protekt::sim::Fixed;
using protekt::sim::Order;
using protekt::sim::OrderKind;
using protekt::sim::UnitId;

[[nodiscard]] UnitId id(std::uint32_t value) { return UnitId{value}; }
[[nodiscard]] Fixed tiles(int value) { return Fixed::from_int(value); }

/// The roster deliberately arrives OUT OF ORDER: everything downstream must see ascending ids.
[[nodiscard]] std::vector<UnitId> shuffled_roster() { return {id(9), id(2), id(17), id(5)}; }

[[nodiscard]] std::vector<std::uint32_t> unit_values(const std::vector<Order>& orders) {
    std::vector<std::uint32_t> values;
    values.reserve(orders.size());
    for (const Order& order : orders) {
        values.push_back(order.unit.value);
    }
    return values;
}

TEST(Command, MoveToEmitsOneOrderPerUnitInAscendingIdOrder) {
    const std::vector<Order> orders
        = protekt::cmd::move_to(shuffled_roster(), tiles(30), tiles(40));
    ASSERT_EQ(orders.size(), 4U);
    const std::vector<std::uint32_t> expected{2, 5, 9, 17};
    EXPECT_EQ(unit_values(orders), expected);
    for (const Order& order : orders) {
        EXPECT_EQ(order.kind, OrderKind::MoveTo);
        EXPECT_EQ(order.x.raw, tiles(30).raw);
        EXPECT_EQ(order.y.raw, tiles(40).raw);
    }
}

TEST(Command, GuardAtCarriesThePostAndLeavesTheTargetEmpty) {
    const std::vector<Order> orders
        = protekt::cmd::guard_at(std::vector<UnitId>{id(3)}, tiles(11), tiles(12));
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders[0].kind, OrderKind::GuardAt);
    EXPECT_EQ(orders[0].unit.value, 3U);
    EXPECT_EQ(orders[0].x.raw, tiles(11).raw);
    EXPECT_EQ(orders[0].y.raw, tiles(12).raw);
    EXPECT_FALSE(orders[0].target.valid()) << "a guard post is not a target";
}

TEST(Command, PatrolCarriesBothEnds) {
    const std::vector<Order> orders = protekt::cmd::patrol(std::vector<UnitId>{id(1)}, tiles(4),
                                                           tiles(5), tiles(14), tiles(15));
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders[0].kind, OrderKind::Patrol);
    EXPECT_EQ(orders[0].x.raw, tiles(4).raw);
    EXPECT_EQ(orders[0].y.raw, tiles(5).raw);
    EXPECT_EQ(orders[0].x2.raw, tiles(14).raw);
    EXPECT_EQ(orders[0].y2.raw, tiles(15).raw);
}

TEST(Command, AttackCarriesTheTargetForEveryUnit) {
    const std::vector<Order> orders = protekt::cmd::attack(shuffled_roster(), id(42));
    ASSERT_EQ(orders.size(), 4U);
    for (const Order& order : orders) {
        EXPECT_EQ(order.kind, OrderKind::Attack);
        EXPECT_EQ(order.target.value, 42U);
    }
}

TEST(Command, AttackRefusesAnInvalidTarget) {
    const std::vector<Order> orders = protekt::cmd::attack(shuffled_roster(), id(0));
    EXPECT_TRUE(orders.empty())
        << "an attack on nothing is not a move: it must be refused, not repaired";
}

TEST(Command, StopEmitsStopForEveryUnit) {
    const std::vector<Order> orders = protekt::cmd::stop(shuffled_roster());
    ASSERT_EQ(orders.size(), 4U);
    for (const Order& order : orders) {
        EXPECT_EQ(order.kind, OrderKind::Stop);
    }
}

TEST(Command, AnEmptyRosterEmitsNothing) {
    const std::vector<UnitId> nobody;
    EXPECT_TRUE(protekt::cmd::move_to(nobody, tiles(1), tiles(2)).empty());
    EXPECT_TRUE(protekt::cmd::guard_at(nobody, tiles(1), tiles(2)).empty());
    EXPECT_TRUE(protekt::cmd::patrol(nobody, tiles(1), tiles(2), tiles(3), tiles(4)).empty());
    EXPECT_TRUE(protekt::cmd::attack(nobody, id(7)).empty());
    EXPECT_TRUE(protekt::cmd::stop(nobody).empty());
}

TEST(Command, TheSameSelectionAlwaysProducesTheSameOrders) {
    // The claim that makes a replay of human play exact: selection order is invisible downstream.
    const std::vector<UnitId> reversed{id(17), id(5), id(2), id(9)};
    const std::vector<Order> first = protekt::cmd::move_to(shuffled_roster(), tiles(8), tiles(8));
    const std::vector<Order> second = protekt::cmd::move_to(reversed, tiles(8), tiles(8));
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].unit.value, second[i].unit.value);
        EXPECT_EQ(first[i].x.raw, second[i].x.raw);
        EXPECT_EQ(first[i].y.raw, second[i].y.raw);
    }
}

} // namespace
