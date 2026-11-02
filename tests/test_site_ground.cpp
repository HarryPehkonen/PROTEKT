// A DEPLOYED THING ALTERS THE GROUND AROUND IT (Harri, round 1 on the recycler: "change the tiles
// under it to look different. Kind of like those tiles became part of the installation. I can
// still drive over them, but not crash into the recycler."; round 3 on the scavenger: "change the
// atlas of the tiles around it ... it should look like the ground was altered"). ONE mechanism,
// three consumers: the deployed recycler, the deployed scavenger and the biometal pool. Each kind
// says HOW FAR and WHICH TILE (`site_ground`); the scene paints those world tiles from the atlas
// tile. Presentation only: the tiles stay drivable and nothing in the sim moves.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "protekt/game/atlas.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/site_ground.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::Scene;
using protekt::game::SceneOptions;
using protekt::game::SceneVertex;
using protekt::game::SiteGroundMap;
using protekt::game::SiteKind;
using protekt::game::TileRange;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::UnitKind;

using Tiles = std::set<std::pair<int, int>>;

constexpr std::uint64_t kSeed = 42;
constexpr int kSiteX = 60;
constexpr int kSiteY = 60;

Sim world(std::vector<protekt::sim::Spawn> spawns, bool with_pool) {
    protekt::sim::Scenario s;
    s.seed = kSeed;
    if (with_pool) {
        s.pools = {protekt::sim::Pool{kSiteX, kSiteY}};
    }
    spawns.push_back({UnitKind::Tank, Side::Player, kSiteX - 8, kSiteY});
    s.spawns = std::move(spawns);
    return Sim::create(s);
}

SceneOptions rig() {
    SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 14;
    o.camera.x = (static_cast<float>(kSiteX) - 8.0F) * protekt::game::kMetresPerTile;
    o.camera.z = static_cast<float>(kSiteY) * protekt::game::kMetresPerTile;
    o.camera.y = 8.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

/// The world tiles whose ground quad is painted from atlas tile `name`: a triangle of the scene
/// whose three vertices all sample inside that tile's rectangle, by the triangle's centroid.
/// Nothing but the terrain samples a site tile, so this is exactly "the ground it altered".
Tiles painted_with(const Scene& scene, std::string_view name) {
    const protekt::game::UvRect rect = protekt::game::atlas_manifest().rect(name);
    const auto inside = [&](const SceneVertex& v) {
        return v.u >= rect.u0 && v.u <= rect.u1 && v.v >= rect.v0 && v.v <= rect.v1;
    };
    Tiles found;
    for (std::size_t i = 0; i + 2 < scene.vertices.size(); i += 3) {
        const SceneVertex& a = scene.vertices.at(i);
        const SceneVertex& b = scene.vertices.at(i + 1);
        const SceneVertex& c = scene.vertices.at(i + 2);
        if (!inside(a) || !inside(b) || !inside(c)) {
            continue;
        }
        const float x = (a.position[0] + b.position[0] + c.position[0]) / 3.0F;
        const float z = (a.position[2] + b.position[2] + c.position[2]) / 3.0F;
        found.insert({static_cast<int>(std::floor(x / protekt::game::kMetresPerTile)),
                      static_cast<int>(std::floor(z / protekt::game::kMetresPerTile))});
    }
    return found;
}

Tiles range_tiles(const TileRange& r) {
    Tiles tiles;
    for (int y = r.y0; y <= r.y1; ++y) {
        for (int x = r.x0; x <= r.x1; ++x) {
            tiles.insert({x, y});
        }
    }
    return tiles;
}

std::size_t opaque_vertices(const Scene& scene) {
    std::size_t n = 0;
    for (const SceneVertex& v : scene.vertices) {
        n += v.alpha >= 1.0F ? 1U : 0U;
    }
    return n;
}

TEST(SiteGround, EachKindSaysHowFarAndWhichTileAndTheRangeIsDerivedFromIt) {
    for (const SiteKind kind :
         {SiteKind::Pool, SiteKind::DeployedScavenger, SiteKind::DeployedRecycler}) {
        const auto& ground = protekt::game::site_ground(kind);
        EXPECT_GT(ground.reach, 0) << "a site that alters nothing is not a site";
        EXPECT_NE(protekt::game::atlas_manifest().tile(ground.tile), protekt::game::kFallbackTile)
            << ground.tile << " is listed in the manifest";
        const TileRange r = protekt::game::site_range(kind, kSiteX, kSiteY);
        EXPECT_EQ(r.x1 - r.x0 + 1, ground.core + (2 * ground.reach));
        EXPECT_EQ(r.y1 - r.y0 + 1, ground.core + (2 * ground.reach));
        EXPECT_TRUE(r.covers(kSiteX, kSiteY));
    }
    // The three kinds are told apart by tile and by reach: not one look with three names.
    const auto& pool = protekt::game::site_ground(SiteKind::Pool);
    const auto& scavenger = protekt::game::site_ground(SiteKind::DeployedScavenger);
    const auto& recycler = protekt::game::site_ground(SiteKind::DeployedRecycler);
    EXPECT_NE(pool.tile, scavenger.tile);
    EXPECT_NE(pool.tile, recycler.tile);
    EXPECT_NE(scavenger.tile, recycler.tile);
    // The recycler's core IS the sim's footprint, not a copy of its number, and the pad covers
    // every tile the building stands on.
    EXPECT_EQ(recycler.core, protekt::sim::kBuildingFootprintTiles);
    const protekt::sim::Footprint foot = protekt::sim::footprint_around(
        protekt::sim::Fixed::from_int(kSiteX), protekt::sim::Fixed::from_int(kSiteY));
    const TileRange pad = protekt::game::site_range(SiteKind::DeployedRecycler, kSiteX, kSiteY);
    for (int y = foot.y; y < foot.y + protekt::sim::kBuildingFootprintTiles; ++y) {
        for (int x = foot.x; x < foot.x + protekt::sim::kBuildingFootprintTiles; ++x) {
            EXPECT_TRUE(pad.covers(x, y)) << x << "," << y;
        }
    }
    // A pool's altered ground reaches past a scavenger's, so a scavenger deployed on it sits on a
    // rig inside the pool's own ground.
    EXPECT_GT(pool.reach, scavenger.reach);
}

TEST(SiteGround, TheMapGivesATileToTheStrongestKindThatReachesIt) {
    SiteGroundMap map;
    EXPECT_FALSE(map.at(kSiteX, kSiteY).has_value());
    map.add(SiteKind::Pool, kSiteX, kSiteY);
    map.add(SiteKind::DeployedRecycler, kSiteX, kSiteY);
    map.add(SiteKind::DeployedScavenger, kSiteX, kSiteY);
    EXPECT_EQ(map.at(kSiteX, kSiteY), SiteKind::DeployedRecycler)
        << "the order of adding changes nothing";
    const TileRange vent = protekt::game::site_range(SiteKind::Pool, kSiteX, kSiteY);
    EXPECT_EQ(map.at(vent.x1, vent.y1), SiteKind::Pool) << "the pool's own outer ring";
    EXPECT_FALSE(map.at(vent.x0 - 1, vent.y0).has_value());
    EXPECT_FALSE(map.at(vent.x1, vent.y1 + 1).has_value());
}

TEST(SiteGround, ADeployedRecyclerPaintsItsPadAndNoTileBeyondIt) {
    const Sim sim = world({{UnitKind::DeployedRecycler, Side::Player, kSiteX, kSiteY}}, false);
    const Scene scene = protekt::game::build_scene(sim, rig());
    const auto& pad = protekt::game::site_ground(SiteKind::DeployedRecycler);
    const protekt::sim::Footprint foot
        = protekt::sim::footprint_around(sim.units().front().x, sim.units().front().y);
    const Tiles painted = painted_with(scene, pad.tile);
    EXPECT_EQ(painted, range_tiles(protekt::game::site_range(SiteKind::DeployedRecycler,
                                                             foot.centre_x(), foot.centre_y())));
    EXPECT_TRUE(painted.contains({foot.x, foot.y}));
    EXPECT_TRUE(painted.contains({foot.x - pad.reach, foot.y - pad.reach}));
    EXPECT_FALSE(painted.contains({foot.x - pad.reach - 1, foot.y}));
}

TEST(SiteGround, ADeployedScavengerAltersTheGroundRoundItInsteadOfDrawingADisc) {
    const Sim sim = world({{UnitKind::DeployedScavenger, Side::Player, kSiteX, kSiteY}}, true);
    SceneOptions options = rig();
    const Scene scene = protekt::game::build_scene(sim, options);
    const auto& rig_ground = protekt::game::site_ground(SiteKind::DeployedScavenger);
    const Tiles rig_tiles = painted_with(scene, rig_ground.tile);
    EXPECT_EQ(rig_tiles,
              range_tiles(protekt::game::site_range(SiteKind::DeployedScavenger, kSiteX, kSiteY)));
    // The pool it stands on keeps its own ground in the ring the rig does not cover.
    const Tiles vent_tiles = painted_with(scene, protekt::game::site_ground(SiteKind::Pool).tile);
    EXPECT_FALSE(vent_tiles.empty());
    for (const auto& tile : rig_tiles) {
        EXPECT_FALSE(vent_tiles.contains(tile)) << "one tile, one look";
    }
    // THE DISC IS GONE: with the pools layer off the only difference left in the opaque geometry
    // is none at all -- the pool layer adds a see-through plume and no solid shape of any kind.
    options.show_pools = false;
    const Scene without = protekt::game::build_scene(sim, options);
    EXPECT_EQ(opaque_vertices(scene), opaque_vertices(without));
    EXPECT_GT(scene.vertices.size(), without.vertices.size()) << "the plume is still drawn";
}

TEST(SiteGround, APoolAloneAltersItsSiteAndTheLayerSwitchTakesTheGroundBackToo) {
    const Sim sim = world({}, true);
    SceneOptions options = rig();
    const Scene scene = protekt::game::build_scene(sim, options);
    const auto& vent = protekt::game::site_ground(SiteKind::Pool);
    EXPECT_EQ(painted_with(scene, vent.tile),
              range_tiles(protekt::game::site_range(SiteKind::Pool, kSiteX, kSiteY)));
    options.show_pools = false;
    EXPECT_TRUE(painted_with(protekt::game::build_scene(sim, options), vent.tile).empty());
    EXPECT_TRUE(
        painted_with(protekt::game::build_scene(world({}, false), rig()), vent.tile).empty())
        << "no pool, no altered ground";
}

TEST(SiteGround, AFarSiteIsNotMeshedAndNothingElseUsesASiteTile) {
    SceneOptions far = rig();
    far.camera.x = 10.0F * protekt::game::kMetresPerTile;
    far.camera.z = 10.0F * protekt::game::kMetresPerTile;
    const Sim sim = world({{UnitKind::DeployedRecycler, Side::Player, kSiteX, kSiteY}}, true);
    for (const SiteKind kind :
         {SiteKind::Pool, SiteKind::DeployedScavenger, SiteKind::DeployedRecycler}) {
        EXPECT_TRUE(painted_with(protekt::game::build_scene(sim, far),
                                 protekt::game::site_ground(kind).tile)
                        .empty());
    }
}

TEST(SiteGround, TheAlteredTilesStayDrivableAndNothingInTheSimMoves) {
    const Sim sim = world({{UnitKind::DeployedRecycler, Side::Player, kSiteX, kSiteY}}, true);
    const protekt::sim::Footprint foot
        = protekt::sim::footprint_around(sim.units().front().x, sim.units().front().y);
    const TileRange pad
        = protekt::game::site_range(SiteKind::DeployedRecycler, foot.centre_x(), foot.centre_y());
    const auto before = sim.state_hash();
    (void)protekt::game::build_scene(sim, rig());
    EXPECT_EQ(sim.state_hash(), before) << "drawing the ground is not a sim event";
    int ring = 0;
    for (int y = pad.y0; y <= pad.y1; ++y) {
        for (int x = pad.x0; x <= pad.x1; ++x) {
            if (foot.covers(x, y)) {
                continue; // the building itself is the one thing in the way (the sim's own rule)
            }
            ++ring;
            EXPECT_TRUE(protekt::sim::tile_passable(kSeed, x, y))
                << "open ground at " << x << "," << y;
            EXPECT_FALSE(protekt::sim::building_blocks(sim.units(), x, y))
                << "the pad outside the footprint is drivable: " << x << "," << y;
        }
    }
    constexpr int kSide = protekt::sim::kBuildingFootprintTiles;
    EXPECT_EQ(ring, ((kSide + 2) * (kSide + 2)) - (kSide * kSide));
}

} // namespace
