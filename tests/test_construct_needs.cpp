// ROUND 17, J4: a panel row that cannot be built DIMS, and the refusal names what is missing.
//
// The owner: "Dimming rule: correct. Dim means inert. Refusal names the obstacle." and the tree:
// Power needs only scrap; Factory, Relay Bunker and Gun Tower need one unit of power ("Each Power
// unit supplies three units of power"); Armory and Service Bay need a Factory; Training Ctr. an
// Armory; Tech Centre an Armory AND a Service Bay; Bomber Bay a Training Ctr.
//
// The table below is written as LITERALS, independently of `construct_requirement`, so the code
// cannot agree with itself. "One unit of power" is PROVISIONAL: read as one SPARE unit at build
// time. A building whose Power is destroyed is NOT ruled, and nothing here tests a rule for it.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::UnitKind;
using B = ConstructBuilding;

constexpr std::uint64_t kSeed = 42;

[[nodiscard]] StandingBuildings stand(std::initializer_list<B> list) {
    StandingBuildings s{};
    for (const B b : list) {
        ++s.at(static_cast<std::size_t>(b));
    }
    return s;
}

[[nodiscard]] std::vector<MenuRow> panel(const StandingBuildings& standing, int ore) {
    MenuInput in;
    in.subject = MenuSubject::Constructor;
    in.construct_panel = true;
    in.ore = ore;
    in.standing = standing;
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

// The owner's table, as literals: (row key, buildings it needs, needs a spare power unit).
struct Need {
    std::string_view key;
    std::vector<B> buildings;
    bool power;
};
const std::vector<Need>& table() {
    static const std::vector<Need> t{
        {"1", {}, false},
        {"2", {}, true},
        {"3", {}, true},
        {"4", {B::Factory}, false},
        {"5", {B::Factory}, false},
        {"6", {}, true},
        {"7", {B::Armory}, false},
        {"8", {B::Armory, B::ServiceBay}, false},
        {"9", {B::TrainingCentre}, false},
    };
    return t;
}

// ----------------------------------------------------------------------- the table, both ways

TEST(ConstructNeeds, EveryRowIsBrightOrDimExactlyAsTheTableSaysForEveryBuildingSet) {
    // Every subset of the nine buildings standing (one of each), with and without a Power plant
    // standing in it; the bank is never the obstacle.
    for (unsigned mask = 0; mask < 512U; ++mask) {
        StandingBuildings s{};
        for (std::size_t b = 0; b < 9; ++b) {
            s.at(b) = (mask >> b) & 1U ? 1 : 0;
        }
        const bool power_spare = spare_power(s) >= 1;
        const std::vector<MenuRow> rows = panel(s, 999);
        for (const Need& need : table()) {
            bool met = !need.power || power_spare;
            for (const B b : need.buildings) {
                met = met && s.at(static_cast<std::size_t>(b)) > 0;
            }
            const MenuRow& row = keyed(rows, need.key);
            EXPECT_EQ(row.enabled, met) << "row " << need.key << " mask " << mask;
            EXPECT_EQ(row.reason == MenuDim::None, met) << "row " << need.key << " mask " << mask;
        }
    }
}

TEST(ConstructNeeds, WithNothingStandingOnlyPowerIsBright) {
    const std::vector<MenuRow> rows = panel({}, 999);
    EXPECT_TRUE(keyed(rows, "1").enabled);
    for (const char* key : {"2", "3", "4", "5", "6", "7", "8", "9"}) {
        EXPECT_FALSE(keyed(rows, key).enabled) << key;
    }
    EXPECT_EQ(keyed(rows, "2").reason, MenuDim::NeedsPower);
    EXPECT_EQ(keyed(rows, "4").reason, MenuDim::NeedsBuilding);
}

TEST(ConstructNeeds, ARowWithEveryRequirementMetIsBright) {
    // Everything each row needs stands, power to spare, and the bank pays: all nine live.
    StandingBuildings s{};
    s.at(static_cast<std::size_t>(B::Power)) = 2;
    for (const B b : {B::Factory, B::Armory, B::ServiceBay, B::TrainingCentre}) {
        s.at(static_cast<std::size_t>(b)) = 1;
    }
    const std::vector<MenuRow> rows = panel(s, 999);
    for (const Need& need : table()) {
        EXPECT_TRUE(keyed(rows, need.key).enabled) << need.key;
        EXPECT_EQ(keyed(rows, need.key).reason, MenuDim::None) << need.key;
    }
}

// ----------------------------------------------------------------------------- the refusals

TEST(ConstructNeeds, TheRefusalNamesTheRightObstacle) {
    EXPECT_EQ(refusal_message(keyed(panel({}, 999), "4")), "Needs a Factory");
    EXPECT_EQ(refusal_message(keyed(panel({}, 999), "5")), "Needs a Factory");
    EXPECT_EQ(refusal_message(keyed(panel(stand({B::Factory}), 999), "7")), "Needs an Armory");
    EXPECT_EQ(refusal_message(keyed(panel({}, 999), "9")), "Needs a Training Ctr.");
    EXPECT_EQ(refusal_message(keyed(panel({}, 999), "2")), "Needs power");
    EXPECT_EQ(refusal_message(keyed(panel({}, 999), "3")), "Needs power");
    EXPECT_EQ(refusal_message(keyed(panel({}, 999), "6")), "Needs power");
}

TEST(ConstructNeeds, ARowMissingTwoBuildingsNamesBoth) {
    // The Tech Centre needs an Armory and a Service Bay.
    EXPECT_EQ(refusal_message(keyed(panel({}, 999), "8")), "Needs an Armory and a Service Bay");
    // With one of them built, only the other is named.
    EXPECT_EQ(refusal_message(keyed(panel(stand({B::Armory}), 999), "8")), "Needs a Service Bay");
    EXPECT_EQ(refusal_message(keyed(panel(stand({B::ServiceBay}), 999), "8")), "Needs an Armory");
}

TEST(ConstructNeeds, AnUnaffordableRowStillReportsItsShortfallWithTheNumber) {
    // Power is the row with no requirement: 44 banked, the Factory costs 55 but a Power stands.
    const std::vector<MenuRow> rows = panel(stand({B::Power}), 44);
    EXPECT_EQ(keyed(rows, "2").reason, MenuDim::Unaffordable);
    EXPECT_EQ(refusal_message(keyed(rows, "2")), "Add 11 scrap (have 44, costs 55)");
    EXPECT_EQ(refusal_message(keyed(panel({}, 10), "1")), "Add 20 scrap (have 10, costs 30)");
}

TEST(ConstructNeeds, ABuildingNotBuiltIsNamedBeforeTheScrapThatIsShort) {
    // Both are missing: the obstacle that waiting cannot fix is the one named.
    const std::vector<MenuRow> rows = panel({}, 0);
    EXPECT_EQ(keyed(rows, "4").reason, MenuDim::NeedsBuilding);
    EXPECT_EQ(refusal_message(keyed(rows, "4")), "Needs a Factory");
    EXPECT_EQ(keyed(rows, "2").reason, MenuDim::NeedsPower);
}

// --------------------------------------------------------------------------- power arithmetic

TEST(ConstructNeeds, ThePowerArithmeticAtItsBoundary) {
    // One Power supplies three: the first three consumers fit, the fourth does not.
    StandingBuildings s = stand({B::Power});
    EXPECT_EQ(spare_power(s), 3);
    EXPECT_TRUE(keyed(panel(s, 999), "2").enabled);
    s.at(static_cast<std::size_t>(B::Factory)) = 1;
    EXPECT_EQ(spare_power(s), 2);
    s.at(static_cast<std::size_t>(B::RelayBunker)) = 1;
    EXPECT_EQ(spare_power(s), 1);
    EXPECT_TRUE(keyed(panel(s, 999), "6").enabled) << "the third consumer fits";
    s.at(static_cast<std::size_t>(B::GunTower)) = 1;
    EXPECT_EQ(spare_power(s), 0);
    for (const char* key : {"2", "3", "6"}) {
        EXPECT_FALSE(keyed(panel(s, 999), key).enabled) << "the fourth does not fit: " << key;
        EXPECT_EQ(keyed(panel(s, 999), key).reason, MenuDim::NeedsPower);
    }
    // A second Power plant brings three more.
    s.at(static_cast<std::size_t>(B::Power)) = 2;
    EXPECT_EQ(spare_power(s), 3);
    EXPECT_TRUE(keyed(panel(s, 999), "6").enabled);
}

TEST(ConstructNeeds, OnlyTheThreeConsumersDrawPower) {
    StandingBuildings s = stand(
        {B::Power, B::Armory, B::ServiceBay, B::TrainingCentre, B::TechCentre, B::BomberBay});
    EXPECT_EQ(spare_power(s), 3) << "no other building draws any";
}

// ------------------------------------------------------------- the sim's units, end to end

struct Yard {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    sim::UnitId driven;
    sim::UnitId constructor;
};

[[nodiscard]] Yard make_yard(std::vector<sim::Spawn> extra, std::uint16_t ore) {
    std::vector<sim::Spawn> units{{UnitKind::Tank, Side::Player, 60, 60},
                                  {UnitKind::Constructor, Side::Player, 61, 60}};
    units.insert(units.end(), extra.begin(), extra.end());
    sim::Scenario scenario{kSeed, units};
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    Yard y{};
    y.sim = sim::Sim::create(scenario);
    y.driven = y.sim.units().at(0).id;
    y.constructor = y.sim.units().at(1).id;
    y.options.seed = kSeed;
    y.options.extent = Extent{1280, 720};
    y.options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
    y.options.viewer_unit = y.driven;
    y.row.update(y.sim, Side::Player, 0);
    y.options.row = &y.row;
    return y;
}

[[nodiscard]] input::InputFrame key_frame(std::int32_t scancode) {
    input::InputFrame frame{};
    frame.keys.push_back(input::KeyEvent{scancode, true, false});
    return frame;
}

void open_panel(const Yard& y, Session& session, std::vector<sim::Order>& orders) {
    for (int slot = 0; slot < 10; ++slot) {
        if (session.selection().size() == 1U
            && session.selection().units().front() == y.constructor) {
            break;
        }
        session.frame(key_frame(input::kKeyF1 + slot), y.sim, y.options, orders);
    }
    ASSERT_EQ(session.selection().units().front(), y.constructor);
    session.frame(key_frame(input::kKey1 + 2), y.sim, y.options, orders); // 3 Construct
    ASSERT_TRUE(session.construct_armed());
}

TEST(ConstructNeeds, StandingBuildingsAreCountedFromTheLivingUnitsOfThatSideOnly) {
    // The scenario places a Power and a Factory for the player and a Power for the enemy.
    const Yard y = make_yard({{UnitKind::Power, Side::Player, 80, 80},
                              {UnitKind::Factory, Side::Player, 90, 90},
                              {UnitKind::Power, Side::Enemy, 150, 150}},
                             400);
    const StandingBuildings mine = standing_buildings(y.sim, Side::Player);
    EXPECT_EQ(mine.at(static_cast<std::size_t>(B::Power)), 1);
    EXPECT_EQ(mine.at(static_cast<std::size_t>(B::Factory)), 1);
    EXPECT_EQ(spare_power(mine), 2) << "3 supplied, the Factory draws 1";
    const StandingBuildings theirs = standing_buildings(y.sim, Side::Enemy);
    EXPECT_EQ(theirs.at(static_cast<std::size_t>(B::Power)), 1);
    EXPECT_EQ(theirs.at(static_cast<std::size_t>(B::Factory)), 0);
}

TEST(ConstructNeeds, ThePanelInTheGameReadsTheStandingBuildingsAndThePressNamesTheObstacle) {
    // No Power stands: the Factory row is dim in the drawn panel and its press names power; the
    // Armory names the Factory. Nothing is charged.
    Yard y = make_yard({}, 400);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    const std::vector<MenuRow> rows
        = app::menu_for(y.sim, session, y.options, session.attack_armed());
    EXPECT_EQ(keyed(rows, "2").reason, MenuDim::NeedsPower);
    EXPECT_EQ(keyed(rows, "4").reason, MenuDim::NeedsBuilding);
    session.frame(key_frame(input::kKey1 + 1), y.sim, y.options, orders);
    EXPECT_EQ(session.message(), "Needs power");
    session.frame(key_frame(input::kKey1 + 3), y.sim, y.options, orders);
    EXPECT_EQ(session.message(), "Needs a Factory");
    session.frame(key_frame(input::kKey1 + 7), y.sim, y.options, orders);
    EXPECT_EQ(session.message(), "Needs an Armory and a Service Bay");
    EXPECT_EQ(y.sim.ore(Side::Player), 400U);
    EXPECT_FALSE(session.placing().has_value());
}

TEST(ConstructNeeds, WithAPowerAndAFactoryStandingTheGameLiftsTheDim) {
    Yard y = make_yard(
        {{UnitKind::Power, Side::Player, 80, 80}, {UnitKind::Factory, Side::Player, 90, 90}}, 400);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    const std::vector<MenuRow> rows
        = app::menu_for(y.sim, session, y.options, session.attack_armed());
    EXPECT_TRUE(keyed(rows, "2").enabled) << "2 spare units";
    EXPECT_TRUE(keyed(rows, "4").enabled) << "a Factory stands";
    EXPECT_FALSE(keyed(rows, "7").enabled) << "no Armory";
}

// ------------------------------------------------------------------------ nothing else moved

TEST(ConstructNeeds, TheOtherMenusDoNotReadTheStandingBuildings) {
    for (const MenuSubject subject : {MenuSubject::Deployment, MenuSubject::Factory}) {
        MenuInput in;
        in.subject = subject;
        in.ore = 400;
        const std::vector<MenuRow> without = menu_rows(in);
        in.standing = stand({B::Power, B::Factory});
        const std::vector<MenuRow> with = menu_rows(in);
        ASSERT_EQ(without.size(), with.size());
        for (std::size_t i = 0; i < with.size(); ++i) {
            EXPECT_EQ(with.at(i).enabled, without.at(i).enabled);
            EXPECT_EQ(with.at(i).verb, without.at(i).verb);
        }
    }
    // The panel's three non-building rows and the heading are as they were.
    const std::vector<MenuRow> rows = panel({}, 0);
    ASSERT_EQ(rows.size(), 12U);
    EXPECT_EQ(rows.at(0).verb, "Close Menu");
    EXPECT_TRUE(rows.at(2).heading);
}

} // namespace
} // namespace protekt::game
