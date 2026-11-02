// THE COMMAND ROW IS FOR ASSETS YOU COMMAND, NOT FOR YOU (Harri, 2026-11-02):
//  * every turret is under a function key, always, deployed or not -- a turret stays a VEHICLE;
//  * the vehicle the player sits in is NEVER under a function key;
//  * a swap re-derives the row: the vehicle you LEFT takes a free key, the one you ENTERED loses
//    its own, and the swap is DISALLOWED when there is no key for the one you are leaving.
// One test per rule, each isolating its own number, so a red names the rule that broke.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

/// Which slot holds `id`, or -1.
[[nodiscard]] int slot_of(const RowState& row, UnitId id) {
    for (std::size_t slot = 0; slot < kRowSlots; ++slot) {
        const auto& units = row.slots.at(slot).units;
        if (std::find(units.begin(), units.end(), id) != units.end()) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

[[nodiscard]] std::size_t group_keys_in_use(const RowState& row) {
    std::size_t used = 0;
    for (std::size_t slot = kRowFirstGroup; slot < kRowSlots; ++slot) {
        used += row.occupied(slot) ? 1U : 0U;
    }
    return used;
}

[[nodiscard]] UnitId first_of(const sim::Sim& sim, UnitKind kind) {
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == Side::Player && unit.kind == kind) {
            return unit.id;
        }
    }
    return UnitId{};
}

// ============================================================ turrets are always on a key

TEST(CommandRowTurrets, EveryStartingTurretIsUnderAFunctionKey) {
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed, sim::default_data()));
    const UnitId driven = first_of(sim, UnitKind::Tank);
    RowState row;
    row.seed_from_start(sim, Side::Player, 0, driven);

    int turrets = 0;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == Side::Player && unit.kind == UnitKind::Turret) {
            ++turrets;
            EXPECT_GE(slot_of(row, unit.id), static_cast<int>(kRowFirstGroup))
                << "turret " << unit.id.value << " must hold a function key";
        }
    }
    EXPECT_EQ(turrets, 2) << "the default force starts with two";
}

TEST(CommandRowTurrets, ADeployedTurretKeepsItsKeyAndIsStillAVehicle) {
    // CHARACTERISATION: the row already did this; the test pins it against "make it consistent"
    // with the deployed recycler (a building, on `1`) or the deployed scavenger (on no key).
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.spawns = {{UnitKind::Tank, Side::Player, 40, 40},
                       {UnitKind::Turret, Side::Player, 36, 41},
                       {UnitKind::Tank, Side::Enemy, 60, 62}};
    sim::Sim sim = sim::Sim::create(scenario);
    const UnitId driven = sim.units()[0].id;
    const UnitId turret = sim.units()[1].id;
    RowState row;
    row.seed_from_start(sim, Side::Player, 0, driven);
    const int key = slot_of(row, turret);
    ASSERT_GE(key, static_cast<int>(kRowFirstGroup));

    sim.enqueue(sim::Order{turret, sim::orders::Deploy{sim.find(turret)->x.floor_int(),
                                                       sim.find(turret)->y.floor_int()}});
    for (std::uint32_t i = 0; i < sim::kTurretDeployTicks + 5; ++i) {
        sim.step();
    }
    row.update(sim, Side::Player, 0, driven);
    ASSERT_EQ(sim.find(turret)->kind, UnitKind::DeployedTurret);
    EXPECT_EQ(slot_of(row, turret), key) << "deployed: the same function key";
    EXPECT_GE(key, static_cast<int>(kRowFirstGroup)) << "a group key, never a building key";
    EXPECT_EQ(row.slots.at(1).units.size(), 0U) << "`1` is the recycler's";
    EXPECT_EQ(row.slots.at(2).units.size(), 0U) << "`2` is the factory's";
}

TEST(CommandRowTurrets, ADeployedTurretStillTakesFollowMeDefendAndUndeploy) {
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.spawns = {{UnitKind::Tank, Side::Player, 40, 44},
                       {UnitKind::Turret, Side::Player, 34, 50},
                       {UnitKind::Tank, Side::Enemy, 50, 52}};
    sim::Sim sim = sim::Sim::create(scenario);
    const UnitId driven = sim.units()[0].id;
    const UnitId turret = sim.units()[1].id;
    sim.enqueue(sim::Order{turret, sim::orders::Deploy{sim.find(turret)->x.floor_int(),
                                                       sim.find(turret)->y.floor_int()}});
    for (std::uint32_t i = 0; i < sim::kTurretDeployTicks + 5; ++i) {
        sim.step();
    }
    ASSERT_EQ(sim.find(turret)->kind, UnitKind::DeployedTurret);
    RowState row;
    row.update(sim, Side::Player, 0, driven);
    const int key = slot_of(row, turret);
    ASSERT_GE(key, static_cast<int>(kRowFirstGroup));
    SceneOptions options;
    options.seed = kSeed;
    options.extent = kExtent;
    options.driven_unit = driven;
    options.viewer_unit = driven;
    options.camera = Camera{40.0F * kMetresPerTile, height_metres(kSeed, 40, 44) + 3.0F,
                            44.0F * kMetresPerTile, 0.0F, 0.0F};
    options.row = &row;

    const auto first_order = [&](std::int32_t digit) {
        Interaction player(driven);
        player.handle(key_frame(input::kKeyF1 + static_cast<std::int32_t>(key - kRowFirstGroup)),
                      sim, options);
        EXPECT_EQ(player.selection().size(), 1U) << "the key selects the deployed turret";
        player.handle(key_frame(digit), sim, options);
        const std::vector<sim::Order> orders = player.take_orders();
        EXPECT_EQ(orders.size(), 1U) << player.message();
        return orders.empty() ? sim::OrderKind::Stop : orders.front().kind();
    };
    EXPECT_EQ(first_order(input::kKey1), sim::OrderKind::Follow);
    EXPECT_EQ(first_order(input::kKey1 + 5), sim::OrderKind::Defend);
    EXPECT_EQ(first_order(input::kKey1 + 6), sim::OrderKind::Undeploy);
}

// ============================================================ the player's own vehicle

TEST(CommandRowDriven, TheVehicleYouAreInIsNeverUnderTheStartRowsKeys) {
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed, sim::default_data()));
    const UnitId driven = first_of(sim, UnitKind::Tank);
    RowState row;
    row.seed_from_start(sim, Side::Player, 0, driven);

    EXPECT_EQ(slot_of(row, driven), -1);
}

TEST(CommandRowDriven, ALoneDrivenVehicleLeavesTheRowEmpty) {
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.spawns
        = {{UnitKind::Tank, Side::Player, 40, 40}, {UnitKind::Tank, Side::Enemy, 60, 62}};
    const sim::Sim sim = sim::Sim::create(scenario);
    RowState row;
    row.update(sim, Side::Player, 0, sim.units()[0].id);

    EXPECT_EQ(group_keys_in_use(row), 0U) << "nothing to command, so no key is drawn";
}

TEST(CommandRowDriven, AnUpdateTakesTheVehicleYouClimbedIntoOffItsKey) {
    sim::Scenario scenario;
    scenario.seed = kSeed;
    scenario.spawns
        = {{UnitKind::Tank, Side::Player, 40, 40}, {UnitKind::Tank, Side::Player, 44, 41}};
    const sim::Sim sim = sim::Sim::create(scenario);
    const UnitId first = sim.units()[0].id;
    const UnitId second = sim.units()[1].id;
    RowState row;
    row.update(sim, Side::Player, 0); // nobody is driving yet: both are on a key
    ASSERT_GE(slot_of(row, second), static_cast<int>(kRowFirstGroup));

    row.update(sim, Side::Player, 0, second);

    EXPECT_EQ(slot_of(row, second), -1) << "now driven: no key";
    EXPECT_GE(slot_of(row, first), static_cast<int>(kRowFirstGroup)) << "the other keeps its own";
}

// ============================================================ the swap

struct SwapRig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven{};
    UnitId entered{};
    std::vector<UnitId> baits; ///< tanks far from the camera's line, one key each
};

/// Camera at tile (40,30) looking down +Z. The driven tank is at (40,40); `baits` tanks spread
/// along y=50, then the ENTERED tank at (46,38) -- spawned last, so with ten baits it finds every
/// function key taken and has none of its own.
[[nodiscard]] SwapRig make_swap_rig(int baits, bool entered_last = true) {
    SwapRig rig{};
    std::vector<sim::Spawn> spawns{{UnitKind::Tank, Side::Player, 40, 40}};
    if (!entered_last) {
        spawns.push_back({UnitKind::Tank, Side::Player, 46, 38});
    }
    for (int i = 0; i < baits; ++i) {
        spawns.push_back({UnitKind::Tank, Side::Player, 29 + (2 * i), 50});
    }
    if (entered_last) {
        spawns.push_back({UnitKind::Tank, Side::Player, 46, 38});
    }
    rig.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    rig.driven = rig.sim.units().front().id;
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.id == rig.driven) {
            continue;
        }
        if (unit.x.floor_int() == 46) {
            rig.entered = unit.id;
        } else {
            rig.baits.push_back(unit.id);
        }
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.driven_unit = rig.driven;
    rig.options.viewer_unit = rig.driven;
    rig.options.camera = Camera{40.0F * kMetresPerTile, height_metres(kSeed, 40, 30) + 3.0F,
                                30.0F * kMetresPerTile, 0.0F, -0.12F};
    rig.row.update(rig.sim, Side::Player, 0, rig.driven);
    rig.options.row = &rig.row;
    return rig;
}

void aim_at(SwapRig& rig, UnitId id) {
    const sim::Unit* u = rig.sim.find(id);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
}

TEST(CommandRowSwap, TheVehicleYouLeftTakesAFreeKeyAndTheOneYouEnteredLosesItsOwn) {
    SwapRig rig = make_swap_rig(2, false);
    ASSERT_EQ(slot_of(rig.row, rig.driven), -1);
    const int entered_key = slot_of(rig.row, rig.entered);
    ASSERT_GE(entered_key, static_cast<int>(kRowFirstGroup));

    Interaction player(rig.driven);
    aim_at(rig, rig.entered);
    player.handle(key_frame(input::kKeyX), rig.sim, rig.options);

    ASSERT_EQ(player.driven(), rig.entered) << player.message();
    EXPECT_EQ(slot_of(rig.row, rig.entered), -1) << "the one you are in has no key";
    EXPECT_GE(slot_of(rig.row, rig.driven), static_cast<int>(kRowFirstGroup))
        << "the one you left is a command now";
}

TEST(CommandRowSwap, ThatStaysTrueAfterTheNextFramesUpdate) {
    SwapRig rig = make_swap_rig(2, false);
    Interaction player(rig.driven);
    aim_at(rig, rig.entered);
    player.handle(key_frame(input::kKeyX), rig.sim, rig.options);
    ASSERT_EQ(player.driven(), rig.entered);
    const RowState swapped = rig.row;

    rig.row.update(rig.sim, Side::Player, 0, player.driven());

    EXPECT_EQ(rig.row.slots, swapped.slots) << "the swap left the row as the next frame derives it";
}

TEST(CommandRowSwap, WithNoFreeKeyForTheOneYouAreLeavingTheSwapIsRefused) {
    SwapRig rig = make_swap_rig(static_cast<int>(kRowGroups));
    ASSERT_EQ(group_keys_in_use(rig.row), static_cast<std::size_t>(kRowGroups)) << "a full row";
    ASSERT_EQ(slot_of(rig.row, rig.entered), -1) << "the entered tank holds no key of its own";
    const RowState before = rig.row;

    Interaction player(rig.driven);
    aim_at(rig, rig.entered);
    player.handle(key_frame(input::kKeyX), rig.sim, rig.options);

    EXPECT_EQ(player.driven(), rig.driven) << "still in the same vehicle";
    EXPECT_EQ(rig.row.slots, before.slots) << "and not a key moved";
    EXPECT_EQ(player.message(), "No function key is free for the vehicle you would leave.");
    EXPECT_TRUE(player.take_orders().empty()) << "a refusal orders nothing, not even a Stop";
}

TEST(CommandRowSwap, EnteringTheOnlyVehicleOnAKeyFreesThatKeyForTheOneYouLeft) {
    // A full row, but the vehicle entered is alone on its key: leaving it empties the key, and
    // that is the free key the old vehicle takes. Refusing here would make a full row unswappable.
    SwapRig rig = make_swap_rig(static_cast<int>(kRowGroups));
    const UnitId entered = rig.baits.at(3);
    const int its_key = slot_of(rig.row, entered);
    ASSERT_GE(its_key, static_cast<int>(kRowFirstGroup));

    Interaction player(rig.driven);
    aim_at(rig, entered);
    player.handle(key_frame(input::kKeyX), rig.sim, rig.options);

    ASSERT_EQ(player.driven(), entered) << player.message();
    EXPECT_EQ(slot_of(rig.row, entered), -1);
    EXPECT_EQ(slot_of(rig.row, rig.driven), its_key) << "it took the key the entered one gave up";
}

TEST(CommandRowSwap, TheRowStateRefusesAndChangesNothingOnItsOwn) {
    SwapRig rig = make_swap_rig(static_cast<int>(kRowGroups));
    const RowState before = rig.row;

    EXPECT_FALSE(rig.row.swap_driven(rig.sim, rig.driven, rig.entered));
    EXPECT_EQ(rig.row.slots, before.slots);
}

TEST(CommandRowSwap, AVehicleThatIsGoneNeedsNoKey) {
    // The tank the player was in is dead (or never was): there is nothing to put on a key, so a
    // full row does not stand in the way of climbing into another.
    SwapRig rig = make_swap_rig(static_cast<int>(kRowGroups));
    const UnitId entered = rig.baits.at(0);

    EXPECT_TRUE(rig.row.swap_driven(rig.sim, UnitId{}, entered));
    EXPECT_EQ(slot_of(rig.row, entered), -1);
}

} // namespace
} // namespace protekt::game
