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
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

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

TEST(Menu, DrawnWithNoRowsItEmitsNothingAndEveryFrameIsByteIdentical) {
    const std::vector<MenuRow> empty;
    const std::vector<MenuRow> rows{row_of("1", "HOLD")};
    for (const bool roster : {false, true}) {
        SceneOptions base = menu_rig();
        base.show_roster = roster;
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
    const std::vector<MenuRow> bailout{row_of("9", "BAILOUT")};
    const std::vector<MenuRow> follow{row_of("1", "FOLLOW ME")};
    const std::vector<SceneVertex> one = menu_vertices(bailout);
    ASSERT_FALSE(one.empty()) << "a row must reach the frame";
    EXPECT_EQ(one.size() % 6U, 0U) << "a menu is quads: six vertices each";

    // The same row twice is twice the vertices: a row's quad count is its glyphs', nothing shared
    // and nothing collapsed.
    const std::vector<MenuRow> twice{bailout.front(), bailout.front()};
    EXPECT_EQ(menu_vertices(twice).size(), 2U * one.size());

    // DESCENDING keys, so a renderer that sorted by key would swap them. Row 0 is drawn first,
    // byte-for-byte where it is drawn alone; the row handed in LAST is drawn LOWEST.
    const std::vector<MenuRow> handed{bailout.front(), follow.front()};
    const std::vector<SceneVertex> drawn = menu_vertices(handed);
    const std::size_t second = menu_vertices(follow).size();
    ASSERT_EQ(drawn.size(), one.size() + second);
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

    // The harvester's real menu, gap and all: exactly the sum of its rows drawn one by one, so no
    // row is dropped, added or renumbered into the gap where 6, 7 and 8 would be.
    MenuInput harvester;
    harvester.subject = MenuSubject::Harvester;
    const std::vector<MenuRow> menu = menu_rows(harvester);
    std::size_t sum = 0;
    for (const MenuRow& row : menu) {
        sum += menu_vertices(std::vector<MenuRow>{row}).size();
    }
    EXPECT_EQ(menu_vertices(menu).size(), sum);

    // The state row naming nothing draws its key alone: some ink, and none past the key column.
    const std::vector<MenuRow> bare{row_of("SPACE", "")};
    const std::vector<SceneVertex> key_only = menu_vertices(bare);
    ASSERT_FALSE(key_only.empty()) << "SPACE with no verb must still draw SPACE";
    const float scale = static_cast<float>(options.extent.height) / 720.0F;
    // The key column: the 6 px margin plus SPACE's width, five glyphs of 6 cells less the trailing
    // blank, at the 2 px cell.
    const float key_right = (6.0F + (29.0F * 2.0F)) * scale;
    for (const SceneVertex& v : key_only) {
        EXPECT_LE(on_screen(options, v).x, key_right + 0.05F)
            << "a verb was drawn for an empty one";
    }
}

TEST(Menu, DrawnDownTheLeftMarginUnderTheStripAndTheNotice) {
    // Measured by projecting each vertex the menu added back to frame pixels -- the attack list's
    // own method in test_scene.cpp -- over the two longest real menus, with the strip on.
    SceneOptions options = menu_rig();
    options.show_roster = true;
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
        // The key column starts ON the strip's 6 px margin: the SPACE row's S has ink in its
        // leftmost column.
        EXPECT_NEAR(left, 6.0F * scale, 0.05F) << "subject " << static_cast<int>(subject);
    }
}

TEST(Menu, DrawnADimmedRowIsADimmerInkWhateverTheReason) {
    const std::vector<MenuRow> live{row_of("1", "HOLD")};
    const std::vector<MenuRow> moot{row_of("1", "HOLD", MenuDim::DoesNotApply)};
    const std::vector<MenuRow> broke{row_of("1", "HOLD", MenuDim::Unaffordable)};
    const std::vector<SceneVertex> lit = menu_vertices(live);
    const std::vector<SceneVertex> dim = menu_vertices(moot);
    const std::vector<SceneVertex> poor = menu_vertices(broke);
    ASSERT_FALSE(lit.empty());
    ASSERT_EQ(dim.size(), lit.size()) << "dimming changes the ink, never the glyphs";
    ASSERT_EQ(poor.size(), lit.size());
    constexpr std::array<float, 3> kLiveInk{0.95F, 0.95F, 0.95F};
    for (std::size_t i = 0; i < lit.size(); ++i) {
        ASSERT_EQ(dim.at(i).position, lit.at(i).position) << "vertex " << i;
        EXPECT_EQ(lit.at(i).colour, kLiveInk) << "a live row is the strip's white, vertex " << i;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            EXPECT_LT(dim.at(i).colour.at(channel), lit.at(i).colour.at(channel))
                << "the dimmed row is not darker in channel " << channel << ", vertex " << i;
        }
        // Two causes, ONE look: the reason is for the code, the ink is the same.
        EXPECT_EQ(poor.at(i).colour, dim.at(i).colour) << "vertex " << i;
    }
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

} // namespace
} // namespace protekt::game
