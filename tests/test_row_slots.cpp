// ROW-SLOTS: the command row -- `~`, `1`, `2`, `3`, `F1`..`F10`. Fourteen fixed positions; a key
// that holds nothing is not drawn and the row does not close up. The F keys hold GROUPS OF
// VEHICLES (not kinds): a new unit takes the first empty group slot, a group that dies leaves its
// slot empty where it was, and the digit row selects nothing.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
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

/// The F-key `n` (1-10) as the window reports it: USB HID 58 is F1. After K2 the pool's keys are
/// the function row; the digit row answers marks and attack-list rows and selects nothing.
[[nodiscard]] InputFrame group_key(int n) { return key_frame(input::kKeyF1 + n - 1); }

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    std::vector<UnitId> own; ///< the player's units, in spawn order
    RowState row;            ///< the command row over `sim`, as the app would hold it
};

[[nodiscard]] std::vector<UnitId> own_ids(const sim::Sim& sim) {
    std::vector<UnitId> ids;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            ids.push_back(unit.id);
        }
    }
    return ids;
}

/// Camera at tile (40,30) looking down +Z. Own units, in spawn order: tank at (40,40), tank at
/// (44,41), a tank at (40,2) far BEHIND the camera, a mortar at (60,41), a turret at (36,41) --
/// so F1..F5 each hold one of them. One enemy tank, in front.
[[nodiscard]] Rig make_rig(std::vector<Spawn> extra = {}) {
    Rig rig{};
    std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 40, 40},   {UnitKind::Tank, Side::Player, 44, 41},
        {UnitKind::Tank, Side::Player, 40, 2},    {UnitKind::Mortar, Side::Player, 60, 41},
        {UnitKind::Turret, Side::Player, 36, 41}, {UnitKind::Tank, Side::Enemy, 38, 60}};
    spawns.insert(spawns.end(), extra.begin(), extra.end());
    rig.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    rig.own = own_ids(rig.sim);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.viewer_unit = rig.own.at(0);
    rig.options.aim = Aim{640.0F, 360.0F};
    rig.row.update(rig.sim, Side::Player, 0);
    rig.options.row = &rig.row;
    return rig;
}

/// Whether ANY pixel of the frame, on a grid, picks `id`: the mouse could find it from here.
[[nodiscard]] bool mouse_could_find(const Rig& rig, UnitId id) {
    for (float y = 0.0F; y < 720.0F; y += 6.0F) {
        for (float x = 0.0F; x < 1280.0F; x += 6.0F) {
            if (pick(rig.sim, rig.options, x, y).unit == id) {
                return true;
            }
        }
    }
    return false;
}

void aim_at_unit(Rig& rig, UnitId unit) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
}

[[nodiscard]] std::vector<UnitId> selected(const Interaction& player) {
    const auto units = player.selection().units();
    return {units.begin(), units.end()};
}

// ------------------------------------------------------------------ the slot model

/// The units of F`n`, or empty.
[[nodiscard]] std::vector<UnitId> group_of(const RowState& row, int n) {
    return row.slots.at(group_slot(n)).units;
}

/// A layout where the MIDDLE group dies: a tank far off in the corner (F1), a turret at (44,41)
/// in front of three enemy tanks (F2), a mortar far off (F3). With `late`, one more tank
/// (spawned last, so with the highest id) is in the sim too: the unit that arrives after the death.
[[nodiscard]] std::vector<Spawn> middle_dies(bool late) {
    std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 4, 4},   {UnitKind::Turret, Side::Player, 44, 41},
        {UnitKind::Mortar, Side::Player, 8, 4}, {UnitKind::Tank, Side::Enemy, 45, 41},
        {UnitKind::Tank, Side::Enemy, 46, 42},  {UnitKind::Tank, Side::Enemy, 45, 40}};
    if (late) {
        spawns.push_back({UnitKind::Tank, Side::Player, 12, 4});
    }
    return spawns;
}

/// Step until `id` is dead (or give up, and say so).
void step_until_dead(sim::Sim& sim, UnitId id) {
    for (int tick = 0; tick < 8000 && sim.find(id)->alive; ++tick) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(id)->alive) << "the unit outlived the test, so nothing is proved";
}

TEST(RowSlots, EveryVehicleIsItsOwnGroupInSpawnOrderFromF1) {
    const Rig rig = make_rig();
    for (std::size_t i = 0; i < rig.own.size(); ++i) {
        EXPECT_EQ(rig.row.slots.at(kRowFirstGroup + i).units, (std::vector<UnitId>{rig.own.at(i)}))
            << "F" << i + 1;
    }
    for (std::size_t i = rig.own.size(); i < kRowGroups; ++i) {
        EXPECT_FALSE(rig.row.occupied(kRowFirstGroup + i)) << "F" << i + 1;
    }
}

TEST(RowSlots, TheFixedPositionsAreTheRulersOrder) {
    EXPECT_EQ(kRowSlots, 14U);
    EXPECT_EQ(group_slot(1), 4U) << "F1 is slot 4";
    EXPECT_EQ(group_slot(10), 13U) << "F10 is slot 13";
    const std::vector<std::string_view> labels{"~",  "1",  "2",  "3",  "F1", "F2", "F3",
                                               "F4", "F5", "F6", "F7", "F8", "F9", "F10"};
    for (std::size_t i = 0; i < kRowSlots; ++i) {
        EXPECT_EQ(row_key_label(i), labels.at(i));
    }
    EXPECT_EQ(row_slot_kind(0), RowSlotKind::Navs);
    EXPECT_EQ(row_slot_kind(1), RowSlotKind::RecyclerKey);
    EXPECT_EQ(row_slot_kind(2), RowSlotKind::FactoryKey);
    EXPECT_EQ(row_slot_kind(3), RowSlotKind::ArmoryKey);
    for (std::size_t i = 4; i < kRowSlots; ++i) {
        EXPECT_EQ(row_slot_kind(i), RowSlotKind::Group);
    }
}

TEST(RowSlots, ANewUnitTakesTheLowestEmptySlotAndASecondOneTheNext) {
    // The same scenario with units appended: the ids already in it are unchanged, so the second sim
    // is "the first one, later, with new arrivals".
    const std::vector<Spawn> base{{UnitKind::Tank, Side::Player, 10, 10},
                                  {UnitKind::Turret, Side::Player, 12, 10}};
    std::vector<Spawn> one_more = base;
    one_more.push_back({UnitKind::Mortar, Side::Player, 14, 10});
    std::vector<Spawn> two_more = one_more;
    two_more.push_back({UnitKind::Rocket, Side::Player, 16, 10});

    RowState row;
    row.update(sim::Sim::create(sim::Scenario{kSeed, base}), Side::Player, 0);
    EXPECT_TRUE(row.occupied(group_slot(2)));
    EXPECT_FALSE(row.occupied(group_slot(3)));

    const sim::Sim with_one = sim::Sim::create(sim::Scenario{kSeed, one_more});
    row.update(with_one, Side::Player, 0);
    EXPECT_EQ(group_of(row, 3), (std::vector<UnitId>{with_one.units().at(2).id}));
    EXPECT_FALSE(row.occupied(group_slot(4)));

    const sim::Sim with_two = sim::Sim::create(sim::Scenario{kSeed, two_more});
    row.update(with_two, Side::Player, 0);
    EXPECT_EQ(group_of(row, 3), (std::vector<UnitId>{with_two.units().at(2).id}))
        << "a unit keeps the slot it was given";
    EXPECT_EQ(group_of(row, 4), (std::vector<UnitId>{with_two.units().at(3).id}));
}

TEST(RowSlots, AGroupThatDiesLeavesItsSlotEmptyAndNothingMoves) {
    sim::Sim sim = sim::Sim::create(sim::Scenario{kSeed, middle_dies(false)});
    RowState row;
    row.update(sim, Side::Player, 0);
    const UnitId first = sim.units().at(0).id;
    const UnitId middle = sim.units().at(1).id;
    const UnitId last = sim.units().at(2).id;
    ASSERT_EQ(group_of(row, 2), (std::vector<UnitId>{middle}));

    step_until_dead(sim, middle);
    row.update(sim, Side::Player, 0);
    EXPECT_FALSE(row.occupied(group_slot(2))) << "the dead group's key is empty";
    EXPECT_EQ(group_of(row, 1), (std::vector<UnitId>{first}));
    EXPECT_EQ(group_of(row, 3), (std::vector<UnitId>{last})) << "F3 did not close up into F2";
    EXPECT_FALSE(row.occupied(group_slot(4)));

    // And pressing the dead key says so, while the live ones still select.
    SceneOptions options;
    options.row = &row;
    Interaction player{first};
    player.handle(group_key(2), sim, options);
    EXPECT_TRUE(player.selection().empty());
    EXPECT_EQ(player.message(), "You have no group F2.");
    player.handle(group_key(3), sim, options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{last}));
}

TEST(RowSlots, TheNextNewUnitFillsTheMiddleGapBeforeAnythingAfterIt) {
    sim::Sim sim = sim::Sim::create(sim::Scenario{kSeed, middle_dies(true)});
    // The row has seen the sim before the late tank existed: it is the same sim without it.
    RowState row;
    row.update(sim::Sim::create(sim::Scenario{kSeed, middle_dies(false)}), Side::Player, 0);
    const UnitId first = sim.units().at(0).id;
    const UnitId middle = sim.units().at(1).id;
    const UnitId last = sim.units().at(2).id;
    const UnitId late = sim.units().at(6).id;
    ASSERT_EQ(sim.find(late)->side, Side::Player);
    ASSERT_EQ(group_of(row, 2), (std::vector<UnitId>{middle}));

    step_until_dead(sim, middle);
    row.update(sim, Side::Player, 0);
    EXPECT_EQ(group_of(row, 1), (std::vector<UnitId>{first}));
    EXPECT_EQ(group_of(row, 2), (std::vector<UnitId>{late})) << "the gap is the first free slot";
    EXPECT_EQ(group_of(row, 3), (std::vector<UnitId>{last}));
    EXPECT_FALSE(row.occupied(group_slot(4)));
}

TEST(RowSlots, AnUnseenUnitWithEveryGroupSlotFullIsLeftOutNotLost) {
    std::vector<Spawn> spawns;
    spawns.reserve(11); // eleven spawning units, one per slot: what the loop below makes
    for (int i = 0; i < 11; ++i) {
        spawns.push_back({UnitKind::Tank, Side::Player, 10 + (3 * i), 10});
    }
    const sim::Sim sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    RowState row;
    row.update(sim, Side::Player, 0);
    for (int n = 1; n <= kRowGroups; ++n) {
        EXPECT_EQ(group_of(row, n), (std::vector<UnitId>{sim.units().at(n - 1).id})) << "F" << n;
    }
    std::size_t held = 0;
    for (const RowSlot& slot : row.slots) {
        held += slot.units.size();
    }
    EXPECT_EQ(held, 10U) << "the eleventh tank has no key to sit under yet";
}

TEST(RowSlots, UpdatingTwiceChangesNothing) {
    Rig rig = make_rig();
    const RowState once = rig.row;
    rig.row.update(rig.sim, Side::Player, 0);
    for (std::size_t i = 0; i < kRowSlots; ++i) {
        EXPECT_EQ(rig.row.slots.at(i).units, once.slots.at(i).units) << "slot " << i;
    }
}

TEST(RowSlots, OnlyTheSidesOwnUnitsAreHeld) {
    Rig rig = make_rig();
    for (const RowSlot& slot : rig.row.slots) {
        for (const UnitId id : slot.units) {
            EXPECT_EQ(rig.sim.find(id)->side, Side::Player);
        }
    }
    RowState theirs;
    theirs.update(rig.sim, Side::Enemy, 0);
    ASSERT_TRUE(theirs.occupied(group_slot(1)));
    EXPECT_EQ(rig.sim.find(group_of(theirs, 1).front())->side, Side::Enemy);
    EXPECT_FALSE(theirs.occupied(group_slot(2)));
}

TEST(RowSlots, TheFactoryIsOnKeyTwoAndNeverUnderAnFKey) {
    Rig rig = make_rig({{UnitKind::Factory, Side::Player, 30, 30}});
    const UnitId factory = rig.own.back();
    ASSERT_EQ(rig.sim.find(factory)->kind, UnitKind::Factory);
    EXPECT_EQ(rig.row.slots.at(2).units, (std::vector<UnitId>{factory}));
    EXPECT_TRUE(rig.row.occupied(2));
    for (std::size_t i = kRowFirstGroup; i < kRowSlots; ++i) {
        for (const UnitId id : rig.row.slots.at(i).units) {
            EXPECT_NE(id, factory) << "a building under F" << i - kRowFirstGroup + 1;
        }
    }
    EXPECT_FALSE(rig.row.occupied(group_slot(6))) << "the five vehicles still fill F1..F5 only";
}

TEST(RowSlots, TheBuildingKeysAppearOnlyWhenTheThingExists) {
    const Rig rig = make_rig(); // no factory in this rig
    EXPECT_FALSE(rig.row.occupied(2)) << "no factory, no key 2";
    EXPECT_FALSE(rig.row.occupied(1)) << "the deployed recycler has no kind yet";
    EXPECT_FALSE(rig.row.occupied(3)) << "the armory has no kind yet";
}

TEST(RowSlots, TheRecyclerStaysAVehicleInTheFRowUntilDeployExists) {
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    RowState row;
    row.update(sim, Side::Player, 0);
    EXPECT_FALSE(row.occupied(1)) << "key 1 is for the DEPLOYED recycler";
    ASSERT_TRUE(row.occupied(2)) << "the skirmish has a factory";
    EXPECT_EQ(sim.find(row.slots.at(2).units.front())->kind, UnitKind::Factory);
    const std::optional<SlotReading> last = read_slot(sim, row.slots.at(group_slot(9)));
    ASSERT_TRUE(last.has_value()) << "F9 holds the recycler";
    if (!last.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    EXPECT_EQ(last->kind, UnitKind::Recycler);
    EXPECT_FALSE(row.occupied(group_slot(10)));
}

TEST(RowSlots, TheNavKeyIsOnTheScreenExactlyWhenThereIsAMark) {
    Rig rig = make_rig();
    EXPECT_FALSE(rig.row.occupied(0));
    rig.row.update(rig.sim, Side::Player, 1);
    EXPECT_TRUE(rig.row.occupied(0));
    EXPECT_EQ(rig.row.nav_marks, 1U);
    rig.row.update(rig.sim, Side::Player, 0);
    EXPECT_FALSE(rig.row.occupied(0)) << "the last mark cleared";
}

TEST(RowSlots, AReadingCountsTheLivingUnitsAndSumsTheirHitPoints) {
    Rig rig = make_rig();
    // A batch of the first two tanks under F1 (arranged by hand: nothing builds a batch yet).
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1)};
    const std::optional<SlotReading> reading = read_slot(rig.sim, rig.row.slots.at(group_slot(1)));
    ASSERT_TRUE(reading.has_value()) << "F1 holds the batch";
    if (!reading.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    EXPECT_EQ(reading->kind, UnitKind::Tank);
    EXPECT_EQ(reading->count, 2U);
    EXPECT_EQ(reading->max_hp, 2 * sim::profile(UnitKind::Tank).hp);
    EXPECT_EQ(reading->hp, rig.sim.find(rig.own.at(0))->hp + rig.sim.find(rig.own.at(1))->hp);
    EXPECT_FALSE(read_slot(rig.sim, RowSlot{}).has_value()) << "an empty slot reads as nothing";
}

TEST(RowSlots, TheGroupsHitPointsFollowDamage) {
    // Two enemy tanks beside the second own tank: it takes damage before anything else does.
    Rig rig
        = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41}, {UnitKind::Tank, Side::Enemy, 47, 42}});
    const UnitId hurt = rig.own.at(1);
    const std::optional<SlotReading> first = read_slot(rig.sim, rig.row.slots.at(group_slot(2)));
    ASSERT_TRUE(first.has_value());
    if (!first.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    bool lower = false;
    for (int tick = 0; tick < 600 && !lower; ++tick) {
        rig.sim.step();
        const std::optional<SlotReading> now = read_slot(rig.sim, rig.row.slots.at(group_slot(2)));
        if (now.has_value() && now->hp < first->hp) {
            lower = true;
            EXPECT_EQ(now->hp, rig.sim.find(hurt)->hp);
            EXPECT_EQ(now->max_hp, first->max_hp) << "the ceiling does not move with damage";
        }
    }
    EXPECT_TRUE(lower) << "nothing was hurt, so nothing is proved";
}

TEST(RowSlots, ADeadUnitDoesNotReadEvenBeforeTheRowIsUpdated) {
    sim::Sim sim = sim::Sim::create(sim::Scenario{kSeed, middle_dies(false)});
    RowState row;
    row.update(sim, Side::Player, 0);
    step_until_dead(sim, sim.units().at(1).id);
    EXPECT_FALSE(read_slot(sim, row.slots.at(group_slot(2))).has_value())
        << "a stale row must not show a dead key";
}

TEST(RowSlots, TheLabelsAreTheShortNames) {
    EXPECT_EQ(kind_label(UnitKind::Tank), "TANK");
    EXPECT_EQ(kind_label(UnitKind::HoverTank), "HOVER");
    EXPECT_EQ(kind_label(UnitKind::Mortar), "MORTAR");
    EXPECT_EQ(kind_label(UnitKind::Rocket), "ROCKET");
    EXPECT_EQ(kind_label(UnitKind::Turret), "TURRET");
    EXPECT_EQ(kind_label(UnitKind::Factory), "FACTORY");
    EXPECT_EQ(kind_label(UnitKind::Recycler), "RECYCLER");
}

// ------------------------------------------------------------------ the keys

TEST(RowKeys, F1SelectsItsGroupFromWhereItCannotBeSeen) {
    Rig rig = make_rig();
    const UnitId behind = rig.own.at(2);
    ASSERT_FALSE(mouse_could_find(rig, behind))
        << "the third tank is behind the camera: the mouse must not be able to pick it";
    Interaction player{rig.own.at(0)};
    player.handle(group_key(3), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{behind}));
    EXPECT_TRUE(player.message().empty());
}

TEST(RowKeys, AGroupOfSeveralSelectsAllOfThem) {
    Rig rig = make_rig();
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1), rig.own.at(2)};
    Interaction player{rig.own.at(0)};
    player.handle(group_key(1), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0), rig.own.at(1), rig.own.at(2)}));
}

TEST(RowKeys, TheDigitRowSelectsNothing) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    player.handle(key_frame(input::kKey1), rig.sim, rig.options);
    EXPECT_TRUE(selected(player).empty()) << "a digit is a mark key, never a group key";
    EXPECT_TRUE(player.message().empty()) << "a digit that selects nothing has nothing to say";
}

TEST(RowKeys, SpaceSelectsTheOneUnitThePlayerIsLookingAt) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    aim_at_unit(rig, rig.own.at(3)); // the mortar alone: no group key needed, just the crosshair
    player.handle(key_frame(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(3)}));
}

TEST(RowKeys, TheSelectedGroupObeysFollowWithNoNewPlumbing) {
    Rig rig = make_rig();
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1), rig.own.at(2)};
    Interaction player{rig.own.at(0)};
    player.handle(group_key(1), rig.sim, rig.options); // three tanks
    aim_at_unit(rig, rig.own.at(3));                   // the mortar, which is NOT in the group
    player.handle(key_frame(input::kKeyF), rig.sim, rig.options);
    // Two orders, not three: the tank the player is SITTING IN is not sent anywhere, and it is
    // own.at(0). So the group order reaches the rest of the group and leaves the driven one alone.
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 2U) << "one order per unit of the group, less the one being driven";
    for (const sim::Order& order : orders) {
        EXPECT_TRUE(order.unit == rig.own.at(1) || order.unit == rig.own.at(2));
        const auto* follow = std::get_if<sim::orders::Follow>(&order.payload);
        ASSERT_NE(follow, nullptr);
        EXPECT_EQ(follow->target, rig.own.at(3));
    }
}

TEST(RowKeys, EveryKeySelectsItsOwnSlotAndReplacesTheLast) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    player.handle(group_key(1), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0)}));
    player.handle(group_key(4), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(3)})) << "the mortar alone";
    player.handle(group_key(5), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(4)})) << "the turret alone";
}

TEST(RowKeys, AnEmptyKeyLeavesTheSelectionAloneAndSaysSo) {
    Rig rig = make_rig(); // five vehicles: F6..F10 are empty
    Interaction player{rig.own.at(0)};
    player.handle(group_key(1), rig.sim, rig.options);
    ASSERT_EQ(selected(player).size(), 1U);
    player.handle(group_key(7), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0)}));
    EXPECT_EQ(player.message(), "You have no group F7.");
    EXPECT_EQ(empty_key_message(7), "You have no group F7.");
    EXPECT_EQ(empty_key_message(10), "You have no group F10.");
}

TEST(RowKeys, AFrameWithNoRowHasNoGroups) {
    Rig rig = make_rig();
    rig.options.row = nullptr;
    Interaction player{rig.own.at(0)};
    player.handle(group_key(1), rig.sim, rig.options);
    EXPECT_TRUE(selected(player).empty());
    EXPECT_EQ(player.message(), "You have no group F1.");
}

// ------------------------------------------------------------------ the drawing

/// One vertex of the row's own geometry, back in frame pixels.
struct Px {
    float x{};
    float y{};
    std::array<float, 3> colour{};
};

constexpr float kMarginPx = 6.0F;
constexpr float kGapPx = 6.0F;
constexpr float kSlotPx = 85.0F; ///< (1280 - 12 + 6) / 14 - 6: the 14 slots just fit the frame
constexpr float kBarTopPx = 36.0F;
constexpr float kBarBottomPx = 39.0F;
constexpr float kPadPx = 3.0F;

[[nodiscard]] float slot_left(std::size_t index) {
    return kMarginPx + (static_cast<float>(index) * (kSlotPx + kGapPx));
}

/// What `options.row` added to the frame, as pixels: the frame with the row, less the same frame
/// without. The HUD block is contiguous, so the added vertices are one run.
[[nodiscard]] std::vector<Px> row_pixels(const sim::Sim& sim, SceneOptions options,
                                         std::span<const UnitId> chosen = {}) {
    const RowState* row = options.row;
    options.row = nullptr;
    const Scene base = build_scene(sim, options, chosen);
    options.row = row;
    const Scene with = build_scene(sim, options, chosen);
    std::vector<Px> out;
    if (with.vertices.size() < base.vertices.size()) {
        ADD_FAILURE() << "the row removed geometry";
        return out;
    }
    const std::size_t added = with.vertices.size() - base.vertices.size();
    // Members, not bytes: SceneVertex has padding, so its object representation is not unique and a
    // memcmp of ONE vertex is unsound (clang-tidy's suspicious-memory-comparison -- which the
    // repo's buffer-wide memcmps in test_scene.cpp do not trigger, because the check matches a
    // single object's size).
    const auto same_vertex = [](const SceneVertex& a, const SceneVertex& b) {
        return a.position == b.position && a.colour == b.colour && a.alpha == b.alpha && a.u == b.u
               && a.v == b.v;
    };
    std::size_t first = 0;
    while (first < base.vertices.size()
           && same_vertex(base.vertices.at(first), with.vertices.at(first))) {
        ++first;
    }
    for (std::size_t i = first + added; i < with.vertices.size(); ++i) {
        EXPECT_TRUE(same_vertex(base.vertices.at(i - added), with.vertices.at(i)))
            << "the row's vertices are one contiguous run";
    }
    for (std::size_t i = first; i < first + added; ++i) {
        const SceneVertex& v = with.vertices.at(i);
        const Projected p
            = project(options.camera, options.extent, v.position[0], v.position[1], v.position[2]);
        out.push_back(Px{p.x, p.y, v.colour});
    }
    return out;
}

/// The slot a pixel belongs to, or -1 for the gaps between them (nothing is ever drawn there).
[[nodiscard]] int slot_at(float x) {
    const int index = static_cast<int>(std::floor((x - kMarginPx + 0.5F) / (kSlotPx + kGapPx)));
    if (index < 0 || index >= static_cast<int>(kRowSlots)) {
        return -1;
    }
    return x <= slot_left(static_cast<std::size_t>(index)) + kSlotPx + 0.5F ? index : -1;
}

[[nodiscard]] std::vector<int> drawn_slots(const std::vector<Px>& pixels) {
    std::vector<int> slots;
    for (const Px& p : pixels) {
        const int slot = slot_at(p.x);
        EXPECT_GE(slot, 0) << "geometry in a gap or off the row at x=" << p.x;
        if (slot >= 0 && std::find(slots.begin(), slots.end(), slot) == slots.end()) {
            slots.push_back(slot);
        }
    }
    std::sort(slots.begin(), slots.end());
    return slots;
}

TEST(RowDrawing, OnlyTheOccupiedKeysAreDrawnEachInItsOwnFixedSpot) {
    Rig rig = make_rig();
    EXPECT_EQ(drawn_slots(row_pixels(rig.sim, rig.options)), (std::vector<int>{4, 5, 6, 7, 8}))
        << "F1..F5, and nothing for ~ 1 2 3 or F6..F10";
}

TEST(RowDrawing, TheRowIsNotDrawnWithoutOne) {
    Rig rig = make_rig();
    const Scene with = build_scene(rig.sim, rig.options);
    rig.options.row = nullptr;
    EXPECT_LT(build_scene(rig.sim, rig.options).vertices.size(), with.vertices.size());
}

TEST(RowDrawing, AnEmptyRowDrawsNothingAtAll) {
    // No dim ink, no placeholder, no zero: not one vertex differs from a frame with no row.
    Rig rig = make_rig();
    const RowState empty;
    rig.options.row = &empty;
    const Scene with_empty = build_scene(rig.sim, rig.options);
    rig.options.row = nullptr;
    const Scene without = build_scene(rig.sim, rig.options);
    ASSERT_EQ(with_empty.vertices.size(), without.vertices.size());
    EXPECT_EQ(std::memcmp(with_empty.vertices.data(), without.vertices.data(),
                          without.vertices.size() * sizeof(SceneVertex)),
              0);
}

TEST(RowDrawing, AnEmptiedSlotLeavesABlankSpaceAndTheOthersDoNotMove) {
    Rig rig = make_rig();
    const std::vector<Px> full = row_pixels(rig.sim, rig.options);
    rig.row.slots.at(group_slot(2)).units.clear(); // F2's group is gone
    const std::vector<Px> gapped = row_pixels(rig.sim, rig.options);
    EXPECT_EQ(drawn_slots(gapped), (std::vector<int>{4, 6, 7, 8})) << "F2's space stays blank";
    const auto in_slot = [](const std::vector<Px>& pixels, int slot) {
        std::vector<Px> mine;
        std::copy_if(pixels.begin(), pixels.end(), std::back_inserter(mine),
                     [slot](const Px& p) { return slot_at(p.x) == slot; });
        return mine;
    };
    for (const int slot : {4, 6, 7, 8}) {
        const std::vector<Px> before = in_slot(full, slot);
        const std::vector<Px> after = in_slot(gapped, slot);
        ASSERT_EQ(before.size(), after.size()) << "slot " << slot;
        for (std::size_t i = 0; i < before.size(); ++i) {
            EXPECT_EQ(before.at(i).x, after.at(i).x) << "slot " << slot << " moved";
        }
    }
}

TEST(RowDrawing, TheFactoryIsDrawnOnKeyTwoAndTheNavKeyOnlyWithAMark) {
    Rig rig = make_rig({{UnitKind::Factory, Side::Player, 30, 30}});
    EXPECT_EQ(drawn_slots(row_pixels(rig.sim, rig.options)), (std::vector<int>{2, 4, 5, 6, 7, 8}))
        << "the factory on 2, the five vehicles on F1..F5";
    rig.row.update(rig.sim, Side::Player, 3);
    EXPECT_EQ(drawn_slots(row_pixels(rig.sim, rig.options)),
              (std::vector<int>{0, 2, 4, 5, 6, 7, 8}))
        << "~ appears with the marks";
}

/// The bar's fill is green, yellow or red by how hurt the group is; the plate round it is white.
[[nodiscard]] bool is_fill(const Px& p) {
    const std::array<float, 3>& c = p.colour;
    return c == std::array<float, 3>{0.1F, 0.85F, 0.2F}
           || c == std::array<float, 3>{1.0F, 0.85F, 0.1F}
           || c == std::array<float, 3>{1.0F, 0.15F, 0.1F};
}

/// The fraction of the slot's bar the fill covers, measured off the frame; -1 when it has no bar.
[[nodiscard]] float bar_fraction(const std::vector<Px>& pixels, int slot) {
    const float inside_left = slot_left(static_cast<std::size_t>(slot)) + kPadPx;
    const float inside_width = kSlotPx - (2.0F * kPadPx);
    float right = -1.0F;
    bool any = false;
    for (const Px& p : pixels) {
        if (slot_at(p.x) != slot || p.y < kBarTopPx - 0.1F || p.y > kBarBottomPx + 0.1F
            || !is_fill(p)) {
            continue;
        }
        any = true;
        right = std::max(right, p.x);
    }
    return any ? (right - inside_left) / inside_width : -1.0F;
}

TEST(RowDrawing, AVehicleKeyHasAFullBarAtFullHealth) {
    Rig rig = make_rig();
    const std::vector<Px> pixels = row_pixels(rig.sim, rig.options);
    EXPECT_NEAR(bar_fraction(pixels, 4), 1.0F, 0.01F);
}

TEST(RowDrawing, TheBarIsTheGroupsHitPointsOverItsCeiling) {
    Rig rig
        = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41}, {UnitKind::Tank, Side::Enemy, 47, 42}});
    // F2's tank is the one under fire. Group it with the healthy first tank: the bar is the SUM.
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1)};
    rig.row.slots.at(group_slot(2)).units.clear();
    float fraction = 1.0F;
    for (int tick = 0; tick < 600 && fraction >= 1.0F; ++tick) {
        rig.sim.step();
        const std::optional<SlotReading> now = read_slot(rig.sim, rig.row.slots.at(group_slot(1)));
        ASSERT_TRUE(now.has_value());
        if (!now.has_value()) {
            return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
        }
        fraction = static_cast<float>(now->hp) / static_cast<float>(now->max_hp);
    }
    ASSERT_LT(fraction, 1.0F) << "nothing was hurt, so nothing is proved";
    const std::vector<Px> pixels = row_pixels(rig.sim, rig.options);
    EXPECT_NEAR(bar_fraction(pixels, 4), fraction, 0.01F);
}

TEST(RowDrawing, TheNavKeyHasNoBarAndTheFactoryDoes) {
    Rig rig = make_rig({{UnitKind::Factory, Side::Player, 30, 30}});
    rig.row.update(rig.sim, Side::Player, 1);
    const std::vector<Px> pixels = row_pixels(rig.sim, rig.options);
    EXPECT_LT(bar_fraction(pixels, 0), 0.0F) << "marks have no hit points";
    EXPECT_NEAR(bar_fraction(pixels, 2), 1.0F, 0.01F);
}

TEST(RowDrawing, TheCountIsDrawnAndFollowsTheGroupSize) {
    Rig rig = make_rig();
    const auto count_ink = [&](const std::vector<Px>& pixels) {
        // The count sits in the slot's top right: the right half, above the label.
        std::vector<float> xs;
        for (const Px& p : pixels) {
            if (slot_at(p.x) == 4 && p.x > slot_left(4) + (kSlotPx / 2.0F) && p.y > 8.5F
                && p.y < 20.0F) {
                xs.push_back(p.x);
            }
        }
        return xs;
    };
    const std::vector<float> one = count_ink(row_pixels(rig.sim, rig.options));
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1)};
    const std::vector<float> two = count_ink(row_pixels(rig.sim, rig.options));
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1)};
    const std::vector<float> two_again = count_ink(row_pixels(rig.sim, rig.options));
    EXPECT_FALSE(one.empty());
    EXPECT_NE(one, two) << "x1 and x2 are different glyphs";
    EXPECT_EQ(two, two_again);
}

TEST(RowDrawing, TheRowTouchesNoSimAndTwoSameSeedFramesAreByteIdentical) {
    const auto run = [] {
        Rig rig = make_rig();
        rig.options.notice = "You have no group F7.";
        Interaction player{rig.own.at(0)};
        player.handle(group_key(1), rig.sim, rig.options);
        for (int i = 0; i < 30; ++i) {
            rig.sim.step();
        }
        rig.row.update(rig.sim, Side::Player, 0);
        const std::vector<UnitId> chosen = selected(player);
        return std::make_pair(chosen, build_scene(rig.sim, rig.options, chosen));
    };
    const auto a = run();
    const auto b = run();
    EXPECT_EQ(a.first, b.first);
    ASSERT_EQ(a.second.vertices.size(), b.second.vertices.size());
    EXPECT_EQ(std::memcmp(a.second.vertices.data(), b.second.vertices.data(),
                          a.second.vertices.size() * sizeof(a.second.vertices[0])),
              0);

    Rig rig = make_rig();
    const std::uint64_t hash = rig.sim.state_hash();
    static_cast<void>(build_scene(rig.sim, rig.options));
    EXPECT_EQ(rig.sim.state_hash(), hash);
}

} // namespace
} // namespace protekt::game
