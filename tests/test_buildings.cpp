// ROUND 18, K3: THE OTHER EIGHT BUILDINGS exist -- a kind, a footprint, a look and the SAME job
// (K2) each: the press pays, a site appears, the Constructor drives to it within one tile, and the
// building rises on the tick timer. The owner read the footprints off the game (2026-11-12):
//
//   FACTORY 2x2, RELAY BUNKER 1x1 (PROVISIONAL, he did not name it), ARMORY 1x1, SERVICE BAY 2x2,
//   GUN TOWER 1x1 (PROVISIONAL), TRAINING CTR. 2x1, TECH CENTRE 2x1, BOMBER BAY 2x2.
//
// Every number below is a LITERAL, never read back from the table under test.
//
// NOT BUILT, and no test here pretends otherwise (the gun tower's gun is K5, test_gun_tower.cpp):
// the service bay repairs nothing, the bomber bay holds no bomber, the relay bunker relays nothing,
// the armory unlocks nothing, the training centre and the tech centre do nothing beyond their
// prerequisites.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using sim::Fixed;
using sim::FootprintSize;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kBlockW = 16;
constexpr int kBlockH = 14;

/// One of the eight: the kind, its kind NUMBER, the owner's footprint and the cost, as literals.
struct Want {
    UnitKind kind;
    int number;
    int width;
    int height;
    int cost;
    const char* name;
};

constexpr std::array<Want, 8> kEight{{
    {UnitKind::Factory, 2, 2, 2, 55, "FACTORY"},
    {UnitKind::RelayBunker, 22, 1, 1, 50, "RELAY BUNKER"},
    {UnitKind::Armory, 23, 1, 1, 60, "ARMORY"},
    {UnitKind::ServiceBay, 24, 2, 2, 50, "SERVICE BAY"},
    {UnitKind::GunTower, 25, 1, 1, 50, "GUN TOWER"},
    {UnitKind::TrainingCentre, 26, 2, 1, 70, "TRAINING CTR."},
    {UnitKind::TechCentre, 27, 2, 1, 80, "TECH CENTRE"},
    {UnitKind::BomberBay, 28, 2, 2, 100, "BOMBER BAY"},
}};

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

/// A Constructor `dx`, `dy` tiles into an open block, with `ore` banked.
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

} // namespace

// ------------------------------------------------------------------------------ kinds and spec

TEST(Buildings, TheEightKindsAreAppendedNeverInsertedAndTheCountMovedDeliberately) {
    for (const Want& w : kEight) {
        EXPECT_EQ(static_cast<int>(w.kind), w.number) << w.name;
    }
    EXPECT_EQ(static_cast<int>(UnitKind::Power), 21) << "the earlier kinds did not move";
    EXPECT_EQ(sim::kUnitKindCount, 29)
        << "22 before K3, seven kinds appended (the Factory existed)";
    EXPECT_EQ(static_cast<int>(UnitKind::BomberBay), sim::kUnitKindCount - 1);
}

TEST(Buildings, EachHasASpecRowWithItsOwnersCostAndABuildTimeAndNoProducerQueuesIt) {
    for (const Want& w : kEight) {
        const sim::UnitSpec& row = sim::spec_of(w.kind);
        EXPECT_EQ(static_cast<int>(row.cost), w.cost) << w.name;
        EXPECT_GT(row.build_ticks, 0U) << w.name;
        EXPECT_GT(row.hp, 0U) << w.name;
        if (w.kind == UnitKind::GunTower) {
            EXPECT_GT(row.damage, 0) << w.name << ": the one armed building (K5)";
        } else {
            EXPECT_EQ(row.damage, 0) << w.name << ": unarmed -- its function is not built";
        }
        EXPECT_TRUE(sim::is_placeable(w.kind)) << w.name;
        EXPECT_FALSE(sim::is_queueable(w.kind)) << w.name << ": a Constructor places it";
    }
    EXPECT_TRUE(sim::is_placeable(UnitKind::Power));
    EXPECT_FALSE(sim::is_placeable(UnitKind::Tank));
}

TEST(Buildings, TheFootprintsAreTheOwnersAndTwoOfThemAreRectangles) {
    for (const Want& w : kEight) {
        EXPECT_EQ(sim::footprint_of(w.kind), (FootprintSize{w.width, w.height})) << w.name;
    }
    EXPECT_NE(sim::footprint_of(UnitKind::TrainingCentre).width,
              sim::footprint_of(UnitKind::TrainingCentre).height);
    EXPECT_NE(sim::footprint_of(UnitKind::TechCentre).width,
              sim::footprint_of(UnitKind::TechCentre).height);
}

// ------------------------------------------------------------------------------ the job

TEST(Buildings, EachRaisesASiteOnItsOwnFootprintAndChargesExactlyItsCost) {
    for (const Want& w : kEight) {
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
        EXPECT_TRUE(sim::has_footprint(*site)) << w.name;
    }
}

TEST(Buildings, EachIsBuiltByTheConstructorDrivingToItAndStandsAtTheEndOfItsOwnTimer) {
    for (const Want& w : kEight) {
        Yard y = make_yard(12, 1); // well out of reach of the site
        const int x = y.block.x + 4;
        const int z = y.block.y + 4;
        y.sim.enqueue(place(y.constructor, w.kind, x, z));
        y.sim.step();
        const sim::Unit* site = first_of(y.sim, w.kind);
        ASSERT_NE(site, nullptr) << w.name;
        EXPECT_FALSE(sim::site_builder(y.sim.units(), *site) != nullptr)
            << w.name << ": the Constructor is not within one tile yet";
        const int total = sim::spec_of(w.kind).build_ticks;
        bool stood = false;
        for (int tick = 0; tick < total + 600 && !stood; ++tick) {
            y.sim.step();
            stood = !sim::under_construction(*first_of(y.sim, w.kind));
        }
        EXPECT_TRUE(stood) << w.name << ": the Constructor drove there and built it";
        EXPECT_EQ(static_cast<int>(y.sim.ore(Side::Player)), 400 - w.cost)
            << w.name << ": charged once, not again at the end";
    }
}

TEST(Buildings, ARefusedPlacementChargesNothingAndRaisesNothing) {
    for (const Want& w : kEight) {
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
            y.sim.enqueue(place(y.constructor, w.kind, y.block.x + 5, y.block.y + 5)); // on itself
            y.sim.step();
            EXPECT_EQ(count_of(y.sim, w.kind), 1U) << w.name << ": the overlap is refused";
            EXPECT_EQ(y.sim.ore(Side::Player), after_first) << w.name << ": and costs nothing";
        }
        {
            Yard y = make_yard(1, 1);
            y.sim.enqueue(place(y.constructor, w.kind, -1, 5));
            y.sim.step();
            EXPECT_EQ(first_of(y.sim, w.kind), nullptr) << w.name << ": off the map";
            EXPECT_EQ(static_cast<int>(y.sim.ore(Side::Player)), 400) << w.name;
        }
    }
}

// ------------------------------------------------------------------------------ rectangles

namespace {

/// Spawns around one tile `(x, y)` so exactly the tiles in `gap` (offsets from it) are free in
/// the ring of Armories (1x1 each) one tile wide round it.
[[nodiscard]] sim::Sim walled_gap(int x, int y, const std::vector<std::pair<int, int>>& gap) {
    std::vector<sim::Spawn> spawns;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 2; ++dx) {
            const bool free_tile = std::any_of(gap.begin(), gap.end(), [&](const auto& g) {
                return g.first == dx && g.second == dy;
            });
            if (!free_tile) {
                spawns.push_back({UnitKind::Armory, Side::Player, x + dx, y + dy});
            }
        }
    }
    sim::Scenario scenario{kSeed, spawns};
    return sim::Sim::create(scenario);
}

} // namespace

TEST(BuildingRectangles, ATwoByOneSiteFitsATwoByOneGapAndIsRefusedAOneByOneOne) {
    const Block b = open_block();
    const int x = b.x + 5;
    const int y = b.y + 5;
    const sim::Sim wide_gap = walled_gap(x, y, {{0, 0}, {1, 0}});
    const sim::Sim narrow_gap = walled_gap(x, y, {{0, 0}});
    for (const UnitKind kind : {UnitKind::TrainingCentre, UnitKind::TechCentre}) {
        EXPECT_TRUE(sim::place_ok(kSeed, wide_gap.units(), kind, false, x, y));
        EXPECT_FALSE(sim::place_ok(kSeed, narrow_gap.units(), kind, false, x, y))
            << "two tiles will not go in one";
    }
    // The one-tile buildings go in either gap: it is the rectangle that is refused.
    EXPECT_TRUE(sim::place_ok(kSeed, narrow_gap.units(), UnitKind::Armory, false, x, y));
    EXPECT_TRUE(sim::place_ok(kSeed, narrow_gap.units(), UnitKind::GunTower, false, x, y));
}

TEST(BuildingRectangles, RotationSwapsARectanglesAxesAndLeavesASquareAlone) {
    const Block b = open_block();
    const int x = b.x + 5;
    const int y = b.y + 5;
    // A 2-wide, 1-tall gap takes the unturned rectangle and refuses the turned one; a 1-wide,
    // 2-tall gap is the reverse.
    const sim::Sim wide_gap = walled_gap(x, y, {{0, 0}, {1, 0}});
    const sim::Sim tall_gap = walled_gap(x, y, {{0, 0}, {0, 1}});
    for (const UnitKind kind : {UnitKind::TrainingCentre, UnitKind::TechCentre}) {
        EXPECT_TRUE(sim::place_ok(kSeed, wide_gap.units(), kind, false, x, y));
        EXPECT_FALSE(sim::place_ok(kSeed, wide_gap.units(), kind, true, x, y));
        EXPECT_FALSE(sim::place_ok(kSeed, tall_gap.units(), kind, false, x, y));
        EXPECT_TRUE(sim::place_ok(kSeed, tall_gap.units(), kind, true, x, y));
    }
    Yard yard = make_yard(1, 1);
    yard.sim.enqueue(place(yard.constructor, UnitKind::TechCentre, x, y, true));
    yard.sim.step();
    const sim::Unit* turned = first_of(yard.sim, UnitKind::TechCentre);
    ASSERT_NE(turned, nullptr);
    const sim::Footprint f = sim::footprint_around(*turned);
    EXPECT_EQ(f.width, 1) << "2x1 turned a quarter is 1x2";
    EXPECT_EQ(f.height, 2);
    EXPECT_EQ(sim::footprint_of(*turned), (FootprintSize{1, 2}));
    // A 2x2 and a 1x1 are the same turned or not.
    for (const UnitKind kind : {UnitKind::BomberBay, UnitKind::ServiceBay, UnitKind::Armory}) {
        const FootprintSize base = sim::footprint_of(kind);
        EXPECT_EQ(base.width, base.height) << "a square: turning changes nothing";
    }
}

// ------------------------------------------------------------------------------ the Factory

TEST(BuildingFactory, TheKindThatAlreadyExistedIsWiredAndAScenariosOwnStaysAsItWas) {
    // A scenario's factory is a point unit with NO footprint: every recorded battle depends on it.
    sim::Scenario scenario{kSeed, {{UnitKind::Factory, Side::Player, 40, 40}}};
    const sim::Sim spawned = sim::Sim::create(scenario);
    const sim::Unit& old_factory = spawned.units().at(0);
    EXPECT_FALSE(old_factory.placed);
    EXPECT_FALSE(sim::has_footprint(old_factory));
    EXPECT_FALSE(sim::building_blocks(spawned.units(), 41, 41));
    EXPECT_EQ(sim::footprint_of(old_factory), (FootprintSize{}));
    // One a Constructor raises DOES stand on its 2x2.
    Yard y = make_yard(1, 1);
    y.sim.enqueue(place(y.constructor, UnitKind::Factory, y.block.x + 5, y.block.y + 5));
    y.sim.step();
    const sim::Unit* raised = first_of(y.sim, UnitKind::Factory);
    ASSERT_NE(raised, nullptr);
    EXPECT_TRUE(raised->placed);
    EXPECT_TRUE(sim::has_footprint(*raised));
    EXPECT_TRUE(sim::building_blocks(y.sim.units(), y.block.x + 6, y.block.y + 6));
    EXPECT_FALSE(sim::building_blocks(y.sim.units(), y.block.x + 7, y.block.y + 5));
}

TEST(BuildingFactory, ASiteTakesNoProductionOrderUntilItStands) {
    Yard y = make_yard(1, 1);
    y.sim.enqueue(place(y.constructor, UnitKind::Factory, y.block.x + 5, y.block.y + 5));
    y.sim.step();
    const sim::Unit* site = first_of(y.sim, UnitKind::Factory);
    ASSERT_NE(site, nullptr);
    const sim::UnitId id = site->id;
    y.sim.enqueue(sim::Order{id, sim::orders::Build{UnitKind::Tank, 0}});
    for (int tick = 0; tick < 5; ++tick) {
        y.sim.step();
    }
    EXPECT_EQ(y.sim.find(id)->queue.count, 0U) << "a factory being built builds nothing";
}

// ------------------------------------------------------------------------------ the panel

namespace game {
namespace {

using input::InputFrame;
using input::KeyEvent;

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    sim::UnitId driven;
    sim::UnitId constructor;
};

/// A tank, a Constructor, and the standing buildings that satisfy every prerequisite on the
/// panel: two POWER plants (six units), a Factory, an Armory, a Service Bay and a Training Ctr.
///
/// ROUND 18, K4 (power must reach the building): the three buildings that DRAW power (Factory,
/// Relay Bunker, Gun Tower) are only placed where a powered building touches, so the rig also
/// stands a third plant on the tile just west of where the reticle lands (every footprint is
/// anchored at its north-west tile, so a plant there touches any of them). That plant is the ONLY
/// change to this rig and it exists for the ruling, not to make a test pass: the placement refusals
/// are pinned in test_power_reach.cpp.
[[nodiscard]] Rig make_rig(std::uint16_t ore) {
    int aim_x = 0;
    int aim_y = 0;
    {
        sim::Scenario bare{kSeed,
                           {{UnitKind::Tank, Side::Player, 60, 60},
                            {UnitKind::Constructor, Side::Player, 61, 60}}};
        const sim::Sim probe = sim::Sim::create(bare);
        SceneOptions probe_options;
        probe_options.seed = kSeed;
        probe_options.extent = Extent{1280, 720};
        probe_options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
        probe_options.viewer_unit = probe.units().at(0).id;
        probe_options.aim = Aim{640.0F, 700.0F};
        const NavAim aim = nav_aim(probe, probe_options);
        aim_x = aim.tile_x;
        aim_y = aim.tile_y;
    }
    sim::Scenario scenario{kSeed,
                           {{UnitKind::Tank, Side::Player, 60, 60},
                            {UnitKind::Constructor, Side::Player, 61, 60},
                            {UnitKind::Power, Side::Player, 30, 30},
                            {UnitKind::Power, Side::Player, 33, 30},
                            {UnitKind::Power, Side::Player, aim_x - 1, aim_y},
                            {UnitKind::Factory, Side::Player, 30, 34},
                            {UnitKind::Armory, Side::Player, 36, 30},
                            {UnitKind::ServiceBay, Side::Player, 36, 34},
                            {UnitKind::TrainingCentre, Side::Player, 40, 30}}};
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

} // namespace

TEST(BuildingPanel, EveryHumanRowNamesAKindAndTheCostDrawnIsTheCostCharged) {
    const std::span<const BuildItem> items = construct_items();
    ASSERT_EQ(items.size(), 9U);
    ASSERT_TRUE(items[0].kind.has_value());
    EXPECT_EQ(items[0].kind.value_or(UnitKind::Tank), UnitKind::Power);
    for (std::size_t i = 0; i < kEight.size(); ++i) {
        const Want& w = kEight.at(i);
        ASSERT_TRUE(items[i + 1].kind.has_value()) << w.name;
        EXPECT_EQ(items[i + 1].kind.value_or(UnitKind::Tank), w.kind) << w.name;
        EXPECT_EQ(items[i + 1].label, w.name);
        EXPECT_EQ(items[i + 1].cost, w.cost) << w.name;
    }
}

TEST(BuildingPanel, EachPressArmsItsBuildingOnItsFootprintAndSpacePlacesIt) {
    for (std::size_t i = 0; i < kEight.size(); ++i) {
        const Want& w = kEight.at(i);
        Rig r = make_rig(400);
        Session session{r.driven};
        std::vector<sim::Order> orders;
        open_panel(r, session, orders);
        press(r, session, orders, input::kKey1 + static_cast<std::int32_t>(i) + 1);
        ASSERT_TRUE(session.placing().has_value()) << w.name << ": " << session.message();
        const Placing armed = session.placing().value_or(Placing{});
        EXPECT_EQ(armed.kind, w.kind) << w.name;
        EXPECT_EQ(armed.cost, w.cost) << w.name;
        EXPECT_EQ(armed.size, (FootprintSize{w.width, w.height})) << w.name;
        EXPECT_EQ(r.sim.ore(Side::Player), 400U) << w.name << ": arming charges nothing";
        // `~` turns it: the axes swap (a square is unchanged).
        press(r, session, orders, input::kKeyGrave);
        const Placing turned = session.placing().value_or(Placing{});
        EXPECT_EQ(turned.size, (FootprintSize{w.height, w.width})) << w.name;
    }
}

TEST(BuildingPanel, ThePressRaisesTheBuildingAndChargesItsCostThroughTheSimsOrder) {
    for (std::size_t i = 0; i < kEight.size(); ++i) {
        const Want& w = kEight.at(i);
        Rig r = make_rig(400);
        Session session{r.driven};
        std::vector<sim::Order> orders;
        open_panel(r, session, orders);
        press(r, session, orders, input::kKey1 + static_cast<std::int32_t>(i) + 1);
        ASSERT_TRUE(session.placing().has_value()) << w.name;
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

TEST(BuildingPanel, TheAlienSideStillRefusesEveryRow) {
    // No alien building has a kind: the alien rows are still name rows and nothing is placed.
    sim::Scenario scenario{
        kSeed,
        {{UnitKind::Tank, Side::Player, 60, 60}, {UnitKind::Constructor, Side::Player, 61, 60}}};
    scenario.store_capacity = 400;
    scenario.start_ore = 400;
    const std::span<const BuildItem> aliens = construct_items(Faction::Alien);
    ASSERT_EQ(aliens.size(), 6U);
    for (const BuildItem& item : aliens) {
        EXPECT_FALSE(item.kind.has_value()) << item.label;
    }
}

} // namespace game

// ------------------------------------------------------------------------------ the looks

namespace {

using game::Scene;
using game::SceneOptions;

using Quantised = std::tuple<int, int, int>;

[[nodiscard]] SceneOptions view(int tile_x, int tile_y) {
    SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 14;
    o.camera.x = static_cast<float>(tile_x + 2) * game::kMetresPerTile;
    o.camera.z = static_cast<float>(tile_y - 8) * game::kMetresPerTile;
    o.camera.y = 9.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

[[nodiscard]] std::set<Quantised> positions(const Scene& scene) {
    std::set<Quantised> out;
    for (const game::SceneVertex& v : scene.vertices) {
        out.emplace(static_cast<int>(std::lround(v.position.at(0) * 100.0F)),
                    static_cast<int>(std::lround(v.position.at(1) * 100.0F)),
                    static_cast<int>(std::lround(v.position.at(2) * 100.0F)));
    }
    return out;
}

/// What a finished building of `kind` adds to the scene, shifted so its own centre is the origin
/// (x, z) -- the SHAPE, wherever it stands. Spawned through a scenario, so it is whole.
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
    const std::set<Quantised> in_with = positions(game::build_scene(a, options));
    const std::set<Quantised> in_base = positions(game::build_scene(base, options));
    const sim::Unit& unit = a.units().at(0);
    const float centre_x
        = static_cast<float>(unit.x.raw) / static_cast<float>(Fixed::kOne) * game::kMetresPerTile;
    const float centre_z
        = static_cast<float>(unit.y.raw) / static_cast<float>(Fixed::kOne) * game::kMetresPerTile;
    Shape shape;
    for (const Quantised& p : in_with) {
        if (in_base.count(p) != 0U) {
            continue;
        }
        const float px = static_cast<float>(std::get<0>(p)) / 100.0F - centre_x;
        const float pz = static_cast<float>(std::get<2>(p)) / 100.0F - centre_z;
        // Only what stands on the ground: the shadow and the floor are not the model.
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

} // namespace

TEST(BuildingLooks, EachOfTheNineIsDrawnAndNoTwoKindsShareAModel) {
    constexpr std::array<UnitKind, 9> kAll{
        UnitKind::Power,          UnitKind::Factory,    UnitKind::RelayBunker,
        UnitKind::Armory,         UnitKind::ServiceBay, UnitKind::GunTower,
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

TEST(BuildingLooks, EachNewModelStaysInsideItsOwnFootprint) {
    // Half the footprint, in metres, per axis (2 m a tile), with a few centimetres for the lofts'
    // facets. The Factory is not here: its model is the one it always had.
    for (const Want& w : kEight) {
        if (w.kind == UnitKind::Factory) {
            continue;
        }
        const Shape shape = built_shape(w.kind);
        const float half_x = static_cast<float>(w.width) * game::kMetresPerTile * 0.5F + 0.05F;
        const float half_z = static_cast<float>(w.height) * game::kMetresPerTile * 0.5F + 0.05F;
        // A player's building faces +Y and is turned a half turn for an enemy: the footprint is
        // the same rectangle either way, so the bound is on the absolute extent.
        EXPECT_LE(std::max(-shape.low_x, shape.high_x), half_x) << w.name;
        EXPECT_LE(std::max(-shape.low_z, shape.high_z), half_z) << w.name;
    }
}

TEST(BuildingLooks, AGrowingSiteOfEachKindDrawsLessThanTheFinishedBuilding) {
    for (const Want& w : kEight) {
        Yard y = make_yard(12, 1);
        const int x = y.block.x + 4;
        const int z = y.block.y + 4;
        y.sim.enqueue(place(y.constructor, w.kind, x, z));
        y.sim.step();
        const int total = sim::spec_of(w.kind).build_ticks;
        const SceneOptions options = view(x, z);
        // How much of the model is drawn over the footprint: the vertices inside its rectangle,
        // the beam's own ink left out. A site cut at a third of its height draws fewer of them than
        // the finished building. (A count, not a top: the Constructor beside it may be taller.)
        const auto drawn_over = [&](const Scene& scene) {
            std::size_t count = 0;
            for (const game::SceneVertex& v : scene.vertices) {
                const bool is_beam
                    = std::abs(v.colour.at(0) - game::kBuildBeamInk.at(0)) < 0.002F
                      && std::abs(v.colour.at(1) - game::kBuildBeamInk.at(1)) < 0.002F
                      && std::abs(v.colour.at(2) - game::kBuildBeamInk.at(2)) < 0.002F;
                const float px = v.position.at(0);
                const float pz = v.position.at(2);
                constexpr float kM = game::kMetresPerTile;
                if (!is_beam && px >= static_cast<float>(x) * kM
                    && px <= static_cast<float>(x + w.width) * kM
                    && pz >= static_cast<float>(z) * kM
                    && pz <= static_cast<float>(z + w.height) * kM) {
                    ++count;
                }
            }
            return count;
        };
        // Run until the Constructor has arrived and about a third of the build is done.
        int guard = 0;
        while ((sim::site_builder(y.sim.units(), *first_of(y.sim, w.kind)) == nullptr
                || first_of(y.sim, w.kind)->build_left > total * 2 / 3)
               && guard++ < total + 800) {
            y.sim.step();
        }
        ASSERT_TRUE(sim::under_construction(*first_of(y.sim, w.kind))) << w.name;
        const std::size_t partial = drawn_over(game::build_scene(y.sim, options));
        for (int tick = 0; tick < total + 10; ++tick) {
            y.sim.step();
        }
        ASSERT_FALSE(sim::under_construction(*first_of(y.sim, w.kind))) << w.name;
        const std::size_t whole = drawn_over(game::build_scene(y.sim, options));
        EXPECT_LT(partial, whole) << w.name << ": it rises, bottom to top";
    }
}

} // namespace protekt
