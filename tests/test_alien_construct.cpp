// ROUND 17, J5: the ALIEN Builder's panel -- six rows, and the gate is a BUILDING, never power.
//
// The owner's capture of the Builder's panel (KILN 60, GUN SPIRE 75, ANTENNA 60, DOWER 50,
// STRONGHOLD 70, JAMMER 50, no second page) and his confirmed prerequisites: ANTENNA and DOWER need
// a KILN, JAMMER an OVERSEER (the Antenna upgraded in place), STRONGHOLD a FORGE (the Kiln upgraded
// in place). The tables below are LITERALS, independent of `construct_requirement`, and the
// standing array is indexed by the literal slot numbers (KILN 0, GUN SPIRE 1, ANTENNA 2, DOWER 3,
// STRONGHOLD 4, JAMMER 5, FORGE 6, OVERSEER 7) so the enum cannot agree with itself.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/faction.hpp"
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

constexpr std::uint64_t kSeed = 42;
constexpr std::size_t kKiln = 0;
constexpr std::size_t kAntenna = 2;
constexpr std::size_t kForge = 6;
constexpr std::size_t kOverseer = 7;

[[nodiscard]] std::vector<MenuRow> alien_panel(const StandingBuildings& standing, int ore) {
    MenuInput in;
    in.subject = MenuSubject::Constructor;
    in.faction = Faction::Alien;
    in.construct_panel = true;
    in.ore = ore;
    in.standing = standing;
    return menu_rows(in);
}

[[nodiscard]] std::vector<MenuRow> human_panel(int ore) {
    MenuInput in;
    in.subject = MenuSubject::Constructor;
    in.construct_panel = true;
    in.ore = ore;
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

[[nodiscard]] std::size_t keyed_count(const std::vector<MenuRow>& rows) {
    std::size_t n = 0;
    for (const MenuRow& row : rows) {
        n += (!row.heading && !row.key.empty() && row.key != "~" && row.key != "TAB") ? 1U : 0U;
    }
    return n;
}

struct AlienRow {
    std::string_view key;
    std::string_view label;
    int cost;
    std::vector<std::size_t> needs; // slots of the buildings it needs
    std::string_view refusal;       // when exactly those are missing
};
const std::vector<AlienRow>& table() {
    static const std::vector<AlienRow> t{
        {"1", "KILN", 60, {}, ""},
        {"2", "GUN SPIRE", 75, {}, ""},
        {"3", "ANTENNA", 60, {kKiln}, "Needs a Kiln"},
        {"4", "DOWER", 50, {kKiln}, "Needs a Kiln"},
        {"5", "STRONGHOLD", 70, {kForge}, "Needs a Forge"},
        {"6", "JAMMER", 50, {kOverseer}, "Needs an Overseer"},
    };
    return t;
}

TEST(AlienConstruct, TheBuildersPanelHoldsExactlySixKeyedRowsWithTheCapturedKeysLabelsAndCosts) {
    const std::vector<MenuRow> rows = alien_panel({}, 999);
    EXPECT_EQ(keyed_count(rows), 6U);
    EXPECT_EQ(rows.size(), 9U) << "TAB, ~ Rotate Buildings, the heading, six rows";
    EXPECT_EQ(rows.at(0).key, "TAB");
    EXPECT_EQ(rows.at(1).verb, "Rotate Buildings");
    EXPECT_FALSE(rows.at(1).enabled);
    EXPECT_TRUE(rows.at(2).heading);
    for (const AlienRow& want : table()) {
        const MenuRow& row = keyed(rows, want.key);
        EXPECT_EQ(row.verb, std::string{want.label} + " " + std::to_string(want.cost)) << want.key;
        EXPECT_EQ(row.cost, want.cost) << want.key;
    }
    for (const MenuRow& row : rows) {
        EXPECT_NE(row.key, "7") << "there is no seventh row, and no second page";
    }
}

TEST(AlienConstruct, EveryRowDimsAndRefusesByItsOwnPrerequisiteBuilding) {
    // Every subset of the four buildings that gate anything, with the bank never the obstacle.
    for (unsigned mask = 0; mask < 16U; ++mask) {
        StandingBuildings s{};
        s.at(kKiln) = (mask & 1U) != 0U ? 1 : 0;
        s.at(kAntenna) = (mask & 2U) != 0U ? 1 : 0;
        s.at(kForge) = (mask & 4U) != 0U ? 1 : 0;
        s.at(kOverseer) = (mask & 8U) != 0U ? 1 : 0;
        const std::vector<MenuRow> rows = alien_panel(s, 999);
        for (const AlienRow& want : table()) {
            bool met = true;
            for (const std::size_t b : want.needs) {
                met = met && s.at(b) > 0;
            }
            const MenuRow& row = keyed(rows, want.key);
            EXPECT_EQ(row.enabled, met) << want.key << " mask " << mask;
            EXPECT_EQ(row.reason, met ? MenuDim::None : MenuDim::NeedsBuilding)
                << want.key << " mask " << mask;
            if (!met) {
                EXPECT_EQ(refusal_message(row), want.refusal) << want.key << " mask " << mask;
            }
        }
    }
}

TEST(AlienConstruct, KilnAndGunSpireAreBrightWithNothingStandingAndNoPowerIsAsked) {
    const std::vector<MenuRow> rows = alien_panel({}, 999);
    EXPECT_TRUE(keyed(rows, "1").enabled);
    EXPECT_TRUE(keyed(rows, "2").enabled) << "the reference's Power Lung rule is NOT confirmed";
    for (const char* key : {"3", "4", "5", "6"}) {
        EXPECT_NE(keyed(rows, key).reason, MenuDim::NeedsPower) << key;
    }
}

TEST(AlienConstruct, ForgeAndOverseerRequirementsCannotBeMetToday) {
    // Neither an upgrade nor an alien building exists, so what the sim stands is read as ZERO
    // alien buildings: the Stronghold and the Jammer stay dim whatever is on the board.
    const std::vector<MenuRow> rows = alien_panel(StandingBuildings{}, 999);
    EXPECT_EQ(refusal_message(keyed(rows, "5")), "Needs a Forge");
    EXPECT_EQ(refusal_message(keyed(rows, "6")), "Needs an Overseer");
}

TEST(AlienConstruct, TheScrapShortfallIsTheHumanSlicesOwnSentenceAndComesAfterTheBuilding) {
    StandingBuildings s{};
    s.at(kKiln) = 1;
    const std::vector<MenuRow> rows = alien_panel(s, 10);
    EXPECT_EQ(refusal_message(keyed(rows, "1")), "Add 50 scrap (have 10, costs 60)");
    EXPECT_EQ(refusal_message(keyed(rows, "3")), "Add 50 scrap (have 10, costs 60)");
    EXPECT_EQ(refusal_message(keyed(rows, "5")), "Needs a Forge") << "the building is named first";
}

TEST(AlienConstruct, ADeadRowsNamesNeverLeakTheHumanOnes) {
    const std::vector<MenuRow> rows = alien_panel({}, 999);
    for (const MenuRow& row : rows) {
        for (const char* human : {"POWER", "FACTORY", "ARMORY", "RELAY", "TOWER", "BAY"}) {
            EXPECT_EQ(row.verb.find(human), std::string::npos) << row.verb;
        }
    }
}

TEST(AlienConstruct, AHumanConstructorStillSeesItsNineRowsAndItsOwnReasons) {
    const std::vector<MenuRow> rows = human_panel(999);
    EXPECT_EQ(keyed_count(rows), 9U);
    EXPECT_EQ(keyed(rows, "1").verb, "POWER 30");
    EXPECT_EQ(keyed(rows, "9").verb, "BOMBER BAY 100");
    EXPECT_EQ(refusal_message(keyed(rows, "2")), "Needs power");
    EXPECT_EQ(refusal_message(keyed(rows, "4")), "Needs a Factory");
    EXPECT_EQ(refusal_message(keyed(human_panel(5), "1")), "Add 25 scrap (have 5, costs 30)");
}

TEST(AlienConstruct, TheNameTableGivesTheAlienNamesAndExtractorOnBothSides) {
    EXPECT_EQ(kind_label(UnitKind::Constructor, Faction::Alien), "BUILDER");
    EXPECT_EQ(kind_label(UnitKind::Constructor, Faction::Human), "CONSTRUCTOR");
    EXPECT_EQ(kind_label(UnitKind::Scavenger, Faction::Alien), "HARVESTER");
    EXPECT_EQ(kind_label(UnitKind::Scavenger, Faction::Human), "SCAVENGER");
    EXPECT_EQ(kind_label(UnitKind::Turret, Faction::Alien), "GUARDIAN");
    EXPECT_EQ(kind_label(UnitKind::Tug, Faction::Alien), "HAULER");
    EXPECT_EQ(kind_label(UnitKind::DeployedScavenger, Faction::Alien), "EXTRACTOR");
    EXPECT_EQ(kind_label(UnitKind::DeployedScavenger, Faction::Human), "EXTRACTOR");
}

TEST(AlienConstruct, EveryLetterOfEveryPanelLabelHasAGlyphSoNoRowDrawsWithAGap) {
    // JAMMER drew as "AMMER" until the 5x5 font had a J: a missing glyph is a silent gap.
    for (const Faction faction : {Faction::Human, Faction::Alien}) {
        for (const BuildItem& item : construct_items(faction)) {
            for (const char c : std::string{item.label}) {
                if (c == ' ') {
                    continue;
                }
                EXPECT_FALSE(glyph_coverage(c, 5, 5).empty())
                    << "no glyph for '" << c << "' in " << item.label;
            }
        }
    }
}

// ------------------------------------------------------------------- through the game's route

struct Yard {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    sim::UnitId driven;
    sim::UnitId constructor;
};

[[nodiscard]] Yard make_yard(bool alien, std::uint16_t ore) {
    std::vector<sim::Spawn> units{{UnitKind::Tank, Side::Player, 60, 60},
                                  {UnitKind::Constructor, Side::Player, 61, 60},
                                  {UnitKind::Power, Side::Player, 80, 80},
                                  {UnitKind::Factory, Side::Player, 90, 90}};
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
    y.options.factions = factions_for(alien);
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

TEST(AlienConstruct, TheBuilderOpensTheSixRowPanelAndEachPressRefusesByItsReason) {
    Yard y = make_yard(true, 400);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    const std::vector<MenuRow> rows
        = app::menu_for(y.sim, session, y.options, session.attack_armed());
    EXPECT_EQ(keyed_count(rows), 6U);
    EXPECT_EQ(keyed(rows, "5").reason, MenuDim::NeedsBuilding);
    session.frame(key_frame(input::kKey1 + 2), y.sim, y.options, orders); // ANTENNA
    EXPECT_EQ(session.message(), "Needs a Kiln");
    session.frame(key_frame(input::kKey1 + 4), y.sim, y.options, orders); // STRONGHOLD
    EXPECT_EQ(session.message(), "Needs a Forge");
    session.frame(key_frame(input::kKey1 + 5), y.sim, y.options, orders); // JAMMER
    EXPECT_EQ(session.message(), "Needs an Overseer");
    EXPECT_EQ(y.sim.ore(Side::Player), 400U);
    EXPECT_FALSE(session.placing().has_value());
}

TEST(AlienConstruct, AnAffordableRowWhosePrerequisiteStandsRefusesWithTheHonestPlaceholder) {
    // The asymmetry with the human POWER, which places: no alien building can be placed yet.
    // The human power and factory the yard spawns are NOT alien buildings and must not satisfy
    // anything (they would read as slots 0 and 1, the Kiln and the Gun Spire).
    Yard y = make_yard(true, 400);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    session.frame(key_frame(input::kKey1), y.sim, y.options, orders); // KILN
    EXPECT_EQ(session.message(), "Nothing can be built yet. Placing a building is not done.");
    EXPECT_FALSE(session.placing().has_value());
    session.frame(key_frame(input::kKey1 + 2), y.sim, y.options, orders); // ANTENNA: no Kiln
    EXPECT_EQ(session.message(), "Needs a Kiln");
}

TEST(AlienConstruct, AScrapShortfallIsTheHumanSlicesSentence) {
    Yard y = make_yard(true, 20);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    session.frame(key_frame(input::kKey1 + 1), y.sim, y.options, orders); // GUN SPIRE 75
    EXPECT_EQ(session.message(), "Add 55 scrap (have 20, costs 75)");
}

TEST(AlienConstruct, AHumanConstructorInTheGameStillGetsItsNineAndItsPowerPlacement) {
    Yard y = make_yard(false, 400);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_panel(y, session, orders);
    const std::vector<MenuRow> rows
        = app::menu_for(y.sim, session, y.options, session.attack_armed());
    EXPECT_EQ(keyed_count(rows), 9U);
    session.frame(key_frame(input::kKey1), y.sim, y.options, orders); // POWER arms
    EXPECT_TRUE(session.placing().has_value());
}

} // namespace
} // namespace protekt::game
