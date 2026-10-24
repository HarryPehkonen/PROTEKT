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
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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
    EXPECT_EQ(selectable.front().verb, "SELECT");
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
        EXPECT_EQ(menu_rows(armed).front().verb, "DEPLOY") << "target " << static_cast<int>(target);
    }

    // Case 4: a build row armed -> SPACE names the item it will place.
    MenuInput building = deployed(500);
    building.armed = ArmedCommand{MenuCommand::Build, "SCOUT"};
    EXPECT_EQ(menu_rows(building).front().verb, "SCOUT");

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
    EXPECT_EQ(before.front().verb, "SELECT");
    EXPECT_EQ(after.front().verb, "DEPLOY") << "SPACE did not pick up the armed command";
    EXPECT_EQ(row_for(before, "3")->verb, "DEPLOY");
    EXPECT_EQ(row_for(after, "3")->verb, "DEPLOY")
        << "arming changed the row that armed it: it is a binding, not a readout";
    EXPECT_EQ(after.at(5).key, "3") << "arming moved rows around";
}

TEST(Menu, TheSecondRowClosesTheMenu) {
    for (const MenuSubject subject :
         {MenuSubject::Recycler, MenuSubject::Harvester, MenuSubject::Deployment}) {
        const std::vector<MenuRow> rows = menu_rows(query(subject));
        ASSERT_GT(rows.size(), 2U) << "subject " << static_cast<int>(subject);
        EXPECT_EQ(rows.at(1).key, "TAB");
        EXPECT_EQ(rows.at(1).verb, "CLOSE MENU");
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
    EXPECT_EQ(recycler.at(5).verb, "DEPLOY");
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
    EXPECT_EQ(row_for(recycler, "3")->verb, "DEPLOY");
    EXPECT_EQ(row_for(harvester, "3")->verb, "SCAVENGE");
    EXPECT_EQ(row_for(deployment, "3")->verb, "GUARDIAN 40");

    // The verbs the capture verified, in the order it draws them -- the rest of both menus.
    ASSERT_NE(row_for(harvester, "9"), nullptr);
    EXPECT_EQ(row_for(harvester, "9")->verb, "BAILOUT");
    EXPECT_EQ(row_for(harvester, "0")->verb, "RECYCLE");
    EXPECT_EQ(row_for(harvester, "4")->verb, "HOLD");
    EXPECT_EQ(row_for(recycler, "1")->verb, "FOLLOW ME");
    EXPECT_EQ(row_for(harvester, "2")->verb, "GO TO NAV");
    EXPECT_EQ(row_for(recycler, "~")->verb, "ROTATE BUILDINGS");
    EXPECT_EQ(row_for(harvester, "~")->verb, "SELECT UNIT");
    EXPECT_EQ(row_for(harvester, "5")->verb, "PICK ME UP");
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
    EXPECT_EQ(scout->verb, "SCOUT 45");
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
                        in.armed = ArmedCommand{command, "SCOUT"};
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
                            EXPECT_EQ(rows.front().verb, "DEPLOY") << where;
                            break;
                        case MenuCommand::Build:
                            EXPECT_EQ(rows.front().verb, "SCOUT") << where;
                            break;
                        case MenuCommand::None:
                            EXPECT_TRUE(rows.front().verb.empty() || rows.front().verb == "SELECT")
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
    for (const BuildItem& item : build_items(sim::Side::Player)) {
        const MenuRow* row = row_for(rich, item.key);
        ASSERT_NE(row, nullptr) << "build key " << item.key << " is not drawn";
        EXPECT_EQ(row->verb, std::string{item.label} + " " + std::to_string(item.cost))
            << "build row " << item.key << " does not draw its cost";
        EXPECT_TRUE(row->enabled) << "with 999 banked, " << item.label << " is affordable";
    }
    // And the captured names and costs themselves, as the quotation they are.
    EXPECT_EQ(row_for(rich, "1")->verb, "HARVESTER 20");
    EXPECT_EQ(row_for(rich, "3")->verb, "GUARDIAN 40");
    EXPECT_EQ(row_for(rich, "4")->verb, "HAULER 30");
    EXPECT_EQ(row_for(rich, "5")->verb, "BUILDER 40");
    EXPECT_EQ(row_for(rich, "6")->verb, "HEALER 50");
    EXPECT_EQ(row_for(rich, "7")->verb, "SERVICE POD 2");

    // 44 scrap buys the 20-scrap harvester and the 2-scrap service pod and nothing else here: the
    // user's reading of the dim rows is exactly this comparison.
    const std::vector<MenuRow> poor = menu_rows(deployed(44));
    ASSERT_EQ(poor.size(), 10U);
    EXPECT_TRUE(row_for(poor, "1")->enabled);
    EXPECT_FALSE(row_for(poor, "2")->enabled) << "SCOUT 45 was affordable on 44 scrap";
    EXPECT_EQ(row_for(poor, "2")->reason, MenuDim::Unaffordable);
    EXPECT_FALSE(row_for(poor, "6")->enabled) << "HEALER 50 was affordable on 44 scrap";
    EXPECT_TRUE(row_for(poor, "7")->enabled);

    // Exactly the price is affordable: 45 buys the 45-scrap scout.
    const std::vector<MenuRow> exact = menu_rows(deployed(45));
    EXPECT_TRUE(row_for(exact, "2")->enabled);
}

TEST(Menu, TheEnemyBuildListIsNotInvented) {
    // Only the player's list was captured. The enemy's is empty until RB5 and the R3 roster fill it
    // (a per-side item name, a per-kind cost), so its deployment draws the rows it does know and no
    // build rows at all -- rather than a copy of the player's names.
    EXPECT_EQ(build_items(sim::Side::Player).size(), 7U);
    EXPECT_TRUE(build_items(sim::Side::Enemy).empty());

    MenuInput in = deployed(999);
    in.side = sim::Side::Enemy;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_EQ(rows.size(), 3U) << "the enemy deployment grew rows nobody has seen";
    EXPECT_EQ(rows.at(0).key, "SPACE");
    EXPECT_EQ(rows.at(1).verb, "CLOSE MENU");
    EXPECT_EQ(rows.at(2).verb, "DROP OFF");
    for (const std::string_view absent : {"1", "2", "3", "4", "5", "6", "7"}) {
        EXPECT_EQ(row_for(rows, absent), nullptr) << "key " << absent << " invented a build row";
    }
}

TEST(Menu, TheDeploymentKeepsTheDropOffStringItCannotExplain) {
    // `~ DROP OFF` is drawn as the capture draws it and means nothing to us: the brief's own words
    // are that it is unknown to the user as well. The string is rendered and NO behaviour is
    // invented behind it -- not a dim, not a renamed verb.
    const std::vector<MenuRow> rows = menu_rows(deployed(999));
    const MenuRow* drop_off = row_for(rows, "~");
    ASSERT_NE(drop_off, nullptr) << "the deployment lost its tilde row";
    EXPECT_EQ(drop_off->verb, "DROP OFF");
    EXPECT_TRUE(drop_off->enabled);
    EXPECT_EQ(drop_off->reason, MenuDim::None);
    // The tilde row is drawn ABOVE the build list on every deployment, banked scrap or not.
    const std::vector<MenuRow> broke = menu_rows(deployed(0));
    ASSERT_FALSE(broke.empty());
    EXPECT_EQ(row_for(broke, "~")->verb, "DROP OFF") << "affordability reached the tilde row";
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
    EXPECT_EQ(rows.at(0).verb, "SELECT");
    EXPECT_TRUE(rows.at(0).enabled);
    EXPECT_EQ(rows.at(0).reason, MenuDim::None);
}

} // namespace
} // namespace protekt::game
