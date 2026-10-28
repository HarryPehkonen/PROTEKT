// The BUILDING KEYS: `1` brings up the recycler and `2` the factory from anywhere on the map, with
// no pointing. The command row has always DRAWN them (`RowState` refills slots 1 and 2 from the sim
// every frame) but nothing ever read them back: the digit path only consulted the selected
// subject's menu, and with nothing selected there is no menu, so the digit fell through to nothing
// at all. Pointing at the building and pressing SPACE was the only way in.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
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

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

struct Base {
    sim::Sim sim;
    SceneOptions options;
    UnitId tank;
    UnitId second_tank;
    UnitId building; ///< the player's recycler (deployed or not) or factory, as the test asks
    RowState row;    ///< the command row over `sim`, as the app would hold it
};

/// The player's driven tank, a second tank, one building of `kind` far from both, and an enemy
/// factory that no building key may ever reach.
[[nodiscard]] Base base_with(UnitKind kind) {
    Base base{};
    base.sim = sim::Sim::create(sim::Scenario{kSeed,
                                              {{UnitKind::Tank, Side::Player, 40, 40},
                                               {UnitKind::Tank, Side::Player, 44, 41},
                                               {kind, Side::Player, 90, 90},
                                               {UnitKind::Factory, Side::Enemy, 20, 20}}});
    for (const sim::Unit& unit : base.sim.units()) {
        if (unit.side != Side::Player) {
            continue;
        }
        if (unit.kind == kind) {
            base.building = unit.id;
        } else if (!base.tank.valid()) {
            base.tank = unit.id;
        } else {
            base.second_tank = unit.id;
        }
    }
    base.options.seed = kSeed;
    base.options.extent = Extent{1280, 720};
    base.row.update(base.sim, Side::Player, 0);
    return base;
}

/// The digit-row key for `n` (1-9), as the window reports it: USB HID 30 is "1".
[[nodiscard]] InputFrame digit_frame(int n) { return key_frame(input::kKey1 + n - 1); }

[[nodiscard]] std::vector<UnitId> selected(const Interaction& interaction) {
    std::vector<UnitId> ids;
    for (const UnitId id : interaction.selection().units()) {
        ids.push_back(id);
    }
    return ids;
}

} // namespace

TEST(BuildingKeys, KeyOneBringsUpTheDeployedRecyclerWithoutPointingAtIt) {
    Base base = base_with(UnitKind::DeployedRecycler);
    ASSERT_TRUE(base.building.valid());
    ASSERT_EQ(base.sim.find(base.building)->kind, UnitKind::DeployedRecycler);
    Interaction player{base.tank};
    ASSERT_FALSE(base.options.aim.has_value()) << "no crosshair at all: nothing is pointed at";

    player.handle(digit_frame(1), base.sim, base.options);

    EXPECT_EQ(selected(player), (std::vector<UnitId>{base.building}))
        << "the row has always drawn key 1 as the deployed recycler; pressing it must act on it, "
           "and the player is not asked to find it with the crosshair first";
    EXPECT_TRUE(player.take_orders().empty()) << "bringing a building up orders nothing";
}

TEST(BuildingKeys, KeyTwoBringsUpTheFactory) {
    Base base = base_with(UnitKind::Factory);
    Interaction player{base.tank};

    player.handle(digit_frame(2), base.sim, base.options);

    EXPECT_EQ(selected(player), (std::vector<UnitId>{base.building}))
        << "key 2 is the factory, and an ENEMY factory on the map is not the player's";
}

TEST(BuildingKeys, AnUndeployedRecyclerIsStillTheRecyclerAndKeyOneFindsIt) {
    Base base = base_with(UnitKind::Recycler);
    ASSERT_EQ(base.sim.find(base.building)->kind, UnitKind::Recycler);
    Interaction player{base.tank};

    player.handle(digit_frame(1), base.sim, base.options);

    EXPECT_EQ(selected(player), (std::vector<UnitId>{base.building}))
        << "the kind changes when the recycler deploys; the key must not care which side of the "
           "deployment it is on";
}

TEST(BuildingKeys, ABuildingKeyWhoseBuildingIsNotAliveDoesNothing) {
    Base base = base_with(UnitKind::Factory); // no recycler of the player's anywhere
    Interaction player{base.tank};

    player.handle(digit_frame(1), base.sim, base.options);

    EXPECT_TRUE(player.selection().empty()) << "no recycler, so no selection";
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.message().empty()) << "the player is told why nothing came up";
}

TEST(BuildingKeys, AFactoryKeyDoesNotReachTheEnemyFactory) {
    Base base = base_with(UnitKind::DeployedRecycler); // the only factory is the enemy's
    Interaction player{base.tank};

    player.handle(digit_frame(2), base.sim, base.options);

    EXPECT_TRUE(player.selection().empty()) << "the enemy's factory is not the player's to select";
}

TEST(BuildingKeys, TheArmoryKeyHasNoKindYetAndDoesNothing) {
    Base base = base_with(UnitKind::Factory);
    Interaction player{base.tank};

    player.handle(digit_frame(3), base.sim, base.options);

    EXPECT_TRUE(player.selection().empty());
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(BuildingKeys, WithAMenuOpenTheDigitIsTheMenusAndNotTheBuildings) {
    // The menu spec: a left menu owns its keys and the top menu is inaccessible while it is open.
    // With a tank selected, `1` is `Follow Me`, never the recycler.
    Base base = base_with(UnitKind::DeployedRecycler);
    base.options.row = &base.row;
    Interaction player{base.tank};
    player.handle(key_frame(input::kKeyF1 + 1), base.sim, base.options); // the second tank's group
    ASSERT_EQ(selected(player), (std::vector<UnitId>{base.second_tank}));

    player.handle(digit_frame(1), base.sim, base.options);

    EXPECT_EQ(selected(player), (std::vector<UnitId>{base.second_tank}))
        << "the selection stays on the tank: the digit went to the tank's menu";
}

TEST(BuildingKeys, TheHolderIsTheSameFactTheRowDraws) {
    Base base = base_with(UnitKind::DeployedRecycler);
    RowState row;
    row.update(base.sim, Side::Player, 0);
    ASSERT_TRUE(row.occupied(1));
    EXPECT_EQ(building_key_holder(base.sim, Side::Player, 1), row.slots.at(1).units.front())
        << "one mapping from kind to key: the row's drawing and the key's reach cannot disagree";
    EXPECT_FALSE(building_key_holder(base.sim, Side::Player, 2).valid());
    EXPECT_FALSE(building_key_holder(base.sim, Side::Player, 3).valid());
}

} // namespace protekt::game
