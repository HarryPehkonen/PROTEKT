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
/// (44,41), a tank at (40,2) far BEHIND the camera, a mortar bike at (60,41), a turret at (36,41)
/// -- so F1..F5 each hold one of them. One enemy tank, in front.
[[nodiscard]] Rig make_rig(std::vector<Spawn> extra = {}) {
    Rig rig{};
    std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 40, 40},   {UnitKind::Tank, Side::Player, 44, 41},
        {UnitKind::Tank, Side::Player, 40, 2},    {UnitKind::MortarBike, Side::Player, 60, 41},
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
/// in front of three enemy tanks (F2), a mortar bike far off (F3). With `late`, one more tank
/// (spawned last, so with the highest id) is in the sim too: the unit that arrives after the death.
[[nodiscard]] std::vector<Spawn> middle_dies(bool late) {
    std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 4, 4},       {UnitKind::Turret, Side::Player, 44, 41},
        {UnitKind::MortarBike, Side::Player, 8, 4}, {UnitKind::Tank, Side::Enemy, 45, 41},
        {UnitKind::Tank, Side::Enemy, 46, 42},      {UnitKind::Tank, Side::Enemy, 45, 40}};
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
    one_more.push_back({UnitKind::MortarBike, Side::Player, 14, 10});
    std::vector<Spawn> two_more = one_more;
    two_more.push_back({UnitKind::RocketTank, Side::Player, 16, 10});

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

TEST(RowSlots, TheRecyclerIsAVehicleOnItsFKeyUntilItDeploysThenItsKeyGoesBlankAndOneShowsIt) {
    // DEPLOY flips what ROW-SLOTS could only promise: the recycler sits on an F key while it is a
    // vehicle, and the moment it is a building that key goes blank IN PLACE -- nothing re-packs --
    // and key 1 shows it. The skirmish's recycler is the player's tenth vehicle: F10 (HAULER: the
    // scavenger took the ninth).
    sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    RowState row;
    row.update(sim, Side::Player, 0);
    EXPECT_FALSE(row.occupied(1)) << "key 1 is for the DEPLOYED recycler";
    ASSERT_TRUE(row.occupied(2)) << "the skirmish has a factory";
    EXPECT_EQ(sim.find(row.slots.at(2).units.front())->kind, UnitKind::Factory);
    const std::optional<SlotReading> last = read_slot(sim, row.slots.at(group_slot(10)));
    ASSERT_TRUE(last.has_value()) << "F10 holds the recycler";
    if (!last.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    EXPECT_EQ(last->kind, UnitKind::Recycler);
    // HAULER: ten vehicles and ten group keys -- the row is exactly full, and F10 is the last key
    // there is, so the recycler sits on the row's final slot.
    EXPECT_EQ(group_slot(10) + 1, kRowSlots) << "F10 is the row's last group key";
    const UnitId recycler = row.slots.at(group_slot(10)).units.front();
    std::array<RowSlot, kRowSlots> before = row.slots;

    // Deployed where it stands: its position is the centre of the square to its north-west.
    const sim::Unit* vehicle = sim.find(recycler);
    const int corner_x = vehicle->x.floor_int() - 1;
    const int corner_y = vehicle->y.floor_int() - 1;
    ASSERT_TRUE(sim::deploy_site_ok(kSeed, sim.units(), corner_x, corner_y))
        << "the skirmish recycler can deploy where it was spawned";
    sim.enqueue(sim::Order{recycler, sim::orders::Deploy{corner_x, corner_y}});
    for (int tick = 0; tick + 1 < sim::kDeployTicks; ++tick) {
        sim.step();
    }
    row.update(sim, Side::Player, 0);
    EXPECT_EQ(sim.find(recycler)->kind, UnitKind::Recycler) << "one tick short";
    EXPECT_EQ(row.slots, before) << "still on F10, and key 1 still blank";

    sim.step();
    row.update(sim, Side::Player, 0);
    ASSERT_EQ(sim.find(recycler)->kind, UnitKind::DeployedRecycler);
    EXPECT_FALSE(row.occupied(group_slot(10))) << "its F key is blank";
    ASSERT_TRUE(row.occupied(1)) << "and key 1 shows the deployed recycler";
    EXPECT_EQ(row.slots.at(1).units, (std::vector<UnitId>{recycler}));
    for (std::size_t index = kRowFirstGroup; index < kRowSlots; ++index) {
        if (index != group_slot(10)) {
            EXPECT_EQ(row.slots.at(index), before.at(index))
                << row_key_label(index) << ": nothing else moved, nothing re-packed";
        }
    }
    EXPECT_EQ(row.slots.at(2), before.at(2)) << "the factory keeps key 2";
}

TEST(RowSlots, ADeployedRecyclerIsNeverSeatedInAnFGroup) {
    // Spawned already deployed, beside a vehicle: key 1 holds it, and no F key ever does -- however
    // many frames the row is updated, and however many group slots stand empty.
    sim::Sim sim
        = sim::Sim::create(sim::Scenario{kSeed,
                                         {{UnitKind::DeployedRecycler, Side::Player, 40, 40},
                                          {UnitKind::Tank, Side::Player, 46, 40}}});
    RowState row;
    for (int frame = 0; frame < 3; ++frame) {
        row.update(sim, Side::Player, 0);
        sim.step();
    }
    const UnitId building = sim.units().front().id;
    EXPECT_EQ(row.slots.at(1).units, (std::vector<UnitId>{building}));
    for (std::size_t index = kRowFirstGroup; index < kRowSlots; ++index) {
        EXPECT_EQ(std::count(row.slots.at(index).units.begin(), row.slots.at(index).units.end(),
                             building),
                  0)
            << row_key_label(index);
    }
    ASSERT_TRUE(row.occupied(group_slot(1))) << "the tank took F1, the first empty group";
    EXPECT_EQ(sim.find(row.slots.at(group_slot(1)).units.front())->kind, UnitKind::Tank);
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

TEST(RowSlots, SlotReadingExposesOneFractionPerLivingVehicle) {
    // Two enemy tanks beside the second own tank: it is hurt, then killed, while the others live.
    Rig rig
        = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41}, {UnitKind::Tank, Side::Enemy, 47, 42}});
    const UnitId hurt = rig.own.at(1);
    // The hurt one first: a key's order is the player's (a move appends), and an order that read
    // the same backwards would not show the reading keeps it.
    const RowSlot slot{{hurt, rig.own.at(0), rig.own.at(2)}};
    const auto fraction_of = [&rig](UnitId id) {
        return static_cast<float>(rig.sim.find(id)->hp)
               / static_cast<float>(sim::profile(UnitKind::Tank).hp);
    };
    for (int tick = 0; tick < 600 && rig.sim.find(hurt)->hp == sim::profile(UnitKind::Tank).hp;
         ++tick) {
        rig.sim.step();
    }
    ASSERT_LT(rig.sim.find(hurt)->hp, sim::profile(UnitKind::Tank).hp) << "nothing was hurt";
    ASSERT_TRUE(rig.sim.find(hurt)->alive);
    const std::optional<SlotReading> hurt_reading = read_slot(rig.sim, slot);
    ASSERT_TRUE(hurt_reading.has_value());
    if (!hurt_reading.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    EXPECT_EQ(hurt_reading->fractions,
              (std::vector<float>{fraction_of(hurt), fraction_of(rig.own.at(0)),
                                  fraction_of(rig.own.at(2))}))
        << "one per vehicle, in slot order";
    ASSERT_FALSE(hurt_reading->fractions.empty());
    EXPECT_LT(hurt_reading->fractions.front(), hurt_reading->fractions.back())
        << "the far tank is untouched, so the order is visible";

    step_until_dead(rig.sim, hurt);
    // The anchor: the corpse is still in the sim's table, so skipping it is the reading's doing.
    ASSERT_NE(rig.sim.find(hurt), nullptr);
    const std::optional<SlotReading> dead_reading = read_slot(rig.sim, slot);
    ASSERT_TRUE(dead_reading.has_value());
    if (!dead_reading.has_value()) {
        return; // unreachable once the ASSERT above has passed: for the analyzer, not the test
    }
    EXPECT_EQ(dead_reading->fractions,
              (std::vector<float>{fraction_of(rig.own.at(0)), fraction_of(rig.own.at(2))}))
        << "the corpse contributes none";
    EXPECT_EQ(dead_reading->fractions.size(), dead_reading->count);
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
    EXPECT_EQ(kind_label(UnitKind::MortarBike), "MORTAR BIKE");
    EXPECT_EQ(kind_label(UnitKind::RocketTank), "ROCKET TANK");
    EXPECT_EQ(kind_label(UnitKind::Turret), "TURRET");
    EXPECT_EQ(kind_label(UnitKind::Factory), "FACTORY");
    EXPECT_EQ(kind_label(UnitKind::Recycler), "RECYCLER");
    EXPECT_EQ(kind_label(UnitKind::Scout), "SCOUT");
    EXPECT_EQ(kind_label(UnitKind::MslScout), "MSL SCOUT");
}

// ------------------------------------------------------------------ the bar's look (BAR-LOOK)

constexpr std::array<float, 3> kBrightGreen{0.15F, 0.95F, 0.25F};
constexpr std::array<float, 3> kYellow{1.00F, 0.85F, 0.10F};
constexpr std::array<float, 3> kRed{1.00F, 0.15F, 0.10F};
constexpr std::array<float, 3> kNearBlack{0.12F, 0.05F, 0.05F};
constexpr float kColourTolerance = 1e-4F;

/// The one segment `fraction` alone makes.
[[nodiscard]] std::array<float, 3> ramp(float fraction) {
    const std::array<float, 1> one{fraction};
    const std::vector<BarSegment> segments = bar_segments(one);
    EXPECT_EQ(segments.size(), 1U);
    return segments.empty() ? std::array<float, 3>{-1.0F, -1.0F, -1.0F} : segments.front().rgb;
}

void expect_colour(const std::array<float, 3>& got, const std::array<float, 3>& want) {
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(got.at(c), want.at(c), kColourTolerance) << "channel " << c;
    }
}

TEST(BarLook, OneSegmentPerVehicleInSlotOrder) {
    const std::array<float, 4> fractions{1.0F, 0.5F, 0.2F, 0.9F};
    const std::vector<BarSegment> segments = bar_segments(fractions);
    ASSERT_EQ(segments.size(), fractions.size());
    for (std::size_t i = 0; i < fractions.size(); ++i) {
        EXPECT_FLOAT_EQ(segments.at(i).fraction, fractions.at(i)) << "segment " << i;
        EXPECT_EQ(segments.at(i).rgb, ramp(fractions.at(i))) << "segment " << i;
    }
}

TEST(BarLook, HealthyIsBrightGreen) { expect_colour(ramp(1.0F), kBrightGreen); }

TEST(BarLook, AtTwoThirdsIsYellow) { expect_colour(ramp(0.66F), kYellow); }

TEST(BarLook, AtOneThirdIsRed) { expect_colour(ramp(0.33F), kRed); }

TEST(BarLook, AtZeroIsNearlyBlackButNotBlack) {
    const std::array<float, 3> floor = ramp(0.0F);
    expect_colour(floor, kNearBlack);
    for (std::size_t c = 0; c < 3; ++c) {
        // The plate behind the bar is black: a black segment would read as no vehicle at all.
        EXPECT_GT(floor.at(c), 0.0F) << "channel " << c;
    }
}

TEST(BarLook, InterpolatesBetweenAnchors) {
    // Strictly between its two anchors on every channel where they differ.
    const auto between
        = [](float fraction, const std::array<float, 3>& low, const std::array<float, 3>& high) {
              const std::array<float, 3> got = ramp(fraction);
              for (std::size_t c = 0; c < 3; ++c) {
                  const float lo = std::min(low.at(c), high.at(c));
                  const float hi = std::max(low.at(c), high.at(c));
                  if (lo == hi) {
                      EXPECT_NEAR(got.at(c), lo, kColourTolerance) << fraction << " channel " << c;
                  } else {
                      EXPECT_GT(got.at(c), lo) << fraction << " channel " << c;
                      EXPECT_LT(got.at(c), hi) << fraction << " channel " << c;
                  }
              }
          };
    between(0.5F, kRed, kYellow);
    between(0.83F, kYellow, kBrightGreen);
    between(0.165F, kNearBlack, kRed);
}

TEST(BarLook, NoVehiclesGivesNoSegments) { EXPECT_TRUE(bar_segments({}).empty()); }

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
    // `1` is the BUILDING key now (tests/test_building_keys.cpp): the rig has no recycler, so the
    // digit says that rather than selecting some vehicle's group.
    EXPECT_EQ(player.message(), "You have no recycler.");
}

TEST(RowKeys, SpaceSelectsTheOneUnitThePlayerIsLookingAt) {
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    aim_at_unit(rig,
                rig.own.at(3)); // the mortar bike alone: no group key needed, just the crosshair
    player.handle(key_frame(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(3)}));
}

TEST(RowKeys, TheSelectedGroupObeysFollowWithNoNewPlumbing) {
    Rig rig = make_rig();
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1), rig.own.at(2)};
    Interaction player{rig.own.at(0)};
    player.handle(group_key(1), rig.sim, rig.options); // three tanks
    aim_at_unit(rig, rig.own.at(3));                   // the mortar bike, which is NOT in the group
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
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(3)})) << "the mortar bike alone";
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
constexpr float kSlotPx = 85.0F;      ///< (1280 - 12 + 6) / 14 - 6: the 14 slots just fit the frame
constexpr float kColumnTopPx = 41.0F; ///< the plate ends at 6 + 34 = 40; the column leads in 1 px
constexpr float kPadPx = 3.0F;

[[nodiscard]] float slot_left(std::size_t index) {
    return kMarginPx + (static_cast<float>(index) * (kSlotPx + kGapPx));
}

/// What `options.row` added to the frame, as pixels: the frame with the row, less the same frame
/// without. The HUD block is contiguous, so the added vertices are one run.
[[nodiscard]] std::vector<Px> row_pixels(const sim::Sim& sim, SceneOptions options,
                                         std::span<const UnitId> chosen = {}) {
    RowState* row = options.row;
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
    RowState empty;
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

/// The bar's ink: anything in the bar's rows that is not its dark track, the white frame or the
/// plate's black face (whose rim runs along the bar's bottom edge). The segments' colours are a
/// continuous ramp, so they are found by what they are not.
[[nodiscard]] bool is_fill(const Px& p) {
    return p.colour != std::array<float, 3>{0.25F, 0.25F, 0.25F}
           && p.colour != std::array<float, 3>{0.95F, 0.95F, 0.95F}
           && p.colour != std::array<float, 3>{0.0F, 0.0F, 0.0F};
}

/// One drawn bar of a slot's column, measured off the frame.
struct BarRun {
    float top{};
    float bottom{};
    float left{};
    float right{};
    std::array<float, 3> colour{};
};

constexpr float kEdgeTolerance = 0.05F;

[[nodiscard]] float inside_left_of(int slot) {
    return slot_left(static_cast<std::size_t>(slot)) + kPadPx;
}
constexpr float kInsideWidth = kSlotPx - (2.0F * kPadPx);

/// The slot's bars, top to bottom. The column hangs under the plate, so it is found below the
/// plate's bottom edge; each bar is a quad with two distinct y edges, and they are 1 px apart, so
/// the sorted distinct edges pair off one bar at a time.
[[nodiscard]] std::vector<BarRun> bar_runs(const std::vector<Px>& pixels, int slot) {
    std::vector<Px> fill;
    std::copy_if(pixels.begin(), pixels.end(), std::back_inserter(fill), [slot](const Px& p) {
        return slot_at(p.x) == slot && p.y >= kColumnTopPx - 0.1F && is_fill(p);
    });
    std::vector<float> edges;
    for (const Px& p : fill) {
        if (std::none_of(edges.begin(), edges.end(),
                         [&p](float y) { return std::abs(y - p.y) < kEdgeTolerance; })) {
            edges.push_back(p.y);
        }
    }
    std::sort(edges.begin(), edges.end());
    std::vector<BarRun> runs;
    if (edges.size() % 2 != 0) {
        ADD_FAILURE() << "slot " << slot << ": an odd number of bar edges";
        return runs;
    }
    for (std::size_t i = 0; i < edges.size(); i += 2) {
        BarRun run{edges.at(i), edges.at(i + 1), 1.0e9F, -1.0e9F, {}};
        bool first = true;
        for (const Px& p : fill) {
            if (p.y < run.top - kEdgeTolerance || p.y > run.bottom + kEdgeTolerance) {
                continue;
            }
            run.left = std::min(run.left, p.x);
            run.right = std::max(run.right, p.x);
            if (first) {
                run.colour = p.colour;
                first = false;
            }
            EXPECT_EQ(p.colour, run.colour) << "slot " << slot << ": a bar is one colour";
        }
        runs.push_back(run);
    }
    return runs;
}

/// A column: every bar the slot's inside wide and 3 px tall, 1 px apart, the first 1 px under the
/// plate -- the same size whatever the group holds and however hurt it is.
void expect_column(const std::vector<BarRun>& runs, int slot) {
    ASSERT_FALSE(runs.empty());
    for (std::size_t i = 0; i < runs.size(); ++i) {
        EXPECT_NEAR(runs.at(i).left, inside_left_of(slot), kEdgeTolerance) << "bar " << i;
        EXPECT_NEAR(runs.at(i).right, inside_left_of(slot) + kInsideWidth, kEdgeTolerance)
            << "bar " << i;
        EXPECT_NEAR(runs.at(i).bottom - runs.at(i).top, 3.0F, kEdgeTolerance) << "bar " << i;
        EXPECT_NEAR(runs.at(i).top, kColumnTopPx + (4.0F * static_cast<float>(i)), kEdgeTolerance)
            << "bar " << i << " is 4 px below the one before";
    }
}

TEST(RowDrawing, AVehicleKeyHasAFullBarAtFullHealth) {
    Rig rig = make_rig();
    const std::vector<Px> pixels = row_pixels(rig.sim, rig.options);
    const std::vector<BarRun> runs = bar_runs(pixels, 4);
    ASSERT_EQ(runs.size(), 1U) << "one vehicle, one bar";
    expect_column(runs, 4);
    EXPECT_EQ(runs.front().colour, (std::array<float, 3>{0.15F, 0.95F, 0.25F})) << "bright green";
}

// Harri: "Enemy ships' health bar is always red. They need to be easy to distinguish from my own
// ships' health." Red for them, the ramp for mine: the pair is the whole point.
TEST(RowDrawing, AnEnemysReadoutBarIsRedAtFullHealthWhileMyRowBarIsGreen) {
    Rig rig = make_rig();
    UnitId enemy{};
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.side == Side::Enemy) {
            enemy = unit.id;
        }
    }
    ASSERT_TRUE(enemy.valid());
    ASSERT_EQ(rig.sim.find(enemy)->hp, sim::profile(UnitKind::Tank).hp) << "a full-health enemy";

    SceneOptions plain = rig.options;
    plain.row = nullptr;
    const Scene bare = build_scene(rig.sim, plain);
    plain.attack_target = enemy;
    const Scene with = build_scene(rig.sim, plain);
    ASSERT_GT(with.vertices.size(), bare.vertices.size()) << "the readout was not drawn";
    // The readout is the run of vertices `with` has that `bare` does not: cut the common tail away,
    // and the bar's fill is the last quad of what is left.
    const auto same_vertex = [](const SceneVertex& a, const SceneVertex& b) {
        return a.position == b.position && a.colour == b.colour && a.alpha == b.alpha && a.u == b.u
               && a.v == b.v;
    };
    std::size_t tail = 0;
    while (tail < bare.vertices.size()
           && same_vertex(bare.vertices.at(bare.vertices.size() - 1 - tail),
                          with.vertices.at(with.vertices.size() - 1 - tail))) {
        ++tail;
    }
    const std::array<float, 3> fill = with.vertices.at(with.vertices.size() - tail - 1).colour;
    EXPECT_EQ(fill, kEnemyBarRed) << "an enemy's bar is red";
    const std::array<float, 3> friendly
        = bar_runs(row_pixels(rig.sim, rig.options), 4).front().colour;
    EXPECT_EQ(friendly, bar_segments(std::array<float, 1>{1.0F}).front().rgb)
        << "and mine, at full health, is still the green end of the ramp";
    EXPECT_NE(fill, friendly);
}

TEST(RowDrawing, TheColumnIsOneBarPerVehicleColouredNotShrunkByItsHealth) {
    Rig rig
        = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41}, {UnitKind::Tank, Side::Enemy, 47, 42}});
    // F2's tank is the one under fire. Group it with the healthy first tank, F2 left blank.
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1)};
    rig.row.slots.at(group_slot(2)).units.clear();
    const std::vector<BarRun> healthy = bar_runs(row_pixels(rig.sim, rig.options), 4);
    for (int tick = 0;
         tick < 600 && rig.sim.find(rig.own.at(1))->hp == sim::profile(UnitKind::Tank).hp; ++tick) {
        rig.sim.step();
    }
    ASSERT_LT(rig.sim.find(rig.own.at(1))->hp, sim::profile(UnitKind::Tank).hp)
        << "nothing was hurt, so nothing is proved";
    const std::vector<BarRun> hurt = bar_runs(row_pixels(rig.sim, rig.options), 4);
    ASSERT_EQ(hurt.size(), 2U) << "x2, two segments";
    expect_column(hurt, 4);
    ASSERT_EQ(healthy.size(), hurt.size());
    for (std::size_t i = 0; i < hurt.size(); ++i) {
        EXPECT_NEAR(hurt.at(i).top, healthy.at(i).top, kEdgeTolerance) << "size, not colour";
        EXPECT_NEAR(hurt.at(i).bottom, healthy.at(i).bottom, kEdgeTolerance) << "size, not colour";
        EXPECT_NEAR(hurt.at(i).left, healthy.at(i).left, kEdgeTolerance) << "size, not colour";
        EXPECT_NEAR(hurt.at(i).right, healthy.at(i).right, kEdgeTolerance) << "size, not colour";
    }
    EXPECT_EQ(hurt.at(0).colour, healthy.at(0).colour) << "the unhurt bar did not change";
    EXPECT_NE(hurt.at(1).colour, healthy.at(1).colour) << "exactly the hurt vehicle's bar changed";
    // Each vehicle's own hit points, straight off the sim, in F1's order.
    std::vector<float> fractions;
    for (const UnitId id : rig.row.slots.at(group_slot(1)).units) {
        fractions.push_back(static_cast<float>(rig.sim.find(id)->hp)
                            / static_cast<float>(sim::profile(UnitKind::Tank).hp));
    }
    const std::vector<BarSegment> want = bar_segments(fractions);
    ASSERT_EQ(want.size(), hurt.size());
    for (std::size_t i = 0; i < hurt.size(); ++i) {
        EXPECT_EQ(hurt.at(i).colour, want.at(i).rgb) << "segment " << i << " is its own vehicle's";
    }
    EXPECT_NE(hurt.at(1).colour, hurt.at(0).colour) << "the hurt one reads differently";
}

TEST(RowDrawing, ThreeVehiclesStandInAColumnOfThreeSameSizedBars) {
    Rig rig = make_rig();
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1), rig.own.at(2)};
    const std::vector<BarRun> runs = bar_runs(row_pixels(rig.sim, rig.options), 4);
    ASSERT_EQ(runs.size(), 3U);
    expect_column(runs, 4);
    // The column grows DOWN: a third vehicle adds a bar below the two, and moves neither of them
    // sideways. A row layout would re-divide the width and so fail the x checks.
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1)};
    const std::vector<BarRun> two = bar_runs(row_pixels(rig.sim, rig.options), 4);
    ASSERT_EQ(two.size(), 2U);
    for (std::size_t i = 0; i < two.size(); ++i) {
        EXPECT_NEAR(runs.at(i).top, two.at(i).top, kEdgeTolerance) << "bar " << i;
        EXPECT_NEAR(runs.at(i).left, two.at(i).left, kEdgeTolerance) << "bar " << i;
        EXPECT_NEAR(runs.at(i).right, two.at(i).right, kEdgeTolerance) << "bar " << i;
    }
    EXPECT_GT(runs.at(2).top, two.at(1).bottom) << "the new bar is under the others";
    EXPECT_NEAR(runs.at(2).left, runs.at(0).left, kEdgeTolerance) << "in the same column";
}

TEST(RowDrawing, ADeadVehiclesBarIsRemovedAndTheRestMoveUp) {
    Rig rig
        = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41}, {UnitKind::Tank, Side::Enemy, 47, 42}});
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1)};
    rig.row.slots.at(group_slot(2)).units.clear();
    ASSERT_EQ(bar_runs(row_pixels(rig.sim, rig.options), 4).size(), 2U);
    step_until_dead(rig.sim, rig.own.at(1));
    // The row is NOT updated: the corpse is still in F1, and still the bar does not draw it.
    ASSERT_EQ(rig.row.slots.at(group_slot(1)).units.size(), 2U);
    const std::vector<BarRun> runs = bar_runs(row_pixels(rig.sim, rig.options), 4);
    ASSERT_EQ(runs.size(), 1U) << "removed, not drawn black";
    expect_column(runs, 4);
}

TEST(RowDrawing, AnAbsurdGroupFillsTheColumnsRoomAndStops) {
    // More tanks than the column has room for (ten).
    constexpr int kCrowd = 14;
    std::vector<Spawn> crowd;
    crowd.reserve(kCrowd);
    for (int i = 0; i < kCrowd; ++i) {
        crowd.push_back({UnitKind::Tank, Side::Player, 10 + (i % 7), 10 + (i / 7)});
    }
    Rig rig = make_rig(crowd);
    std::vector<UnitId> tanks;
    for (const UnitId id : rig.own) {
        if (rig.sim.find(id)->kind == UnitKind::Tank) {
            tanks.push_back(id);
        }
    }
    ASSERT_GT(tanks.size(), 10U);
    rig.row.slots.at(group_slot(1)).units = tanks;
    const std::vector<BarRun> runs = bar_runs(row_pixels(rig.sim, rig.options), 4);
    ASSERT_EQ(runs.size(), 10U) << "as many as fit, top to bottom";
    expect_column(runs, 4);
}

/// The top edge of the notice's yellow plate in `options`, in pixels. The row's own pixels exclude
/// the notice (it is in the frame without the row too), so it is read off the whole frame -- and
/// something yellow MUST be found, or a comparison against it proves nothing.
[[nodiscard]] float notice_top_px(const sim::Sim& sim, const SceneOptions& options) {
    constexpr std::array<float, 3> kNoticeYellow{1.0F, 0.85F, 0.10F};
    const Scene scene = build_scene(sim, options);
    float top = 1.0e9F;
    std::size_t yellow = 0;
    for (const SceneVertex& v : scene.vertices) {
        if (v.colour == kNoticeYellow) {
            const Projected p = project(options.camera, options.extent, v.position[0],
                                        v.position[1], v.position[2]);
            top = std::min(top, p.y);
            ++yellow;
        }
    }
    EXPECT_GT(yellow, 0U) << "no notice was drawn, so nothing is proved";
    return top;
}

TEST(RowDrawing, ANoticeThatReachesUnderAKeyCutsItsColumnShortRatherThanBeingOverlapped) {
    Rig rig = make_rig();
    rig.row.slots.at(group_slot(1)).units = {rig.own.at(0), rig.own.at(1), rig.own.at(2)};
    const std::vector<BarRun> clear = bar_runs(row_pixels(rig.sim, rig.options), 4);
    ASSERT_EQ(clear.size(), 3U) << "three vehicles, three bars, with no notice in the way";
    // 40 capitals is wider than the 370 px that lie to the left of F1's plate, so it runs under it.
    rig.options.notice = "THIS NOTICE IS LONG ENOUGH TO REACH F1.";
    const float notice_top = notice_top_px(rig.sim, rig.options);
    // The notice is in the row's pixels too (nothing else draws it without a row). Its yellow is
    // the ramp's own 0.66 anchor, but these three tanks are all unhurt, so no bar is yellow here
    // and the notice can be told from the bars by colour alone.
    constexpr std::array<float, 3> kNoticeYellow{1.0F, 0.85F, 0.10F};
    std::vector<Px> above_notice;
    for (const Px& p : row_pixels(rig.sim, rig.options)) {
        if (p.colour != kNoticeYellow) {
            above_notice.push_back(p);
        }
    }
    const std::vector<BarRun> cut = bar_runs(above_notice, 4);
    ASSERT_FALSE(cut.empty()) << "the column is cut short, not removed";
    EXPECT_LT(cut.size(), clear.size());
    EXPECT_LT(cut.back().bottom, notice_top) << "the column ends above the notice's plate";
    // Nothing of the column was lost to the cut above: the same bars are there with the notice
    // plate's top as the floor, and an uncut column would have run on past it.
    EXPECT_GT(clear.back().bottom, notice_top) << "the uncut column WOULD have reached the notice";
    // The bars that remain are the first ones, in the same places.
    for (std::size_t i = 0; i < cut.size(); ++i) {
        EXPECT_NEAR(cut.at(i).top, clear.at(i).top, kEdgeTolerance) << "bar " << i;
    }
}

TEST(RowDrawing, TheNavKeyHasNoBarAndTheFactoryDoes) {
    Rig rig = make_rig({{UnitKind::Factory, Side::Player, 30, 30}});
    rig.row.update(rig.sim, Side::Player, 1);
    const std::vector<Px> pixels = row_pixels(rig.sim, rig.options);
    EXPECT_TRUE(bar_runs(pixels, 0).empty()) << "marks have no hit points";
    const std::vector<BarRun> factory = bar_runs(pixels, 2);
    ASSERT_EQ(factory.size(), 1U) << "a building is one segment";
    expect_column(factory, 2);
    EXPECT_EQ(factory.front().colour, (std::array<float, 3>{0.15F, 0.95F, 0.25F}));
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

// ------------------------------------------------------- a batch of built units on one key

/// A player's factory with scrap on the ground and haulers to work it. This is the only way a test
/// can put scrap in the bank, and so the only way it can make a BATCH: units the sim BUILT, which
/// arrive carrying the batch their order named, rather than units the scenario placed, which
/// belong to no batch.
[[nodiscard]] sim::Scenario batch_battle(int pieces) {
    sim::Scenario s;
    s.seed = kSeed;
    s.objectives = {};
    s.spawns = {sim::Spawn{UnitKind::Factory, Side::Player, 52, 45},
                sim::Spawn{UnitKind::Scavenger, Side::Player, 48, 47},
                sim::Spawn{UnitKind::Scavenger, Side::Player, 48, 48},
                sim::Spawn{UnitKind::Scavenger, Side::Player, 48, 49}};
    const sim::Fixed half = sim::Fixed::from_raw(sim::Fixed::kOne / 2);
    for (int i = 0; i < pieces; ++i) {
        s.scrap.push_back(sim::Scrap{sim::fixed_add(sim::Fixed::from_int(42 + (i % 10)), half),
                                     sim::fixed_add(sim::Fixed::from_int(36 + (i / 10)), half)});
    }
    return s;
}

[[nodiscard]] UnitId the_factory(const sim::Sim& sim) {
    for (const sim::Unit& unit : sim.units()) {
        if (unit.kind == UnitKind::Factory) {
            return unit.id;
        }
    }
    return UnitId{};
}

[[nodiscard]] sim::Order queue_one(UnitId producer, std::uint32_t batch) {
    return sim::Order{producer, sim::orders::Build{UnitKind::Scavenger, batch}};
}

/// Steps until `wanted` units exist past `first`, or gives up: what the queue produced.
[[nodiscard]] std::vector<UnitId> run_until_built(sim::Sim& sim, UnitId first, std::size_t wanted) {
    constexpr int kBudget = 4000;
    for (int i = 0; i < kBudget; ++i) {
        std::vector<UnitId> made;
        for (const sim::Unit& unit : sim.units()) {
            if (unit.id.value > first.value) {
                made.push_back(unit.id);
            }
        }
        if (made.size() >= wanted) {
            return made;
        }
        sim.step();
    }
    return {};
}

/// Which group key holds `id`, or kRowGroups + 1 if the row does not hold it at all.
[[nodiscard]] int key_holding(const RowState& row, UnitId id) {
    for (int n = 1; n <= kRowGroups; ++n) {
        const std::vector<UnitId> units = group_of(row, n);
        if (std::find(units.begin(), units.end(), id) != units.end()) {
            return n;
        }
    }
    return kRowGroups + 1;
}

TEST(RowBatch, TheUnitsOfOneBatchShareTheKeyTheFirstOfThemTook) {
    // THE RULING: a shift-built batch occupies ONE function key. The row cannot see the gesture --
    // it sees the batch the units arrived with, and gives a batch the slot its first member takes.
    // Two haulers built under one token must therefore sit together under a single key.
    sim::Sim sim = sim::Sim::create(batch_battle(90));
    const UnitId factory = the_factory(sim);
    const UnitId first = sim.units().back().id;
    sim.enqueue(queue_one(factory, 9));
    sim.enqueue(queue_one(factory, 9));
    const std::vector<UnitId> made = run_until_built(sim, first, 2);
    ASSERT_EQ(made.size(), 2U) << "the queue never produced the batch";

    RowState row;
    row.update(sim, Side::Player, 0);
    const int key_a = key_holding(row, made.at(0));
    const int key_b = key_holding(row, made.at(1));
    EXPECT_LT(key_a, kRowGroups + 1) << "a built unit has to be on the row at all";
    EXPECT_EQ(key_a, key_b)
        << "one batch, one key: the second member joins the first member's slot";
    EXPECT_EQ(group_of(row, key_a).size(), 2U) << "and that key holds the batch, no one else";
}

TEST(RowBatch, TwoLoneBuildsTakeTwoKeysAndNeverShareOne) {
    // The other half of the ruling: a lone build is a batch of one and takes its own key. Only a
    // shared shift token may put two units together -- nothing else about two new units groups
    // them.
    sim::Sim sim = sim::Sim::create(batch_battle(90));
    const UnitId factory = the_factory(sim);
    const UnitId first = sim.units().back().id;
    sim.enqueue(queue_one(factory, 0));
    ASSERT_EQ(run_until_built(sim, first, 1).size(), 1U) << "the first lone build never arrived";
    // The producer is busy until it has: a second lone press is only taken once the first is done.
    while (sim.find(factory)->queue.count > 0) {
        sim.step();
    }
    sim.enqueue(queue_one(factory, 0));
    const std::vector<UnitId> made = run_until_built(sim, first, 2);
    ASSERT_EQ(made.size(), 2U) << "the queue never produced the two lone builds";

    RowState row;
    row.update(sim, Side::Player, 0);
    const int key_a = key_holding(row, made.at(0));
    const int key_b = key_holding(row, made.at(1));
    EXPECT_LT(key_b, kRowGroups + 1) << "both have to be on the row";
    EXPECT_NE(key_a, key_b) << "two lone builds must not be grouped onto one key";
}

TEST(RowBatch, TwoBatchesSitOnTwoKeysAndNeitherKeyMixesThem) {
    // Batch IDENTITY, not "any new unit joins any new key": token 9's pair and token 4's pair each
    // get a key of their own, and neither key holds a member of the other batch.
    sim::Sim sim = sim::Sim::create(batch_battle(90));
    const UnitId factory = the_factory(sim);
    const UnitId first = sim.units().back().id;
    sim.enqueue(queue_one(factory, 9));
    sim.enqueue(queue_one(factory, 9));
    ASSERT_EQ(run_until_built(sim, first, 2).size(), 2U) << "the first batch never arrived";
    while (sim.find(factory)->queue.count > 0) { // the second batch waits for the first to be done
        sim.step();
    }
    sim.enqueue(queue_one(factory, 4));
    sim.enqueue(queue_one(factory, 4));
    const std::vector<UnitId> made = run_until_built(sim, first, 4);
    ASSERT_EQ(made.size(), 4U) << "the queue never produced both batches";

    RowState row;
    row.update(sim, Side::Player, 0);
    const int a = key_holding(row, made.at(0)); // token 9's first
    const int b = key_holding(row, made.at(2)); // token 4's first
    EXPECT_LT(b, kRowGroups + 1);
    EXPECT_EQ(key_holding(row, made.at(1)), a) << "token 9's second member joins token 9's key";
    EXPECT_EQ(key_holding(row, made.at(3)), b) << "token 4's second member joins token 4's key";
    EXPECT_NE(a, b) << "two different batches do not share a key";
    EXPECT_EQ(group_of(row, a).size(), 2U);
    EXPECT_EQ(group_of(row, b).size(), 2U);
}

} // namespace
} // namespace protekt::game
