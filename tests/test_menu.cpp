// THE NUMBERED COMMAND MENU, as a model (RB0).
//
// These tests pin the three things that make the menu a menu and not a list of strings: the keys
// are NOT CONTIGUOUS (the gap where 6, 7 and 8 would be, drawn open), a key's verb is
// CONTEXT-DEPENDENT (`3` is three different things), and the TOP ROW IS A READOUT of what is
// armed and what is under the cursor rather than a binding of its own. The dimming carries two
// meanings under one look, so both are pinned along with the reason that tells them apart.
//
// The table below is read off the user's own BattleZone II captures (2026-10-24), which is why
// these strings are quotations and not choices: where the capture could not resolve a verb, the
// test says so and the model refuses to invent one.
#include "protekt/game/menu.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

constexpr std::array kEverySubject{MenuSubject::None, MenuSubject::Recycler, MenuSubject::Harvester,
                                   MenuSubject::Deployment, MenuSubject::Turret};
constexpr std::array kEveryTarget{MenuTarget::None, MenuTarget::Ground, MenuTarget::Selectable,
                                  MenuTarget::Hostile};

[[nodiscard]] MenuInput query(MenuSubject subject) {
    MenuInput in;
    in.subject = subject;
    return in;
}

/// A deployed recycler's menu with `ore` scrap banked.
[[nodiscard]] MenuInput deployed(int ore) {
    MenuInput in = query(MenuSubject::Deployment);
    in.ore = ore;
    return in;
}

/// The row that `key` draws, or nullptr when this menu has no such row. The rows must be a NAMED
/// LOCAL: the pointer is into them.
[[nodiscard]] const MenuRow* row_for(const std::vector<MenuRow>& rows, std::string_view key) {
    for (const MenuRow& row : rows) {
        if (row.key == key) {
            return &row;
        }
    }
    return nullptr;
}

/// `row_for(menu_rows(...), key)` would hand back a pointer into a vector that dies with the call.
/// Deleted so that mistake is a compile error here too, exactly as `game/roster.hpp` does it.
const MenuRow* row_for(std::vector<MenuRow>&& rows, std::string_view key) = delete;

[[nodiscard]] std::vector<std::string> keys_of(const std::vector<MenuRow>& rows) {
    std::vector<std::string> keys;
    keys.reserve(rows.size());
    for (const MenuRow& row : rows) {
        keys.push_back(std::string{row.key});
    }
    return keys;
}

TEST(Menu, NothingSelectedDrawsNoMenu) {
    // No subject, no rows -- which is what keeps a frame with nothing selected byte-identical: a
    // layer that draws these rows draws NOTHING in the state most frames are in.
    for (const MenuSubject subject : {MenuSubject::None}) {
        for (const MenuTarget target : kEveryTarget) {
            for (const sim::Side side : {sim::Side::Player, sim::Side::Enemy}) {
                MenuInput in = query(subject);
                in.target = target;
                in.side = side;
                in.ore = 500;
                in.armed = ArmedCommand{MenuCommand::Deploy, {}};
                EXPECT_TRUE(menu_rows(in).empty())
                    << "a menu was drawn with nothing selected (target " << static_cast<int>(target)
                    << ")";
            }
        }
    }
}

TEST(Menu, TheTopRowIsLiveStateNotABinding) {
    // Case 1: nothing armed, something selectable under the cursor -> SPACE would SELECT it.
    MenuInput idle = query(MenuSubject::Recycler);
    idle.target = MenuTarget::Selectable;
    const std::vector<MenuRow> selectable = menu_rows(idle);
    ASSERT_FALSE(selectable.empty());
    EXPECT_EQ(selectable.front().key, "SPACE");
    EXPECT_EQ(selectable.front().verb, "Select");
    EXPECT_TRUE(selectable.front().enabled);
    EXPECT_EQ(selectable.front().reason, MenuDim::None);

    // Case 2: nothing armed and nothing SELECTABLE under the cursor -> no verb. Bare ground and a
    // hostile both name none: SPACE selects what is yours, and a mark on an enemy is what the
    // TRIGGER is for (`game/interaction.hpp`), so the readout must not promise a selection there.
    for (const MenuTarget empty : {MenuTarget::None, MenuTarget::Ground, MenuTarget::Hostile}) {
        MenuInput in = query(MenuSubject::Harvester);
        in.target = empty;
        const std::vector<MenuRow> rows = menu_rows(in);
        ASSERT_FALSE(rows.empty());
        EXPECT_EQ(rows.front().key, "SPACE") << "the state row is always the first row";
        EXPECT_TRUE(rows.front().verb.empty()) << "the top row named a verb with nothing armed and "
                                                  "nothing selectable under the cursor";
        EXPECT_TRUE(rows.front().enabled) << "the state row is a readout: it is never dimmed";
    }

    // Case 3: a deploy armed -> SPACE DEPLOY, whoever's menu it is and whatever is under the
    // cursor.
    MenuInput armed = query(MenuSubject::Recycler);
    armed.armed = ArmedCommand{MenuCommand::Deploy, {}};
    for (const MenuTarget target : kEveryTarget) {
        armed.target = target;
        EXPECT_EQ(menu_rows(armed).front().verb, "Deploy") << "target " << static_cast<int>(target);
    }

    // Case 3b: the armed deploy reads its distance at the right -- `SPACE DEPLOY 68m` -- and only
    // while it has one to read: off the ground there is none, and nothing armed reads none.
    MenuInput far = query(MenuSubject::Recycler);
    far.armed = ArmedCommand{MenuCommand::Deploy, {}};
    far.metres = 68;
    EXPECT_EQ(menu_rows(far).front().right, "68m");
    far.metres.reset();
    EXPECT_TRUE(menu_rows(far).front().right.empty()) << "the reticle is off the ground";
    MenuInput unarmed = query(MenuSubject::Recycler);
    unarmed.metres = 68;
    EXPECT_TRUE(menu_rows(unarmed).front().right.empty()) << "a distance belongs to an armed verb";

    // Case 4: a build row armed -> SPACE names the item it will place.
    MenuInput building = deployed(500);
    building.armed = ArmedCommand{MenuCommand::Build, "Scout"};
    EXPECT_EQ(menu_rows(building).front().verb, "Scout");

    // The state row is the first row of every menu that exists, and it is never `enabled == false`.
    for (const MenuSubject subject : kEverySubject) {
        if (subject == MenuSubject::None) {
            continue;
        }
        const std::vector<MenuRow> rows = menu_rows(query(subject));
        ASSERT_FALSE(rows.empty())
            << "subject " << static_cast<int>(subject) << " drew no state row";
        EXPECT_EQ(rows.front().key, "SPACE");
        EXPECT_TRUE(rows.front().enabled);
    }
}

TEST(Menu, ArmingDoesNotRewriteTheBindingThatArmedIt) {
    // `3` ARMS the deploy and SPACE confirms it -- so arming must ADD a readout at the top and
    // leave the binding that armed it exactly where it was, or the same action would appear twice
    // and SPACE would read as a second `3`.
    const std::vector<MenuRow> before = menu_rows([] {
        MenuInput in = query(MenuSubject::Recycler);
        in.target = MenuTarget::Selectable;
        return in;
    }());
    MenuInput armed_in = query(MenuSubject::Recycler);
    armed_in.target = MenuTarget::Selectable;
    armed_in.armed = ArmedCommand{MenuCommand::Deploy, {}};
    const std::vector<MenuRow> after = menu_rows(armed_in);

    ASSERT_EQ(before.size(), after.size());
    EXPECT_EQ(before.front().verb, "Select");
    EXPECT_EQ(after.front().verb, "Deploy") << "SPACE did not pick up the armed command";
    EXPECT_EQ(row_for(before, "3")->verb, "Deploy");
    EXPECT_EQ(row_for(after, "3")->verb, "Deploy")
        << "arming changed the row that armed it: it is a binding, not a readout";
    EXPECT_EQ(after.at(5).key, "3") << "arming moved rows around";
}

TEST(Menu, TheSecondRowClosesTheMenu) {
    for (const MenuSubject subject :
         {MenuSubject::Recycler, MenuSubject::Harvester, MenuSubject::Deployment}) {
        const std::vector<MenuRow> rows = menu_rows(query(subject));
        ASSERT_GT(rows.size(), 2U) << "subject " << static_cast<int>(subject);
        EXPECT_EQ(rows.at(1).key, "TAB");
        EXPECT_EQ(rows.at(1).verb, "Close Menu");
        EXPECT_TRUE(rows.at(1).enabled);
    }
    // The lone turret is the exception, and it is not drawn a TAB row it would have nothing to
    // close: its menu is the state row alone (pinned below).
    const std::vector<MenuRow> turret = menu_rows(query(MenuSubject::Turret));
    EXPECT_EQ(turret.size(), 1U);
}

TEST(Menu, TheKeysAreNotContiguousAndTheGapIsDrawn) {
    const std::vector<MenuRow> rows = menu_rows(query(MenuSubject::Harvester));
    ASSERT_EQ(rows.size(), 10U);
    const std::vector<std::string> want{"SPACE", "TAB", "~", "1", "2", "3", "4", "5", "9", "0"};
    EXPECT_EQ(keys_of(rows), want);
    // The three keys the menu skips are NOT drawn. Numbering rows by their position would draw
    // them.
    for (const std::string_view absent : {"6", "7", "8"}) {
        EXPECT_EQ(row_for(rows, absent), nullptr)
            << "key " << absent << " draws a row: the gap is not being left open";
    }
    // With teeth: those three keys ARE drawn one selection away, on the build list, so the absence
    // above is this menu's shape and not a ban on the keys.
    const std::vector<MenuRow> build = menu_rows(deployed(999));
    EXPECT_NE(row_for(build, "6"), nullptr);
    EXPECT_NE(row_for(build, "7"), nullptr);
    EXPECT_EQ(row_for(build, "8"), nullptr)
        << "the build list has seven rows, 1-7: key 8 is not one of them";
}

TEST(Menu, TheRowOrderIsNotKeyOrder) {
    // Drawn top to bottom on the harvester's menu. Sorted, that would read 0, 1, 2, 3, 4, 5, 9,
    // SPACE, TAB, ~ -- so the drawn order is no key order at all: 9 and 0 are drawn LAST, below the
    // gap 1-5 they do not sort beside.
    const std::vector<MenuRow> rows = menu_rows(query(MenuSubject::Harvester));
    ASSERT_EQ(rows.size(), 10U);
    const bool sorted = std::is_sorted(
        rows.begin(), rows.end(), [](const MenuRow& a, const MenuRow& b) { return a.key < b.key; });
    EXPECT_FALSE(sorted)
        << "the rows came back sorted: something is ordering them by key, not drawing "
           "them in the menu's own order";

    // And a key's POSITION is not its number: on the recycler's menu `3 DEPLOY` is the SIXTH row,
    // because the state row, TAB and `~` sit above the digits.
    const std::vector<MenuRow> recycler = menu_rows(query(MenuSubject::Recycler));
    ASSERT_EQ(recycler.size(), 6U);
    EXPECT_EQ(recycler.at(5).key, "3");
    EXPECT_EQ(recycler.at(5).verb, "Deploy");
    EXPECT_EQ(recycler.at(3).key, "1")
        << "row 3 is key 1: an implementation that numbered rows by position would put `3` here";
}

TEST(Menu, TheThreeKeyMeansWhateverTheSelectionMakesIt) {
    // One key, three verbs -- the reason rows are {key, verb} pairs.
    const std::vector<MenuRow> recycler = menu_rows(query(MenuSubject::Recycler));
    const std::vector<MenuRow> harvester = menu_rows(query(MenuSubject::Harvester));
    const std::vector<MenuRow> deployment = menu_rows(deployed(999));
    ASSERT_NE(row_for(recycler, "3"), nullptr);
    ASSERT_NE(row_for(harvester, "3"), nullptr);
    ASSERT_NE(row_for(deployment, "3"), nullptr);
    EXPECT_EQ(row_for(recycler, "3")->verb, "Deploy");
    EXPECT_EQ(row_for(harvester, "3")->verb, "Scavenge");
    EXPECT_EQ(row_for(deployment, "3")->verb, "Tank 55");

    // The verbs the capture verified, in the order it draws them -- the rest of both menus.
    ASSERT_NE(row_for(harvester, "9"), nullptr);
    EXPECT_EQ(row_for(harvester, "9")->verb, "Bailout");
    EXPECT_EQ(row_for(harvester, "0")->verb, "Recycle");
    EXPECT_EQ(row_for(harvester, "4")->verb, "Hold");
    EXPECT_EQ(row_for(recycler, "1")->verb, "Follow Me");
    EXPECT_EQ(row_for(harvester, "2")->verb, "Go to Nav");
    EXPECT_EQ(row_for(recycler, "~")->verb, "Rotate Buildings");
    EXPECT_EQ(row_for(harvester, "~")->verb, "Select Unit");
    EXPECT_EQ(row_for(harvester, "5")->verb, "Pick Me Up");
}

TEST(Menu, ADimmedRowCarriesTheReasonItIsDimmed) {
    // An ACTION row that does not apply to this selection: dim ink, and the reason says so.
    const std::vector<MenuRow> harvester = menu_rows(query(MenuSubject::Harvester));
    const MenuRow* select_unit = row_for(harvester, "~");
    ASSERT_NE(select_unit, nullptr);
    EXPECT_FALSE(select_unit->enabled);
    EXPECT_EQ(select_unit->reason, MenuDim::DoesNotApply);
    const MenuRow* pick_up = row_for(harvester, "5");
    ASSERT_NE(pick_up, nullptr);
    EXPECT_FALSE(pick_up->enabled);
    EXPECT_EQ(pick_up->reason, MenuDim::DoesNotApply);
    // ... and not because of money: this menu has no costs at all.
    const MenuRow* follow = row_for(harvester, "1");
    ASSERT_NE(follow, nullptr);
    EXPECT_TRUE(follow->enabled);
    EXPECT_EQ(follow->reason, MenuDim::None);

    // A BUILD row, dim for the other reason: the item is buildable, there is just not enough scrap
    // banked. Same ink as above, different cause, and the model says which.
    const std::vector<MenuRow> broke = menu_rows(deployed(0));
    const MenuRow* scout = row_for(broke, "2");
    ASSERT_NE(scout, nullptr);
    EXPECT_FALSE(scout->enabled);
    EXPECT_EQ(scout->reason, MenuDim::Unaffordable);
    EXPECT_EQ(scout->verb, "Scout 45");
}

TEST(Menu, NoRowIsEverDimmedForTheWrongReason) {
    // The invariants, swept over every input the model accepts rather than spot-checked. A row's
    // `enabled` and its `reason` can never disagree; a key is drawn at most once per menu; an
    // action row is never dimmed for money and a build row never for inapplicability; and only the
    // deployment grows build rows.
    for (const MenuSubject subject : kEverySubject) {
        for (const sim::Side side : {sim::Side::Player, sim::Side::Enemy}) {
            for (const MenuTarget target : kEveryTarget) {
                for (const MenuCommand command :
                     {MenuCommand::None, MenuCommand::Deploy, MenuCommand::Build}) {
                    for (const int ore : {0, 44, 999}) {
                        MenuInput in = query(subject);
                        in.side = side;
                        in.target = target;
                        in.armed = ArmedCommand{command, "Scout"};
                        in.ore = ore;
                        const std::vector<MenuRow> rows = menu_rows(in);
                        const std::string where
                            = "subject " + std::to_string(static_cast<int>(subject)) + ", side "
                              + std::to_string(static_cast<int>(side)) + ", target "
                              + std::to_string(static_cast<int>(target)) + ", ore "
                              + std::to_string(ore);
                        // Nothing selected is no menu at all -- pinned in full by its own test, and
                        // asserted here so the sweep cannot quietly grow a menu for it.
                        if (subject == MenuSubject::None) {
                            EXPECT_TRUE(rows.empty())
                                << where << ": a selection-less frame grew a menu";
                            continue;
                        }
                        ASSERT_FALSE(rows.empty()) << where;
                        // The state row reads the ARMED COMMAND and nothing else: a build item must
                        // not leak into a DEPLOY row, and DEPLOY must not leak into a build row.
                        switch (command) {
                        case MenuCommand::Deploy:
                            EXPECT_EQ(rows.front().verb, "Deploy") << where;
                            break;
                        case MenuCommand::Build:
                            EXPECT_EQ(rows.front().verb, "Scout") << where;
                            break;
                        case MenuCommand::None:
                            EXPECT_TRUE(rows.front().verb.empty() || rows.front().verb == "Select")
                                << where;
                            break;
                        }
                        for (std::size_t i = 0; i < rows.size(); ++i) {
                            const MenuRow& row = rows.at(i);
                            EXPECT_EQ(row.enabled, row.reason == MenuDim::None)
                                << where << ": row " << i << " has enabled/reason disagreeing";
                            EXPECT_FALSE(row.key.empty()) << where << ": a row with no key";
                            for (std::size_t j = 0; j < i; ++j) {
                                EXPECT_NE(rows.at(j).key, row.key)
                                    << where << ": key " << row.key << " is drawn twice";
                            }
                            if (subject != MenuSubject::Deployment) {
                                EXPECT_NE(row.reason, MenuDim::Unaffordable)
                                    << where
                                    << ": an action row was dimmed for affordability at row " << i;
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(Menu, ABuildRowDrawsItsCostAndDimsWhenItIsUnaffordable) {
    // The deployment: the state row, TAB, `~ DROP OFF`, then the seven build rows. The cost is
    // drawn ON the row, so it rides in the verb.
    const std::vector<MenuRow> rich = menu_rows(deployed(999));
    ASSERT_EQ(rich.size(), 10U);
    // The three fixed rows, then the build list's own keys -- 1-7, CONTIGUOUS here, in contrast to
    // the harvester's gap below them: the two menus share a key space and not a shape.
    const std::vector<std::string> want{"SPACE", "TAB", "~", "1", "2", "3", "4", "5", "6", "7"};
    EXPECT_EQ(keys_of(rich), want);
    for (const BuildItem& item : build_items(sim::Side::Player, MenuSubject::Deployment)) {
        const MenuRow* row = row_for(rich, item.key);
        ASSERT_NE(row, nullptr) << "build key " << item.key << " is not drawn";
        EXPECT_EQ(row->verb, std::string{item.label} + " " + std::to_string(item.cost))
            << "build row " << item.key << " does not draw its cost";
        EXPECT_TRUE(row->enabled) << "with 999 banked, " << item.label << " is affordable";
    }
    // And the captured names and costs themselves, as the quotation they are.
    EXPECT_EQ(row_for(rich, "1")->verb, "Scavenger 20");
    EXPECT_EQ(row_for(rich, "3")->verb, "Tank 55");
    EXPECT_EQ(row_for(rich, "4")->verb, "Hauler 30");
    EXPECT_EQ(row_for(rich, "5")->verb, "Builder 40");
    EXPECT_EQ(row_for(rich, "6")->verb, "Healer 50");
    EXPECT_EQ(row_for(rich, "7")->verb, "Service Pod 2");

    // 44 scrap buys the 20-scrap harvester and the 2-scrap service pod and nothing else here: the
    // user's reading of the dim rows is exactly this comparison.
    const std::vector<MenuRow> poor = menu_rows(deployed(44));
    ASSERT_EQ(poor.size(), 10U);
    EXPECT_TRUE(row_for(poor, "1")->enabled);
    EXPECT_FALSE(row_for(poor, "2")->enabled) << "Scout 45 was affordable on 44 scrap";
    EXPECT_EQ(row_for(poor, "2")->reason, MenuDim::Unaffordable);
    EXPECT_FALSE(row_for(poor, "6")->enabled) << "Healer 50 was affordable on 44 scrap";
    EXPECT_TRUE(row_for(poor, "7")->enabled);

    // Exactly the price is affordable: 45 buys the 45-scrap scout.
    const std::vector<MenuRow> exact = menu_rows(deployed(45));
    EXPECT_TRUE(row_for(exact, "2")->enabled);
}

TEST(Menu, ABuildRowThatNamesAKindDrawsTheScrapTheSimWillCharge) {
    // The row and the queue must be one number, not two that agree today: a row that says 45 while
    // the sim charges 50 would leave the player with a queue that pauses on a price he was never
    // shown. A row with no kind (the Hauler and the rest) has no sim price to disagree with.
    int with_kind = 0;
    for (const BuildItem& item : build_items(sim::Side::Player, MenuSubject::Deployment)) {
        if (item.kind.has_value()) {
            ++with_kind;
            EXPECT_EQ(item.cost, sim::spec_of(*item.kind).cost) << item.label;
            EXPECT_GT(sim::spec_of(*item.kind).build_ticks, 0) << item.label;
        }
    }
    EXPECT_EQ(with_kind, 3) << "the harvester, the scout and the guardian";
    const std::span<const BuildItem> items
        = build_items(sim::Side::Player, MenuSubject::Deployment);
    EXPECT_EQ(items[0].kind, sim::UnitKind::Scavenger);
    EXPECT_EQ(items[1].kind, sim::UnitKind::Scout);
    EXPECT_EQ(items[2].kind, sim::UnitKind::Tank);
    EXPECT_FALSE(items[3].kind.has_value()) << "no UnitKind is the hauler yet";
}

TEST(Menu, TheEnemyBuildListIsNotInvented) {
    // Only the player's list was captured. The enemy's is empty until RB5 and the R3 roster fill it
    // (a per-side item name, a per-kind cost), so its deployment draws the rows it does know and no
    // build rows at all -- rather than a copy of the player's names.
    EXPECT_EQ(build_items(sim::Side::Player, MenuSubject::Deployment).size(), 7U);
    EXPECT_TRUE(build_items(sim::Side::Enemy, MenuSubject::Deployment).empty());

    MenuInput in = deployed(999);
    in.side = sim::Side::Enemy;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_EQ(rows.size(), 3U) << "the enemy deployment grew rows nobody has seen";
    EXPECT_EQ(rows.at(0).key, "SPACE");
    EXPECT_EQ(rows.at(1).verb, "Close Menu");
    EXPECT_EQ(rows.at(2).verb, "Drop Off");
    for (const std::string_view absent : {"1", "2", "3", "4", "5", "6", "7"}) {
        EXPECT_EQ(row_for(rows, absent), nullptr) << "key " << absent << " invented a build row";
    }
}

TEST(Menu, TheRecyclersListIsTheSevenItsKeysAndCostsHaveAlwaysBeen) {
    // The recycler's list is pinned whole -- key, name, cost, in order -- so a change to the
    // FACTORY's list can never reach it.
    struct Row {
        std::string_view key;
        std::string_view label;
        int cost;
    };
    const std::array<Row, 7> want{
        {{"1", "Scavenger", 20},
         {"2", "Scout", 45},
         {"3", "Tank", 55}, // the sim charges one price per kind: Harri's 55
         {"4", "Hauler", 30},
         {"5", "Builder", 40},
         {"6", "Healer", 50},
         {"7", "Service Pod", 2}}};
    const std::span<const BuildItem> items
        = build_items(sim::Side::Player, MenuSubject::Deployment);
    ASSERT_EQ(items.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        // `std::span::at` is C++26 and this is C++20, so bind through the iterator: the tidy check
        // (cppcoreguidelines-pro-bounds-constant-array-index) wants a constant index or no
        // subscript.
        const BuildItem& got = *(items.begin() + static_cast<std::ptrdiff_t>(i));
        const Row& expected = want.at(i);
        EXPECT_EQ(got.key, expected.key);
        EXPECT_EQ(got.label, expected.label);
        EXPECT_EQ(got.cost, expected.cost);
    }
}

TEST(Menu, TheFactorysListIsEightRowsInKeyOrderWithTheirOwnLabelsAndCosts) {
    struct Row {
        std::string_view key;
        std::string_view label;
        int cost;
        sim::UnitKind kind;
    };
    // Harri's factory menu (2026-10-30), pinned whole. The costs are PROVISIONAL until he has
    // played it, so a later edit has to be deliberate and name itself here.
    const std::array<Row, 8> want{{{"1", "Mortar Bike", 45, sim::UnitKind::MortarBike},
                                   {"2", "MSL Scout", 45, sim::UnitKind::Scout},
                                   {"3", "Tank", 55, sim::UnitKind::Tank},
                                   {"4", "Rocket Tank", 65, sim::UnitKind::RocketTank},
                                   {"5", "Assault Tank", 70, sim::UnitKind::AssaultTank},
                                   {"6", "Walker", 100, sim::UnitKind::Walker},
                                   {"7", "APC", 50, sim::UnitKind::Apc},
                                   {"8", "Bomber", 75, sim::UnitKind::Bomber}}};
    const std::span<const BuildItem> items = build_items(sim::Side::Player, MenuSubject::Factory);
    ASSERT_EQ(items.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const BuildItem& got = *(items.begin() + static_cast<std::ptrdiff_t>(i));
        const Row& expected = want.at(i);
        EXPECT_EQ(got.key, expected.key);
        EXPECT_EQ(got.label, expected.label);
        EXPECT_EQ(got.cost, expected.cost) << expected.label;
        if (!got.kind.has_value()) {
            // tidy's bugprone-unchecked-optional-access wants the guard in a branch it can see: an
            // ASSERT_TRUE above is not enough, and neither is `.value()`.
            ADD_FAILURE() << expected.label << " names a kind the sim has";
            continue;
        }
        EXPECT_EQ(got.kind.value(), expected.kind) << expected.label;
        EXPECT_EQ(got.cost, sim::spec_of(expected.kind).cost) << "the row is the queue's price";
    }
    // The menu draws the state row, TAB, `~`, then the eight build rows.
    MenuInput in = query(MenuSubject::Factory);
    in.ore = 999;
    EXPECT_EQ(keys_of(menu_rows(in)), (std::vector<std::string>{"SPACE", "TAB", "~", "1", "2", "3",
                                                                "4", "5", "6", "7", "8"}));
    EXPECT_TRUE(build_items(sim::Side::Enemy, MenuSubject::Factory).empty());
}

TEST(Menu, TheFactoryAndTheRecyclerShareNoRow) {
    // Harri's factory names its row 2 `MSL Scout` and his recycler names it `Scout`: two lists, two
    // rows, free to differ. Compared by address, a shared `BuildItem` would be the same object.
    const std::span<const BuildItem> factory = build_items(sim::Side::Player, MenuSubject::Factory);
    const std::span<const BuildItem> recycler
        = build_items(sim::Side::Player, MenuSubject::Deployment);
    for (const BuildItem& f : factory) {
        for (const BuildItem& r : recycler) {
            EXPECT_NE(&f, &r) << f.label;
        }
    }
    EXPECT_EQ((*(factory.begin() + 1)).label, "MSL Scout");
    EXPECT_EQ((*(recycler.begin() + 1)).label, "Scout");
}

TEST(Menu, OnlyAProducersSubjectHasABuildList) {
    for (const MenuSubject subject :
         {MenuSubject::None, MenuSubject::Recycler, MenuSubject::Harvester, MenuSubject::Turret,
          MenuSubject::Vehicle}) {
        EXPECT_TRUE(build_items(sim::Side::Player, subject).empty());
    }
}

TEST(Menu, ARowThatNamesAKindNamesOneTheQueueWillAccept) {
    // A kind the sim cannot queue (no price or no time) must never be a build row a key acts on:
    // the order would be refused by the sim after the player was told it was queued.
    for (const MenuSubject subject : {MenuSubject::Deployment, MenuSubject::Factory}) {
        for (const BuildItem& item : build_items(sim::Side::Player, subject)) {
            if (item.kind.has_value()) {
                EXPECT_TRUE(sim::is_queueable(*item.kind)) << item.label;
            }
        }
    }
}

TEST(Menu, TheDeploymentKeepsTheDropOffStringItCannotExplain) {
    // `~ DROP OFF` is drawn as the capture draws it and means nothing to us: the brief's own words
    // are that it is unknown to the user as well. The string is rendered and NO behaviour is
    // invented behind it -- not a dim, not a renamed verb.
    const std::vector<MenuRow> rows = menu_rows(deployed(999));
    const MenuRow* drop_off = row_for(rows, "~");
    ASSERT_NE(drop_off, nullptr) << "the deployment lost its tilde row";
    EXPECT_EQ(drop_off->verb, "Drop Off");
    EXPECT_TRUE(drop_off->enabled);
    EXPECT_EQ(drop_off->reason, MenuDim::None);
    // The tilde row is drawn ABOVE the build list on every deployment, banked scrap or not.
    const std::vector<MenuRow> broke = menu_rows(deployed(0));
    ASSERT_FALSE(broke.empty());
    EXPECT_EQ(row_for(broke, "~")->verb, "Drop Off") << "affordability reached the tilde row";
    EXPECT_TRUE(row_for(broke, "~")->enabled);
}

TEST(Menu, ALoneTurretCollapsesToTheStateRow) {
    // The capture of a lone deployed turret reads a one-row `SPACE SELECT`: a thing with no verbs
    // of its own still has a menu, and the menu IS the state row.
    MenuInput in = query(MenuSubject::Turret);
    in.target = MenuTarget::Selectable;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.at(0).key, "SPACE");
    EXPECT_EQ(rows.at(0).verb, "Select");
    EXPECT_TRUE(rows.at(0).enabled);
    EXPECT_EQ(rows.at(0).reason, MenuDim::None);
}

// THE MENU, DRAWN (RB0.5). The rows above reach the frame through `SceneOptions::menu`: these pin
// that the layer emits NOTHING without rows, draws every row in the order it was handed, sits down
// the left margin under the strip, and carries `enabled` into the ink.

/// A world scene with the always-on HUD layers off, exactly as `test_scene.cpp`'s rigs have them:
/// the menu is the last thing `append_hud_panels` draws and nothing after it draws here (no aim,
/// no ledger, no map, an undecided skirmish), so the vertices a menu adds are the frame's tail.
[[nodiscard]] SceneOptions menu_rig() {
    SceneOptions o;
    o.seed = 42;
    o.radius_tiles = 8;
    o.camera.x = 60.0F;
    o.camera.z = 60.0F;
    o.camera.y = 8.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

[[nodiscard]] const sim::Sim& skirmish_sim() {
    static const sim::Sim kSim = sim::Sim::create(sim::skirmish(42));
    return kSim;
}

/// The player's command row over that skirmish: the strip the menu is drawn under.
[[nodiscard]] RowState& skirmish_row() {
    static RowState shared = [] {
        RowState row;
        row.update(skirmish_sim(), sim::Side::Player, 0);
        return row;
    }();
    return shared;
}

/// Every vertex the same, field by field -- the strict form of "byte-identical".
void expect_same_frame(const Scene& want, const Scene& got, std::string_view what) {
    ASSERT_EQ(got.vertices.size(), want.vertices.size()) << what;
    for (std::size_t i = 0; i < want.vertices.size(); ++i) {
        const SceneVertex& a = want.vertices.at(i);
        const SceneVertex& b = got.vertices.at(i);
        ASSERT_EQ(a.position, b.position) << what << ": vertex " << i;
        ASSERT_EQ(a.colour, b.colour) << what << ": vertex " << i;
        ASSERT_EQ(a.alpha, b.alpha) << what << ": vertex " << i;
        ASSERT_EQ(a.u, b.u) << what << ": vertex " << i;
        ASSERT_EQ(a.v, b.v) << what << ": vertex " << i;
    }
}

/// The vertices `rows` add to the rig's frame: the tail of the frame with the menu on, after
/// checking that everything before it is the menu-less frame unchanged.
[[nodiscard]] std::vector<SceneVertex> menu_vertices(const std::vector<MenuRow>& rows,
                                                     SceneOptions options = menu_rig()) {
    const Scene without = build_scene(skirmish_sim(), options);
    options.show_menu = true;
    options.menu = &rows;
    const Scene with = build_scene(skirmish_sim(), options);
    EXPECT_GE(with.vertices.size(), without.vertices.size());
    if (with.vertices.size() < without.vertices.size()) {
        return {};
    }
    const Scene head{{with.vertices.begin(),
                      with.vertices.begin() + static_cast<std::ptrdiff_t>(without.vertices.size())},
                     with.view_projection};
    expect_same_frame(without, head, "the menu changed something drawn before it");
    return {with.vertices.begin() + static_cast<std::ptrdiff_t>(without.vertices.size()),
            with.vertices.end()};
}

/// A hand-built row. The key goes in as a `string_view` on purpose: a bare literal would be
/// ambiguous against `MenuRow`'s deleted `std::string&&` constructor.
[[nodiscard]] MenuRow row_of(std::string_view key, std::string_view verb,
                             MenuDim dim = MenuDim::None) {
    return MenuRow{key, std::string{verb}, dim == MenuDim::None, dim};
}

[[nodiscard]] Projected on_screen(const SceneOptions& options, const SceneVertex& v) {
    return project(options.camera, options.extent, v.position[0], v.position[1], v.position[2]);
}

/// The leader line is one quad in a frame with no radar to lead to (the menu rig has no viewer).
constexpr std::size_t kLineVertices = 6U;
/// With a radar and a selected unit it is three (down, along, down) and a dot: a fan of eight
/// triangles round the blip, whose FIRST vertex is always the centre.
constexpr std::size_t kLegVertices = 3U * kLineVertices;
constexpr std::size_t kDotVertices = std::size_t{8} * 3U;
constexpr std::array<float, 3> kOutline{0.0F, 0.0F, 0.0F};

/// What `append_hud_panels` lays the menu out with, in this rig's frame pixels: the strip's 6 px
/// margin and the HUD's 2 px cell, both scaled by the frame's height. Only the X positions are
/// read from it -- the menu's top is the notice row's business, not this slice's.
[[nodiscard]] MenuLayout rig_layout(const std::vector<MenuRow>& rows,
                                    const SceneOptions& options = menu_rig()) {
    const float scale = static_cast<float>(options.extent.height) / 720.0F;
    return menu_layout(rows, 6.0F * scale, 0.0F, 2.0F * scale);
}

/// The two inks a glyph is drawn in: the strip's white on a live row, the grey on a dimmed one.
constexpr std::array<float, 3> kLiveInk{0.95F, 0.95F, 0.95F};
constexpr std::array<float, 3> kDimInk{0.67F, 0.67F, 0.67F};

/// A glyph's ink: either ink colour, at whatever coverage the pixel has.
[[nodiscard]] bool is_ink(const SceneVertex& v) {
    return v.colour == kLiveInk || v.colour == kDimInk;
}

/// A bar vertex is a face or a ring: neither a glyph's ink, nor its outline's black, nor the line's
/// amber. Read by colour, because a glyph's antialiased edge is as see-through as a bar is.
[[nodiscard]] bool is_bar(const SceneVertex& v) {
    return !is_ink(v) && v.colour != kOutline && v.colour != kLedgerInk;
}

/// The menu's capitals, in frame pixels each way: five cells of its 1.5 px text cell at the 720
/// reference (MENU-FONT), whole pixels because every glyph quad is one.
[[nodiscard]] int menu_cap(const SceneOptions& options) {
    return static_cast<int>(
        std::lround(5.0F * 1.5F * static_cast<float>(options.extent.height) / 720.0F));
}

/// The face band `colour` is on an `enabled` row, or -1 when it is none of them (a ring).
[[nodiscard]] int face_band(const std::array<float, 3>& colour, bool enabled) {
    for (int band = 0; band < kMenuFaceBands; ++band) {
        if (menu_face_colour(enabled, band) == colour) {
            return band;
        }
    }
    return -1;
}

TEST(Menu, DrawnWithNoRowsItEmitsNothingAndEveryFrameIsByteIdentical) {
    const std::vector<MenuRow> empty;
    const std::vector<MenuRow> rows{row_of("1", "Hold")};
    for (const bool roster : {false, true}) {
        SceneOptions base = menu_rig();
        base.row = roster ? &skirmish_row() : nullptr;
        // A notice on a roster-less frame is NOT drawn today: `append_hud_panels` returns before
        // it. So the menu's term in that early-return guard is load-bearing -- if a menu with no
        // rows got past the guard, the notice would appear and this frame would change.
        base.notice = "UNIT 4 IS DESTROYED.";
        const std::string where = roster ? "roster on" : "roster off";
        const Scene untouched = build_scene(skirmish_sim(), base);

        SceneOptions null_menu = base;
        null_menu.show_menu = true;
        expect_same_frame(untouched, build_scene(skirmish_sim(), null_menu),
                          where + ": show_menu with a null menu");

        SceneOptions empty_menu = base;
        empty_menu.show_menu = true;
        empty_menu.menu = &empty;
        expect_same_frame(untouched, build_scene(skirmish_sim(), empty_menu),
                          where + ": show_menu with an empty menu");

        // And the switch is a switch: rows handed in with the menu off draw nothing either.
        SceneOptions switched_off = base;
        switched_off.menu = &rows;
        expect_same_frame(untouched, build_scene(skirmish_sim(), switched_off),
                          where + ": rows handed in with show_menu off");
    }
}

TEST(Menu, DrawnEveryRowInTheOrderItWasHanded) {
    const std::vector<MenuRow> bailout{row_of("9", "Bailout")};
    const std::vector<MenuRow> follow{row_of("1", "Follow Me")};
    const std::vector<SceneVertex> one = menu_vertices(bailout);
    ASSERT_FALSE(one.empty()) << "a row must reach the frame";
    EXPECT_EQ(one.size() % 6U, 0U) << "a menu is quads: six vertices each";

    // The same row twice is twice the vertices plus the leader line: a row's quads are its own
    // bars and glyphs, nothing collapsed, and the ONE thing shared is the line down the gutter,
    // which a lone row has no second row to lead to.
    const std::vector<MenuRow> twice{bailout.front(), bailout.front()};
    EXPECT_EQ(menu_vertices(twice).size(), (2U * one.size()) + kLineVertices);

    // DESCENDING keys, so a renderer that sorted by key would swap them. Row 0 is drawn first,
    // byte-for-byte where it is drawn alone; the row handed in LAST is drawn LOWEST.
    const std::vector<MenuRow> handed{bailout.front(), follow.front()};
    const std::vector<SceneVertex> drawn = menu_vertices(handed);
    const std::size_t second = menu_vertices(follow).size();
    ASSERT_EQ(drawn.size(), one.size() + second + kLineVertices);
    for (std::size_t i = 0; i < one.size(); ++i) {
        ASSERT_EQ(drawn.at(i).position, one.at(i).position)
            << "the first row handed in is not the first row drawn (vertex " << i << ")";
    }
    const SceneOptions options = menu_rig();
    float first_bottom = std::numeric_limits<float>::lowest();
    for (std::size_t i = 0; i < one.size(); ++i) {
        first_bottom = std::max(first_bottom, on_screen(options, drawn.at(i)).y);
    }
    for (std::size_t i = one.size(); i < drawn.size(); ++i) {
        ASSERT_GT(on_screen(options, drawn.at(i)).y, first_bottom)
            << "the last row handed in is not below the first";
    }

    // The harvester's real menu, gap and all: exactly the sum of its rows drawn one by one, and
    // the one line, so no row is dropped, added or renumbered into the gap where 6, 7 and 8 would
    // be.
    MenuInput harvester;
    harvester.subject = MenuSubject::Harvester;
    const std::vector<MenuRow> menu = menu_rows(harvester);
    std::size_t sum = 0;
    for (const MenuRow& row : menu) {
        sum += menu_vertices(std::vector<MenuRow>{row}).size();
    }
    EXPECT_EQ(menu_vertices(menu).size(), sum + kLineVertices);

    // The state row naming nothing draws its key alone: some ink, and none past the key column.
    const std::vector<MenuRow> bare{row_of("SPACE", "")};
    const std::vector<SceneVertex> key_only = menu_vertices(bare);
    ASSERT_FALSE(key_only.empty()) << "SPACE with no verb must still draw SPACE";
    const float scale = static_cast<float>(options.extent.height) / 720.0F;
    // The key column's right edge: the 6 px margin, then SPACE's bar -- five glyphs of 6 cells less
    // the trailing blank, and 3.5 cells of bar either side of them -- at the 2 px cell.
    const float key_right = (6.0F + (29.0F * 2.0F) + (7.0F * 2.0F)) * scale;
    for (const SceneVertex& v : key_only) {
        EXPECT_LE(on_screen(options, v).x, key_right + 0.05F)
            << "a verb was drawn for an empty one";
    }
}

TEST(Menu, DrawnDownTheLeftMarginUnderTheStripAndTheNotice) {
    // Measured by projecting each vertex the menu added back to frame pixels -- the attack list's
    // own method in test_scene.cpp -- over the two longest real menus, with the strip on.
    SceneOptions options = menu_rig();
    options.row = &skirmish_row();
    const float scale = static_cast<float>(options.extent.height) / 720.0F;
    // The strip's bottom edge (6 px margin, 34 px plate), and the bottom of the notice row's plate
    // under it (the row is reserved whether or not a notice is up): the menu clears both.
    constexpr float kStripBottomPixels = 40.0F;
    constexpr float kNoticeBottomPixels = 40.0F + 6.0F + 3.0F + 10.0F + 3.0F;
    for (const MenuSubject subject : {MenuSubject::Recycler, MenuSubject::Deployment}) {
        MenuInput in;
        in.subject = subject;
        in.ore = 30;
        const std::vector<MenuRow> rows = menu_rows(in);
        const std::vector<SceneVertex> drawn = menu_vertices(rows, options);
        ASSERT_FALSE(drawn.empty()) << "subject " << static_cast<int>(subject);
        float left = std::numeric_limits<float>::max();
        for (const SceneVertex& v : drawn) {
            const Projected p = on_screen(options, v);
            ASSERT_GT(p.depth, 0.0F) << "a HUD vertex must be in front of the camera";
            EXPECT_GE(p.y, kStripBottomPixels * scale) << "the menu reached up into the strip";
            EXPECT_GE(p.y, kNoticeBottomPixels * scale - 0.05F) << "the menu overlaps the notice";
            EXPECT_LT(p.x, static_cast<float>(options.extent.width) / 3.0F)
                << "the menu left the left third of the frame";
            EXPECT_LT(p.y, static_cast<float>(options.extent.height) * 0.6F)
                << "the menu ran down into the radar's corner";
            left = std::min(left, p.x);
        }
        // The key column starts ON the strip's 6 px margin: the widest key's bar (SPACE, the state
        // row's) has its rounded outer end there, and every narrower key ends at the same right
        // edge and so starts further in.
        EXPECT_NEAR(left, 6.0F * scale, 0.05F) << "subject " << static_cast<int>(subject);
    }
}

TEST(Menu, DrawnADimmedRowIsADimmerInkWhateverTheReason) {
    const std::vector<MenuRow> live{row_of("1", "Hold")};
    const std::vector<MenuRow> moot{row_of("1", "Hold", MenuDim::DoesNotApply)};
    const std::vector<MenuRow> broke{row_of("1", "Hold", MenuDim::Unaffordable)};
    const std::vector<SceneVertex> lit_all = menu_vertices(live);
    const std::vector<SceneVertex> dim_all = menu_vertices(moot);
    // Two causes, ONE look: the reason is for the code, so the two frames are the same frame.
    expect_same_frame(Scene{dim_all, {}}, Scene{menu_vertices(broke), {}},
                      "an unaffordable row and a row that does not apply look different");
    std::vector<SceneVertex> lit;
    std::vector<SceneVertex> dim;
    std::copy_if(lit_all.begin(), lit_all.end(), std::back_inserter(lit), is_ink);
    std::copy_if(dim_all.begin(), dim_all.end(), std::back_inserter(dim), is_ink);
    ASSERT_FALSE(lit.empty());
    ASSERT_EQ(dim.size(), lit.size()) << "dimming changes the ink, never the glyphs";
    for (std::size_t i = 0; i < lit.size(); ++i) {
        ASSERT_EQ(dim.at(i).position, lit.at(i).position) << "vertex " << i;
        EXPECT_EQ(lit.at(i).colour, kLiveInk) << "a live row is the strip's white, vertex " << i;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            EXPECT_LT(dim.at(i).colour.at(channel), lit.at(i).colour.at(channel))
                << "the dimmed row is not darker in channel " << channel << ", vertex " << i;
            // ...and still READS: the old 0.40 grey vanished into the dark unlit bar.
            EXPECT_GT(dim.at(i).colour.at(channel), 0.6F)
                << "the dimmed ink is too dark to read on its bar, channel " << channel;
        }
    }
}

// THE MENU'S LOOK (MENU-READ): BZ2's blue bars, read off the user's own captures. The layout is a
// pure function, so its geometry is pinned without a frame; the drawn tests then hold the frame to
// that layout, the gradient's order, the line's colour and the outline.

/// The vehicle menu's keys, widest first, then the narrow ones the gutter must still hug.
[[nodiscard]] std::vector<MenuRow> column_rows() {
    return {row_of("SPACE", "Select"), row_of("TAB", "Close Menu"),
            row_of("~", "Select Unit", MenuDim::DoesNotApply), row_of("1", "Follow Me"),
            row_of("9", "Bailout")};
}

[[nodiscard]] float text_width(std::string_view text, float cell) {
    return ((static_cast<float>(text.size()) * 6.0F) - 1.0F) * cell;
}

TEST(Menu, LaidOutTheKeyColumnIsRightAlignedSoADigitHugsItsVerb) {
    const std::vector<MenuRow> rows = column_rows();
    constexpr float kLeft = 6.0F;
    for (const float cell : {2.0F, 3.0F}) {
        const MenuLayout layout = menu_layout(rows, kLeft, 0.0F, cell);
        ASSERT_EQ(layout.rows.size(), rows.size());
        const float key_right = layout.rows.front().key_right;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const MenuRowLayout& at = layout.rows.at(i);
            const std::string key{rows.at(i).key};
            // ONE right edge for the whole key column, whatever the key's width.
            EXPECT_FLOAT_EQ(at.key_right, key_right) << key;
            // The bar hugs its own key, 3.5 cells either side, so a wide key grows LEFT.
            EXPECT_FLOAT_EQ(at.key_text, at.key_left + (3.5F * cell)) << key;
            EXPECT_FLOAT_EQ(at.key_text + text_width(rows.at(i).key, cell),
                            at.key_right - (3.5F * cell))
                << key;
            // The gutter: the verb bar starts 4 cells past the key bar, every row alike, and its
            // text 4 cells into it.
            EXPECT_FLOAT_EQ(at.verb_left, key_right + (4.0F * cell)) << key;
            EXPECT_FLOAT_EQ(at.verb_text, at.verb_left + (4.0F * cell)) << key;
            EXPECT_FLOAT_EQ(at.verb_right,
                            at.verb_text + text_width(rows.at(i).verb, cell) + (4.0F * cell))
                << key;
            EXPECT_EQ(at.enabled, rows.at(i).enabled) << key;
        }
        // The widest key, SPACE, starts ON the margin; a digit's bar starts far in from it, so the
        // digit sits the same gutter from its verb as SPACE does instead of in a column sized for
        // SPACE.
        EXPECT_FLOAT_EQ(layout.rows.at(0).key_left, kLeft);
        const MenuRowLayout& digit = layout.rows.at(3);
        const MenuRowLayout& space = layout.rows.at(0);
        EXPECT_FLOAT_EQ(digit.verb_text - (digit.key_text + text_width("1", cell)),
                        space.verb_text - (space.key_text + text_width("SPACE", cell)))
            << "a digit is further from its verb than SPACE is";
        EXPECT_GT(digit.key_left, space.key_left + text_width("SPA", cell));
    }
}

TEST(Menu, LaidOutBarsAreThirteenCellsTallAtAFifteenAndAHalfCellPitch) {
    const std::vector<MenuRow> rows = column_rows();
    for (const float cell : {2.0F, 3.0F}) {
        constexpr float kTop = 65.0F;
        const MenuLayout layout = menu_layout(rows, 6.0F, kTop, cell);
        ASSERT_EQ(layout.rows.size(), rows.size());
        EXPECT_FLOAT_EQ(layout.rows.front().top, kTop);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const MenuRowLayout& at = layout.rows.at(i);
            EXPECT_FLOAT_EQ(at.height, 13.0F * cell) << "row " << i;
            // The 5-cell glyphs sit in the middle of the bar: 4 cells above, 4 below.
            EXPECT_FLOAT_EQ(at.text_top, at.top + (4.0F * cell)) << "row " << i;
            if (i > 0) {
                EXPECT_FLOAT_EQ(at.top - layout.rows.at(i - 1).top, 15.5F * cell) << "row " << i;
            }
        }
    }
}

TEST(Menu, LaidOutTheLeaderLineIsOneStraightSegmentDownTheGutter) {
    const std::vector<MenuRow> rows = column_rows();
    constexpr float kCell = 2.0F;
    const MenuLayout layout = menu_layout(rows, 6.0F, 65.0F, kCell);
    ASSERT_TRUE(layout.has_line);
    const MenuRowLayout& state = layout.rows.front();
    const MenuRowLayout& second = layout.rows.at(1);
    const MenuRowLayout& last = layout.rows.back();
    // One cell wide, centred 2 cells past the key column: the middle of the 4-cell gutter, so it
    // touches neither the key bars' square ends nor the verb bars'.
    EXPECT_FLOAT_EQ(layout.line_right - layout.line_left, kCell);
    EXPECT_FLOAT_EQ((layout.line_left + layout.line_right) * 0.5F,
                    state.key_right + (2.0F * kCell));
    EXPECT_GT(layout.line_left, state.key_right);
    EXPECT_LT(layout.line_right, state.verb_left);
    // It starts BELOW the state row (it leads from the menu, not from what SPACE does) and ends
    // just below the last row, inside the gaps between bars.
    EXPECT_GT(layout.line_top, state.top + state.height);
    EXPECT_LT(layout.line_top, second.top);
    EXPECT_GT(layout.line_bottom, last.top + last.height);
    EXPECT_LT(layout.line_bottom, last.top + (15.5F * kCell));

    // A lone row has no gutter to lead down.
    const std::vector<MenuRow> lone{rows.front()};
    EXPECT_FALSE(menu_layout(lone, 6.0F, 65.0F, kCell).has_line);
}

TEST(Menu, LaidOutTheBarsDidNotMoveWhenTheTypeShrank) {
    // MENU-FONT made the TYPE smaller and left every bar where it was: "only the font would be
    // smaller while the menu item background shape would remain the same size". These are the
    // rectangles `menu_layout` produced at 8884bc3, before the font changed, at the HUD's 2 px
    // layout cell from the 6 px margin.
    const std::vector<MenuRow> rows = column_rows();
    constexpr float kCell = 2.0F;
    const MenuLayout layout = menu_layout(rows, 6.0F, 65.0F, kCell);
    struct Pinned {
        float key_left;
        float key_right;
        float verb_left;
        float verb_right;
        float top;
    };
    constexpr std::array<Pinned, 5> kBefore{{
        {6.0F, 78.0F, 86.0F, 172.0F, 65.0F},   // SPACE  SELECT
        {30.0F, 78.0F, 86.0F, 220.0F, 96.0F},  // TAB    CLOSE MENU
        {54.0F, 78.0F, 86.0F, 232.0F, 127.0F}, // ~      SELECT UNIT
        {54.0F, 78.0F, 86.0F, 208.0F, 158.0F}, // 1      FOLLOW ME
        {54.0F, 78.0F, 86.0F, 184.0F, 189.0F}, // 9      BAILOUT
    }};
    ASSERT_EQ(layout.rows.size(), kBefore.size());
    for (std::size_t i = 0; i < kBefore.size(); ++i) {
        const MenuRowLayout& at = layout.rows.at(i);
        EXPECT_FLOAT_EQ(at.key_left, kBefore.at(i).key_left) << "row " << i;
        EXPECT_FLOAT_EQ(at.key_right, kBefore.at(i).key_right) << "row " << i;
        EXPECT_FLOAT_EQ(at.verb_left, kBefore.at(i).verb_left) << "row " << i;
        EXPECT_FLOAT_EQ(at.verb_right, kBefore.at(i).verb_right) << "row " << i;
        EXPECT_FLOAT_EQ(at.top, kBefore.at(i).top) << "row " << i;
        // 13 layout cells tall at a 15.5-cell pitch, in the LAYOUT cell -- not the text's.
        EXPECT_FLOAT_EQ(at.height, 13.0F * kCell) << "row " << i;
        if (i > 0) {
            EXPECT_FLOAT_EQ(at.top - layout.rows.at(i - 1).top, 15.5F * kCell) << "row " << i;
        }
    }
    EXPECT_FLOAT_EQ(layout.line_left, 81.0F);
    EXPECT_FLOAT_EQ(layout.line_right, 83.0F);
    EXPECT_FLOAT_EQ(layout.line_top, 93.0F);
    EXPECT_FLOAT_EQ(layout.line_bottom, 217.0F);
}

// THE MENU'S TYPE (MENU-FONT): the 5x5 masks box-filtered onto real pixels, so an edge is a
// coverage between 0 and 1 drawn as alpha, and a lowercase letter is its capital's mask, smaller.

TEST(Menu, AGlyphsStemIsFullyCoveredAndItsCornerIsOnlyPartlyCovered) {
    // At 5x5 the grid IS the mask: coverage is the bits, nothing invented.
    const std::vector<float> exact = glyph_coverage('L', 5, 5);
    ASSERT_EQ(exact.size(), 25U);
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 5; ++col) {
            const bool lit = col == 0 || row == 4;
            EXPECT_EQ(exact.at(static_cast<std::size_t>((row * 5) + col)), lit ? 1.0F : 0.0F)
                << "row " << row << " col " << col;
        }
    }
    // At the menu's 8 px capital each pixel is 5/8 of a mask cell. L's stem is the mask's first
    // column, so pixel column 0 lies wholly inside it: exactly 1, not nearly.
    constexpr int kCap = 8;
    const std::vector<float> ell = glyph_coverage('L', kCap, kCap);
    ASSERT_EQ(ell.size(), static_cast<std::size_t>(kCap * kCap));
    EXPECT_EQ(ell.at(static_cast<std::size_t>(3 * kCap)), 1.0F) << "the stem's interior";
    // O's corner: the mask's top-left cell is blank and both its neighbours are lit, so the pixel
    // straddling all three is partly covered -- the edge a 1-bit glyph cannot draw.
    const std::vector<float> oh = glyph_coverage('O', kCap, kCap);
    ASSERT_EQ(oh.size(), static_cast<std::size_t>(kCap * kCap));
    EXPECT_EQ(oh.at(0), 0.0F) << "the blank corner cell's own pixel";
    const float corner = oh.at(static_cast<std::size_t>(kCap) + 1U);
    EXPECT_GT(corner, 0.0F);
    EXPECT_LT(corner, 1.0F);
    for (const float c : oh) {
        EXPECT_GE(c, 0.0F);
        EXPECT_LE(c, 1.0F);
    }
    // A space and a letter the HUD has no mask for are no grid at all.
    EXPECT_TRUE(glyph_coverage(' ', kCap, kCap).empty());
    EXPECT_TRUE(glyph_coverage('Q', kCap, kCap).empty());
}

TEST(Menu, ALowercaseLetterIsItsCapitalAtSevenTenthsOnBothAxes) {
    // The user's ruling: lowercase looks EXACTLY like uppercase, slightly smaller in width and
    // height -- small capitals, 0.71 of the capital, measured off BZ2 (7 px capitals, 5 px lower).
    for (const int cap : {8, 11, 15}) {
        const MenuGlyphGrid upper = menu_glyph_grid('F', cap);
        const MenuGlyphGrid lower = menu_glyph_grid('f', cap);
        EXPECT_EQ(upper.rows, cap);
        EXPECT_EQ(upper.cols, cap);
        EXPECT_EQ(lower.rows, static_cast<int>(std::lround(0.71F * static_cast<float>(cap))));
        EXPECT_EQ(lower.cols, lower.rows) << "the width shrinks with the height";
        EXPECT_LT(lower.rows, upper.rows) << "cap " << cap;
        // Digits and signs have no lowercase: they stay capital size.
        EXPECT_EQ(menu_glyph_grid('7', cap).rows, cap);
        EXPECT_EQ(menu_glyph_grid('~', cap).cols, cap);
        // Both grids hold the SAME shape: the lowercase f is the F's mask, never a blank.
        const std::vector<float> big = glyph_coverage('F', upper.rows, upper.cols);
        const std::vector<float> small = glyph_coverage('f', lower.rows, lower.cols);
        ASSERT_EQ(big.size(), static_cast<std::size_t>(upper.rows * upper.cols));
        ASSERT_EQ(small.size(), static_cast<std::size_t>(lower.rows * lower.cols));
        EXPECT_EQ(small, glyph_coverage('F', lower.rows, lower.cols));
        EXPECT_GT(std::count_if(small.begin(), small.end(), [](float c) { return c > 0.0F; }), 0);
    }
}

TEST(Menu, DrawnAGlyphIsOneQuadPerCoveredPixelAndNoneWhereNothingIsCovered) {
    const SceneOptions options = menu_rig();
    const int cap = menu_cap(options);
    // Mixed case on purpose: a capital, lowercase letters and a digit.
    const std::vector<MenuRow> rows{row_of("1", "Hold")};
    std::size_t covered = 0;
    float partial = 0.0F;
    for (const std::string_view word : {std::string_view{"1"}, std::string_view{"Hold"}}) {
        for (const char glyph : word) {
            const MenuGlyphGrid grid = menu_glyph_grid(glyph, cap);
            for (const float c : glyph_coverage(glyph, grid.rows, grid.cols)) {
                covered += c > 0.0F ? 1U : 0U;
                partial = std::max(partial, c < 1.0F ? c : 0.0F);
            }
        }
    }
    ASSERT_GT(partial, 0.0F) << "the text has no edge pixel to antialias";
    std::vector<SceneVertex> ink;
    for (const SceneVertex& v : menu_vertices(rows)) {
        if (is_ink(v)) {
            ink.push_back(v);
        }
    }
    ASSERT_EQ(ink.size() % 6U, 0U);
    // ONE quad per pixel with any coverage, and none for a pixel with none.
    EXPECT_EQ(ink.size() / 6U, covered) << "an ink quad is not one covered pixel";
    const MenuRowLayout at = rig_layout(rows).rows.front();
    const float gutter = (at.key_right + at.verb_left) * 0.5F;
    struct Box {
        float left{std::numeric_limits<float>::max()};
        float top{std::numeric_limits<float>::max()};
        float bottom{std::numeric_limits<float>::lowest()};
    };
    std::vector<Box> verb; // one box per ink quad of "Hold", in drawing order
    float softest = 1.0F;
    for (std::size_t q = 0; q < ink.size(); q += 6U) {
        float left = std::numeric_limits<float>::max();
        float right = std::numeric_limits<float>::lowest();
        float top = std::numeric_limits<float>::max();
        float bottom = std::numeric_limits<float>::lowest();
        for (std::size_t i = q; i < q + 6U; ++i) {
            const Projected p = on_screen(options, ink.at(i));
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
            EXPECT_GT(ink.at(i).alpha, 0.0F) << "a quad for a pixel with no coverage";
            EXPECT_LE(ink.at(i).alpha, 1.0F);
            softest = std::min(softest, ink.at(i).alpha);
        }
        // One frame pixel, on the pixel grid, to the projection's round trip.
        constexpr float kTol = 0.05F;
        EXPECT_NEAR(right - left, 1.0F, kTol) << "quad " << q / 6U;
        EXPECT_NEAR(bottom - top, 1.0F, kTol) << "quad " << q / 6U;
        EXPECT_NEAR(left, std::round(left), kTol) << "quad " << q / 6U;
        EXPECT_NEAR(top, std::round(top), kTol) << "quad " << q / 6U;
        if (left > gutter) {
            verb.push_back({left, top, bottom});
        }
    }
    EXPECT_LT(softest, 1.0F) << "every pixel is fully covered: the edges are hard";

    // The capital H is the verb's first `cap` columns; "old" after it is lowercase, which sits on
    // the H's baseline and stands 0.71 of its height -- smaller, not floating, not centred.
    ASSERT_FALSE(verb.empty());
    float verb_left = std::numeric_limits<float>::max();
    for (const Box& b : verb) {
        verb_left = std::min(verb_left, b.left);
    }
    Box capital;
    Box lower;
    for (const Box& b : verb) {
        Box& into = b.left < verb_left + static_cast<float>(cap) ? capital : lower;
        into.top = std::min(into.top, b.top);
        into.bottom = std::max(into.bottom, b.bottom);
    }
    EXPECT_NEAR(capital.bottom - capital.top, static_cast<float>(cap), 0.05F);
    EXPECT_NEAR(lower.bottom, capital.bottom, 0.05F) << "lowercase is off the baseline";
    EXPECT_NEAR(lower.bottom - lower.top, static_cast<float>(menu_glyph_grid('o', cap).rows),
                0.05F);
}

TEST(Menu, AFaceIsABlueGradientWhereRedHoldsAndOnlyGreenAndBlueFade) {
    const std::array<float, 3> top = menu_face_colour(true, 0);
    const std::array<float, 3> bottom = menu_face_colour(true, kMenuFaceBands - 1);
    const float red_drift = std::fabs(top.at(0) - bottom.at(0));
    const float green_drop = top.at(1) - bottom.at(1);
    const float blue_drop = top.at(2) - bottom.at(2);
    // BLUE light: blue leads at the top, and loses more than half of itself down the bar...
    EXPECT_GT(top.at(2), top.at(1));
    EXPECT_GT(top.at(1), top.at(0));
    EXPECT_LT(bottom.at(2), 0.5F * top.at(2)) << "blue does not fall by more than half";
    EXPECT_GT(green_drop, 0.0F) << "green does not fade";
    // ...while red barely moves. Fading all three together reads as a lighter CARD, not blue.
    EXPECT_LT(red_drift, 0.25F * blue_drop) << "red fades with blue: a grey card, not BZ2's blue";
    EXPECT_LT(red_drift, 0.25F * green_drop) << "red fades with green: a grey card";
    // The top edge is a HIGHLIGHT, brighter than the fade alone would make it: the first band
    // steps down to the second by more than twice the fade's own step.
    const std::array<float, 3> second = menu_face_colour(true, 1);
    const std::array<float, 3> third = menu_face_colour(true, 2);
    EXPECT_GT(top.at(2) - second.at(2), 2.0F * (second.at(2) - third.at(2)))
        << "the bar has no highlight along its top edge";
    // And it fades one way, top to bottom, band by band.
    for (int band = 1; band < kMenuFaceBands; ++band) {
        const std::array<float, 3> above = menu_face_colour(true, band - 1);
        const std::array<float, 3> here = menu_face_colour(true, band);
        EXPECT_LE(here.at(2), above.at(2)) << "band " << band;
        EXPECT_LE(here.at(1), above.at(1)) << "band " << band;
    }

    // An unavailable row is FLAT: no gradient at all, and darker than the lit bar's darkest band.
    const std::array<float, 3> flat = menu_face_colour(false, 0);
    for (int band = 0; band < kMenuFaceBands; ++band) {
        EXPECT_EQ(menu_face_colour(false, band), flat) << "the unlit face has a gradient";
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        EXPECT_LT(flat.at(channel), bottom.at(channel)) << "channel " << channel;
    }
}

TEST(Menu, DrawnTheFaceRunsTheGradientTopToBottomAndAnUnlitRowIsFlat) {
    const SceneOptions options = menu_rig();
    for (const bool enabled : {true, false}) {
        const std::vector<MenuRow> rows{
            row_of("1", "Hold", enabled ? MenuDim::None : MenuDim::DoesNotApply)};
        const std::vector<SceneVertex> drawn = menu_vertices(rows);
        float top_y = std::numeric_limits<float>::max();
        float bottom_y = std::numeric_limits<float>::lowest();
        int top_band = -1;
        int bottom_band = -1;
        std::size_t rings = 0;
        std::vector<std::array<float, 3>> colours;
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            const SceneVertex& v = drawn.at(i);
            if (!is_bar(v)) {
                continue;
            }
            if (std::find(colours.begin(), colours.end(), v.colour) == colours.end()) {
                colours.push_back(v.colour);
            }
            const int band = face_band(v.colour, enabled);
            if (band < 0) {
                ++rings;
                continue;
            }
            // Each band is drawn as quads of its own: a band's highest point names it.
            const float y = on_screen(options, v).y;
            if (y < top_y - 0.01F) {
                top_y = y;
                top_band = band;
            }
            if (y > bottom_y + 0.01F) {
                bottom_y = y;
                bottom_band = band;
            }
        }
        if (enabled) {
            EXPECT_EQ(top_band, 0) << "the bar's top is not the gradient's first band";
            EXPECT_EQ(bottom_band, kMenuFaceBands - 1) << "the bar's bottom is not its last band";
            EXPECT_GT(rings, 0U) << "a lit bar has a ring round its outer end";
        } else {
            EXPECT_GE(top_band, 0) << "the unlit face is not the flat colour";
            EXPECT_EQ(rings, 0U) << "an unlit bar has no ring";
            // Flat, read off the frame itself: every see-through vertex is one colour.
            EXPECT_EQ(colours.size(), 1U) << "the unlit bar is not one flat colour";
        }
    }
}

TEST(Menu, DrawnABarIsSquareOnTheGutterSideAndRoundOnItsOuterEnd) {
    const SceneOptions options = menu_rig();
    const float scale = static_cast<float>(options.extent.height) / 720.0F;
    const std::vector<MenuRow> rows{row_of("1", "Hold")};
    const MenuLayout layout = rig_layout(rows);
    ASSERT_EQ(layout.rows.size(), 1U);
    const MenuRowLayout& at = layout.rows.front();
    const float radius = 6.5F * 2.0F * scale; // half the 13-cell bar
    const float gutter = (at.key_right + at.verb_left) * 0.5F;
    struct Bar {
        float left{std::numeric_limits<float>::max()};
        float right{std::numeric_limits<float>::lowest()};
        float top{std::numeric_limits<float>::max()};
        float bottom{std::numeric_limits<float>::lowest()};
        std::vector<std::array<float, 2>> points;
    };
    std::array<Bar, 2> bars; // the key's, then the verb's
    for (const SceneVertex& v : menu_vertices(rows)) {
        if (!is_bar(v)) {
            continue;
        }
        const Projected p = on_screen(options, v);
        Bar& bar = bars.at(p.x < gutter ? 0 : 1);
        bar.left = std::min(bar.left, p.x);
        bar.right = std::max(bar.right, p.x);
        bar.top = std::min(bar.top, p.y);
        bar.bottom = std::max(bar.bottom, p.y);
        bar.points.push_back({p.x, p.y});
    }
    constexpr float kTol = 0.05F;
    EXPECT_NEAR(bars.at(0).left, at.key_left, kTol);
    EXPECT_NEAR(bars.at(0).right, at.key_right, kTol);
    EXPECT_NEAR(bars.at(1).left, at.verb_left, kTol);
    EXPECT_NEAR(bars.at(1).right, at.verb_right, kTol);
    for (std::size_t side = 0; side < bars.size(); ++side) {
        const Bar& bar = bars.at(side);
        EXPECT_NEAR(bar.bottom - bar.top, 13.0F * 2.0F * scale, kTol) << "bar " << side;
        const float inner = side == 0 ? bar.right : bar.left;
        const float outer = side == 0 ? bar.left : bar.right;
        bool inner_top = false;
        bool inner_bottom = false;
        float outer_reach_at_edge = 0.0F; // how close to the outer end a top/bottom point gets
        float outer_y = 0.0F;             // the height at which the outer end is reached
        for (const std::array<float, 2>& point : bar.points) {
            const bool on_edge = std::fabs(point.at(1) - bar.top) < kTol
                                 || std::fabs(point.at(1) - bar.bottom) < kTol;
            if (std::fabs(point.at(0) - inner) < kTol) {
                inner_top = inner_top || std::fabs(point.at(1) - bar.top) < kTol;
                inner_bottom = inner_bottom || std::fabs(point.at(1) - bar.bottom) < kTol;
            }
            if (on_edge) {
                outer_reach_at_edge
                    = std::max(outer_reach_at_edge, radius - std::fabs(point.at(0) - outer));
            }
            if (std::fabs(point.at(0) - outer) < kTol) {
                outer_y = point.at(1);
            }
        }
        // SQUARE where the line runs: the inner edge has a corner at the top AND the bottom.
        EXPECT_TRUE(inner_top && inner_bottom) << "bar " << side << "'s inner edge is not square";
        // ROUND outside: no point on the top or bottom edge comes within the radius of the outer
        // end, and the outer end is reached at the bar's middle height.
        EXPECT_LE(outer_reach_at_edge, kTol) << "bar " << side << "'s outer end has a corner";
        EXPECT_NEAR(outer_y, (bar.top + bar.bottom) * 0.5F, kTol) << "bar " << side;
    }
}

TEST(Menu, DrawnTheLeaderLineIsTheLedgerAmberDrawnOverTheBars) {
    const SceneOptions options = menu_rig();
    const std::vector<MenuRow> rows = column_rows();
    const MenuLayout layout = rig_layout(rows);
    const std::vector<SceneVertex> drawn = menu_vertices(rows);
    std::vector<std::size_t> line;
    std::size_t last_bar = 0;
    float first_row_top = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        if (drawn.at(i).colour == kLedgerInk) {
            line.push_back(i);
        }
        if (is_bar(drawn.at(i))) {
            last_bar = i;
            const float y = on_screen(options, drawn.at(i)).y;
            first_row_top = std::min(first_row_top, y);
        }
    }
    // ONE segment, in the ledger's own amber, emitted after every bar so it crosses over them.
    ASSERT_EQ(line.size(), kLineVertices) << "the line is not one quad of kLedgerInk";
    EXPECT_EQ(line.back() - line.front() + 1U, kLineVertices) << "the line's quad is split";
    EXPECT_GT(line.front(), last_bar) << "a bar is drawn over the line";
    const float scale = static_cast<float>(options.extent.height) / 720.0F;
    const float state_row_bottom = first_row_top + (13.0F * 2.0F * scale);
    const float second_row_top = first_row_top + (15.5F * 2.0F * scale);
    float top = std::numeric_limits<float>::max();
    for (const std::size_t i : line) {
        const Projected p = on_screen(options, drawn.at(i));
        EXPECT_EQ(drawn.at(i).alpha, 1.0F);
        // Constant x: the gutter, all the way down.
        const bool on_left = std::fabs(p.x - layout.line_left) < 0.05F;
        const bool on_right = std::fabs(p.x - layout.line_right) < 0.05F;
        EXPECT_TRUE(on_left || on_right) << "the line left the gutter at x " << p.x;
        top = std::min(top, p.y);
    }
    EXPECT_GT(top, state_row_bottom) << "the line starts inside the state row";
    EXPECT_LT(top, second_row_top) << "the line starts below the second row";
}

// THE LEG (MENU-LEG): the line leaves the menu, runs along the radar's top edge and lands on the
// selected unit's blip. The blip is the RADAR's: these tests ask `radar_marks` -- the call the
// radar itself makes -- where the unit is, so a leader that projected the unit on its own would
// miss.

TEST(Menu, LaidOutTheLeaderTurnsSquareAlongTheRadarsTopAndEndsOnTheBlip) {
    const MenuLayout layout = menu_layout(column_rows(), 6.0F, 65.0F, 2.0F);
    ASSERT_TRUE(layout.has_line);
    const float gutter = (layout.line_left + layout.line_right) * 0.5F;
    constexpr float kRadarTop = 578.0F;
    using Point = std::array<float, 2>;
    for (const Point blip : {Point{200.0F, 640.0F}, Point{gutter - 9.0F, 600.0F}}) {
        // Down the gutter without a jog, along the top edge (right, or left for a blip left of the
        // gutter), down onto the blip: three straight segments, and the dot on the last corner.
        const MenuLeader led = menu_leader(layout, kRadarTop, blip);
        ASSERT_EQ(led.points.size(), 4U);
        EXPECT_TRUE(led.dot);
        EXPECT_EQ(led.points.at(0), (Point{gutter, layout.line_top}));
        EXPECT_EQ(led.points.at(1), (Point{gutter, kRadarTop}));
        EXPECT_EQ(led.points.at(2), (Point{blip.at(0), kRadarTop}));
        EXPECT_EQ(led.points.at(3), blip);
    }
    // Nothing selected: down to the radar's top edge, and it stops there. No dot.
    const MenuLeader bare = menu_leader(layout, kRadarTop, std::nullopt);
    ASSERT_EQ(bare.points.size(), 2U);
    EXPECT_FALSE(bare.dot);
    EXPECT_EQ(bare.points.back(), (Point{gutter, kRadarTop}));
    // No radar (the world map is up, or there is no viewer): MENU-READ's line, ending a cell under
    // the last row, whatever is selected.
    const MenuLeader menu_only = menu_leader(layout, std::nullopt, Point{200.0F, 640.0F});
    ASSERT_EQ(menu_only.points.size(), 2U);
    EXPECT_FALSE(menu_only.dot);
    EXPECT_EQ(menu_only.points.back(), (Point{gutter, layout.line_bottom}));
    // A lone row has no line to extend.
    const std::vector<MenuRow> lone{column_rows().front()};
    EXPECT_TRUE(menu_leader(menu_layout(lone, 6.0F, 65.0F, 2.0F), kRadarTop, Point{200.0F, 640.0F})
                    .points.empty());
}

/// The menu rig with a radar: centred on the player's first tank, and one of the player's other
/// units -- one the radar marks as a dot -- selected.
struct LegRig {
    SceneOptions options{menu_rig()};
    std::vector<sim::UnitId> selected;
};

/// The radar's marks for `rig`'s frame, by the call `append_radar` makes.
[[nodiscard]] std::vector<RadarMark> rig_marks(const SceneOptions& options) {
    const sim::Sim& sim = skirmish_sim();
    return radar_marks(sim.units(), options.viewer_unit, options.camera.yaw,
                       radar_panel(options.extent), viewer_radar_radius(sim, options.viewer_unit));
}

[[nodiscard]] LegRig leg_rig() {
    LegRig rig;
    for (const sim::Unit& unit : skirmish_sim().units()) {
        if (unit.alive && unit.side == sim::Side::Player && unit.kind == sim::UnitKind::Tank) {
            rig.options.viewer_unit = unit.id;
            break;
        }
    }
    for (const RadarMark& mark : rig_marks(rig.options)) {
        if (mark.shape == RadarShape::Square && mark.tone == RadarTone::Yours) {
            rig.selected.push_back(mark.unit);
            break;
        }
    }
    return rig;
}

/// What the menu adds to `rig`'s frame, with its selection: the tail, as `menu_vertices` takes it.
[[nodiscard]] std::vector<SceneVertex> leg_vertices(const std::vector<MenuRow>& rows,
                                                    const LegRig& rig) {
    const Scene without = build_scene(skirmish_sim(), rig.options, rig.selected);
    SceneOptions options = rig.options;
    options.show_menu = true;
    options.menu = &rows;
    const Scene with = build_scene(skirmish_sim(), options, rig.selected);
    EXPECT_GE(with.vertices.size(), without.vertices.size());
    if (with.vertices.size() < without.vertices.size()) {
        return {};
    }
    return {with.vertices.begin() + static_cast<std::ptrdiff_t>(without.vertices.size()),
            with.vertices.end()};
}

TEST(Menu, DrawnTheWholeLeaderAndItsDotAreTheLedgerAmberDrawnLast) {
    const LegRig rig = leg_rig();
    ASSERT_TRUE(rig.options.viewer_unit.valid());
    ASSERT_EQ(rig.selected.size(), 1U) << "the radar marks none of the player's units as a dot";
    ASSERT_TRUE(rig.selected.front().valid()) << "the radar's mark does not say whose it is";
    const std::vector<SceneVertex> drawn = leg_vertices(column_rows(), rig);
    std::vector<std::size_t> amber;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        if (drawn.at(i).colour == kLedgerInk) {
            amber.push_back(i);
            EXPECT_EQ(drawn.at(i).alpha, 1.0F) << "vertex " << i;
        }
    }
    // Three segments and the dot, every vertex in the SCRAP label's own constant, and nothing else
    // in that run: the leg is the menu's last word, over every bar and glyph.
    ASSERT_EQ(amber.size(), kLegVertices + kDotVertices) << "the leg is not 3 quads and a dot";
    EXPECT_EQ(amber.back() - amber.front() + 1U, amber.size()) << "the leg is interrupted";
    EXPECT_EQ(amber.back() + 1U, drawn.size()) << "something is drawn over the leg";
}

TEST(Menu, DrawnTheLeadersDotLandsOnTheRadarsOwnMarkForTheSelectedUnit) {
    const LegRig rig = leg_rig();
    ASSERT_EQ(rig.selected.size(), 1U) << "the radar marks none of the player's units as a dot";
    ASSERT_TRUE(rig.selected.front().valid()) << "the radar's mark does not say whose it is";
    const std::vector<RadarMark> marks = rig_marks(rig.options);
    const auto mark = std::find_if(marks.begin(), marks.end(), [&](const RadarMark& m) {
        return m.unit == rig.selected.front();
    });
    ASSERT_NE(mark, marks.end());
    const RadarPanel panel = radar_panel(rig.options.extent);
    const float blip_x = panel.x + mark->px;
    const float blip_y = panel.y + mark->py;

    std::vector<Projected> leg;
    for (const SceneVertex& v : leg_vertices(column_rows(), rig)) {
        if (v.colour == kLedgerInk) {
            leg.push_back(on_screen(rig.options, v));
        }
    }
    ASSERT_EQ(leg.size(), kLegVertices + kDotVertices);
    // A twentieth of a pixel, the round trip through the camera; a leg that placed the unit by
    // arithmetic of its own would be whole pixels out, or the panel's corner out.
    constexpr float kTol = 0.05F;
    const float scale = static_cast<float>(rig.options.extent.height) / 720.0F;
    // The dot: every triangle's first vertex is ON the radar's mark, and the rim is 2 px out.
    for (std::size_t i = kLegVertices; i < leg.size(); ++i) {
        const Projected& p = leg.at(i);
        const float reach = std::hypot(p.x - blip_x, p.y - blip_y);
        if ((i - kLegVertices) % 3U == 0U) {
            EXPECT_NEAR(p.x, blip_x, kTol) << "the dot's centre is off the blip, vertex " << i;
            EXPECT_NEAR(p.y, blip_y, kTol) << "the dot's centre is off the blip, vertex " << i;
        } else {
            EXPECT_NEAR(reach, 2.0F * scale, kTol) << "the dot's rim, vertex " << i;
        }
    }
    // The last segment comes straight down onto the blip from the radar's top edge, in its x.
    float bottom = std::numeric_limits<float>::lowest();
    float top = std::numeric_limits<float>::max();
    float left = std::numeric_limits<float>::max();
    float right = std::numeric_limits<float>::lowest();
    for (std::size_t i = kLegVertices - kLineVertices; i < kLegVertices; ++i) {
        bottom = std::max(bottom, leg.at(i).y);
        top = std::min(top, leg.at(i).y);
        left = std::min(left, leg.at(i).x);
        right = std::max(right, leg.at(i).x);
    }
    EXPECT_NEAR(bottom, blip_y, kTol);
    EXPECT_NEAR((left + right) * 0.5F, blip_x, kTol);
    EXPECT_NEAR(right - left, 2.0F * scale, kTol) << "the line is not 2 px wide at 720";
    EXPECT_NEAR(top, panel.y - scale, kTol) << "the last segment does not start at the elbow";
}

TEST(Menu, DrawnTheLeadersDotIsInFrontOfTheBlipItMarks) {
    // MENU-LEG's first capture had no dot at all: drawn at the radar marks' own distance, it lost
    // the depth test to the blip under it. Every HUD layer sits a distance along its pixel's ray,
    // so "in front" is each vertex's distance from the eye.
    const LegRig rig = leg_rig();
    ASSERT_EQ(rig.selected.size(), 1U) << "the radar marks none of the player's units as a dot";
    ASSERT_TRUE(rig.selected.front().valid()) << "the radar's mark does not say whose it is";
    const std::vector<RadarMark> marks = rig_marks(rig.options);
    const auto mark = std::find_if(marks.begin(), marks.end(), [&](const RadarMark& m) {
        return m.unit == rig.selected.front();
    });
    ASSERT_NE(mark, marks.end());
    const RadarPanel panel = radar_panel(rig.options.extent);
    const float blip_x = panel.x + mark->px;
    const float blip_y = panel.y + mark->py;
    const float scale = static_cast<float>(rig.options.extent.height) / 720.0F;

    SceneOptions options = rig.options;
    const std::vector<MenuRow> rows = column_rows();
    options.show_menu = true;
    options.menu = &rows;
    const Scene frame = build_scene(skirmish_sim(), options, rig.selected);
    const auto from_eye = [&](const SceneVertex& v) {
        return std::hypot(v.position[0] - options.camera.x, v.position[1] - options.camera.y,
                          v.position[2] - options.camera.z);
    };
    constexpr float kTol = 0.05F;
    float dot_farthest = std::numeric_limits<float>::lowest();
    float blip_nearest = std::numeric_limits<float>::max();
    std::size_t corners = 0;
    for (const SceneVertex& v : frame.vertices) {
        const Projected p = on_screen(options, v);
        if (v.colour == kLedgerInk) {
            if (std::hypot(p.x - blip_x, p.y - blip_y) <= (2.0F * scale) + kTol) {
                dot_farthest = std::max(dot_farthest, from_eye(v));
            }
            continue;
        }
        // The blip's own square: a vertex on one of its four corners.
        if (std::fabs(std::fabs(p.x - blip_x) - mark->half) < kTol
            && std::fabs(std::fabs(p.y - blip_y) - mark->half) < kTol) {
            ++corners;
            blip_nearest = std::min(blip_nearest, from_eye(v));
        }
    }
    ASSERT_GT(corners, 0U) << "the radar drew no square for the selected unit";
    ASSERT_GT(dot_farthest, 0.0F) << "no dot";
    EXPECT_LT(dot_farthest, blip_nearest) << "the dot is behind the blip it marks";
}

TEST(Menu, DrawnEveryGlyphSitsWhereTheLayoutPutsIt) {
    // `TAB` and `HOLD`: the T and the H have ink in their leftmost column, the B and the D in
    // their rightmost, and the H in its top and bottom rows, so the ink's box IS the text's box.
    const SceneOptions options = menu_rig();
    const float cell = 2.0F * static_cast<float>(options.extent.height) / 720.0F;
    const std::vector<MenuRow> rows{row_of("TAB", "Hold")};
    const MenuLayout layout = rig_layout(rows);
    ASSERT_EQ(layout.rows.size(), 1U);
    const MenuRowLayout& at = layout.rows.front();
    const float gutter = (at.key_right + at.verb_left) * 0.5F;
    struct Box {
        float left{std::numeric_limits<float>::max()};
        float right{std::numeric_limits<float>::lowest()};
        float top{std::numeric_limits<float>::max()};
        float bottom{std::numeric_limits<float>::lowest()};
        void grow(const Projected& p) {
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
    };
    Box bar;
    Box key;
    Box verb;
    for (const SceneVertex& v : menu_vertices(rows)) {
        const Projected p = on_screen(options, v);
        if (is_bar(v)) {
            bar.grow(p);
        } else if (is_ink(v)) {
            (p.x < gutter ? key : verb).grow(p);
        }
    }
    // Every glyph starts on a whole pixel, so an anchor is met to within half of one.
    constexpr float kSnap = 0.51F;
    // The key right-aligned in its bar, 3.5 cells short of the column edge; the verb 4 cells into
    // its bar. The bars are sized for the 2 px layout cell and the type is smaller (MENU-FONT), so
    // the text ends short of the far pad and the spare room is on the bar's rounded end.
    EXPECT_NEAR(key.right, at.key_right - (3.5F * cell), kSnap);
    EXPECT_GE(key.left, at.key_text - kSnap);
    EXPECT_NEAR(verb.left, at.verb_left + (4.0F * cell), kSnap);
    EXPECT_LT(verb.right, at.verb_right - (4.0F * cell));
    // Smaller than the layout's 5-cell slot, and centred down the bar within a pixel.
    const float cap = static_cast<float>(menu_cap(options));
    EXPECT_LT(cap, 5.0F * cell) << "the type did not get smaller";
    for (const Box* text : {&key, &verb}) {
        EXPECT_NEAR(text->bottom - text->top, cap, 0.05F);
        EXPECT_NEAR(text->top - bar.top, bar.bottom - text->bottom, 1.05F);
    }
}

TEST(Menu, DrawnEveryGlyphIsOutlinedInBlackOnAllFourSides) {
    const SceneOptions options = menu_rig();
    const float half_cell = 0.5F * 2.0F * static_cast<float>(options.extent.height) / 720.0F;
    const std::vector<MenuRow> rows{row_of("1", "Hold")};
    const std::vector<SceneVertex> drawn = menu_vertices(rows);
    std::size_t ink = 0;
    std::size_t outline = 0;
    std::array<float, 4> ink_box{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    std::array<float, 4> outline_box = ink_box;
    const auto grow = [&](std::array<float, 4>& box, const SceneVertex& v) {
        const Projected p = on_screen(options, v);
        box = {std::min(box.at(0), p.x), std::min(box.at(1), p.y), std::max(box.at(2), p.x),
               std::max(box.at(3), p.y)};
    };
    for (const SceneVertex& v : drawn) {
        if (is_ink(v)) {
            ++ink;
            grow(ink_box, v);
        } else if (!is_bar(v) && v.colour == kOutline) {
            ++outline;
            grow(outline_box, v);
        }
    }
    ASSERT_GT(ink, 0U);
    // The outline is the ink's coverage grown by a pixel: every inked pixel has black under it,
    // plus the ring round the outside, and never five quads per pixel.
    EXPECT_GT(outline, ink) << "the outline does not reach past the ink";
    EXPECT_LT(outline, 3U * ink) << "the outline is drawn per neighbour, not per pixel";
    // The outline's box is the ink's grown by half a layout cell all round: 1 px at the 720
    // reference.
    EXPECT_NEAR(outline_box.at(0), ink_box.at(0) - half_cell, 0.05F);
    EXPECT_NEAR(outline_box.at(1), ink_box.at(1) - half_cell, 0.05F);
    EXPECT_NEAR(outline_box.at(2), ink_box.at(2) + half_cell, 0.05F);
    EXPECT_NEAR(outline_box.at(3), ink_box.at(3) + half_cell, 0.05F);
}

TEST(Menu, ASelectionOwnsTheSubjectOfTheLivingKindItHolds) {
    // The ONE rule from selection to subject (`menu_subject_of`). Covered where the app's frame
    // draws it, in tests/test_player_view.cpp: a lone Factory, Scavenger, Recycler and Turret, a
    // lone tank, a mixed pair, an empty selection, an id that is not in the sim on its own, and a
    // lone dead unit. Pinned HERE: every kind on its own (the table, once per kind), and the two
    // ways a selection can hold something that is not part of it.
    constexpr sim::Side kPlayer = sim::Side::Player;
    constexpr sim::Side kEnemy = sim::Side::Enemy;
    std::vector<sim::Spawn> spawns;
    spawns.reserve(static_cast<std::size_t>(sim::kUnitKindCount)); // one spawn per kind, exactly
    for (int kind = 0; kind < sim::kUnitKindCount; ++kind) {
        spawns.push_back(sim::Spawn{static_cast<sim::UnitKind>(kind), kPlayer, 20 + kind, 20});
    }
    const sim::Sim every_kind = sim::Sim::create(sim::Scenario{1, spawns});
    ASSERT_EQ(every_kind.units().size(), static_cast<std::size_t>(sim::kUnitKindCount));
    for (const sim::Unit& unit : every_kind.units()) {
        const std::vector<sim::UnitId> alone{unit.id};
        EXPECT_EQ(menu_subject_of(every_kind, alone), menu_subject_of_kind(unit.kind))
            << "kind " << static_cast<int>(unit.kind);
    }

    // A tank that dies, beside a harvester that does not: the corpse is not part of the selection,
    // so the selection is a HARVESTER one and not a mixed one. The tank is shot by a pair of
    // enemy tanks and the sim keeps it in its table with `alive` false.
    sim::Sim sim = sim::Sim::create(sim::Scenario{1,
                                                  {{sim::UnitKind::Tank, kPlayer, 40, 40},
                                                   {sim::UnitKind::Scavenger, kPlayer, 120, 120},
                                                   {sim::UnitKind::Tank, kEnemy, 43, 40},
                                                   {sim::UnitKind::Tank, kEnemy, 43, 42}}});
    const sim::UnitId tank = sim.units()[0].id;
    const sim::UnitId harvester = sim.units()[1].id;
    const std::vector<sim::UnitId> both{tank, harvester};
    ASSERT_EQ(menu_subject_of(sim, both), MenuSubject::None)
        << "alive, a tank and a harvester together are a mixed selection";
    int ticks = 0;
    while (ticks++ < 30000 && sim.find(tank)->alive) {
        sim.step();
    }
    // The ANCHOR: the dead stay in the table, and the harvester is still there to be selected.
    ASSERT_NE(sim.find(tank), nullptr);
    ASSERT_FALSE(sim.find(tank)->alive) << "the enemy never killed the tank";
    ASSERT_TRUE(sim.find(harvester)->alive) << "the harvester must outlive the tank";
    EXPECT_EQ(menu_subject_of(sim, both), MenuSubject::Harvester);

    // An id that is not in the sim is skipped the same way, beside a living unit.
    const std::vector<sim::UnitId> with_stranger{sim::UnitId{9999}, harvester};
    ASSERT_EQ(sim.find(sim::UnitId{9999}), nullptr);
    EXPECT_EQ(menu_subject_of(sim, with_stranger), MenuSubject::Harvester);
}

// Harri's naming rule, not a cosmetic preference: a Scavenger and a Harvester are the same machine
// differing by FACTION, and "Guardian" is the alien TURRET. The sim has no faction yet, so each row
// prints the name of the kind it actually builds.
TEST(Menu, ARowPrintsTheNameOfTheKindItBuildsNotTheAlienFactionsName) {
    const std::span<const BuildItem> items
        = build_items(sim::Side::Player, MenuSubject::Deployment);
    bool saw_scavenger = false;
    bool saw_tank = false;
    for (const BuildItem& item : items) {
        if (item.kind == sim::UnitKind::Scavenger) {
            saw_scavenger = true;
            EXPECT_EQ(item.label, "Scavenger") << "Harvester is the alien faction's name";
        }
        if (item.kind == sim::UnitKind::Tank) {
            saw_tank = true;
            EXPECT_EQ(item.label, "Tank") << "Guardian is the alien faction's turret";
        }
    }
    EXPECT_TRUE(saw_scavenger);
    EXPECT_TRUE(saw_tank);
}

} // namespace
} // namespace protekt::game
