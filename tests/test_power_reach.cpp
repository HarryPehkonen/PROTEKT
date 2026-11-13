// ROUND 18, K4: POWER HAS TO REACH THE BUILDING.
//
// The owner: "The Relay Bunker has a couple of jobs. First, it relays power to a remote site. I
// generally put all power units near the deployed recycler ... I then place relay bunkers near any
// entrance where the enemy would try to penetrate. I can construct Gun Towers next to relay
// bunkers, touching the relay bunkers so power can be transferred."
//
// The rule under test (`powered_buildings`): a POWER plant is a source; the deployed RECYCLER and
// the RELAY BUNKER are conductors; everything else is a terminal; "touching" is a shared edge. The
// footprints below are written as literals, independently of the code.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::game {
namespace {

using sim::Footprint;
using sim::Side;
using sim::UnitKind;
using B = ConstructBuilding;

// A row of 1 x 1 buildings along y = 10, named by the x tile they stand on.
[[nodiscard]] NetworkBuilding at(UnitKind kind, int x, int y = 10) {
    return NetworkBuilding{kind, Footprint{x, y, 1, 1}};
}

// ----------------------------------------------------------------------------- touching

TEST(PowerReach, TouchingIsASharedEdgeAndACornerAloneDoesNot) {
    const Footprint a{10, 10, 2, 2}; // tiles 10..11 x 10..11
    EXPECT_TRUE(touches(a, Footprint{12, 10, 1, 1})) << "east, sharing an edge";
    EXPECT_TRUE(touches(a, Footprint{9, 11, 1, 1})) << "west, sharing an edge";
    EXPECT_TRUE(touches(a, Footprint{11, 12, 1, 1})) << "south";
    EXPECT_TRUE(touches(a, Footprint{10, 9, 2, 1})) << "north, a whole side";
    EXPECT_TRUE(touches(Footprint{12, 11, 1, 1}, a)) << "symmetric, one tile of overlap enough";
    EXPECT_FALSE(touches(a, Footprint{12, 12, 1, 1})) << "a CORNER only";
    EXPECT_FALSE(touches(a, Footprint{9, 9, 1, 1})) << "the other corner";
    EXPECT_FALSE(touches(a, Footprint{13, 10, 1, 1})) << "a one-tile gap";
    EXPECT_FALSE(touches(a, Footprint{12, 13, 1, 1})) << "diagonal and apart";
}

// ----------------------------------------------------------------------- the connection rule

TEST(PowerReach, APlantIsASourceAndAConsumerTouchingItIsPowered) {
    const std::array<NetworkBuilding, 2> net{at(UnitKind::Power, 10), at(UnitKind::GunTower, 11)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_TRUE(on.at(0)) << "a plant is a source";
    EXPECT_TRUE(on.at(1)) << "the tower touches it";
}

TEST(PowerReach, ARelayBunkerCarriesPowerAcrossToAConsumerThatTouchesOnlyTheBunker) {
    // plant | bunker | tower in a row: the tower touches the bunker and NOT the plant.
    const std::array<NetworkBuilding, 3> net{at(UnitKind::Power, 10), at(UnitKind::RelayBunker, 11),
                                             at(UnitKind::GunTower, 12)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_TRUE(on.at(1)) << "the bunker touches the plant";
    EXPECT_TRUE(on.at(2)) << "the relay works: the tower is powered through it";
}

TEST(PowerReach, AConsumerThatTouchesNothingPoweredIsNotPowered) {
    const std::array<NetworkBuilding, 2> net{at(UnitKind::Power, 10), at(UnitKind::GunTower, 20)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_TRUE(on.at(0));
    EXPECT_FALSE(on.at(1)) << "a gap, and no bunker across it";
}

TEST(PowerReach, AChainOfTwoBunkersCarriesPower) {
    const std::array<NetworkBuilding, 4> net{at(UnitKind::Power, 10), at(UnitKind::RelayBunker, 11),
                                             at(UnitKind::RelayBunker, 12),
                                             at(UnitKind::GunTower, 13)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_TRUE(on.at(1));
    EXPECT_TRUE(on.at(2)) << "the second bunker touches only the first";
    EXPECT_TRUE(on.at(3)) << "and the tower touches only the second";
}

TEST(PowerReach, ABuildingTouchingOnlyAnUnpoweredBunkerIsNotPowered) {
    // The failure mode his doctrine exists to prevent: a bunker with no line back to a plant
    // carries nothing, so the tower beside it stands dead. The plant is a gap away.
    const std::array<NetworkBuilding, 3> net{at(UnitKind::Power, 10), at(UnitKind::RelayBunker, 13),
                                             at(UnitKind::GunTower, 14)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_FALSE(on.at(1)) << "the bunker touches nothing powered";
    EXPECT_FALSE(on.at(2)) << "so the tower touching it is not powered";
}

TEST(PowerReach, ATerminalPassesNothingOn) {
    // plant | factory | bunker: a Factory is a terminal, so the bunker past it is NOT reached.
    // (Otherwise any row of consumers would carry power for ever and the bunker would be
    // pointless.)
    const std::array<NetworkBuilding, 3> net{at(UnitKind::Power, 10), at(UnitKind::Factory, 11),
                                             at(UnitKind::RelayBunker, 12)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_TRUE(on.at(1));
    EXPECT_FALSE(on.at(2));
}

TEST(PowerReach, TheRecyclerIsTheHubAPlantFeedsAndABunkerBesideItIsPowered) {
    // plant | recycler | bunker | tower.  The recycler conducts what the plant gives it.
    const std::array<NetworkBuilding, 4> net{
        at(UnitKind::Power, 10),
        NetworkBuilding{UnitKind::DeployedRecycler, Footprint{11, 10, 2, 2}},
        at(UnitKind::RelayBunker, 13), at(UnitKind::GunTower, 14)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_TRUE(on.at(1)) << "the plant feeds the hub";
    EXPECT_TRUE(on.at(2)) << "the bunker beside the hub is powered through it";
    EXPECT_TRUE(on.at(3));
}

TEST(PowerReach, TheRecyclerAloneSuppliesNothing) {
    // No plant: the hub is not a source, so the bunker touching it is dead too.
    const std::array<NetworkBuilding, 2> net{
        NetworkBuilding{UnitKind::DeployedRecycler, Footprint{11, 10, 2, 2}},
        at(UnitKind::RelayBunker, 13)};
    const std::vector<bool> on = powered_buildings(net);
    EXPECT_FALSE(on.at(0));
    EXPECT_FALSE(on.at(1));
}

TEST(PowerReach, ACornerOnlyContactDoesNotConnect) {
    const std::array<NetworkBuilding, 2> net{at(UnitKind::Power, 10, 10),
                                             at(UnitKind::GunTower, 11, 11)};
    EXPECT_FALSE(powered_buildings(net).at(1));
}

TEST(PowerReach, ThreePlantsAndThreeConsumersStillBalanceWhereAllOfThemTouch) {
    // The GLOBAL count is still his rule (three per plant, one per consumer) and it still holds:
    // three plants and three consumers, every consumer touching a plant, are all powered and the
    // count says nine supplied, three drawn.
    const std::array<NetworkBuilding, 6> net{
        at(UnitKind::Power, 10),   at(UnitKind::Power, 12),       at(UnitKind::Power, 14),
        at(UnitKind::Factory, 11), at(UnitKind::RelayBunker, 13), at(UnitKind::GunTower, 15)};
    const std::vector<bool> on = powered_buildings(net);
    for (std::size_t i = 0; i < on.size(); ++i) {
        EXPECT_TRUE(on.at(i)) << i;
    }
    StandingBuildings standing{};
    standing.at(static_cast<std::size_t>(B::Power)) = 3;
    standing.at(static_cast<std::size_t>(B::Factory)) = 1;
    standing.at(static_cast<std::size_t>(B::RelayBunker)) = 1;
    standing.at(static_cast<std::size_t>(B::GunTower)) = 1;
    EXPECT_EQ(spare_power(standing), 6) << "9 supplied, 3 drawn";
}

TEST(PowerReach, ASiteIsConnectedWhenItTouchesAPoweredConductorOnly) {
    const std::array<NetworkBuilding, 3> net{at(UnitKind::Power, 10), at(UnitKind::RelayBunker, 11),
                                             at(UnitKind::RelayBunker, 20)};
    EXPECT_TRUE(site_connected(net, Footprint{10, 11, 1, 1})) << "below the plant";
    EXPECT_TRUE(site_connected(net, Footprint{12, 10, 1, 1})) << "beside the powered bunker";
    EXPECT_FALSE(site_connected(net, Footprint{21, 10, 1, 1})) << "beside the DEAD bunker";
    EXPECT_FALSE(site_connected(net, Footprint{15, 15, 1, 1})) << "in the open";
}

TEST(PowerReach, OnlyTheThreeConsumersDrawPower) {
    EXPECT_TRUE(draws_power(UnitKind::Factory));
    EXPECT_TRUE(draws_power(UnitKind::RelayBunker));
    EXPECT_TRUE(draws_power(UnitKind::GunTower));
    EXPECT_FALSE(draws_power(UnitKind::Power));
    EXPECT_FALSE(draws_power(UnitKind::Armory));
    EXPECT_FALSE(draws_power(UnitKind::ServiceBay));
    EXPECT_FALSE(draws_power(UnitKind::Tank));
}

// ------------------------------------------------------------------------------- the panel

[[nodiscard]] std::vector<MenuRow> panel(const StandingBuildings& standing, bool powered_node) {
    MenuInput in;
    in.subject = MenuSubject::Constructor;
    in.construct_panel = true;
    in.ore = 999;
    in.standing = standing;
    in.powered_node = powered_node;
    return menu_rows(in);
}

[[nodiscard]] const MenuRow& keyed(const std::vector<MenuRow>& rows, std::string_view key) {
    for (const MenuRow& row : rows) {
        if (row.key == key) {
            return row;
        }
    }
    ADD_FAILURE() << "no row " << key;
    return rows.front();
}

TEST(PowerReach, APowerRowDimsAndNamesTheConnectionWhenNothingPoweredStandsToTouch) {
    StandingBuildings standing{};
    standing.at(static_cast<std::size_t>(B::Power)) = 1;      // capacity is there ...
    const std::vector<MenuRow> rows = panel(standing, false); // ... but nothing powered to touch
    for (const char* key : {"2", "3", "6"}) {
        EXPECT_FALSE(keyed(rows, key).enabled) << key;
        EXPECT_EQ(keyed(rows, key).reason, MenuDim::NeedsConnection) << key;
        const std::string text = refusal_message(keyed(rows, key));
        EXPECT_NE(text.find("touch"), std::string::npos) << "names the connection: " << text;
        EXPECT_NE(text, "Needs power");
    }
    const std::vector<MenuRow> lit = panel(standing, true);
    for (const char* key : {"2", "3", "6"}) {
        EXPECT_TRUE(keyed(lit, key).enabled) << key;
    }
}

TEST(PowerReach, TheCountStillRulesWhenEverythingTouches) {
    // The two rules are BOTH required: a powered node to touch, and a spare unit to draw. With the
    // node but no spare unit the row is the count's, "Needs power", as before.
    StandingBuildings standing{};
    standing.at(static_cast<std::size_t>(B::Power)) = 1;
    standing.at(static_cast<std::size_t>(B::Factory)) = 1;
    standing.at(static_cast<std::size_t>(B::RelayBunker)) = 1;
    standing.at(static_cast<std::size_t>(B::GunTower)) = 1;
    const std::vector<MenuRow> rows = panel(standing, true);
    EXPECT_EQ(keyed(rows, "6").reason, MenuDim::NeedsPower);
}

// ------------------------------------------------------------- the game: the armed placement

constexpr std::uint64_t kSeed = 42;

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    sim::UnitId driven;
    sim::UnitId constructor;
};

[[nodiscard]] Rig make_rig(std::vector<sim::Spawn> extra) {
    std::vector<sim::Spawn> units{{UnitKind::Tank, Side::Player, 60, 60},
                                  {UnitKind::Constructor, Side::Player, 61, 60}};
    units.insert(units.end(), extra.begin(), extra.end());
    sim::Scenario scenario{kSeed, units};
    scenario.store_capacity = 400;
    scenario.start_ore = 400;
    Rig r{};
    r.sim = sim::Sim::create(scenario);
    r.driven = r.sim.units().at(0).id;
    r.constructor = r.sim.units().at(1).id;
    r.options.seed = kSeed;
    r.options.extent = Extent{1280, 720};
    r.options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
    r.options.viewer_unit = r.driven;
    r.options.aim = Aim{640.0F, 700.0F};
    r.row.update(r.sim, Side::Player, 0);
    r.options.row = &r.row;
    return r;
}

[[nodiscard]] input::InputFrame key_frame(std::int32_t scancode) {
    input::InputFrame frame{};
    frame.keys.push_back(input::KeyEvent{scancode, true, false});
    return frame;
}

void press(const Rig& r, Session& session, std::vector<sim::Order>& orders, std::int32_t code) {
    session.frame(key_frame(code), r.sim, r.options, orders);
}

void open_panel(const Rig& r, Session& session, std::vector<sim::Order>& orders) {
    for (int slot = 0; slot < 10; ++slot) {
        if (session.selection().size() == 1U
            && session.selection().units().front() == r.constructor) {
            break;
        }
        press(r, session, orders, input::kKeyF1 + slot);
    }
    ASSERT_EQ(session.selection().size(), 1U);
    press(r, session, orders, input::kKey1 + 2); // `3 Construct`
    ASSERT_TRUE(session.construct_armed());
}

[[nodiscard]] std::size_t placements(const Rig& r, const std::vector<sim::Order>& orders) {
    std::size_t n = 0;
    for (const sim::Order& order : orders) {
        n += order.unit == r.constructor
                     && std::holds_alternative<sim::orders::Place>(order.payload)
                 ? 1U
                 : 0U;
    }
    return n;
}

// Where the reticle lands, found on a rig with nothing extra standing (the aim does not depend on
// the plant, which stands four tiles or more off it).
[[nodiscard]] NavAim aimed() {
    const Rig bare = make_rig({});
    return nav_aim(bare.sim, bare.options);
}

TEST(PowerReach, TheArmedGunTowerIsGreenBesideAPlantAndRedAwayFromIt) {
    const NavAim aim = aimed();
    ASSERT_TRUE(aim.on_ground());
    const Rig r = make_rig({{UnitKind::Power, Side::Player, aim.tile_x + 4, aim.tile_y}});
    const std::vector<NetworkBuilding> net = network_of(r.sim, Side::Player);
    ASSERT_EQ(net.size(), 1U) << "the plant stands and has a footprint";
    const Placing tower{UnitKind::GunTower, false, 0, sim::footprint_of(UnitKind::GunTower)};
    EXPECT_TRUE(placement_preview(r.sim, tower, aim.tile_x, aim.tile_y).valid)
        << "touches the plant";
    EXPECT_FALSE(placement_preview(r.sim, tower, aim.tile_x - 2, aim.tile_y).valid)
        << "the tower would end two tiles short of it: nothing powered touches";
    const Placing plant{UnitKind::Power, false, 30, sim::footprint_of(UnitKind::Power)};
    EXPECT_TRUE(placement_preview(r.sim, plant, aim.tile_x - 2, aim.tile_y).valid)
        << "a plant needs no connection, it IS the source";
}

TEST(PowerReach, SpaceRefusesAGunTowerThatNothingPoweredTouchesAndNamesTheConnection) {
    const NavAim aim = aimed();
    ASSERT_TRUE(aim.on_ground());
    // The plant is six tiles from the aimed tile: the 4x4 tower ends at tile +3, so two tiles of
    // gap (ROUND 19, L1: every building is four times as wide, so the old offsets moved with it).
    Rig r = make_rig({{UnitKind::Power, Side::Player, aim.tile_x + 6, aim.tile_y}});
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1 + 5); // 6 GUN TOWER
    ASSERT_TRUE(session.placing().has_value());
    orders.clear();
    press(r, session, orders, input::kKeySpace);
    EXPECT_EQ(placements(r, orders), 0U) << "nothing is ordered";
    EXPECT_NE(session.message().find("touch"), std::string::npos) << session.message();
    EXPECT_NE(session.message(), "There is no room to build there.");
    EXPECT_TRUE(session.placing().has_value()) << "and it stays armed";
    EXPECT_EQ(r.sim.ore(Side::Player), 400U);
}

TEST(PowerReach, SpacePlacesAGunTowerThatTouchesAPlant) {
    const NavAim aim = aimed();
    ASSERT_TRUE(aim.on_ground());
    Rig r = make_rig({{UnitKind::Power, Side::Player, aim.tile_x + 4, aim.tile_y}});
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1 + 5);
    ASSERT_TRUE(session.placing().has_value());
    orders.clear();
    press(r, session, orders, input::kKeySpace);
    EXPECT_EQ(placements(r, orders), 1U);
}

} // namespace
} // namespace protekt::game
