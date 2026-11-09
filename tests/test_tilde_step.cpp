// `~` PICKS AN INDIVIDUAL OUT OF A FUNCTION-KEY GROUP (Harri, 2026-11-03: "~ to select a unit after
// hitting a function key."). F1 holds three tanks; `~` opens the key's LIST (AV, 2026-11-08, which
// replaced the stepping this file first pinned: tests/test_unit_list.cpp owns the list itself) and
// the digit of a row makes that one the selection, so the next order is its alone. One test per
// rule, each isolating its own number, so a red names the rule that broke. `~` is selection state:
// nothing here steps the sim to see it, and the battle hashes are pinned by their own tests.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

[[nodiscard]] std::vector<UnitId> selected(const Interaction& player) {
    const auto units = player.selection().units();
    return {units.begin(), units.end()};
}

/// The player's tank he drives, then the three he commands -- all tanks, so the opening row seats
/// the three under F1 -- and, far from the other two, one in front of three enemy tanks (the
/// middle one, `b`), which is the one that dies when a test wants a corpse.
struct Rig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;
    UnitId a;
    UnitId b;
    UnitId c;
};

[[nodiscard]] Rig make_rig(bool with_enemies = false) {
    std::vector<Spawn> spawns{{UnitKind::Tank, Side::Player, 4, 8},
                              {UnitKind::Tank, Side::Player, 4, 4},
                              {UnitKind::Tank, Side::Player, 44, 41},
                              {UnitKind::Tank, Side::Player, 8, 4}};
    if (with_enemies) {
        spawns.push_back({UnitKind::Tank, Side::Enemy, 45, 41});
        spawns.push_back({UnitKind::Tank, Side::Enemy, 46, 42});
        spawns.push_back({UnitKind::Tank, Side::Enemy, 45, 40});
    }
    Rig rig{sim::Sim::create(sim::Scenario{kSeed, spawns}), {}, {}, {}, {}, {}, {}};
    rig.driven = rig.sim.units().at(0).id;
    rig.a = rig.sim.units().at(1).id;
    rig.b = rig.sim.units().at(2).id;
    rig.c = rig.sim.units().at(3).id;
    rig.row.seed_from_start(rig.sim, Side::Player, 0, rig.driven);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.driven_unit = rig.driven;
    rig.options.viewer_unit = rig.driven;
    rig.options.row = &rig.row;
    return rig;
}

void press(Interaction& player, Rig& rig, std::int32_t scancode) {
    player.handle(key_frame(scancode), rig.sim, rig.options);
}

/// `~`, then the digit of list row `row` (1-9): the individual the key's bars put there.
void pick(Interaction& player, Rig& rig, int row) {
    press(player, rig, input::kKeyGrave);
    press(player, rig, input::kKey1 + row - 1);
}

// ------------------------------------------------------------------ the rule

TEST(TildeStep, TheRigHoldsTheThreeTanksUnderF1) {
    const Rig rig = make_rig();
    EXPECT_EQ(rig.row.slots.at(group_slot(1)).units, (std::vector<UnitId>{rig.a, rig.b, rig.c}));
}

TEST(TildeStep, NarrowsTheNextOrderToExactlyOneUnit) {
    Rig rig = make_rig();
    Interaction player{rig.driven};
    press(player, rig, input::kKeyF1);
    ASSERT_EQ(player.selection().size(), 3U) << "F1 is the whole group";

    pick(player, rig, 1);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.a}));

    press(player, rig, input::kKey1); // 1 FOLLOW ME, no aim needed
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << player.message();
    EXPECT_EQ(orders.front().unit, rig.a) << "the order went to the individual, not the group";
}

TEST(TildeStep, TheListOpensAgainFromAnIndividualOfTheKey) {
    Rig rig = make_rig();
    Interaction player{rig.driven};
    press(player, rig, input::kKeyF1);
    pick(player, rig, 3);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.c}));
    pick(player, rig, 1);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.a}));
    pick(player, rig, 2);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.b}));
}

TEST(TildeStep, AGroupOfOneIsANoOpAndClearsNothing) {
    sim::Sim sim = sim::Sim::create(sim::Scenario{
        kSeed, {{UnitKind::Tank, Side::Player, 4, 8}, {UnitKind::Tank, Side::Player, 4, 4}}});
    RowState row;
    row.seed_from_start(sim, Side::Player, 0, sim.units().at(0).id);
    SceneOptions options;
    options.row = &row;
    options.seed = kSeed;
    options.extent = kExtent;
    Interaction player{sim.units().at(0).id};
    player.handle(key_frame(input::kKeyF1), sim, options);
    ASSERT_EQ(selected(player), (std::vector<UnitId>{sim.units().at(1).id}));

    player.handle(key_frame(input::kKeyGrave), sim, options);

    EXPECT_EQ(selected(player), (std::vector<UnitId>{sim.units().at(1).id})) << "still held";
    EXPECT_TRUE(player.message().empty());
    EXPECT_FALSE(player.tilde_steps());

    // The no-op left no stepping behind: an order uses the selection up and `~` is the nav menu's.
    player.handle(key_frame(input::kKey1), sim, options);
    ASSERT_EQ(player.take_orders().size(), 1U);
    EXPECT_FALSE(player.tilde_steps());
}

TEST(TildeStep, WithNoGroupItDoesNothingAndSaysNothing) {
    Rig rig = make_rig();
    Interaction player{rig.driven};
    press(player, rig, input::kKeyGrave);
    EXPECT_TRUE(player.selection().empty());
    EXPECT_TRUE(player.message().empty()) << "never a notice";
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.tilde_steps());
}

TEST(TildeStep, AfterTabTheGroupIsNoLongerInHand) {
    Rig rig = make_rig();
    Interaction player{rig.driven};
    press(player, rig, input::kKeyF1);
    press(player, rig, input::kKeyGrave);
    press(player, rig, input::kKeyTab);
    ASSERT_TRUE(player.selection().empty());

    press(player, rig, input::kKeyGrave);

    EXPECT_TRUE(player.selection().empty());
    EXPECT_TRUE(player.message().empty());
    EXPECT_FALSE(player.tilde_steps()) << "the nav menu has its key back";
}

TEST(TildeStep, TheGroupLastTouchedIsTheOneSteppedNotAnEarlierKey) {
    Rig rig = make_rig();
    rig.row.slots.at(group_slot(2)).units = {rig.c}; // another key; F1 is {a, b}
    rig.row.slots.at(group_slot(1)).units = {rig.a, rig.b};
    Interaction player{rig.driven};
    press(player, rig, input::kKeyF1);
    press(player, rig, input::kKeyF1 + 1); // F2 is the last key touched now
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.c}));
    press(player, rig, input::kKeyGrave);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.c})) << "F2 holds one: a no-op";
}

TEST(TildeStep, AfterTheOrderTheGroupAndTheIndividualAreIntact) {
    Rig rig = make_rig();
    Interaction player{rig.driven};
    press(player, rig, input::kKeyF1);
    pick(player, rig, 1); // a
    press(player, rig, input::kKey1);
    ASSERT_EQ(player.take_orders().size(), 1U);
    ASSERT_TRUE(player.selection().empty()) << "a finished order clears the selection, as ever";
    EXPECT_TRUE(player.tilde_steps()) << "and `~` is still the list's, not the nav menu's";

    pick(player, rig, 2);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.b}))
        << "the list still opens: the key is in hand";
    press(player, rig, input::kKey1);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, rig.b);
}

TEST(TildeStep, AWholeGroupOrderLeavesTheNavMenuItsKey) {
    Rig rig = make_rig();
    Interaction player{rig.driven};
    press(player, rig, input::kKeyF1);
    press(player, rig, input::kKey1); // the whole group follows; no `~` was pressed
    ASSERT_EQ(player.take_orders().size(), 3U);
    EXPECT_FALSE(player.tilde_steps());
}

TEST(TildeStep, ADeadIndividualIsSteppedOver) {
    Rig rig = make_rig(true);
    Interaction player{rig.driven};
    for (int tick = 0; tick < 8000 && rig.sim.find(rig.b)->alive; ++tick) {
        rig.sim.step();
    }
    ASSERT_FALSE(rig.sim.find(rig.b)->alive) << "the unit outlived the test, so nothing is proved";
    ASSERT_TRUE(rig.sim.find(rig.a)->alive);
    ASSERT_TRUE(rig.sim.find(rig.c)->alive);
    rig.row.update(rig.sim, Side::Player, 0, rig.driven);

    press(player, rig, input::kKeyF1);
    // The bars draw a and c now, so rows 1 and 2 are those two and there is no third.
    pick(player, rig, 2);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.c}));
    pick(player, rig, 1);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.a}));
    pick(player, rig, 3);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.a})) << "no third row: nothing moved";
}

TEST(TildeStep, TheIndividualThatDiesUnderTheCursorIsLeftForTheNextOne) {
    Rig rig = make_rig(true);
    Interaction player{rig.driven};
    press(player, rig, input::kKeyF1);
    pick(player, rig, 2); // b, the one about to die
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.b}));
    for (int tick = 0; tick < 8000 && rig.sim.find(rig.b)->alive; ++tick) {
        rig.sim.step();
    }
    ASSERT_FALSE(rig.sim.find(rig.b)->alive);
    ASSERT_TRUE(rig.sim.find(rig.c)->alive);
    rig.row.update(rig.sim, Side::Player, 0, rig.driven);

    pick(player, rig, 2); // the bars draw a and c now: row 2 is c

    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.c}));
}

// ------------------------------------------------------------------ what the row shows

TEST(TildeStep, TheRowPicksOutTheTickOfTheIndividualTheGameHolds) {
    Rig rig = make_rig();
    Interaction player{rig.driven};
    const auto picked = [&] {
        const std::optional<SlotReading> reading
            = read_slot(rig.sim, rig.row.slots.at(group_slot(1)));
        return picked_ticks(*reading, player.selection().units());
    };
    press(player, rig, input::kKeyF1);
    EXPECT_TRUE(picked().empty()) << "the whole group is the key's lit frame, not a tick";
    for (std::size_t want = 0; want < 3; ++want) {
        pick(player, rig, static_cast<int>(want) + 1);
        EXPECT_EQ(picked(), (std::vector<std::size_t>{want}));
    }
}

TEST(TildeStep, TheSceneRingsThePickedTickAndNothingWhenTheWholeGroupIsHeld) {
    Rig rig = make_rig();
    const std::vector<UnitId> whole{rig.a, rig.b, rig.c};
    const std::vector<UnitId> one{rig.b};
    const std::size_t none = build_scene(rig.sim, rig.options, {}).vertices.size();
    const std::size_t all = build_scene(rig.sim, rig.options, whole).vertices.size();
    const std::size_t picked = build_scene(rig.sim, rig.options, one).vertices.size();
    EXPECT_EQ(all, none) << "a whole group adds no ring";
    EXPECT_GT(picked, all) << "one vehicle's tick is ringed";
}

// THE ROW'S OWN KEY, THROUGH THE WHOLE KEY CHAIN. Harri selected the three scavengers under F2, the
// menu opened, and `~` did nothing: the app hands every key to the nav menu first, and it kept the
// `~` for itself. The other tests here call `Interaction` directly and so never met it.
TEST(TildeStep, ThreeScavengersUnderAFunctionKeyAreListedThroughTheNavMenuToo) {
    sim::Sim sim = sim::Sim::create(sim::Scenario{kSeed,
                                                  {{UnitKind::Tank, Side::Player, 4, 8},
                                                   {UnitKind::Scavenger, Side::Player, 4, 4},
                                                   {UnitKind::Scavenger, Side::Player, 8, 4},
                                                   {UnitKind::Scavenger, Side::Player, 12, 4}}});
    const UnitId driven = sim.units().at(0).id;
    RowState row;
    row.seed_from_start(sim, Side::Player, 0, driven);
    SceneOptions options;
    options.row = &row;
    options.seed = kSeed;
    options.extent = kExtent;
    options.driven_unit = driven;
    options.viewer_unit = driven;
    Interaction player{driven};
    NavMenu nav;
    NavMarks marks;
    // A mark exists, so the nav menu WOULD open on a `~` it was allowed to take.
    static_cast<void>(marks.place(5, 5));
    const auto send = [&](std::int32_t scancode) {
        InputFrame hands = key_frame(scancode);
        InputFrame rest = hands;
        // `top_menu` is false whenever a selection's own menu is open, as in the app.
        rest.keys = nav.handle(hands.keys, marks, NavAim{}, player.selection().empty());
        player.handle(rest, sim, options);
    };
    send(input::kKeyF1);
    ASSERT_EQ(player.selection().size(), 3U) << "F1 holds the three scavengers";

    send(input::kKeyGrave);
    EXPECT_TRUE(player.unit_list_armed()) << "`~` reached the session and opened the list";
    send(input::kKey1 + 1); // row 2

    EXPECT_EQ(player.selection().size(), 1U) << "the list narrowed the group to one";
    EXPECT_EQ(player.selection().units().front(), sim.units().at(2).id);
}

} // namespace
} // namespace protekt::game
