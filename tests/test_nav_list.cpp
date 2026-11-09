// AX: `2 Go to Nav` opens a LIST of the nav points -- one row per mark, numbered by the mark's nav
// slot digit, its distance in the right column -- and the digit that names a row sends the
// selection there through the existing G-digit path and closes the menu.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/bindings.hpp"
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
    NavMarks marks;
    UnitId driven;
    UnitId hover;
};

[[nodiscard]] Rig make_rig() {
    const std::vector<sim::Spawn> spawns{{UnitKind::Tank, Side::Player, 40, 40},
                                         {UnitKind::HoverTank, Side::Player, 44, 41},
                                         {UnitKind::Tank, Side::Enemy, 38, 60}};
    Rig rig{sim::Sim::create(sim::Scenario{kSeed, spawns}), {}, {}, {}, {}, {}};
    rig.driven = rig.sim.units().at(0).id;
    rig.hover = rig.sim.units().at(1).id;
    rig.options.seed = kSeed;
    rig.options.extent = Extent{1280, 720};
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.viewer_unit = rig.driven;
    rig.options.driven_unit = rig.driven;
    rig.options.nav_marks = &rig.marks;
    rig.row.update(rig.sim, Side::Player, 0);
    rig.options.row = &rig.row;
    return rig;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

[[nodiscard]] std::int32_t digit_key(int n) { return n == 0 ? input::kKey0 : input::kKey1 + n - 1; }

[[nodiscard]] std::vector<sim::Order> press(Session& session, Rig& rig, std::int32_t scancode) {
    std::vector<sim::Order> orders;
    session.frame(key_frame(scancode), rig.sim, rig.options, orders);
    // The driven craft's own per-frame orders are the helm's, not the player's: only orders to
    // the OTHER units are kept.
    std::erase_if(orders, [&rig](const sim::Order& order) { return order.unit == rig.driven; });
    return orders;
}

/// The hover tank alone: F2 holds it.
void select_hover(Session& session, Rig& rig) {
    static_cast<void>(press(session, rig, input::kKeyF1 + 1));
    ASSERT_EQ(session.selection().size(), 1U);
    ASSERT_EQ(session.selection().units().front(), rig.hover);
}

void drop(Rig& rig, int count) {
    for (int i = 0; i < count; ++i) {
        static_cast<void>(rig.marks.place(50 + 2 * i, 50 + i));
    }
}

[[nodiscard]] std::vector<MenuRow> rows_now(const Rig& rig, const Session& session) {
    return app::menu_for(rig.sim, session, rig.options, session.attack_armed());
}

TEST(NavList, TwoArmsAListOfEveryMarkAndOrdersNothingYet) {
    Rig rig = make_rig();
    drop(rig, 3);
    Session session{rig.driven};
    select_hover(session, rig);
    const std::vector<sim::Order> orders = press(session, rig, input::kKey1 + 1);
    EXPECT_TRUE(orders.empty()) << "opening the list is not an order";
    EXPECT_TRUE(session.nav_list_armed());
    EXPECT_EQ(session.nav_list().size(), 3U);
    EXPECT_TRUE(session.message().empty());
    EXPECT_EQ(session.selection().size(), 1U) << "the selection waits for the choice";
}

TEST(NavList, RowsAreNumberedByTheNavSlotDigitIncludingZeroForTheTenth) {
    Rig rig = make_rig();
    drop(rig, 10);
    Session session{rig.driven};
    select_hover(session, rig);
    static_cast<void>(press(session, rig, input::kKey1 + 1));
    const std::vector<MenuRow> rows = rows_now(rig, session);
    ASSERT_EQ(rows.size(), 11U) << "TAB Close Menu, then one row per mark";
    EXPECT_EQ(rows.front().key, "TAB");
    const std::vector<NavMarks::Mark> marks = rig.marks.marks();
    for (std::size_t i = 0; i < marks.size(); ++i) {
        EXPECT_EQ(rows.at(i + 1).key,
                  std::string{static_cast<char>('0' + nav_slot_digit(marks.at(i).slot))})
            << "row " << i;
    }
    EXPECT_EQ(rows.back().key, "0") << "the tenth slot is pressed as 0, not 10";
}

TEST(NavList, EachRowReadsItsDistanceInWholeMetres) {
    Rig rig = make_rig();
    drop(rig, 3);
    Session session{rig.driven};
    select_hover(session, rig);
    static_cast<void>(press(session, rig, input::kKey1 + 1));
    const std::vector<MenuRow> rows = rows_now(rig, session);
    ASSERT_EQ(rows.size(), 4U);
    const std::vector<NavMarks::Mark> marks = rig.marks.marks();
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const int metres = nav_metres(rig.options.camera, marks.at(i).tile_x, marks.at(i).tile_y);
        EXPECT_EQ(rows.at(i + 1).right, metres_text(metres)) << "row " << i;
        EXPECT_FALSE(rows.at(i + 1).right.empty());
        EXPECT_FALSE(rows.at(i + 1).unit.valid()) << "a mark is a place: no health or ammo lines";
    }
}

TEST(NavList, ChoosingADigitSendsTheSelectionThereAndDismissesTheMenu) {
    Rig rig = make_rig();
    drop(rig, 3); // marks 1, 2, 3 at (50,50) (52,51) (54,52)
    Session session{rig.driven};
    select_hover(session, rig);
    static_cast<void>(press(session, rig, input::kKey1 + 1));
    const std::vector<sim::Order> orders = press(session, rig, digit_key(2));
    ASSERT_EQ(orders.size(), 1U) << session.message();
    EXPECT_EQ(orders.at(0).unit, rig.hover);
    const auto* move = std::get_if<sim::orders::MoveTo>(&orders.at(0).payload);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->x, sim::Fixed::from_int(52));
    EXPECT_EQ(move->y, sim::Fixed::from_int(51));
    EXPECT_FALSE(session.nav_list_armed()) << "the list is used up";
    EXPECT_TRUE(session.selection().empty()) << "the menu is dismissed: nothing stays selected";
    EXPECT_TRUE(rows_now(rig, session).empty()) << "and no menu is drawn";
}

TEST(NavList, WithNoMarksTwoRefusesAndArmsNothing) {
    Rig rig = make_rig();
    Session session{rig.driven};
    select_hover(session, rig);
    const std::vector<sim::Order> orders = press(session, rig, input::kKey1 + 1);
    EXPECT_TRUE(orders.empty());
    EXPECT_FALSE(session.nav_list_armed());
    EXPECT_EQ(session.message(), "no nav points are set.");
}

TEST(NavList, ADigitWithNoRowRefusesOrdersNothingAndClosesTheList) {
    Rig rig = make_rig();
    drop(rig, 3);
    Session session{rig.driven};
    select_hover(session, rig);
    static_cast<void>(press(session, rig, input::kKey1 + 1));
    const std::vector<sim::Order> orders = press(session, rig, digit_key(7));
    EXPECT_TRUE(orders.empty());
    EXPECT_FALSE(session.nav_list_armed());
    EXPECT_FALSE(session.message().empty());
}

TEST(NavList, EscapeClosesTheListAndKeepsTheSelection) {
    Rig rig = make_rig();
    drop(rig, 3);
    Session session{rig.driven};
    select_hover(session, rig);
    static_cast<void>(press(session, rig, input::kKey1 + 1));
    const std::vector<sim::Order> orders = press(session, rig, input::kKeyEscape);
    EXPECT_TRUE(orders.empty());
    EXPECT_FALSE(session.nav_list_armed());
    EXPECT_EQ(session.selection().size(), 1U);
}

TEST(NavList, ZeroChoosesTheTenthMarkBySlotDigit) {
    Rig rig = make_rig();
    drop(rig, 10); // the tenth is slot 9, drawn and pressed as 0; its tile is (68, 59)
    Session session{rig.driven};
    select_hover(session, rig);
    static_cast<void>(press(session, rig, input::kKey1 + 1));
    const std::vector<sim::Order> orders = press(session, rig, digit_key(0));
    ASSERT_EQ(orders.size(), 1U) << session.message();
    const auto* move = std::get_if<sim::orders::MoveTo>(&orders.at(0).payload);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->x, sim::Fixed::from_int(68));
    EXPECT_EQ(move->y, sim::Fixed::from_int(59));
}

TEST(NavList, TheHighlightIsNoLongerWhatTwoReads) {
    Rig rig = make_rig();
    drop(rig, 2);
    rig.marks.toggle(0); // one highlighted, one plain: both are listed
    Session session{rig.driven};
    select_hover(session, rig);
    static_cast<void>(press(session, rig, input::kKey1 + 1));
    EXPECT_EQ(session.nav_list().size(), 2U);
}

} // namespace
} // namespace protekt::game
