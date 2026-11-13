// ROUND 18, K6: THE ALIEN BUILDINGS exist -- the Builder's six, and NO power grid on that side.
//
// The Builder's panel (the owner's capture): KILN 60, GUN SPIRE 75, ANTENNA 60, DOWER 50,
// STRONGHOLD 70, JAMMER 50. Each goes up through the SAME job as a human building (K2/K3): the
// press pays, a site appears, the Constructor drives to it within one tile, and the building rises
// on the tick timer. The gate on this side is a BUILDING, never power (his ruling), so no row here
// asks the human power or connection rule.
//
// THE ALIEN FOOTPRINTS ARE PROVISIONAL: the owner has not given them. The numbers below are the
// ones chosen, written as literals and never read back from the table under test:
//   KILN 2x2, GUN SPIRE 1x1, ANTENNA 1x1, DOWER 2x2, STRONGHOLD 2x1, JAMMER 1x1.
//
// NOT BUILT, and no test here pretends otherwise: the Kiln produces nothing, the Gun Spire does not
// shoot, the Antenna detects nothing, the Dower services and repairs nothing, the Jammer jams
// nothing, the Stronghold constructs no weapons and no shields, and the Power Lungs do not exist.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/faction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    sim::UnitId driven;
    sim::UnitId constructor;
};

/// A tank, an ALIEN Constructor (the Builder), the `extra` buildings spawned whole, and `ore`
/// banked. The reticle is at the bottom of the screen, as in K3's rig.
[[nodiscard]] Rig make_rig(std::uint16_t ore, const std::vector<sim::Spawn>& extra = {}) {
    std::vector<sim::Spawn> spawns{{UnitKind::Tank, Side::Player, 60, 60},
                                   {UnitKind::Constructor, Side::Player, 61, 60}};
    spawns.insert(spawns.end(), extra.begin(), extra.end());
    sim::Scenario scenario{kSeed, spawns};
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    Rig r{};
    r.sim = sim::Sim::create(scenario);
    r.driven = r.sim.units().at(0).id;
    r.constructor = r.sim.units().at(1).id;
    r.options.seed = kSeed;
    r.options.extent = Extent{1280, 720};
    r.options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
    r.options.viewer_unit = r.driven;
    r.options.aim = Aim{640.0F, 700.0F};
    r.options.factions = factions_for(true);
    r.row.update(r.sim, Side::Player, 0);
    r.options.row = &r.row;
    return r;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
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

TEST(AlienBuildingPanel, EveryAlienRowNamesAKindAndTheCostDrawnIsTheCostCharged) {
    // The first assertion K6 watched RED: before K6 every alien row was a NAME row with no kind.
    const std::span<const BuildItem> items = construct_items(Faction::Alien);
    ASSERT_EQ(items.size(), 6U);
    constexpr std::array<std::string_view, 6> kLabels{"KILN",  "GUN SPIRE",  "ANTENNA",
                                                      "DOWER", "STRONGHOLD", "JAMMER"};
    constexpr std::array<int, 6> kCosts{60, 75, 60, 50, 70, 50};
    for (std::size_t i = 0; i < items.size(); ++i) {
        EXPECT_EQ(items[i].label, kLabels.at(i));
        EXPECT_EQ(items[i].cost, kCosts.at(i)) << items[i].label;
        EXPECT_TRUE(items[i].kind.has_value()) << items[i].label << " names no kind";
    }
}

TEST(AlienBuildingPanel, PressingKilnArmsItAndNothingIsChargedUntilSpacePlacesIt) {
    Rig r = make_rig(400);
    Session session{r.driven};
    std::vector<sim::Order> orders;
    open_panel(r, session, orders);
    press(r, session, orders, input::kKey1); // KILN
    EXPECT_TRUE(session.placing().has_value()) << session.message();
    EXPECT_EQ(r.sim.ore(Side::Player), 400U);
}

// ------------------------------------------------------------------------------ the six, literal

/// One of the six: kind, kind NUMBER, the PROVISIONAL footprint, cost and build time as literals.
struct Want {
    UnitKind kind;
    int number;
    int width;
    int height;
    int cost;
    int ticks;
    const char* name;
};

constexpr std::array<Want, 6> kSix{{
    {UnitKind::Kiln, 29, 8, 8, 60, 1200, "KILN"},
    {UnitKind::GunSpire, 30, 4, 4, 75, 1500, "GUN SPIRE"},
    {UnitKind::Antenna, 31, 4, 4, 60, 1200, "ANTENNA"},
    {UnitKind::Dower, 32, 8, 8, 50, 1000, "DOWER"},
    {UnitKind::Stronghold, 33, 8, 4, 70, 1400, "STRONGHOLD"},
    {UnitKind::Jammer, 34, 4, 4, 50, 1000, "JAMMER"},
}};

constexpr int kBlockW = 16;
constexpr int kBlockH = 14;

struct Block {
    int x{};
    int y{};
};

[[nodiscard]] Block open_block() {
    for (int by = 20; by < 200; by += 2) {
        for (int bx = 20; bx < 200; bx += 2) {
            bool clear = true;
            for (int dy = 0; dy < kBlockH && clear; ++dy) {
                for (int dx = 0; dx < kBlockW && clear; ++dx) {
                    clear = sim::tile_passable(kSeed, bx + dx, by + dy);
                }
            }
            if (clear) {
                return Block{bx, by};
            }
        }
    }
    return Block{-1, -1};
}

struct Yard {
    sim::Sim sim;
    Block block;
    sim::UnitId constructor;
};

[[nodiscard]] Yard make_yard(int dx, int dy, std::uint16_t ore = 400) {
    Yard y{};
    y.block = open_block();
    sim::Scenario scenario{kSeed,
                           {{UnitKind::Constructor, Side::Player, y.block.x + dx, y.block.y + dy}}};
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    y.sim = sim::Sim::create(scenario);
    y.constructor = y.sim.units().at(0).id;
    return y;
}

[[nodiscard]] sim::Order place(sim::UnitId who, UnitKind kind, int x, int y, bool turned = false) {
    return sim::Order{who, sim::orders::Place{kind, x, y, turned}};
}

[[nodiscard]] const sim::Unit* first_of(const sim::Sim& s, UnitKind kind) {
    for (const sim::Unit& u : s.units()) {
        if (u.alive && u.kind == kind) {
            return &u;
        }
    }
    return nullptr;
}

[[nodiscard]] std::size_t count_of(const sim::Sim& s, UnitKind kind) {
    std::size_t n = 0;
    for (const sim::Unit& u : s.units()) {
        n += (u.alive && u.kind == kind) ? 1U : 0U;
    }
    return n;
}

TEST(AlienBuildings, TheSixKindsAreAppendedAfterBomberBayAndNothingBeforeThemMoved) {
    for (const Want& w : kSix) {
        EXPECT_EQ(static_cast<int>(w.kind), w.number) << w.name;
    }
    EXPECT_EQ(static_cast<int>(UnitKind::BomberBay), 28) << "the human nine did not move";
    EXPECT_EQ(static_cast<int>(UnitKind::Power), 21);
    EXPECT_EQ(sim::kUnitKindCount, 35) << "29 before K6, six kinds appended";
    EXPECT_EQ(static_cast<int>(UnitKind::Jammer), sim::kUnitKindCount - 1);
}

TEST(AlienBuildings, EachHasASpecRowAndNoneOfThemDoesAnything) {
    for (const Want& w : kSix) {
        const sim::UnitSpec& row = sim::spec_of(w.kind);
        EXPECT_EQ(static_cast<int>(row.cost), w.cost) << w.name;
        EXPECT_EQ(static_cast<int>(row.build_ticks), w.ticks) << w.name;
        EXPECT_GT(row.hp, 0U) << w.name;
        // THE FUNCTIONS ARE NOT BUILT: the Gun Spire does not shoot, so nothing here is armed.
        EXPECT_EQ(row.damage, 0) << w.name << ": unarmed -- its function is not built";
        EXPECT_EQ(row.weapon, sim::WeaponId::None) << w.name;
        EXPECT_EQ(row.max_speed.raw, 0) << w.name << ": it never moves";
        EXPECT_TRUE(sim::is_placeable(w.kind)) << w.name;
        EXPECT_TRUE(sim::has_footprint(w.kind)) << w.name;
        EXPECT_FALSE(sim::is_queueable(w.kind)) << w.name << ": a Builder places it";
    }
}

TEST(AlienBuildings, TheFootprintsAreTheProvisionalOnesChosenAndOneIsARectangle) {
    for (const Want& w : kSix) {
        EXPECT_EQ(sim::footprint_of(w.kind), (sim::FootprintSize{w.width, w.height})) << w.name;
    }
    EXPECT_NE(sim::footprint_of(UnitKind::Stronghold).width,
              sim::footprint_of(UnitKind::Stronghold).height);
}

// ------------------------------------------------------------------------------ the same job

TEST(AlienBuildings, EachRaisesASiteOnItsOwnFootprintAndChargesExactlyItsCost) {
    for (const Want& w : kSix) {
        Yard y = make_yard(1, 1);
        const int x = y.block.x + 5;
        const int z = y.block.y + 5;
        y.sim.enqueue(place(y.constructor, w.kind, x, z));
        y.sim.step();
        const sim::Unit* site = first_of(y.sim, w.kind);
        ASSERT_NE(site, nullptr) << w.name;
        EXPECT_TRUE(sim::under_construction(*site)) << w.name << ": a site, not yet a building";
        const sim::Footprint f = sim::footprint_around(*site);
        EXPECT_EQ(f.x, x) << w.name;
        EXPECT_EQ(f.y, z) << w.name;
        EXPECT_EQ(f.width, w.width) << w.name;
        EXPECT_EQ(f.height, w.height) << w.name;
        EXPECT_EQ(static_cast<int>(y.sim.ore(Side::Player)), 400 - w.cost) << w.name;
    }
}

TEST(AlienBuildings, EachIsBuiltByTheConstructorDrivingToItAndStandsAtTheEndOfItsTimer) {
    for (const Want& w : kSix) {
        Yard y = make_yard(12, 1);
        y.sim.enqueue(place(y.constructor, w.kind, y.block.x + 4, y.block.y + 4));
        y.sim.step();
        const sim::Unit* site = first_of(y.sim, w.kind);
        ASSERT_NE(site, nullptr) << w.name;
        EXPECT_EQ(sim::site_builder(y.sim.units(), *site), nullptr)
            << w.name << ": the Constructor is not within one tile yet";
        bool stood = false;
        for (int tick = 0; tick < w.ticks + 600 && !stood; ++tick) {
            y.sim.step();
            stood = !sim::under_construction(*first_of(y.sim, w.kind));
        }
        EXPECT_TRUE(stood) << w.name;
        EXPECT_EQ(static_cast<int>(y.sim.ore(Side::Player)), 400 - w.cost)
            << w.name << ": charged once";
    }
}

TEST(AlienBuildings, ARefusedPlacementChargesNothingAndRaisesNothing) {
    for (const Want& w : kSix) {
        {
            Yard y = make_yard(1, 1, static_cast<std::uint16_t>(w.cost - 1));
            y.sim.enqueue(place(y.constructor, w.kind, y.block.x + 5, y.block.y + 5));
            y.sim.step();
            EXPECT_EQ(first_of(y.sim, w.kind), nullptr) << w.name << ": one scrap short";
            EXPECT_EQ(static_cast<int>(y.sim.ore(Side::Player)), w.cost - 1) << w.name;
        }
        {
            Yard y = make_yard(1, 1);
            y.sim.enqueue(place(y.constructor, w.kind, y.block.x + 5, y.block.y + 5));
            y.sim.step();
            const auto after_first = y.sim.ore(Side::Player);
            y.sim.enqueue(place(y.constructor, w.kind, y.block.x + 5, y.block.y + 5));
            y.sim.step();
            EXPECT_EQ(count_of(y.sim, w.kind), 1U) << w.name << ": the overlap is refused";
            EXPECT_EQ(y.sim.ore(Side::Player), after_first) << w.name;
        }
    }
}

TEST(AlienBuildings, TheStrongholdTurnedAQuarterIsFourByEightAndTheSquaresAreUnchanged) {
    Yard y = make_yard(1, 1);
    const Block b = y.block;
    y.sim.enqueue(place(y.constructor, UnitKind::Stronghold, b.x + 5, b.y + 5, true));
    y.sim.step();
    const sim::Unit* turned = first_of(y.sim, UnitKind::Stronghold);
    ASSERT_NE(turned, nullptr);
    const sim::Footprint f = sim::footprint_around(*turned);
    EXPECT_EQ(f.width, 4) << "8x4 turned a quarter is 4x8 (round 19, L1: four times 2x1)";
    EXPECT_EQ(f.height, 8);
    for (const Want& w : kSix) {
        if (w.kind != UnitKind::Stronghold) {
            EXPECT_EQ(w.width, w.height) << w.name << ": a square, turning changes nothing";
        }
    }
}

// ------------------------------------------------------------------------------ the gate

[[nodiscard]] std::vector<MenuRow> alien_rows(const sim::Sim& sim, int ore,
                                              bool powered_node = false) {
    MenuInput in;
    in.subject = MenuSubject::Constructor;
    in.faction = Faction::Alien;
    in.construct_panel = true;
    in.ore = ore;
    in.standing = standing_buildings(sim, Side::Player, Faction::Alien);
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

TEST(AlienBuildingGate, AStandingKilnOpensTheAntennaAndTheDowerAndAnAbsentOneRefusesNamingIt) {
    const Rig none = make_rig(400);
    const std::vector<MenuRow> dim = alien_rows(none.sim, 400);
    for (const char* key : {"3", "4"}) {
        EXPECT_FALSE(keyed(dim, key).enabled) << key << ": drawn dim";
        EXPECT_EQ(keyed(dim, key).reason, MenuDim::NeedsBuilding) << key;
        EXPECT_EQ(refusal_message(keyed(dim, key)), "Needs a Kiln") << key;
    }
    const Rig with = make_rig(400, {{UnitKind::Kiln, Side::Player, 40, 40}});
    EXPECT_EQ(standing_buildings(with.sim, Side::Player, Faction::Alien).at(0), 1);
    const std::vector<MenuRow> bright = alien_rows(with.sim, 400);
    for (const char* key : {"3", "4"}) {
        EXPECT_TRUE(keyed(bright, key).enabled) << key;
        EXPECT_EQ(keyed(bright, key).reason, MenuDim::None) << key;
    }
    // A Kiln alone does not open the two rows that need the UPGRADED forms.
    EXPECT_EQ(refusal_message(keyed(bright, "5")), "Needs a Forge");
    EXPECT_EQ(refusal_message(keyed(bright, "6")), "Needs an Overseer");
}

TEST(AlienBuildingGate, AnAntennaAloneDoesNotOpenTheJammerAndTheKilnAndGunSpireNeedNothing) {
    const Rig r = make_rig(
        400, {{UnitKind::Kiln, Side::Player, 40, 40}, {UnitKind::Antenna, Side::Player, 44, 40}});
    const std::vector<MenuRow> rows = alien_rows(r.sim, 400);
    EXPECT_EQ(refusal_message(keyed(rows, "6")), "Needs an Overseer")
        << "an Antenna is not an Overseer: that is its UPGRADE, which is not built";
    EXPECT_TRUE(keyed(rows, "1").enabled);
    EXPECT_TRUE(keyed(rows, "2").enabled);
    const std::vector<MenuRow> bare = alien_rows(make_rig(400).sim, 400);
    EXPECT_TRUE(keyed(bare, "1").enabled);
    EXPECT_TRUE(keyed(bare, "2").enabled);
}

TEST(AlienBuildingGate, ASiteStillBeingBuiltSatisfiesNothing) {
    Yard y = make_yard(1, 1);
    y.sim.enqueue(place(y.constructor, UnitKind::Kiln, y.block.x + 5, y.block.y + 5));
    y.sim.step();
    ASSERT_TRUE(sim::under_construction(*first_of(y.sim, UnitKind::Kiln)));
    EXPECT_EQ(standing_buildings(y.sim, Side::Player, Faction::Alien).at(0), 0);
    EXPECT_FALSE(keyed(alien_rows(y.sim, 400), "3").enabled);
}

TEST(AlienBuildingGate, TheSidesDoNotReadEachOthersBuildings) {
    // An alien Kiln is no human building, and a human plant, factory or relay is no alien one.
    const Rig mixed = make_rig(400, {{UnitKind::Kiln, Side::Player, 40, 40},
                                     {UnitKind::Power, Side::Player, 30, 30},
                                     {UnitKind::Factory, Side::Player, 34, 30},
                                     {UnitKind::RelayBunker, Side::Player, 38, 30}});
    const StandingBuildings human = standing_buildings(mixed.sim, Side::Player, Faction::Human);
    const StandingBuildings alien = standing_buildings(mixed.sim, Side::Player, Faction::Alien);
    EXPECT_EQ(human, (StandingBuildings{1, 1, 1, 0, 0, 0, 0, 0, 0})) << "the Kiln adds nothing";
    EXPECT_EQ(alien, (StandingBuildings{1, 0, 0, 0, 0, 0, 0, 0, 0}))
        << "only the Kiln: the plant, factory and relay sit in slots 0, 1 and 2 of the OTHER enum";
}

TEST(AlienBuildingGate, NoAlienRowConsultsThePowerRuleOrTheConnectionRule) {
    for (std::size_t row = 0; row < 6; ++row) {
        const ConstructRequirement need = construct_requirement(Faction::Alien, row);
        EXPECT_FALSE(need.power) << "row " << row;
    }
    for (const Want& w : kSix) {
        EXPECT_FALSE(draws_power(w.kind)) << w.name << ": the alien side has no power grid";
    }
    // The rows read the same with a powered building standing or not, with the connection
    // flag set or not, and with human plants, relays and a factory on the board or not.
    const Rig bare = make_rig(400, {{UnitKind::Kiln, Side::Player, 40, 40}});
    const Rig grid = make_rig(400, {{UnitKind::Kiln, Side::Player, 40, 40},
                                    {UnitKind::Power, Side::Player, 30, 30},
                                    {UnitKind::Power, Side::Player, 33, 30},
                                    {UnitKind::Power, Side::Player, 36, 30},
                                    {UnitKind::Factory, Side::Player, 30, 34},
                                    {UnitKind::RelayBunker, Side::Player, 41, 40}});
    ASSERT_TRUE(power_stands(grid.sim, Side::Player)) << "a powered human network does stand";
    const std::vector<MenuRow> a = alien_rows(bare.sim, 400, false);
    const std::vector<MenuRow> b = alien_rows(grid.sim, 400, true);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a.at(i).enabled, b.at(i).enabled) << a.at(i).verb;
        EXPECT_EQ(a.at(i).reason, b.at(i).reason) << a.at(i).verb;
        EXPECT_NE(a.at(i).reason, MenuDim::NeedsPower) << a.at(i).verb;
        EXPECT_NE(a.at(i).reason, MenuDim::NeedsConnection) << a.at(i).verb;
        EXPECT_NE(b.at(i).reason, MenuDim::NeedsPower) << b.at(i).verb;
        EXPECT_NE(b.at(i).reason, MenuDim::NeedsConnection) << b.at(i).verb;
    }
}

TEST(AlienBuildingGate, AnAlienBuildingIsNotInThePowerNetworkAndConnectsToNothing) {
    const Rig r = make_rig(
        400, {{UnitKind::Kiln, Side::Player, 40, 40}, {UnitKind::Antenna, Side::Player, 42, 40}});
    EXPECT_TRUE(network_of(r.sim, Side::Player).empty())
        << "no alien building is a source, a conductor or a terminal of the human grid";
    EXPECT_FALSE(power_stands(r.sim, Side::Player));
}

// ------------------------------------------------------------------------------ through the game

TEST(AlienBuildingPanel, EachOfTheFourWhoseGateCanBeMetArmsOnItsFootprintAndSpacePlacesIt) {
    // The Stronghold and the Jammer need a FORGE and an OVERSEER, the two UPGRADES, which are not
    // built: their presses are refused (below) and their raising is pinned at the sim above.
    struct Press {
        std::int32_t key;
        std::size_t want; ///< index into kSix
    };
    constexpr std::array<Press, 4> kPresses{
        {{input::kKey1, 0}, {input::kKey1 + 1, 1}, {input::kKey1 + 2, 2}, {input::kKey1 + 3, 3}}};
    for (const Press& p : kPresses) {
        const Want& w = kSix.at(p.want);
        // A Kiln stands far from the aim and nothing powers anything: no power is asked.
        Rig r = make_rig(400, {{UnitKind::Kiln, Side::Player, 30, 30}});
        Session session{r.driven};
        std::vector<sim::Order> orders;
        open_panel(r, session, orders);
        press(r, session, orders, p.key);
        ASSERT_TRUE(session.placing().has_value()) << w.name << ": " << session.message();
        const Placing armed = session.placing().value_or(Placing{});
        EXPECT_EQ(armed.kind, w.kind) << w.name;
        EXPECT_EQ(armed.cost, w.cost) << w.name;
        EXPECT_EQ(armed.size, (sim::FootprintSize{w.width, w.height})) << w.name;
        EXPECT_EQ(r.sim.ore(Side::Player), 400U) << w.name << ": arming charges nothing";
        orders.clear();
        press(r, session, orders, input::kKeySpace);
        const sim::orders::Place* order = nullptr;
        for (const sim::Order& o : orders) {
            if (o.unit == r.constructor) {
                order = std::get_if<sim::orders::Place>(&o.payload);
                r.sim.enqueue(o);
            }
        }
        ASSERT_NE(order, nullptr) << w.name << ": " << session.message();
        EXPECT_EQ(order->kind, w.kind) << w.name;
        const std::size_t before = count_of(r.sim, w.kind);
        r.sim.step();
        EXPECT_EQ(count_of(r.sim, w.kind), before + 1U) << w.name << ": a site stands";
        EXPECT_EQ(static_cast<int>(r.sim.ore(Side::Player)), 400 - w.cost) << w.name;
    }
}

TEST(AlienBuildingPanel, ARowMissingItsPrerequisiteRefusesNamingItAndChargesNothing) {
    struct Refuse {
        std::int32_t key;
        const char* message;
    };
    constexpr std::array<Refuse, 4> kRefusals{{{input::kKey1 + 2, "Needs a Kiln"},
                                               {input::kKey1 + 3, "Needs a Kiln"},
                                               {input::kKey1 + 4, "Needs a Forge"},
                                               {input::kKey1 + 5, "Needs an Overseer"}}};
    for (const Refuse& want : kRefusals) {
        Rig r = make_rig(400);
        Session session{r.driven};
        std::vector<sim::Order> orders;
        open_panel(r, session, orders);
        press(r, session, orders, want.key);
        EXPECT_EQ(session.message(), want.message);
        EXPECT_FALSE(session.placing().has_value());
        EXPECT_EQ(r.sim.ore(Side::Player), 400U);
    }
}

TEST(AlienBuildingPanel, TheHumanNineAreUnchanged) {
    struct Nine {
        const char* label;
        int cost;
        UnitKind kind;
        std::uint16_t buildings; ///< bit = the human slot of the building needed
        bool power;
    };
    constexpr std::array<Nine, 9> kNine{{
        {"POWER", 30, UnitKind::Power, 0, false},
        {"FACTORY", 55, UnitKind::Factory, 0, true},
        {"RELAY BUNKER", 50, UnitKind::RelayBunker, 0, true},
        {"ARMORY", 60, UnitKind::Armory, 1U << 1, false},
        {"SERVICE BAY", 50, UnitKind::ServiceBay, 1U << 1, false},
        {"GUN TOWER", 50, UnitKind::GunTower, 0, true},
        {"TRAINING CTR.", 70, UnitKind::TrainingCentre, 1U << 3, false},
        {"TECH CENTRE", 80, UnitKind::TechCentre, (1U << 3) | (1U << 4), false},
        {"BOMBER BAY", 100, UnitKind::BomberBay, 1U << 6, false},
    }};
    const std::span<const BuildItem> items = construct_items(Faction::Human);
    ASSERT_EQ(items.size(), 9U);
    for (std::size_t i = 0; i < kNine.size(); ++i) {
        const Nine& n = kNine.at(i);
        EXPECT_EQ(items[i].label, n.label);
        EXPECT_EQ(items[i].cost, n.cost) << n.label;
        ASSERT_TRUE(items[i].kind.has_value()) << n.label;
        EXPECT_EQ(items[i].kind.value_or(UnitKind::Tank), n.kind) << n.label;
        const ConstructRequirement need = construct_requirement(Faction::Human, i);
        EXPECT_EQ(need.buildings, n.buildings) << n.label;
        EXPECT_EQ(need.power, n.power) << n.label;
        EXPECT_EQ(draws_power(n.kind), n.power) << n.label;
    }
}

// ------------------------------------------------------------------------------ the looks

using Quantised = std::tuple<int, int, int>;

[[nodiscard]] SceneOptions view(int tile_x, int tile_y) {
    SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 14;
    o.camera.x = static_cast<float>(tile_x + 2) * kMetresPerTile;
    o.camera.z = static_cast<float>(tile_y - 8) * kMetresPerTile;
    o.camera.y = 9.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

[[nodiscard]] std::set<Quantised> positions(const Scene& scene) {
    std::set<Quantised> out;
    for (const SceneVertex& v : scene.vertices) {
        out.emplace(static_cast<int>(std::lround(v.position.at(0) * 100.0F)),
                    static_cast<int>(std::lround(v.position.at(1) * 100.0F)),
                    static_cast<int>(std::lround(v.position.at(2) * 100.0F)));
    }
    return out;
}

struct Shape {
    std::set<Quantised> parts;
    float low_x{1.0e9F};
    float high_x{-1.0e9F};
    float low_z{1.0e9F};
    float high_z{-1.0e9F};
};

[[nodiscard]] Shape built_shape(UnitKind kind) {
    const Block b = open_block();
    const int x = b.x + 5;
    const int y = b.y + 5;
    sim::Scenario with{kSeed, {{kind, Side::Player, x, y}}};
    const sim::Scenario without{kSeed, {{UnitKind::Constructor, Side::Player, b.x + 12, y}}};
    sim::Sim a = sim::Sim::create(with);
    const sim::Sim base = sim::Sim::create(without);
    const SceneOptions options = view(x, y);
    const std::set<Quantised> in_with = positions(build_scene(a, options));
    const std::set<Quantised> in_base = positions(build_scene(base, options));
    const sim::Unit& unit = a.units().at(0);
    const float centre_x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float centre_z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    Shape shape;
    for (const Quantised& p : in_with) {
        if (in_base.count(p) != 0U) {
            continue;
        }
        const float px = static_cast<float>(std::get<0>(p)) / 100.0F - centre_x;
        const float pz = static_cast<float>(std::get<2>(p)) / 100.0F - centre_z;
        if (std::get<1>(p) > 0) {
            shape.parts.emplace(static_cast<int>(std::lround(px * 100.0F)), std::get<1>(p),
                                static_cast<int>(std::lround(pz * 100.0F)));
            shape.low_x = std::min(shape.low_x, px);
            shape.high_x = std::max(shape.high_x, px);
            shape.low_z = std::min(shape.low_z, pz);
            shape.high_z = std::max(shape.high_z, pz);
        }
    }
    return shape;
}

TEST(AlienBuildingLooks, TheSixAreDrawnAndNoTwoShareAModelAmongThemselvesOrWithTheHumanOnes) {
    constexpr std::array<UnitKind, 15> kAll{
        UnitKind::Kiln,           UnitKind::GunSpire,   UnitKind::Antenna,    UnitKind::Dower,
        UnitKind::Stronghold,     UnitKind::Jammer,     UnitKind::Power,      UnitKind::Factory,
        UnitKind::RelayBunker,    UnitKind::Armory,     UnitKind::ServiceBay, UnitKind::GunTower,
        UnitKind::TrainingCentre, UnitKind::TechCentre, UnitKind::BomberBay};
    std::vector<Shape> shapes;
    for (const UnitKind kind : kAll) {
        shapes.push_back(built_shape(kind));
        EXPECT_GT(shapes.back().parts.size(), 24U) << static_cast<int>(kind) << " draws something";
    }
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        for (std::size_t j = i + 1; j < shapes.size(); ++j) {
            EXPECT_NE(shapes.at(i).parts, shapes.at(j).parts)
                << static_cast<int>(kAll.at(i)) << " and " << static_cast<int>(kAll.at(j))
                << " share a model";
        }
    }
}

TEST(AlienBuildingLooks, EachModelStaysInsideItsOwnFootprint) {
    for (const Want& w : kSix) {
        const Shape shape = built_shape(w.kind);
        const float half_x = static_cast<float>(w.width) * kMetresPerTile * 0.5F + 0.05F;
        const float half_z = static_cast<float>(w.height) * kMetresPerTile * 0.5F + 0.05F;
        EXPECT_LE(std::max(-shape.low_x, shape.high_x), half_x) << w.name;
        EXPECT_LE(std::max(-shape.low_z, shape.high_z), half_z) << w.name;
    }
}

TEST(AlienBuildingLooks, NoAlienModelIsMadeOfTheHumanBuildingsBoxesAlone) {
    // The art directive: everything alien looks unusual. A box has axis-aligned faces only; each
    // alien model must add faces tilted off every axis (a loft's facets). Counted over what the
    // building ADDS to a scene, so the terrain's own slopes are not in it.
    const auto slanted = [](const Scene& scene) {
        std::size_t n = 0;
        for (const SceneVertex& v : scene.vertices) {
            const bool lit
                = v.normal.at(0) != 0.0F || v.normal.at(1) != 0.0F || v.normal.at(2) != 0.0F;
            const bool axis = std::abs(v.normal.at(0)) > 0.98F || std::abs(v.normal.at(1)) > 0.98F
                              || std::abs(v.normal.at(2)) > 0.98F;
            n += (lit && !axis) ? 1U : 0U;
        }
        return n;
    };
    const Block b = open_block();
    const int x = b.x + 5;
    const int y = b.y + 5;
    const sim::Scenario without{kSeed, {{UnitKind::Constructor, Side::Player, b.x + 12, y}}};
    const std::size_t base = slanted(build_scene(sim::Sim::create(without), view(x, y)));
    for (const Want& w : kSix) {
        sim::Scenario scenario{
            kSeed,
            {{w.kind, Side::Player, x, y}, {UnitKind::Constructor, Side::Player, b.x + 12, y}}};
        const std::size_t with = slanted(build_scene(sim::Sim::create(scenario), view(x, y)));
        EXPECT_GT(with, base + 40U) << w.name << ": it reads as a box";
    }
}

} // namespace
} // namespace protekt::game
