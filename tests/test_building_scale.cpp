// ROUND 19, L1: THE BUILDINGS ARE FOUR TIMES AS WIDE, LONG AND TALL.
//
// The owner, 2026-11-13: "a 'tile' is actually about four tank lengths. Can we make all buildings
// four times as wide, long, and tall? ... The buildings seem... puny.  So, 1 x 1 becomes 4 x 4.
// 2 x 1 becomes 8 x 4."
//
// The fifteen PLACEABLE buildings scale (their footprints, in tiles, and their models). Round 19,
// L2 scales the DEPLOYED RECYCLER and the FACTORY (placed or scenario-spawned) too, and the wheeled
// RECYCLER's model: the owner ruled it, and it is a stated re-record of the battles they stand in.
// Every number below is a LITERAL, never read back from the table under test.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using sim::Footprint;
using sim::FootprintSize;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

struct Want {
    UnitKind kind;
    int width;
    int height;
    const char* name;
};

/// The fifteen, as the owner's x4 reads: 1x1 -> 4x4, 2x1 -> 8x4, 2x2 -> 8x8. The alien six are
/// still PROVISIONAL (the owner has given none): their old provisional rectangle times four.
constexpr std::array<Want, 15> kFifteen{{
    {UnitKind::Power, 4, 4, "POWER"},
    {UnitKind::Factory, 8, 8, "FACTORY"},
    {UnitKind::RelayBunker, 4, 4, "RELAY BUNKER"},
    {UnitKind::Armory, 4, 4, "ARMORY"},
    {UnitKind::ServiceBay, 8, 8, "SERVICE BAY"},
    {UnitKind::GunTower, 4, 4, "GUN TOWER"},
    {UnitKind::TrainingCentre, 8, 4, "TRAINING CTR."},
    {UnitKind::TechCentre, 8, 4, "TECH CENTRE"},
    {UnitKind::BomberBay, 8, 8, "BOMBER BAY"},
    {UnitKind::Kiln, 8, 8, "KILN"},
    {UnitKind::GunSpire, 4, 4, "GUN SPIRE"},
    {UnitKind::Antenna, 4, 4, "ANTENNA"},
    {UnitKind::Dower, 8, 8, "DOWER"},
    {UnitKind::Stronghold, 8, 4, "STRONGHOLD"},
    {UnitKind::Jammer, 4, 4, "JAMMER"},
}};

/// The north-west tile of an open (all passable) `w` x `h` rectangle with `margin` tiles of open
/// ground all round it.
struct Tile {
    int x{};
    int y{};
};
[[nodiscard]] Tile open_rect(int w, int h, int margin) {
    for (int y = 40; y < sim::kMapTiles - 60; ++y) {
        for (int x = 40; x < sim::kMapTiles - 60; ++x) {
            bool open = true;
            for (int ty = y - margin; ty < y + h + margin && open; ++ty) {
                for (int tx = x - margin; tx < x + w + margin && open; ++tx) {
                    open = sim::tile_passable(kSeed, tx, ty);
                }
            }
            if (open) {
                return Tile{x, y};
            }
        }
    }
    return Tile{-1, -1};
}

[[nodiscard]] sim::Sim world(std::vector<sim::Spawn> spawns) {
    return sim::Sim::create(sim::Scenario{kSeed, std::move(spawns)});
}

TEST(BuildingScale, AllFifteenPlaceableFootprintsReadFourTimesWhatTheyWere) {
    for (const Want& w : kFifteen) {
        EXPECT_TRUE(sim::is_placeable(w.kind)) << w.name;
        EXPECT_EQ(sim::footprint_of(w.kind), (FootprintSize{w.width, w.height})) << w.name;
    }
    // The named constants say the same thing, so a reader of either gets the same number.
    EXPECT_EQ(sim::kPowerFootprint, (FootprintSize{4, 4}));
    EXPECT_EQ(sim::kTrainingCentreFootprint, (FootprintSize{8, 4}));
    EXPECT_EQ(sim::kBomberBayFootprint, (FootprintSize{8, 8}));
}

TEST(BuildingScale, TheDeployedRecyclerIsEightByEightAndReservesTheNewTilesNotTheOld) {
    // ROUND 19, L2 (the owner: "we also need to scale the factory and recycler, including a
    // deployed recycler"): 2 x 2 became 8 x 8, so 4 reserved tiles became 64.
    EXPECT_EQ(sim::footprint_of(UnitKind::DeployedRecycler), (FootprintSize{8, 8}));
    EXPECT_EQ(sim::kDeployedRecyclerFootprint, (FootprintSize{8, 8}));
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::DeployedRecycler, Side::Player, t.x + 8, t.y + 8}});
    const sim::Unit& recycler = sim.units().at(0);
    EXPECT_EQ(sim::footprint_of(recycler), (FootprintSize{8, 8}));
    const Footprint f = sim::footprint_around(recycler);
    EXPECT_EQ(f.width, 8);
    EXPECT_EQ(f.height, 8);
    int reserved = 0;
    for (int ty = t.y; ty < t.y + 24; ++ty) {
        for (int tx = t.x; tx < t.x + 24; ++tx) {
            const bool inside = tx >= f.x && tx < f.x + 8 && ty >= f.y && ty < f.y + 8;
            EXPECT_EQ(sim::building_blocks(sim.units(), tx, ty), inside) << tx << "," << ty;
            reserved += sim::building_blocks(sim.units(), tx, ty) ? 1 : 0;
        }
    }
    EXPECT_EQ(reserved, 64) << "8 x 8, not the 2 x 2 (4 tiles) it was";
    EXPECT_TRUE(sim::building_blocks(sim.units(), f.x, f.y));
    EXPECT_TRUE(sim::building_blocks(sim.units(), f.x + 7, f.y + 7));
    EXPECT_FALSE(sim::building_blocks(sim.units(), f.x - 1, f.y - 1));
    EXPECT_FALSE(sim::building_blocks(sim.units(), f.x + 8, f.y + 8));
    // A Deploy order's own site is the new size: beside a deployed recycler at (x + 10, y), a site
    // whose corner is at x + 4 reaches it (4..11 meets 10..17) and one at x + 2 does not (2..9).
    // A 2 x 2 site at x + 4 (4..5) would have been clear.
    const sim::Sim beside = world({{UnitKind::DeployedRecycler, Side::Player, t.x + 10, t.y}});
    EXPECT_FALSE(sim::deploy_site_ok(kSeed, beside.units(), t.x + 4, t.y)) << "8 wide reaches it";
    EXPECT_TRUE(sim::deploy_site_ok(kSeed, beside.units(), t.x + 2, t.y));
}

TEST(BuildingScale, AScenarioFactoryReservesEightByEightAndAUnitWalksRoundNotThrough) {
    const Tile t = open_rect(30, 30, 2);
    ASSERT_GE(t.x, 0);
    EXPECT_TRUE(sim::has_footprint(UnitKind::Factory)) << "the kind itself stands on a footprint";
    sim::Scenario scenario{kSeed,
                           {{UnitKind::Factory, Side::Player, t.x + 14, t.y + 14},
                            {UnitKind::Tank, Side::Player, t.x + 2, t.y + 14}}};
    sim::Sim sim = sim::Sim::create(scenario);
    const sim::Unit& factory = sim.units().at(0);
    const Footprint f = sim::footprint_around(factory);
    EXPECT_EQ(f.width, 8);
    EXPECT_EQ(f.height, 8);
    int reserved = 0;
    for (int ty = t.y; ty < t.y + 30; ++ty) {
        for (int tx = t.x; tx < t.x + 30; ++tx) {
            const bool inside = tx >= f.x && tx < f.x + 8 && ty >= f.y && ty < f.y + 8;
            EXPECT_EQ(sim::building_blocks(sim.units(), tx, ty), inside) << tx << "," << ty;
            reserved += sim::building_blocks(sim.units(), tx, ty) ? 1 : 0;
        }
    }
    EXPECT_EQ(reserved, 64) << "a scenario factory used to reserve none";
    // A tank ordered straight across it, west to east, never enters the rectangle, and gets past.
    const sim::UnitId tank = sim.units().at(1).id;
    sim.enqueue(sim::Order{
        tank, sim::orders::MoveTo{sim::Fixed::from_int(t.x + 27), sim::Fixed::from_int(t.y + 14)}});
    bool entered = false;
    for (int tick = 0; tick < 900; ++tick) {
        sim.step();
        const sim::Unit& u = sim.units().at(1);
        const int tx = u.x.floor_int();
        const int ty = u.y.floor_int();
        entered = entered || (tx >= f.x && tx < f.x + 8 && ty >= f.y && ty < f.y + 8);
    }
    EXPECT_FALSE(entered) << "walked through the factory";
    EXPECT_GT(sim.units().at(1).x.floor_int(), f.x + 8) << "and got past it";
}

TEST(BuildingScale, APlacedFactoryAndAScenarioFactoryHaveTheSameFootprint) {
    const Tile t = open_rect(30, 30, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim spawned = world({{UnitKind::Factory, Side::Player, t.x + 14, t.y + 14}});
    sim::Scenario scenario{kSeed, {{UnitKind::Constructor, Side::Player, t.x + 2, t.y + 2}}};
    scenario.store_capacity = 400;
    scenario.start_ore = 200;
    sim::Sim placing = sim::Sim::create(scenario);
    placing.enqueue(sim::Order{placing.units().at(0).id,
                               sim::orders::Place{UnitKind::Factory, t.x + 10, t.y + 10, false}});
    placing.step();
    const sim::Unit* raised = nullptr;
    for (const sim::Unit& u : placing.units()) {
        if (u.kind == UnitKind::Factory) {
            raised = &u;
        }
    }
    ASSERT_NE(raised, nullptr);
    EXPECT_EQ(sim::footprint_of(spawned.units().front()), (FootprintSize{8, 8}));
    EXPECT_EQ(sim::footprint_of(*raised), (FootprintSize{8, 8}));
    EXPECT_EQ(sim::footprint_of(spawned.units().front()), sim::footprint_of(*raised));
}

TEST(BuildingScale, ASiteTheOldSizeFitsIsRefusedAtTheNewSizeAndTheTurnFollows) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    // A tank five tiles east of the aimed corner, one row down: inside an 8 wide site, outside a
    // 4 wide one.
    const sim::Sim east = world({{UnitKind::Tank, Side::Player, t.x + 5, t.y + 1}});
    EXPECT_TRUE(sim::place_ok(kSeed, east.units(), UnitKind::Power, false, t.x, t.y)) << "4x4 fits";
    EXPECT_FALSE(sim::place_ok(kSeed, east.units(), UnitKind::TrainingCentre, false, t.x, t.y))
        << "8x4 does not";
    // A tank one tile east and FIVE rows down: inside a turned (4x8) site, outside the 8x4 one.
    const sim::Sim south = world({{UnitKind::Tank, Side::Player, t.x + 1, t.y + 5}});
    EXPECT_TRUE(sim::place_ok(kSeed, south.units(), UnitKind::Power, false, t.x, t.y));
    EXPECT_TRUE(sim::place_ok(kSeed, south.units(), UnitKind::TrainingCentre, false, t.x, t.y))
        << "8x4 stops at row 3";
    EXPECT_FALSE(sim::place_ok(kSeed, south.units(), UnitKind::TrainingCentre, true, t.x, t.y))
        << "turned it is 4x8 and reaches row 5";
    // The site clips onto the map as an 8x4 does: the last north-west tile is map - 8.
    EXPECT_EQ(
        sim::footprint_at(sim::footprint_of(UnitKind::TrainingCentre), sim::kMapTiles - 1, 5).x,
        sim::kMapTiles - 8);
    // A 2x2 hole is no longer a place for anything: ground that took the old 1x1 does not take 4x4.
    const sim::Sim hole = world({{UnitKind::Tank, Side::Player, t.x + 3, t.y + 3}});
    EXPECT_FALSE(sim::place_ok(kSeed, hole.units(), UnitKind::Power, false, t.x, t.y))
        << "the 4x4 reaches the tank at (3,3); a 1x1 at the corner never did";
}

TEST(BuildingScale, ABuildingATrainingCentreTurnedAQuarterStandsFourWideAndEightLong) {
    const Tile t = open_rect(30, 30, 2);
    ASSERT_GE(t.x, 0);
    sim::Scenario scenario{kSeed, {{UnitKind::Constructor, Side::Player, t.x + 14, t.y + 14}}};
    scenario.store_capacity = 400;
    scenario.start_ore = 200;
    sim::Sim sim = sim::Sim::create(scenario);
    const sim::UnitId builder = sim.units().at(0).id;
    sim.enqueue(sim::Order{builder, sim::orders::Place{UnitKind::TrainingCentre, t.x, t.y, true}});
    sim.step();
    const sim::Unit* site = nullptr;
    for (const sim::Unit& u : sim.units()) {
        if (u.kind == UnitKind::TrainingCentre) {
            site = &u;
        }
    }
    ASSERT_NE(site, nullptr);
    ASSERT_TRUE(site->turned);
    EXPECT_EQ(sim::footprint_of(*site), (FootprintSize{4, 8}));
    const Footprint f = sim::footprint_around(*site);
    EXPECT_EQ(f.x, t.x);
    EXPECT_EQ(f.y, t.y);
    EXPECT_EQ(f.width, 4);
    EXPECT_EQ(f.height, 8);
    // The reserved tiles are the new rectangle's 32 and no others: not the old 2x1 (or its turn).
    int reserved = 0;
    for (int ty = t.y - 2; ty < t.y + 12; ++ty) {
        for (int tx = t.x - 2; tx < t.x + 12; ++tx) {
            const bool inside = tx >= t.x && tx < t.x + 4 && ty >= t.y && ty < t.y + 8;
            EXPECT_EQ(sim::building_blocks(sim.units(), tx, ty), inside) << tx << "," << ty;
            reserved += sim::building_blocks(sim.units(), tx, ty) ? 1 : 0;
        }
    }
    EXPECT_EQ(reserved, 32);
}

TEST(BuildingScale, ASpawnedServiceBayReservesSixtyFourTilesAndNotTheOldFour) {
    const Tile t = open_rect(20, 20, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::ServiceBay, Side::Player, t.x + 4, t.y + 4}});
    int reserved = 0;
    for (int ty = t.y; ty < t.y + 20; ++ty) {
        for (int tx = t.x; tx < t.x + 20; ++tx) {
            reserved += sim::building_blocks(sim.units(), tx, ty) ? 1 : 0;
        }
    }
    EXPECT_EQ(reserved, 64) << "8 x 8, not the 2 x 2 it was";
    const Footprint f = sim::footprint_around(sim.units().front());
    EXPECT_EQ(f.width, 8);
    EXPECT_EQ(f.height, 8);
    EXPECT_TRUE(sim::building_blocks(sim.units(), f.x + 7, f.y + 7)) << "the far corner";
}

// ------------------------------------------------------------------------------------- the model

/// The drawn model of the one unit in `sim`: the vertices between the scene without it (plus its
/// shadow quad, which is drawn first) and the scene with it, in world metres.
struct Drawn {
    float low{};
    float high{};
    float west{};
    float east{};
    float north{};
    float south{};
    [[nodiscard]] float height() const { return high - low; }
};

[[nodiscard]] game::SceneOptions view_of(float tile_x, float tile_y) {
    game::SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 30;
    o.camera.x = tile_x * game::kMetresPerTile;
    o.camera.z = tile_y * game::kMetresPerTile;
    o.camera.y = 40.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

[[nodiscard]] Drawn measure(const sim::Sim& with, const sim::Sim& without, float tile_x,
                            float tile_y) {
    const game::SceneOptions options = view_of(tile_x, tile_y);
    const game::Scene a = game::build_scene(with, options);
    const game::Scene b = game::build_scene(without, options);
    Drawn d{1.0e9F, -1.0e9F, 1.0e9F, -1.0e9F, 1.0e9F, -1.0e9F};
    const std::size_t first = b.world_vertex_count + game::kShadowVertexCount;
    for (std::size_t i = first; i < a.world_vertex_count; ++i) {
        const auto& p = a.vertices.at(i).position;
        d.low = std::min(d.low, p.at(1));
        d.high = std::max(d.high, p.at(1));
        d.west = std::min(d.west, p.at(0));
        d.east = std::max(d.east, p.at(0));
        d.north = std::min(d.north, p.at(2));
        d.south = std::max(d.south, p.at(2));
    }
    return d;
}

/// What each building's drawn model measured BEFORE the scale (round 18, K6, the commit this
/// slice stands on), in metres: height, then the span along x and along z. Measured by this same
/// `measure` on that commit and written down as literals.
struct Was {
    UnitKind kind;
    float height;
    float across;
    float along;
};

constexpr std::array<Was, 14> kWas{{
    {UnitKind::Power, 2.05F, 1.85F, 0.85F},
    {UnitKind::RelayBunker, 3.30F, 1.70F, 1.73F},
    {UnitKind::Armory, 2.00F, 1.84F, 1.23F},
    {UnitKind::ServiceBay, 1.90F, 3.60F, 3.65F},
    {UnitKind::GunTower, 1.50F, 1.70F, 1.82F},
    {UnitKind::TrainingCentre, 3.00F, 3.70F, 1.72F},
    {UnitKind::TechCentre, 3.90F, 3.96F, 1.37F},
    {UnitKind::BomberBay, 3.25F, 3.65F, 3.65F},
    {UnitKind::Kiln, 4.481F, 3.80F, 3.687F},
    {UnitKind::GunSpire, 3.8608F, 1.60F, 1.7253F},
    {UnitKind::Antenna, 4.5452F, 1.8173F, 1.8173F},
    {UnitKind::Dower, 2.50F, 3.90F, 3.90F},
    {UnitKind::Stronghold, 2.9875F, 3.8743F, 1.87F},
    {UnitKind::Jammer, 3.70F, 1.8687F, 1.8687F},
}};

constexpr float kFour = 4.0F;

TEST(BuildingScale, EveryDrawnModelIsFourTimesAsTallWideAndLongAsItWas) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    for (const Was& was : kWas) {
        const sim::Sim with = world({{was.kind, Side::Player, t.x + 8, t.y + 8}});
        const sim::Sim without = world({});
        const Drawn d
            = measure(with, without, static_cast<float>(t.x + 8), static_cast<float>(t.y + 8));
        EXPECT_NEAR(d.height(), was.height * kFour, 0.02F) << static_cast<int>(was.kind);
        EXPECT_NEAR(d.east - d.west, was.across * kFour, 0.05F) << static_cast<int>(was.kind);
        EXPECT_NEAR(d.south - d.north, was.along * kFour, 0.05F) << static_cast<int>(was.kind);
        // ...and it fits the rectangle the sim reserved for it: the scale and the footprint agree.
        const Footprint f = sim::footprint_around(with.units().front());
        const float m = game::kMetresPerTile;
        EXPECT_GE(d.west, static_cast<float>(f.x) * m - 0.01F) << static_cast<int>(was.kind);
        EXPECT_LE(d.east, static_cast<float>(f.x + f.width) * m + 0.01F)
            << static_cast<int>(was.kind);
        EXPECT_GE(d.north, static_cast<float>(f.y) * m - 0.01F) << static_cast<int>(was.kind);
        EXPECT_LE(d.south, static_cast<float>(f.y + f.height) * m + 0.01F)
            << static_cast<int>(was.kind);
    }
}

/// A Factory a Constructor PLACED, stood complete: the one placeable kind a scenario cannot spawn
/// with a footprint.
[[nodiscard]] sim::Sim built_factory(const Tile& t) {
    sim::Scenario scenario{kSeed, {{UnitKind::Constructor, Side::Player, t.x + 12, t.y + 12}}};
    scenario.store_capacity = 400;
    scenario.start_ore = 200;
    sim::Sim sim = sim::Sim::create(scenario);
    sim.enqueue(sim::Order{sim.units().at(0).id,
                           sim::orders::Place{UnitKind::Factory, t.x + 4, t.y + 4, false}});
    for (int tick = 0; tick < 1800; ++tick) {
        sim.step();
    }
    return sim;
}

TEST(BuildingScale, APlacedFactoryIsEightByEightAndFourTimesAsTallAsTheHallItWas) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim with = built_factory(t);
    const sim::Unit* factory = nullptr;
    for (const sim::Unit& u : with.units()) {
        if (u.kind == UnitKind::Factory) {
            factory = &u;
        }
    }
    ASSERT_NE(factory, nullptr);
    ASSERT_EQ(factory->build_left, 0) << "finished";
    EXPECT_EQ(sim::footprint_of(*factory), (FootprintSize{8, 8}));
    // The same world with the Constructor ALONE: its scene has one shadow and the Constructor's
    // model, so the factory's own vertices are the LAST (with - alone - one shadow) of the world.
    sim::Scenario scenario{kSeed, {{UnitKind::Constructor, Side::Player, t.x + 12, t.y + 12}}};
    const sim::Sim alone = sim::Sim::create(scenario);
    const game::SceneOptions options
        = view_of(static_cast<float>(t.x + 8), static_cast<float>(t.y + 8));
    const game::Scene a = game::build_scene(with, options);
    const game::Scene b = game::build_scene(alone, options);
    ASSERT_EQ(with.units().size(), 2U);
    const std::size_t count
        = a.world_vertex_count - b.world_vertex_count - game::kShadowVertexCount;
    float low = 1.0e9F;
    float high = -1.0e9F;
    float west = 1.0e9F;
    float east = -1.0e9F;
    for (std::size_t i = a.world_vertex_count - count; i < a.world_vertex_count; ++i) {
        const auto& p = a.vertices.at(i).position;
        low = std::min(low, p.at(1));
        high = std::max(high, p.at(1));
        west = std::min(west, p.at(0));
        east = std::max(east, p.at(0));
    }
    // Measured by this same code on the commit this stands on: the placed factory drew 6.2 m high
    // (the stack rises over the 4.5 m hall) and 1.2 m across (one hull width).
    EXPECT_NEAR(high - low, 6.2F * kFour, 0.05F);
    EXPECT_NEAR(east - west, 1.2F * kFour, 0.1F);
}

TEST(BuildingScale, ThePreviewGridAndItsVertexBudgetFollowTheNewFootprint) {
    // 8x4: nine lines of eight, five of four... (h + 1) * w + (w + 1) * h = 5 * 8 + 9 * 4 = 76
    // strokes of six vertices.
    EXPECT_EQ(game::deploy_preview_vertex_count(sim::footprint_of(UnitKind::TrainingCentre)),
              76U * 6U);
    EXPECT_EQ(game::deploy_preview_vertex_count(sim::footprint_of(UnitKind::Power)), 40U * 6U);
    EXPECT_EQ(game::deploy_preview_vertex_count(sim::footprint_of(UnitKind::BomberBay)), 144U * 6U);
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::Tank, Side::Player, t.x, t.y}});
    game::SceneOptions plain = view_of(static_cast<float>(t.x + 8), static_cast<float>(t.y + 8));
    game::SceneOptions previewed = plain;
    const game::Placing armed{UnitKind::TrainingCentre, false, 70,
                              sim::footprint_of(UnitKind::TrainingCentre)};
    previewed.deploy_preview = game::placement_preview(sim, armed, t.x + 8, t.y + 8);
    ASSERT_TRUE(previewed.deploy_preview.has_value());
    EXPECT_EQ(previewed.deploy_preview->site.width, 8);
    EXPECT_EQ(previewed.deploy_preview->site.height, 4);
    EXPECT_EQ(game::build_scene(sim, previewed).vertices.size(),
              game::build_scene(sim, plain).vertices.size() + std::size_t{456});
}

TEST(BuildingScale, ThePickBoxIsTheNewFootprintAndFourTimesTheHeight) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::TrainingCentre, Side::Player, t.x + 8, t.y + 8},
                                {UnitKind::Factory, Side::Player, t.x + 20, t.y + 20}});
    const sim::Unit& centre = sim.units().at(0);
    const sim::Unit& scenario_factory = sim.units().at(1);
    // 8x4 centred on its rectangle: +-8 m across, +-4 m deep; 7.2 m tall (1.8 x 4).
    EXPECT_NEAR(game::unit_box_height_metres(centre), 7.2F, 0.001F);
    EXPECT_NEAR(game::unit_box_height_metres(UnitKind::TrainingCentre), 1.8F, 0.001F)
        << "the kind's own box is the authored one";
    EXPECT_NEAR(game::unit_box_height_metres(scenario_factory), 18.0F, 0.001F)
        << "a scenario factory is scaled too (round 19, L2): 4.5 x 4";
    const float cx = static_cast<float>(centre.x.raw) / static_cast<float>(sim::Fixed::kOne)
                     * game::kMetresPerTile;
    const float cz = static_cast<float>(centre.y.raw) / static_cast<float>(sim::Fixed::kOne)
                     * game::kMetresPerTile;
    const float ground = game::ground_metres(kSeed, centre.x, centre.y);
    game::Camera camera{};
    camera.x = cx;
    camera.y = ground + 3.0F;
    camera.z = cz - 40.0F;
    game::SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;
    options.radius_tiles = 40;
    const game::Extent extent{1280, 720};
    const auto hit = [&](float dx, float height) {
        const auto p = game::project(camera, extent, cx + dx, ground + height, cz);
        return game::pick(sim, options, p.x, p.y).has_unit();
    };
    EXPECT_TRUE(hit(0.0F, 3.0F));
    EXPECT_TRUE(hit(6.0F, 3.0F))
        << "6 m east of the middle is inside +-8 m (the unit radius is 1.5)";
    EXPECT_TRUE(hit(-6.0F, 3.0F));
    EXPECT_TRUE(hit(0.0F, 6.5F)) << "6.5 m up is inside 7.2 m (a 1.8 m box would miss)";
}

TEST(BuildingScale, ABuildingsShadowIsFourTimesAsWideAndATanksIsNot) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const auto shadow_width = [&](UnitKind kind) {
        const sim::Sim with = world({{kind, Side::Player, t.x + 8, t.y + 8}});
        const sim::Sim without = world({});
        const game::SceneOptions options
            = view_of(static_cast<float>(t.x + 8), static_cast<float>(t.y + 8));
        const game::Scene a = game::build_scene(with, options);
        const game::Scene b = game::build_scene(without, options);
        float lo = 1.0e9F;
        float hi = -1.0e9F;
        for (std::size_t i = b.world_vertex_count;
             i < b.world_vertex_count + game::kShadowVertexCount; ++i) {
            lo = std::min(lo, a.vertices.at(i).position.at(0));
            hi = std::max(hi, a.vertices.at(i).position.at(0));
        }
        return hi - lo;
    };
    EXPECT_NEAR(shadow_width(UnitKind::Tank), 2.0F * game::kShadowHalfMetres, 0.001F);
    EXPECT_NEAR(shadow_width(UnitKind::Power), 8.0F * game::kShadowHalfMetres, 0.001F);
}

TEST(BuildingScale, TheModelScaleIsOneNumberTheSimsFootprintFactor) {
    EXPECT_EQ(sim::kBuildingFootprintScale, 4);
    EXPECT_NEAR(game::kBuildingModelScale, 4.0F, 0.0F);
}

TEST(BuildingScale, TheTallestBuildingIsManyTimesTheTanksHeight) {
    // The owner's complaint was proportion: a building beside a tank. The tank is MEASURED here,
    // drawn by the same code; the tallest of the fifteen (the antenna, 4.5452 m authored) is now
    // four times that, so measured from the scene and not read off a constant.
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim tank = world({{UnitKind::Tank, Side::Player, t.x + 8, t.y + 8}});
    const sim::Sim antenna = world({{UnitKind::Antenna, Side::Player, t.x + 8, t.y + 8}});
    const sim::Sim none = world({});
    const float cx = static_cast<float>(t.x + 8);
    const float cy = static_cast<float>(t.y + 8);
    const float tank_height = measure(tank, none, cx, cy).height();
    const float antenna_height = measure(antenna, none, cx, cy).height();
    ASSERT_GT(tank_height, 0.5F);
    EXPECT_NEAR(antenna_height, 4.5452F * kFour, 0.02F);
    // Measured when written: a tank 1.5330 m, the antenna 18.1808 m -- 11.86 times (it was 2.96).
    EXPECT_NEAR(tank_height, 1.533F, 0.01F);
    EXPECT_GT(antenna_height / tank_height, 8.0F) << "a tank is a small thing beside it now";
}

TEST(BuildingScale, TheRecyclersAndTheFactoryDrawFourTimesAsTallWideAndLongAsTheyDid) {
    // Round 19, L2. Measured by `measure` on the commit this stands on (L1), as literals, in
    // metres: the wheeled recycler 2.7320 high, 2.3935 across, 5.1301 along; the deployed
    // one 2.5080, 2.3600, 5.2000; a scenario factory (the placed one's very model) 6.2 high, 1.2
    // square.
    struct Old {
        UnitKind kind;
        float height;
        float across;
        float along;
    };
    constexpr std::array<Old, 3> kOld{{{UnitKind::Recycler, 2.7320F, 2.3935F, 5.1301F},
                                       {UnitKind::DeployedRecycler, 2.5080F, 2.3600F, 5.2000F},
                                       {UnitKind::Factory, 6.2000F, 1.2000F, 1.2000F}}};
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    for (const Old& was : kOld) {
        const sim::Sim with = world({{was.kind, Side::Player, t.x + 8, t.y + 8}});
        const sim::Sim without = world({});
        const Drawn d
            = measure(with, without, static_cast<float>(t.x + 8), static_cast<float>(t.y + 8));
        EXPECT_NEAR(d.height(), was.height * kFour, 0.02F) << static_cast<int>(was.kind);
        EXPECT_NEAR(d.east - d.west, was.across * kFour, 0.05F) << static_cast<int>(was.kind);
        EXPECT_NEAR(d.south - d.north, was.along * kFour, 0.05F) << static_cast<int>(was.kind);
    }
}

TEST(BuildingScale, TheWheeledRecyclerHasNoFootprintAndItsShadowAndBoxGrowWithIt) {
    EXPECT_FALSE(sim::has_footprint(UnitKind::Recycler)) << "a vehicle: no footprint";
    EXPECT_EQ(sim::footprint_of(UnitKind::Recycler), (FootprintSize{}));
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::Recycler, Side::Player, t.x + 8, t.y + 8}});
    EXPECT_NEAR(game::unit_box_height_metres(sim.units().front()), 7.2F, 0.001F) << "1.8 x 4";
}

TEST(BuildingScale, TheFifteenPlaceableBuildingsAreUntouchedByThisSlice) {
    // Their models measure exactly what L1 pinned (the tall test above, still green), their
    // footprints are the L1 table, and their pick box is the L1 one: 4 x the authored height.
    EXPECT_NEAR(game::unit_box_height_metres(sim::Unit{.kind = UnitKind::Power}), 7.2F, 0.001F);
    for (const Want& w : kFifteen) {
        if (w.kind == UnitKind::Factory) {
            continue; // the one of the fifteen this slice moves: its footprint is the same 8 x 8
        }
        EXPECT_EQ(sim::footprint_of(w.kind), (FootprintSize{w.width, w.height})) << w.name;
    }
    EXPECT_EQ(sim::footprint_of(UnitKind::Factory), (FootprintSize{8, 8}));
}

} // namespace
} // namespace protekt
