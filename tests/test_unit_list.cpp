// AV: `~` ON A FUNCTION KEY IS A LIST. Harri, 2026-11-08: "Function key -> '~ Select Unit' should
// give a LIST of units on the left side, in the same order as the health bars under the function
// key. Each unit should be numbered from 1 to 9, and the tenth would have the label 0 because
// that's what the user would hit to select it. The '~ Select Unit' should not be greyed out when
// there are more than one unit selected."
//
// Everything goes through the app's own door: a `Session` fed key frames and `menu_for`, the rows
// the frame draws. The order under test is `read_slot`'s -- the bars' -- and the rigs deliberately
// seat a key's units in a different order from their ids so a list that is merely id-sorted fails.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;
    std::vector<UnitId> others; ///< every non-driven player unit, in spawn order
};

/// The driven tank, then `count` more tanks, all seated under F1 by the opening row.
[[nodiscard]] Rig make_rig(std::size_t count) {
    std::vector<sim::Spawn> spawns{{UnitKind::Tank, Side::Player, 4, 8}};
    spawns.reserve(count + 1U);
    for (std::size_t i = 0; i < count; ++i) {
        spawns.push_back({UnitKind::Tank, Side::Player, 4 + static_cast<int>(i) * 3, 4});
    }
    Rig rig{sim::Sim::create(sim::Scenario{kSeed, spawns}), {}, {}, {}, {}};
    rig.driven = rig.sim.units().at(0).id;
    for (std::size_t i = 1; i <= count; ++i) {
        rig.others.push_back(rig.sim.units().at(i).id);
    }
    rig.row.seed_from_start(rig.sim, Side::Player, 0, rig.driven);
    rig.options.seed = kSeed;
    rig.options.extent = Extent{1280, 720};
    rig.options.driven_unit = rig.driven;
    rig.options.viewer_unit = rig.driven;
    rig.options.row = &rig.row;
    return rig;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

void press(Session& session, Rig& rig, std::int32_t scancode) {
    std::vector<sim::Order> orders;
    session.frame(key_frame(scancode), rig.sim, rig.options, orders);
}

[[nodiscard]] std::vector<MenuRow> rows_now(const Rig& rig, const Session& session) {
    return app::menu_for(rig.sim, session, rig.options, session.attack_armed());
}

/// The row that `key` draws, or nullptr when this list has no such row. The rows must be a NAMED
/// LOCAL: the pointer is into them.
[[nodiscard]] const MenuRow* row_keyed(const std::vector<MenuRow>& rows, std::string_view key) {
    for (const MenuRow& row : rows) {
        if (row.key == key) {
            return &row;
        }
    }
    return nullptr;
}

/// `row_keyed(rows_now(...), key)` would hand back a pointer into a vector that dies with the call,
/// and reading it is a heap-use-after-free -- which ASan caught here in
/// `ADigitSelectsTheUnitTheBarsPutThere` on 2026-11-09.  Deleted so that mistake is a COMPILE
/// error, exactly as `test_menu.cpp`'s `row_for` and `game/roster.hpp` already do it.
const MenuRow* row_keyed(std::vector<MenuRow>&& rows, std::string_view key) = delete;

[[nodiscard]] std::vector<UnitId> selected(const Session& session) {
    const auto units = session.selection().units();
    return {units.begin(), units.end()};
}

/// The units the bars under F1 draw, left to right.
[[nodiscard]] std::vector<UnitId> bars_order(const Rig& rig) {
    return read_slot(rig.sim, rig.row.slots.at(group_slot(1))).value_or(SlotReading{}).units;
}

/// The list's rows with the `TAB Close Menu` row under which they sit taken off.
[[nodiscard]] std::vector<MenuRow> list_of(const std::vector<MenuRow>& rows) {
    return rows.size() < 2 ? std::vector<MenuRow>{}
                           : std::vector<MenuRow>(rows.begin() + 1, rows.end());
}

} // namespace

TEST(UnitList, HasOneRowPerUnitInTheBarsOrder) {
    Rig rig = make_rig(3);
    // Seated against their ids: a list sorted by id is not the bars' list.
    rig.row.slots.at(group_slot(1)).units = {rig.others.at(2), rig.others.at(0), rig.others.at(1)};
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKeyGrave);

    const std::vector<MenuRow> list = list_of(rows_now(rig, session));
    ASSERT_EQ(list.size(), 3U);
    const std::vector<UnitId> bars = bars_order(rig);
    ASSERT_EQ(bars.size(), 3U);
    for (std::size_t i = 0; i < list.size(); ++i) {
        EXPECT_EQ(list.at(i).unit, bars.at(i)) << "row " << i;
        EXPECT_EQ(list.at(i).verb, "TANK");
    }
    EXPECT_EQ(bars, (std::vector<UnitId>{rig.others.at(2), rig.others.at(0), rig.others.at(1)}));
}

TEST(UnitList, IsNumberedOneToNineAndTheTenthIsZero) {
    Rig rig = make_rig(10);
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    ASSERT_EQ(selected(session).size(), 10U);
    press(session, rig, input::kKeyGrave);

    const std::vector<MenuRow> list = list_of(rows_now(rig, session));
    ASSERT_EQ(list.size(), 10U);
    const std::vector<std::string_view> want{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(list.at(i).key, want.at(i)) << "row " << i;
    }
    EXPECT_NE(list.back().key, "10");
    // The row model's own rows carry the digit too: the unit list the interaction hands over is
    // keyed 1-9 then 0, exactly as the attack and repair lists are.
    const std::vector<TargetRow> rows = unit_list_rows(rig.sim, rig.row.slots.at(group_slot(1)));
    ASSERT_EQ(rows.size(), 10U);
    EXPECT_EQ(rows.at(0).key, 1);
    EXPECT_EQ(rows.at(8).key, 9);
    EXPECT_EQ(rows.at(9).key, 0);
}

TEST(UnitList, ADigitSelectsTheUnitTheBarsPutThere) {
    Rig rig = make_rig(3);
    rig.row.slots.at(group_slot(1)).units = {rig.others.at(2), rig.others.at(0), rig.others.at(1)};
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKeyGrave);

    press(session, rig, input::kKey1 + 1); // `2`: the second bar

    EXPECT_EQ(selected(session), (std::vector<UnitId>{rig.others.at(0)}));
    const std::vector<MenuRow> after = rows_now(rig, session);
    const MenuRow* one = row_keyed(after, "1");
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one->verb, "Follow Me") << "the list is spent: the vehicle's own menu is back";
}

TEST(UnitList, TheTenthUnitIsSelectedByZero) {
    Rig rig = make_rig(10);
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKeyGrave);

    press(session, rig, input::kKey1 + 9); // the `0` key

    EXPECT_EQ(selected(session), (std::vector<UnitId>{bars_order(rig).at(9)}));
}

TEST(UnitList, ARowWithNothingBehindItSelectsNothing) {
    Rig rig = make_rig(3);
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKeyGrave);

    press(session, rig, input::kKey1 + 6); // `7`: only three rows

    EXPECT_EQ(selected(session).size(), 1U + 2U) << "the group is still in hand";
}

TEST(UnitList, EscapeClosesItWithoutSelecting) {
    Rig rig = make_rig(3);
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKeyGrave);
    ASSERT_EQ(list_of(rows_now(rig, session)).size(), 3U);

    press(session, rig, input::kKeyEscape);

    EXPECT_EQ(selected(session).size(), 3U);
    const std::vector<MenuRow> after = rows_now(rig, session);
    EXPECT_NE(row_keyed(after, "~"), nullptr);
}

TEST(UnitList, SelectUnitIsLiveWhenTheKeyHoldsSeveral) {
    Rig rig = make_rig(3);
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);

    const std::vector<MenuRow> rows = rows_now(rig, session);
    const MenuRow* tilde = row_keyed(rows, "~");
    ASSERT_NE(tilde, nullptr);
    EXPECT_EQ(tilde->verb, "Select Unit");
    EXPECT_TRUE(tilde->enabled);
    EXPECT_EQ(tilde->reason, MenuDim::None);

    // ... and still so once one of them is the whole selection: the KEY holds several.
    press(session, rig, input::kKeyGrave);
    press(session, rig, input::kKey1);
    ASSERT_EQ(selected(session).size(), 1U);
    const std::vector<MenuRow> after = rows_now(rig, session);
    ASSERT_NE(row_keyed(after, "~"), nullptr);
    EXPECT_TRUE(row_keyed(after, "~")->enabled);
}

TEST(UnitList, AKeyWithOneUnitIsUnchanged) {
    Rig rig = make_rig(1);
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    ASSERT_EQ(selected(session).size(), 1U);

    const std::vector<MenuRow> rows = rows_now(rig, session);
    ASSERT_NE(row_keyed(rows, "~"), nullptr);
    EXPECT_FALSE(row_keyed(rows, "~")->enabled) << "a lone unit has nothing to select within";
    EXPECT_EQ(row_keyed(rows, "~")->reason, MenuDim::DoesNotApply);

    press(session, rig, input::kKeyGrave);

    EXPECT_EQ(selected(session).size(), 1U);
    const std::vector<MenuRow> closed = rows_now(rig, session);
    EXPECT_NE(row_keyed(closed, "1"), nullptr) << "no list opened";
    EXPECT_TRUE(session.message().empty());
}

TEST(UnitList, TheMenuModelDrawsAListWithoutAnythingElse) {
    const std::vector<TargetRow> rows{{1, UnitKind::Tank, UnitId{7}, 0},
                                      {2, UnitKind::Scout, UnitId{8}, 0}};
    MenuInput in;
    in.subject = MenuSubject::Vehicle;
    in.unit_list = &rows;
    const std::vector<MenuRow> out = menu_rows(in);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(out.at(0).key, "TAB");
    EXPECT_EQ(out.at(1).key, "1");
    EXPECT_EQ(out.at(1).unit, UnitId{7});
    EXPECT_TRUE(out.at(1).right.empty()) << "a unit list reads no distance";
    EXPECT_EQ(out.at(2).key, "2");
}

} // namespace protekt::game
