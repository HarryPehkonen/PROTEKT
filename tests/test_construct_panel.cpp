// ROUND 17, J1: the Constructor's CONSTRUCT panel -- the nine rows, drawable and readable.
//
// Harri: "This is what a Constructor can build. It's accessed by selecting the constructor and
// hitting '3 Build'. The items are 1 Power 30, 2 Factory 55, 3 Relay Bunker 50, 4 Armory, 5 Service
// Bay, 6 Gun Tower, 7 Training Facility, 8 Tech Centre, 9 Bomber Bay." And the rule it obeys:
// "Dimming rule: correct. Dim means inert. Refusal names the obstacle."
//
// This slice is the MENU only. Nothing is placed, charged or required: a press is INERT, and every
// test below that presses a row asserts that the bank and the order list did not move.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

struct Panel {
    std::string_view key;
    std::string_view label;
    int cost;
};

// Costs CONFIRMED off the owner's own captures (2026-11-11), all nine. Written as literals so a
// cost cannot drift silently with the table that draws it.
constexpr std::array<Panel, 9> kNine{{
    {"1", "POWER", 30},
    {"2", "FACTORY", 55},
    {"3", "RELAY BUNKER", 50},
    {"4", "ARMORY", 60},
    {"5", "SERVICE BAY", 50},
    {"6", "GUN TOWER", 50},
    {"7", "TRAINING CTR.", 70},
    {"8", "TECH CENTRE", 80},
    {"9", "BOMBER BAY", 100},
}};

struct Yard {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    sim::UnitId driven;
    sim::UnitId constructor;
};

[[nodiscard]] Yard make_yard(std::uint16_t ore, bool powered = false) {
    std::vector<sim::Spawn> units{{UnitKind::Tank, Side::Player, 60, 60},
                                  {UnitKind::Constructor, Side::Player, 61, 60}};
    if (powered) {
        // J4: a standing POWER gives the grid its spare units, so the power rows can be bright.
        units.push_back({UnitKind::Power, Side::Player, 80, 80});
    }
    sim::Scenario scenario{kSeed, units};
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    Yard y{};
    y.sim = sim::Sim::create(scenario);
    y.driven = y.sim.units().at(0).id;
    y.constructor = y.sim.units().at(1).id;
    y.options.seed = kSeed;
    y.options.extent = Extent{1280, 720};
    y.options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
    y.options.viewer_unit = y.driven;
    y.row.update(y.sim, Side::Player, 0);
    y.options.row = &y.row;
    return y;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

[[nodiscard]] std::int32_t digit_key(int digit) { return input::kKey1 + digit - 1; }

void select_constructor(const Yard& y, Session& session, std::vector<sim::Order>& orders) {
    for (int slot = 0; slot < 10; ++slot) {
        if (session.selection().size() == 1U
            && session.selection().units().front() == y.constructor) {
            break;
        }
        session.frame(key_frame(input::kKeyF1 + slot), y.sim, y.options, orders);
    }
    ASSERT_EQ(session.selection().size(), 1U);
    ASSERT_EQ(session.selection().units().front(), y.constructor);
}

[[nodiscard]] std::vector<MenuRow> rows_now(const Yard& y, const Session& session) {
    return app::menu_for(y.sim, session, y.options, session.attack_armed());
}

[[nodiscard]] const MenuRow* row_keyed(const std::vector<MenuRow>& rows, std::string_view key) {
    for (const MenuRow& row : rows) {
        if (row.key == key) {
            return &row;
        }
    }
    return nullptr;
}

/// Select the constructor and press its menu's `3`: the panel is then open.
void open_panel(const Yard& y, Session& session, std::vector<sim::Order>& orders) {
    select_constructor(y, session, orders);
    session.frame(key_frame(digit_key(3)), y.sim, y.options, orders);
}

[[nodiscard]] std::string label_of(const MenuRow& row) {
    // A keyed row's verb is "LABEL COST" (the cost rides in the verb, as on every build row).
    return row.verb.substr(0, row.verb.rfind(' '));
}

} // namespace

// ------------------------------------------------------------------------------ the entry

TEST(ConstructPanel, TheConstructorOwnsAMenuOfItsOwnWithThreeConstruct) {
    EXPECT_EQ(menu_subject_of_kind(UnitKind::Constructor), MenuSubject::Constructor);
    Yard y = make_yard(0);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    select_constructor(y, session, orders);
    const std::vector<MenuRow> rows = rows_now(y, session);
    const MenuRow* three = row_keyed(rows, "3");
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->verb, "Construct");
    EXPECT_TRUE(three->enabled);
    for (const MenuRow& row : rows) {
        EXPECT_NE(row.verb, "Attack") << "a constructor is unarmed";
    }
    EXPECT_FALSE(session.construct_armed()) << "selecting is not opening";
}

TEST(ConstructPanel, ThreeOpensThePanelAndItIsTwelveRowsInTheTreesGrammar) {
    Yard y = make_yard(0);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    ASSERT_TRUE(session.construct_armed());

    const std::vector<MenuRow> rows = rows_now(y, session);
    ASSERT_EQ(rows.size(), 12U) << "TAB, ~, the heading, then nine";
    EXPECT_EQ(rows.at(0).key, "TAB");
    EXPECT_EQ(rows.at(0).verb, "Close Menu");
    EXPECT_TRUE(rows.at(0).enabled);
    // `~ Rotate Buildings` is drawn and greyed: there is nothing to rotate yet.
    EXPECT_EQ(rows.at(1).key, "~");
    EXPECT_EQ(rows.at(1).verb, "Rotate Buildings");
    EXPECT_FALSE(rows.at(1).enabled);
    EXPECT_EQ(rows.at(1).reason, MenuDim::DoesNotApply);
    // The section heading names the list and is not a pressable row.
    EXPECT_TRUE(rows.at(2).heading);
    EXPECT_EQ(rows.at(2).key, "");
    EXPECT_EQ(rows.at(2).verb, "CONSTRUCT");
    for (std::size_t i = 3; i < rows.size(); ++i) {
        EXPECT_FALSE(rows.at(i).heading);
    }
}

// ------------------------------------------------------------------------------ the nine

TEST(ConstructPanel, TheTableHoldsExactlyNineRowsWithTheirKeysLabelsAndCosts) {
    const std::span<const BuildItem> items = construct_items();
    ASSERT_EQ(items.size(), kNine.size());
    for (std::size_t i = 0; i < kNine.size(); ++i) {
        const BuildItem& got = *(items.begin() + static_cast<std::ptrdiff_t>(i));
        EXPECT_EQ(got.key, kNine.at(i).key);
        EXPECT_EQ(got.label, kNine.at(i).label);
        EXPECT_EQ(got.cost, kNine.at(i).cost) << kNine.at(i).label;
        // Every human row names a kind since round 18, K3 (it was POWER alone, round 17, J3).
        EXPECT_TRUE(got.kind.has_value()) << kNine.at(i).label;
    }
}

TEST(ConstructPanel, TheDrawnRowsCarryTheNineWithTheCostOnTheRow) {
    Yard y = make_yard(400);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    const std::vector<MenuRow> rows = rows_now(y, session);
    ASSERT_EQ(rows.size(), 12U);
    for (std::size_t i = 0; i < kNine.size(); ++i) {
        const MenuRow& row = rows.at(3 + i);
        EXPECT_EQ(row.key, kNine.at(i).key);
        EXPECT_EQ(label_of(row), kNine.at(i).label);
        EXPECT_EQ(row.verb,
                  std::string{kNine.at(i).label} + " " + std::to_string(kNine.at(i).cost));
        // J4: the bank pays for every row, but only POWER has no requirement.
        EXPECT_EQ(row.enabled, i == 0U) << kNine.at(i).label;
    }
}

TEST(ConstructPanel, TwoSpellingsDifferFromTheReferenceOnPurpose) {
    // The OWNER ruled Canadian spelling: `TECH CENTRE` where the reference prints "Tech Center",
    // and `TRAINING CTR.` for the Training Centre -- his own abbreviation ("Training Centre can be
    // abbreviated to Training Ctr."), since the full name does not fit the panel (the reference
    // reads `TRAINING FAC.`). Do NOT "correct" either to match the reference.
    const std::span<const BuildItem> items = construct_items();
    ASSERT_EQ(items.size(), 9U);
    EXPECT_EQ(items[6].label, "TRAINING CTR.");
    EXPECT_NE(items[6].label, "TRAINING FAC.");
    EXPECT_EQ(items[7].label, "TECH CENTRE");
    EXPECT_NE(items[7].label, "TECH CENTER");
}

// ------------------------------------------------------------------------------ dim means inert

TEST(ConstructPanel, AnUnaffordableRowDimsAndItsPressIsRefusedNamingTheObstacle) {
    Yard y = make_yard(44, true);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    const std::vector<MenuRow> rows = rows_now(y, session);
    // 44 banked: Power (30) is paid for, Factory (55) is not.
    const MenuRow* power = row_keyed(rows, "1");
    const MenuRow* factory = row_keyed(rows, "2");
    ASSERT_NE(power, nullptr);
    ASSERT_NE(factory, nullptr);
    EXPECT_TRUE(power->enabled);
    EXPECT_FALSE(factory->enabled);
    EXPECT_EQ(factory->reason, MenuDim::Unaffordable);

    orders.clear();
    session.frame(key_frame(digit_key(2)), y.sim, y.options, orders);
    EXPECT_EQ(session.message(), "Add 11 scrap (have 44, costs 55)");
    EXPECT_EQ(session.message(), refusal_message(*factory)) << "the dim and the refusal are one";
    EXPECT_TRUE(session.construct_armed()) << "a refused press leaves the panel up";
    EXPECT_EQ(y.sim.ore(Side::Player), 44U) << "nothing is charged";
}

TEST(ConstructPanel, AnAffordableRowWhosePrerequisitesStandNowArmsItsBuildingAndChargesNothingYet) {
    // MOVED DELIBERATELY with round 18, K3: FACTORY was "a later slice" and refused with the honest
    // placeholder; it names a kind now, so the press ARMS it (SPACE places it, and only then does
    // the sim charge). The placeholder survives for the ALIEN rows alone (`test_alien_construct`).
    Yard y = make_yard(400, true);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    orders.clear();
    session.frame(key_frame(digit_key(2)), y.sim, y.options, orders); // FACTORY
    ASSERT_TRUE(session.placing().has_value());
    EXPECT_EQ(session.placing().value_or(Placing{}).kind, sim::UnitKind::Factory);
    EXPECT_EQ(y.sim.ore(Side::Player), 400U) << "nothing is charged by the press";
    for (const sim::Order& order : orders) {
        EXPECT_NE(order.unit, y.constructor) << "no order is issued for the constructor";
    }
    EXPECT_TRUE(session.construct_armed());
}

TEST(ConstructPanel, EveryRowRefusesForItsObstacleAndAPoorPowerRowNamesItsShortfall) {
    // Nothing stands but the Constructor: what a row is missing is named before its scrap (J4).
    const std::array<std::string_view, 9> missing{"",
                                                  "Needs power",
                                                  "Needs power",
                                                  "Needs a Factory",
                                                  "Needs a Factory",
                                                  "Needs power",
                                                  "Needs an Armory",
                                                  "Needs an Armory and a Service Bay",
                                                  "Needs a Training Ctr."};
    for (const std::uint16_t bank : {std::uint16_t{0}, std::uint16_t{65}, std::uint16_t{400}}) {
        for (std::size_t i = 0; i < kNine.size(); ++i) {
            const Panel& want = kNine.at(i);
            Yard y = make_yard(bank);
            Session session{y.driven};
            std::vector<sim::Order> orders;
            open_panel(y, session, orders);
            orders.clear();
            session.frame(key_frame(digit_key(want.key.front() - '0')), y.sim, y.options, orders);
            if (i != 0U) {
                EXPECT_EQ(session.message(), std::string{missing.at(i)}) << want.label << bank;
            } else if (bank >= want.cost) {
                EXPECT_TRUE(session.placing().has_value()) << "POWER arms (J3), it is not refused";
            } else {
                EXPECT_EQ(session.message(), "Add " + std::to_string(want.cost - bank)
                                                 + " scrap (have " + std::to_string(bank)
                                                 + ", costs " + std::to_string(want.cost) + ")")
                    << want.label << " with " << bank;
            }
        }
    }
}

TEST(ConstructPanel, TabClosesThePanelAndTheMenuIsTheConstructorsAgain) {
    Yard y = make_yard(400);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    ASSERT_TRUE(session.construct_armed());
    session.frame(key_frame(input::kKeyTab), y.sim, y.options, orders);
    EXPECT_FALSE(session.construct_armed());
    const std::vector<MenuRow> rows = rows_now(y, session);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().key, "SPACE") << "the Constructor's own menu, not the panel";
    const MenuRow* three = row_keyed(rows, "3");
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->verb, "Construct");
}

// ------------------------------------------------------------------------------ nothing else moved

TEST(ConstructPanel, NoOtherMenuMoved) {
    // The producers' lists keep their sizes and the constructor's subject builds nothing from
    // `build_items` (its panel is `construct_items`, a separate table).
    EXPECT_EQ(build_items(Faction::Human, MenuSubject::Deployment).size(), 7U);
    EXPECT_EQ(build_items(Faction::Alien, MenuSubject::Deployment).size(), 7U);
    EXPECT_EQ(build_items(Faction::Human, MenuSubject::Factory).size(), 8U);
    EXPECT_TRUE(build_items(Faction::Human, MenuSubject::Constructor).empty());
    // A tank is still `3 Attack`, the truck still `3 Repair`.
    MenuInput in;
    in.subject = menu_subject_of_kind(UnitKind::Tank);
    EXPECT_EQ(row_keyed(menu_rows(in), "3")->verb, "Attack");
    in.subject = menu_subject_of_kind(UnitKind::ServiceTruck);
    EXPECT_EQ(row_keyed(menu_rows(in), "3")->verb, "Repair");
    // The panel flag is the Constructor's alone: on a deployment it changes nothing.
    in.subject = MenuSubject::Deployment;
    in.ore = 400;
    in.construct_panel = true;
    EXPECT_EQ(menu_rows(in).size(), 2U + 1U + 7U);
}

} // namespace protekt::game
