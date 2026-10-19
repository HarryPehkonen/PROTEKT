// The player's command set.
#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <vector>

#include "protekt/cmd/selection.hpp"

namespace {

using protekt::cmd::Selection;
using protekt::sim::UnitId;

[[nodiscard]] UnitId id(std::uint32_t value) { return UnitId{value}; }

TEST(Selection, StartsEmpty) {
    const Selection selection;
    EXPECT_TRUE(selection.empty());
    EXPECT_EQ(selection.size(), 0U);
    EXPECT_TRUE(selection.units().empty());
    EXPECT_FALSE(selection.full());
}

TEST(Selection, AddingTwiceIsANoOp) {
    Selection selection;
    EXPECT_TRUE(selection.add(id(7)));
    EXPECT_FALSE(selection.add(id(7))) << "a set, not a list: the second add must be refused";
    EXPECT_EQ(selection.size(), 1U);
}

TEST(Selection, KeepsUnitsInAscendingOrderHoweverTheyArrive) {
    Selection selection;
    for (const std::uint32_t value : {9U, 3U, 41U, 1U, 17U}) {
        ASSERT_TRUE(selection.add(id(value)));
    }
    const std::vector<UnitId> expected{id(1), id(3), id(9), id(17), id(41)};
    EXPECT_TRUE(std::equal(selection.units().begin(), selection.units().end(), expected.begin(),
                           expected.end()))
        << "the order the player selected in must not survive into the orders";
}

TEST(Selection, ToggleAddsThenRemoves) {
    Selection selection;
    EXPECT_TRUE(selection.toggle(id(5))) << "toggling an absent unit selects it";
    EXPECT_TRUE(selection.contains(id(5)));
    EXPECT_FALSE(selection.toggle(id(5))) << "toggling a present unit deselects it";
    EXPECT_FALSE(selection.contains(id(5)));
    EXPECT_TRUE(selection.empty());
}

TEST(Selection, RemoveReportsWhetherItWasThere) {
    Selection selection;
    ASSERT_TRUE(selection.add(id(4)));
    EXPECT_TRUE(selection.remove(id(4)));
    EXPECT_FALSE(selection.remove(id(4))) << "removing what is not selected is not a success";
}

TEST(Selection, RefusesTheInvalidUnitId) {
    Selection selection;
    EXPECT_FALSE(selection.add(id(0))) << "UnitId{} is \"no unit\"; it must not be selectable";
    EXPECT_FALSE(selection.toggle(id(0)));
    EXPECT_TRUE(selection.empty());
}

TEST(Selection, RefusesMoreThanTheMaximumAndKeepsWhatItHad) {
    Selection selection;
    for (std::uint32_t value = 1; value <= static_cast<std::uint32_t>(Selection::kMax); ++value) {
        ASSERT_TRUE(selection.add(id(value))) << "adding unit " << value;
    }
    EXPECT_TRUE(selection.full());
    EXPECT_EQ(selection.size(), Selection::kMax);
    EXPECT_FALSE(selection.add(id(1000))) << "a drag-box over the map must not select the world";
    EXPECT_EQ(selection.size(), Selection::kMax) << "the refusal must not displace anything";
    EXPECT_FALSE(selection.contains(id(1000)));
    EXPECT_TRUE(selection.contains(id(1))) << "and must not drop the first selection either";
}

TEST(Selection, RetainDropsTheDeadAndKeepsTheOrder) {
    Selection selection;
    for (const std::uint32_t value : {8U, 2U, 5U}) {
        ASSERT_TRUE(selection.add(id(value)));
    }
    const std::vector<UnitId> living{id(5), id(8)};
    selection.retain(living);
    const std::vector<UnitId> expected{id(5), id(8)};
    EXPECT_TRUE(std::equal(selection.units().begin(), selection.units().end(), expected.begin(),
                           expected.end()))
        << "retaining must not disturb the ascending order";
    EXPECT_FALSE(selection.contains(id(2))) << "a dead unit must stop receiving orders";
}

TEST(Selection, RetainIfKeepsExactlyWhatThePredicateSays) {
    Selection selection;
    for (const std::uint32_t value : {8U, 2U, 5U}) {
        ASSERT_TRUE(selection.add(id(value)));
    }
    selection.retain_if([](UnitId unit) { return unit != id(2); });
    const std::vector<UnitId> expected{id(5), id(8)};
    EXPECT_TRUE(std::equal(selection.units().begin(), selection.units().end(), expected.begin(),
                           expected.end()))
        << "two kept, one dropped, and the ascending order must survive";
    EXPECT_EQ(selection.size(), 2U);
}

TEST(Selection, RetainingNothingEmptiesIt) {
    Selection selection;
    ASSERT_TRUE(selection.add(id(3)));
    selection.retain(std::span<const UnitId>{});
    EXPECT_TRUE(selection.empty());
}

TEST(Selection, ClearEmptiesTheSet) {
    Selection selection;
    ASSERT_TRUE(selection.add(id(1)));
    ASSERT_TRUE(selection.add(id(2)));
    selection.clear();
    EXPECT_TRUE(selection.empty());
    EXPECT_FALSE(selection.contains(id(1)));
}

} // namespace
