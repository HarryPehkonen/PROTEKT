// ROW-MOVE: shift + a function key MOVES the selected vehicles into that key. The target keeps its
// units and gains the moved ones; a source emptied goes blank where it stands; nothing else in the
// row moves. A target holding a DIFFERENT kind is refused (the ruler's explicit ruling), and so is
// a selection of more than one kind, because a key holds one. Every refusal changes nothing.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/row.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::KeyEvent;
using sim::Side;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

struct Rig {
    sim::Sim sim;
    RowState row;
    UnitId tank0{};  ///< F1
    UnitId tank1{};  ///< F2
    UnitId tank2{};  ///< F3
    UnitId mortar{}; ///< F4
    UnitId turret{}; ///< F5
    UnitId enemy{};  ///< held by no key
};

[[nodiscard]] std::vector<Spawn> rig_spawns() {
    return {{UnitKind::Tank, Side::Player, 10, 10},   {UnitKind::Tank, Side::Player, 14, 10},
            {UnitKind::Tank, Side::Player, 18, 10},   {UnitKind::Mortar, Side::Player, 22, 10},
            {UnitKind::Turret, Side::Player, 26, 10}, {UnitKind::Tank, Side::Enemy, 90, 90}};
}

/// Five own vehicles of three kinds, one per key F1..F5 in spawn order, and a far-off enemy.
[[nodiscard]] Rig make_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed, rig_spawns()});
    const auto& units = rig.sim.units();
    rig.tank0 = units.at(0).id;
    rig.tank1 = units.at(1).id;
    rig.tank2 = units.at(2).id;
    rig.mortar = units.at(3).id;
    rig.turret = units.at(4).id;
    rig.enemy = units.at(5).id;
    rig.row.update(rig.sim, Side::Player, 0);
    return rig;
}

[[nodiscard]] std::vector<UnitId> group_of(const RowState& row, int n) {
    return row.slots.at(group_slot(n)).units;
}

[[nodiscard]] std::vector<UnitId> ids(std::initializer_list<UnitId> list) { return list; }

[[nodiscard]] RowMove move(Rig& rig, std::vector<UnitId> selection, int key) {
    return rig.row.move_into(rig.sim, selection, key);
}

TEST(RowMove, MovesTheSelectedUnitsIntoAnEmptyKey) {
    Rig rig = make_rig();
    ASSERT_FALSE(rig.row.occupied(group_slot(8)));

    EXPECT_EQ(move(rig, {rig.tank0}, 8), RowMove::Moved);

    EXPECT_EQ(group_of(rig.row, 8), ids({rig.tank0}));
    EXPECT_FALSE(rig.row.occupied(group_slot(1))) << "the source key goes blank";
}

TEST(RowMove, TargetKeepsItsOwnUnitsAndGainsTheMovedOnes) {
    Rig rig = make_rig();
    ASSERT_EQ(group_of(rig.row, 1), ids({rig.tank0}));

    EXPECT_EQ(move(rig, {rig.tank1, rig.tank2}, 1), RowMove::Moved);

    EXPECT_EQ(group_of(rig.row, 1), ids({rig.tank0, rig.tank1, rig.tank2}));
    EXPECT_FALSE(rig.row.occupied(group_slot(2)));
    EXPECT_FALSE(rig.row.occupied(group_slot(3)));
}

TEST(RowMove, AppendsAfterTheTargetsOwnUnits) {
    // The anchor: tank0 has the LOWEST id, yet it lands after the unit the target already held.
    Rig rig = make_rig();
    ASSERT_EQ(group_of(rig.row, 3), ids({rig.tank2}));

    EXPECT_EQ(move(rig, {rig.tank0}, 3), RowMove::Moved);

    EXPECT_EQ(group_of(rig.row, 3).front(), rig.tank2) << "the target's own unit is still first";
    EXPECT_EQ(group_of(rig.row, 3), ids({rig.tank2, rig.tank0}));
}

TEST(RowMove, NothingElseInTheRowMoves) {
    Rig rig = make_rig();
    rig.row.nav_marks = 3;
    const RowState before = rig.row;

    ASSERT_EQ(move(rig, {rig.tank1}, 8), RowMove::Moved);

    for (std::size_t i = 0; i < kRowSlots; ++i) {
        if (i == group_slot(2) || i == group_slot(8)) {
            continue; // the source and the target are the only two that may change
        }
        EXPECT_EQ(rig.row.slots.at(i), before.slots.at(i)) << row_key_label(i);
    }
    EXPECT_EQ(rig.row.nav_marks, before.nav_marks);
    EXPECT_NE(rig.row.slots.at(group_slot(2)), before.slots.at(group_slot(2)))
        << "the source did change, so the loop above was comparing something";
}

TEST(RowMove, RefusesADifferentKindAndChangesNothing) {
    Rig rig = make_rig();
    const RowState before = rig.row;

    EXPECT_EQ(move(rig, {rig.mortar}, 1), RowMove::DifferentKind) << "F1 holds a tank";

    EXPECT_EQ(rig.row.slots, before.slots);
}

TEST(RowMove, RefusesAMixedKindSelection) {
    Rig rig = make_rig();
    const RowState before = rig.row;

    EXPECT_EQ(move(rig, {rig.tank0, rig.mortar}, 8), RowMove::MixedKinds)
        << "F8 is empty, yet two kinds must not make a two-kind key";

    EXPECT_EQ(rig.row.slots, before.slots);
}

TEST(RowMove, RefusesAnEmptySelection) {
    Rig rig = make_rig();
    const RowState before = rig.row;

    EXPECT_EQ(move(rig, {}, 8), RowMove::NothingToMove);

    EXPECT_EQ(rig.row.slots, before.slots);
}

/// Three own turrets: the first (44,41) in front of three enemy tanks, which will kill it; two far
/// off in the corner, which stay alive. They hold F1, F2, F3.
[[nodiscard]] Rig make_rig_with_a_death() {
    Rig rig{};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed,
                                             {{UnitKind::Turret, Side::Player, 44, 41},
                                              {UnitKind::Turret, Side::Player, 4, 4},
                                              {UnitKind::Turret, Side::Player, 8, 4},
                                              {UnitKind::Tank, Side::Enemy, 45, 41},
                                              {UnitKind::Tank, Side::Enemy, 46, 42},
                                              {UnitKind::Tank, Side::Enemy, 45, 40}}});
    rig.tank0 = rig.sim.units().at(0).id; // the doomed turret
    rig.tank1 = rig.sim.units().at(1).id;
    rig.tank2 = rig.sim.units().at(2).id;
    rig.row.update(rig.sim, Side::Player, 0);
    // Deliberately NOT updated again: the row still holds the corpse, as a frame drawn between the
    // death and the next update would.
    for (int tick = 0; tick < 8000 && rig.sim.find(rig.tank0)->alive; ++tick) {
        rig.sim.step();
    }
    return rig;
}

TEST(RowMove, RefusesWhenEverySelectedUnitIsDead) {
    Rig rig = make_rig_with_a_death();
    ASSERT_FALSE(rig.sim.find(rig.tank0)->alive) << "the unit outlived the test";
    const RowState before = rig.row;

    EXPECT_EQ(move(rig, {rig.tank0}, 8), RowMove::NothingToMove);

    EXPECT_EQ(rig.row.slots, before.slots);
}

TEST(RowMove, SkipsDeadUnitsAndMovesTheLivingOnes) {
    Rig rig = make_rig_with_a_death();
    ASSERT_FALSE(rig.sim.find(rig.tank0)->alive) << "the unit outlived the test";
    ASSERT_TRUE(rig.sim.find(rig.tank1)->alive);
    ASSERT_EQ(group_of(rig.row, 1), ids({rig.tank0})) << "anchor: the corpse IS in its slot";

    EXPECT_EQ(move(rig, {rig.tank0, rig.tank1}, 8), RowMove::Moved);

    EXPECT_EQ(group_of(rig.row, 8), ids({rig.tank1})) << "no corpse is carried into the new key";
    EXPECT_FALSE(rig.row.occupied(group_slot(2)));
}

TEST(RowMove, ASelectionSpanningTwoKeysIsRemovedFromBoth) {
    Rig rig = make_rig();

    EXPECT_EQ(move(rig, {rig.tank0, rig.tank1}, 8), RowMove::Moved);

    EXPECT_FALSE(rig.row.occupied(group_slot(1)));
    EXPECT_FALSE(rig.row.occupied(group_slot(2)));
    EXPECT_EQ(group_of(rig.row, 8), ids({rig.tank0, rig.tank1}));
}

TEST(RowMove, SurvivesUpdate) {
    Rig rig = make_rig();
    ASSERT_EQ(move(rig, {rig.tank0}, 8), RowMove::Moved);
    const RowState moved = rig.row;

    rig.row.update(rig.sim, Side::Player, 0);
    EXPECT_EQ(rig.row.slots, moved.slots) << "an update must not undo or re-shuffle the move";

    // A unit seen for the first time fills the gap the move left: the FIRST EMPTY group slot.
    std::vector<Spawn> more = rig_spawns();
    more.push_back({UnitKind::Mortar, Side::Player, 30, 10});
    const sim::Sim later = sim::Sim::create(sim::Scenario{kSeed, more});
    rig.row.update(later, Side::Player, 0);

    EXPECT_EQ(group_of(rig.row, 1), ids({later.units().back().id})) << "the gap is F1";
    EXPECT_EQ(group_of(rig.row, 8), ids({rig.tank0})) << "and the moved key stays where it is";
}

TEST(RowMove, AKeyBeyondF10DoesNothing) {
    Rig rig = make_rig();
    const RowState before = rig.row;

    for (const int key : {-1, 0, 11, 14}) {
        EXPECT_EQ(move(rig, {rig.tank0}, key), RowMove::NoSuchKey) << key;
    }
    EXPECT_EQ(rig.row.slots, before.slots);

    // And the key that would ask for one: F11, and keys that are not F keys at all.
    EXPECT_EQ(input::shift_function_key(KeyEvent{input::kKeyF1 + 10, true, true}), 0);
    EXPECT_EQ(input::shift_function_key(KeyEvent{input::kKeyEscape, true, true}), 0);
    EXPECT_EQ(input::shift_function_key(KeyEvent{input::kKeyF1 - 1, true, true}), 0);
}

TEST(RowMove, MovingIntoTheKeyTheyAreAlreadyInChangesNothing) {
    // Remove-then-append would send the lower id behind the higher one.
    Rig rig = make_rig();
    ASSERT_EQ(move(rig, {rig.tank1}, 1), RowMove::Moved);
    ASSERT_EQ(group_of(rig.row, 1), ids({rig.tank0, rig.tank1}));
    const RowState before = rig.row;

    EXPECT_EQ(move(rig, {rig.tank0}, 1), RowMove::Moved);

    EXPECT_EQ(rig.row.slots, before.slots);
}

TEST(RowMove, OnlyUnitsTheRowHoldsInAGroupMove) {
    Rig rig = make_rig();
    const RowState before = rig.row;

    EXPECT_EQ(move(rig, {rig.enemy}, 8), RowMove::NothingToMove) << "an enemy is not the row's";
    EXPECT_EQ(move(rig, {UnitId{}}, 8), RowMove::NothingToMove) << "nor is no unit at all";
    EXPECT_EQ(rig.row.slots, before.slots);
}

TEST(RowMove, EachRefusalHasItsOwnMessageAndMovedHasNone) {
    EXPECT_TRUE(move_refusal_message(RowMove::Moved).empty());
    EXPECT_FALSE(move_refusal_message(RowMove::NothingToMove).empty());
    EXPECT_FALSE(move_refusal_message(RowMove::MixedKinds).empty());
    EXPECT_FALSE(move_refusal_message(RowMove::DifferentKind).empty());
    EXPECT_NE(move_refusal_message(RowMove::DifferentKind),
              move_refusal_message(RowMove::MixedKinds));
    EXPECT_NE(move_refusal_message(RowMove::DifferentKind), empty_key_message(1));
}

} // namespace
} // namespace protekt::game
