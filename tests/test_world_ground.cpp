// A designed ground: a map's tiles replace the seed's world wherever they lie, and nothing is
// changed while none is installed.
#include <gtest/gtest.h>

#include <cstdint>

#include "protekt/app/map_file.hpp"
#include "protekt/game/world_map.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/ground.hpp"
#include "protekt/world/world.hpp"

namespace {

using namespace protekt;

/// A 16x16 flat map at height 33, one tile of it 77 and ridged, one tile solid.
app::MapDoc small_map() {
    app::MapDoc map = app::generate_map(5, 16);
    for (std::size_t i = 0; i < map.terrain.size(); ++i) {
        map.terrain[i] = world::Terrain::Flat;
        map.heights[i] = 33;
        map.solid[i] = 0;
    }
    map.heights[map.index(3, 5)] = 77;
    map.terrain[map.index(3, 5)] = world::Terrain::Ridge;
    map.solid[map.index(8, 8)] = 1;
    map.spawns = {{"player", 2, 2}, {"enemy", 12, 12}};
    return map;
}

TEST(WorldGround, NothingInstalledIsTheSeedsWorldUnchanged) {
    const world::Tile before = world::sample(42, 3, 5);
    {
        const world::ScopedGround guard(app::map_ground(small_map()));
        EXPECT_TRUE(world::ground_installed());
    }
    EXPECT_FALSE(world::ground_installed());
    const world::Tile after = world::sample(42, 3, 5);
    EXPECT_EQ(before.height, after.height);
    EXPECT_EQ(before.terrain, after.terrain);
}

TEST(WorldGround, AnInstalledGroundIsWhatSampleAndGenerateAnswer) {
    const world::ScopedGround guard(app::map_ground(small_map()));
    EXPECT_EQ(world::sample(42, 3, 5).height, 77);
    EXPECT_EQ(world::sample(42, 3, 5).terrain, world::Terrain::Ridge);
    EXPECT_EQ(world::sample(99, 3, 5).height, 77) << "the seed has no say inside the file";
    EXPECT_EQ(world::sample(42, 0, 0).height, 33);
    // `generate` agrees with `sample` tile for tile, as it always must.
    const world::Chunk chunk = world::generate(42, 0, 0);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            const world::Tile via_sample = world::sample(42, x, y);
            const world::Tile via_chunk = chunk.tiles.at(
                static_cast<std::size_t>(y) * world::kChunkSize + static_cast<std::size_t>(x));
            EXPECT_EQ(via_sample.height, via_chunk.height) << x << "," << y;
            EXPECT_EQ(via_sample.terrain, via_chunk.terrain) << x << "," << y;
        }
    }
}

TEST(WorldGround, PastTheFileIsImpassableAndTheSolidMaskStopsAUnit) {
    const world::ScopedGround guard(app::map_ground(small_map()));
    EXPECT_EQ(world::sample(42, 16, 0).terrain, world::Terrain::Crater);
    EXPECT_EQ(world::sample(42, 200, 200).terrain, world::Terrain::Crater);
    EXPECT_FALSE(sim::tile_passable(42, 16, 0));
    EXPECT_TRUE(sim::tile_passable(42, 7, 8));
    EXPECT_FALSE(sim::tile_passable(42, 8, 8)) << "solid, though its terrain is flat";
    EXPECT_EQ(world::sample(42, 8, 8).terrain, world::Terrain::Flat);
}

TEST(WorldGround, TheWorldMapMemoDoesNotAnswerAMapFromTheSeedsOldTiles) {
    const world::Terrain seed_terrain = game::map_terrain_grid(42).at(5 * game::kWorldMapTiles + 3);
    {
        const world::ScopedGround guard(app::map_ground(small_map()));
        EXPECT_EQ(game::map_terrain_grid(42).at(5 * game::kWorldMapTiles + 3),
                  world::Terrain::Ridge);
    }
    EXPECT_EQ(game::map_terrain_grid(42).at(5 * game::kWorldMapTiles + 3), seed_terrain);
}

TEST(WorldGround, ASkirmishOnAMapStartsEachSideAtItsSpawnAndCarriesItsVolumes) {
    app::MapDoc map = small_map();
    map.volumes.push_back(app::MapVolume{4, 4, false, 0, 6, 1, 1, 1});
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Scenario scenario
        = sim::skirmish(5, app::map_scenario_data(map, sim::default_data()));
    ASSERT_FALSE(scenario.spawns.empty());
    const auto factory_at = [&](sim::Side side) {
        for (const sim::Spawn& s : scenario.spawns) {
            if (s.kind == sim::UnitKind::Factory && s.side == side) {
                return std::pair{s.tile_x, s.tile_y};
            }
        }
        return std::pair{-1, -1};
    };
    EXPECT_EQ(factory_at(sim::Side::Player), (std::pair{2, 2}));
    EXPECT_EQ(factory_at(sim::Side::Enemy), (std::pair{12, 12}));
    EXPECT_EQ(scenario.volumes.size(), 1U) << "the map's volumes reach the sim";
    for (const sim::Spawn& s : scenario.spawns) {
        EXPECT_LT(s.tile_x, 16);
        EXPECT_LT(s.tile_y, 16);
    }
}

} // namespace
