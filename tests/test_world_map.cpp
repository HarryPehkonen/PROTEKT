// N4 + BZ2R: the square world map. R turns the radar into a square map of the whole world, your own
// assets and your nav marks on it and NOTHING of the enemy's, and it is a pure view.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/world_map.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    std::vector<UnitId> own;
    std::vector<UnitId> enemy;
};

[[nodiscard]] Rig make_rig(std::vector<Spawn> extra = {}) {
    Rig rig{};
    std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 40, 40},    {UnitKind::Tank, Side::Player, 44, 41},
        {UnitKind::Factory, Side::Player, 30, 30}, {UnitKind::Mortar, Side::Player, 200, 220},
        {UnitKind::Tank, Side::Enemy, 38, 60},     {UnitKind::Factory, Side::Enemy, 230, 230},
        {UnitKind::Turret, Side::Enemy, 120, 130}};
    spawns.insert(spawns.end(), extra.begin(), extra.end());
    rig.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    for (const sim::Unit& unit : rig.sim.units()) {
        (unit.side == Side::Player ? rig.own : rig.enemy).push_back(unit.id);
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.viewer_unit = rig.own.at(0);
    return rig;
}

[[nodiscard]] WorldMap map_of(const Rig& rig, const NavMarks* marks = nullptr) {
    return world_map(rig.sim, rig.options.extent, Side::Player, rig.options.viewer_unit, marks);
}

[[nodiscard]] bool same_bytes(const Scene& a, const Scene& b) {
    return a.vertices.size() == b.vertices.size()
           && (a.vertices.empty()
               || std::memcmp(a.vertices.data(), b.vertices.data(),
                              a.vertices.size() * sizeof(SceneVertex))
                      == 0);
}

TEST(WorldMap, TheKeyROpensAndClosesAndNoOtherKeyDoes) {
    input::InputFrame frame{};
    EXPECT_FALSE(input::world_map_toggled(frame));
    frame.keys.push_back(input::KeyEvent{input::kKeyR, true, false});
    EXPECT_TRUE(input::world_map_toggled(frame));
    input::InputFrame release{};
    release.keys.push_back(input::KeyEvent{input::kKeyR, false, false});
    EXPECT_FALSE(input::world_map_toggled(release));
    input::InputFrame modified{};
    modified.keys.push_back(input::KeyEvent{input::kKeyR, true, false, true, false});
    EXPECT_FALSE(input::world_map_toggled(modified));
    for (const std::int32_t other : {input::kKeyTab, input::kKeyM, input::kKeyN, input::kKeyG,
                                     input::kKeyC, input::kKeyEscape}) {
        input::InputFrame not_it{};
        not_it.keys.push_back(input::KeyEvent{other, true, false});
        EXPECT_FALSE(input::world_map_toggled(not_it)) << other;
    }
}

TEST(WorldMap, OIsNotTheMapAnyMoreBecauseBZ2UsesItForObjectives) {
    // 18 is HID 'O'. BZ2's O means OBJECTIVES, so it must not keep opening the map by habit: the
    // old binding would make the key do two things the day objectives are built.
    for (const float shift : {0.0F, 1.0F}) {
        input::InputFrame frame{};
        frame.keys.push_back(input::KeyEvent{18, true, false, shift != 0.0F, false});
        EXPECT_FALSE(input::world_map_toggled(frame));
    }
}

TEST(WorldMap, TheMapReplacesTheRadarRatherThanSittingOnTopOfIt) {
    // BZ2's R turns the radar into the map in the same corner. The radar's face is the near-black
    // {0.04, 0.05, 0.06} and nothing else in a frame uses it, so "is the radar drawn?" is a count.
    Rig rig = make_rig();
    SceneOptions radar_options = rig.options;
    radar_options.show_world_map = false;
    SceneOptions map_options = rig.options;
    map_options.show_world_map = true;
    const Scene radar_scene = build_scene(rig.sim, radar_options);
    const Scene map_scene = build_scene(rig.sim, map_options);
    const auto faces = [](const Scene& scene) {
        std::size_t count = 0;
        for (const SceneVertex& vertex : scene.vertices) {
            if (vertex.colour[0] == 0.04F && vertex.colour[1] == 0.05F
                && vertex.colour[2] == 0.06F) {
                ++count;
            }
        }
        return count;
    };
    EXPECT_GT(faces(radar_scene), 0U) << "the radar's own face must be drawn when no map is up";
    EXPECT_EQ(faces(map_scene), 0U) << "the map must replace the radar, not be stacked on it";
    // And the map itself is really there, so the zero above is not just an empty frame.
    EXPECT_GT(map_scene.vertices.size(), std::size_t{0});
}

TEST(WorldMap, RThatIsNotAnyOtherBindingsKey) {
    // R must not be a key another binding already answers to: the interaction layer must do
    // nothing at all with it (the map toggle is the app's, a view).
    Rig rig = make_rig();
    Interaction player{rig.own.at(0)};
    input::InputFrame frame{};
    frame.keys.push_back(input::KeyEvent{input::kKeyR, true, false});
    player.handle(frame, rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_TRUE(player.message().empty());
    EXPECT_FALSE(player.go_armed());
}

TEST(WorldMap, AClosedMapDrawsNothingAndAnOpenOneDrawsTheWorldInsteadOfTheRadar) {
    Rig rig = make_rig();
    const Scene plain = build_scene(rig.sim, rig.options);
    SceneOptions closed = rig.options;
    closed.show_world_map = false;
    EXPECT_TRUE(same_bytes(plain, build_scene(rig.sim, closed)));

    SceneOptions open = rig.options;
    open.show_world_map = true;
    const Scene with_map = build_scene(rig.sim, open);
    EXPECT_GT(with_map.vertices.size(), std::size_t{0}) << "the map itself must be drawn";
    // BZ2R changed this: R turns the radar INTO the map, so the map is not the old frame with a
    // picture appended -- the radar's vertices are gone and the map's are in their place.
    EXPECT_FALSE(same_bytes(plain, with_map));
    // And closing it again returns the very same frame.
    EXPECT_TRUE(same_bytes(plain, build_scene(rig.sim, rig.options)));
}

TEST(WorldMap, TheTransformPutsTheFirstLastAndMiddleTilesInTheirExactCells) {
    for (const Extent extent : {Extent{1280, 720}, Extent{720, 1280}, Extent{1920, 1080}}) {
        const WorldMapLayout layout = world_map_layout(extent);
        const auto cell_of = [&](float tile_x, float tile_y) {
            const MapPoint at = map_point(layout, tile_x, tile_y);
            return map_cell(layout, at);
        };
        EXPECT_EQ(cell_of(0.5F, 0.5F), (MapCell{0, 0}));
        EXPECT_EQ(cell_of(255.5F, 255.5F), (MapCell{255, 255}));
        EXPECT_EQ(cell_of(128.5F, 128.5F), (MapCell{128, 128}));
        EXPECT_EQ(cell_of(17.5F, 200.5F), (MapCell{17, 200}));
        // The corners of the map are the corners of its first and last cells: west edge is x of
        // tile 0, east edge is x of tile 256, and north is up (a larger y is a smaller pixel y).
        EXPECT_FLOAT_EQ(map_point(layout, 0.0F, 0.0F).x, layout.x);
        EXPECT_NEAR(map_point(layout, 256.0F, 0.0F).x, layout.x + layout.size, 1e-3F);
        EXPECT_NEAR(map_point(layout, 0.0F, 0.0F).y, layout.y + layout.size, 1e-3F);
        EXPECT_NEAR(map_point(layout, 0.0F, 256.0F).y, layout.y, 1e-3F);
    }
}

TEST(WorldMap, AnAssetAndAMarkLandInTheCellOfTheirOwnTile) {
    Rig rig = make_rig();
    NavMarks marks;
    ASSERT_EQ(marks.press(0, 0).outcome, NavMarks::Outcome::Dropped);
    ASSERT_EQ(marks.press(255, 255).outcome, NavMarks::Outcome::Dropped);
    ASSERT_EQ(marks.press(128, 77).outcome, NavMarks::Outcome::Dropped);
    const WorldMap map = map_of(rig, &marks);
    const std::vector<NavMarks::Mark> dropped = marks.marks();
    ASSERT_EQ(map.marks.size(), 3U);
    for (std::size_t i = 0; i < dropped.size(); ++i) {
        EXPECT_EQ(map.marks.at(i).number, dropped.at(i).number);
        // A mark's centre is its cell, whatever the cell: even the first and last are not clipped.
        EXPECT_EQ(map_cell(map.layout, MapPoint{map.marks.at(i).px, map.marks.at(i).py}),
                  (MapCell{dropped.at(i).tile_x, dropped.at(i).tile_y}));
    }
    ASSERT_FALSE(map.assets.empty());
    for (const MapAsset& asset : map.assets) {
        const sim::Unit* unit = rig.sim.find(asset.id);
        ASSERT_NE(unit, nullptr);
        EXPECT_EQ(map_cell(map.layout, MapPoint{asset.px, asset.py}),
                  (MapCell{unit->x.floor_int(), unit->y.floor_int()}));
    }
}

TEST(WorldMap, TheMapIsSquareWhateverTheAspectOfTheScreen) {
    for (const Extent extent : {Extent{1280, 720}, Extent{720, 1280}, Extent{2400, 600},
                                Extent{800, 800}, Extent{1920, 1080}, Extent{640, 360}}) {
        const WorldMapLayout layout = world_map_layout(extent);
        const MapPoint origin = map_point(layout, 0.0F, 0.0F);
        const MapPoint east = map_point(layout, 256.0F, 0.0F);
        const MapPoint north = map_point(layout, 0.0F, 256.0F);
        EXPECT_NEAR(east.x - origin.x, origin.y - north.y, 1e-2F)
            << extent.width << "x" << extent.height;
        // One tile is the same number of pixels along both axes.
        const MapPoint one_east = map_point(layout, 1.0F, 0.0F);
        const MapPoint one_north = map_point(layout, 0.0F, 1.0F);
        EXPECT_NEAR(one_east.x - origin.x, origin.y - one_north.y, 1e-3F);
        EXPECT_GT(layout.pixels_per_tile, 0.0F);
        // And it fits inside the frame, centred.
        EXPECT_GE(layout.x, 0.0F);
        EXPECT_GE(layout.y, 0.0F);
        EXPECT_LE(layout.x + layout.size, static_cast<float>(extent.width) + 0.01F);
        EXPECT_LE(layout.y + layout.size, static_cast<float>(extent.height) + 0.01F);
    }
}

TEST(WorldMap, NoEnemyAssetIsEverDrawn) {
    Rig rig = make_rig();
    ASSERT_GE(rig.enemy.size(), 3U) << "enemies certainly exist on this seed";
    ASSERT_GE(rig.own.size(), 3U);
    NavMarks marks;
    (void)marks.press(60, 60);
    const WorldMap map = map_of(rig, &marks);
    for (const MapAsset& asset : map.assets) {
        EXPECT_EQ(std::count(rig.enemy.begin(), rig.enemy.end(), asset.id), 0)
            << "enemy unit " << asset.id.value << " is on the map";
        EXPECT_EQ(rig.sim.find(asset.id)->side, Side::Player);
    }
    // Every own living asset is there, and the count says nothing else is.
    EXPECT_EQ(map.assets.size(), rig.own.size());
    // The enemy factory is a building and the enemy's own: neither it nor a tile near it is drawn.
    for (const MapAsset& asset : map.assets) {
        EXPECT_NE(rig.sim.find(asset.id)->side, Side::Enemy);
    }
    // Even asked as the enemy's viewer the map shows only that viewer's own side: it never has a
    // way to be handed the other one.
    const WorldMap theirs = world_map(rig.sim, kExtent, Side::Enemy, rig.enemy.front(), nullptr);
    for (const MapAsset& asset : theirs.assets) {
        EXPECT_EQ(std::count(rig.own.begin(), rig.own.end(), asset.id), 0);
    }
}

TEST(WorldMap, OwnUnitsAreDrawnTheDrivenOneIsMarkedApartAndADeadOneStops) {
    Rig rig = make_rig({{UnitKind::Tank, Side::Enemy, 47, 41},
                        {UnitKind::Tank, Side::Enemy, 47, 42},
                        {UnitKind::Tank, Side::Enemy, 46, 40}});
    WorldMap map = map_of(rig);
    ASSERT_EQ(map.assets.size(), rig.own.size());
    int driven = 0;
    for (const MapAsset& asset : map.assets) {
        if (asset.driven) {
            ++driven;
            EXPECT_EQ(asset.id, rig.options.viewer_unit);
        }
    }
    EXPECT_EQ(driven, 1);

    // Fight until a (non-driven) own unit dies; it must leave the map and nothing else may.
    UnitId lost{};
    for (int tick = 0; tick < 6000 && !lost.valid(); ++tick) {
        rig.sim.step();
        for (const UnitId id : rig.own) {
            if (!rig.sim.find(id)->alive && id != rig.options.viewer_unit) {
                lost = id;
            }
        }
    }
    ASSERT_TRUE(lost.valid()) << "nothing died, so nothing is proved";
    map = map_of(rig);
    EXPECT_EQ(std::count_if(map.assets.begin(), map.assets.end(),
                            [&](const MapAsset& a) { return a.id == lost; }),
              0);
    std::size_t alive = 0;
    for (const UnitId id : rig.own) {
        alive += rig.sim.find(id)->alive ? 1U : 0U;
    }
    EXPECT_EQ(map.assets.size(), alive);
}

TEST(WorldMap, MarksAppearWithTheirNumbersAndAClearedOneDisappears) {
    Rig rig = make_rig();
    NavMarks marks;
    EXPECT_TRUE(map_of(rig, &marks).marks.empty()) << "no phantom marks before any is dropped";
    EXPECT_TRUE(map_of(rig, nullptr).marks.empty());
    ASSERT_EQ(marks.press(10, 20).number, 0);
    ASSERT_EQ(marks.press(30, 40).number, 1);
    ASSERT_EQ(marks.press(50, 60).number, 2);
    WorldMap map = map_of(rig, &marks);
    ASSERT_EQ(map.marks.size(), 3U);
    EXPECT_EQ(map.marks.at(0).number, 0);
    EXPECT_EQ(map.marks.at(1).number, 1);
    EXPECT_EQ(map.marks.at(2).number, 2);
    ASSERT_EQ(marks.press(30, 40).outcome, NavMarks::Outcome::Cleared);
    map = map_of(rig, &marks);
    ASSERT_EQ(map.marks.size(), 2U);
    EXPECT_EQ(map.marks.at(0).number, 0);
    EXPECT_EQ(map.marks.at(1).number, 2);
    // The drawn marks add vertices: the numbers are in the scene, and one fewer mark is fewer.
    SceneOptions open = rig.options;
    open.show_world_map = true;
    const std::size_t none = build_scene(rig.sim, open).vertices.size();
    open.nav_marks = &marks;
    const std::size_t two = build_scene(rig.sim, open).vertices.size();
    (void)marks.press(70, 80);
    const std::size_t three = build_scene(rig.sim, open).vertices.size();
    EXPECT_GT(two, none);
    EXPECT_GT(three, two);
}

TEST(WorldMap, TheNumbersAreBigEnoughToRead) {
    Rig rig = make_rig();
    NavMarks marks;
    (void)marks.press(100, 100);
    const WorldMap map = map_of(rig, &marks);
    ASSERT_EQ(map.marks.size(), 1U);
    // A digit is 5 cells across, so 2.0 px a cell is 10 px. That is BZ2's own HUD text height,
    // measured by counting pixels: 10 px in its 800x600 originals and still ~10 px in a 1280x730
    // screenshot of the remaster, so it is a fixed pixel font that ignores the resolution. The
    // user's instruction is to match BZ2, and the plainer floor this replaced (15 px, chosen for
    // arm's-length reading on a laptop) is not BZ2's. The plate is 13 px round the 10 px digit.
    EXPECT_GE(map.marks.at(0).cell, 2.0F);
    EXPECT_GE(map.marks.at(0).half, 6.5F);
}

TEST(WorldMap, ThePaletteIsTheGroundsOwn) {
    // The map's colour for a terrain is the mean of that terrain family's own atlas tiles, so it
    // cannot disagree with the ground: four families, four different colours, none black.
    std::vector<MapColour> seen;
    for (int t = 0; t < world::kTerrainCount; ++t) {
        const MapColour colour = map_terrain_colour(static_cast<world::Terrain>(t));
        EXPECT_GT(colour.r + colour.g + colour.b, 0.2F);
        EXPECT_EQ(std::count(seen.begin(), seen.end(), colour), 0);
        seen.push_back(colour);
    }
}

TEST(WorldMap, TheMapIsAPureViewAndTwoRunsAreByteIdentical) {
    const auto run = [] {
        Rig rig = make_rig();
        NavMarks marks;
        (void)marks.press(60, 60);
        (void)marks.press(0, 0);
        (void)marks.press(255, 255);
        SceneOptions open = rig.options;
        open.show_world_map = true;
        open.nav_marks = &marks;
        const std::uint64_t before = rig.sim.state_hash();
        const Scene scene = build_scene(rig.sim, open);
        EXPECT_EQ(rig.sim.state_hash(), before) << "drawing the map changed the sim";
        return scene;
    };
    EXPECT_TRUE(same_bytes(run(), run()));
}

} // namespace
} // namespace protekt::game
namespace protekt::game {
namespace {

TEST(WorldMap, TheTerrainGridAgreesWithTheWorldTileForTile) {
    const std::vector<world::Terrain>& grid = map_terrain_grid(kSeed);
    ASSERT_EQ(grid.size(), static_cast<std::size_t>(kWorldMapTiles) * kWorldMapTiles);
    for (const int x : {0, 1, 63, 64, 127, 128, 200, 255}) {
        for (const int y : {0, 5, 63, 64, 130, 192, 254, 255}) {
            EXPECT_EQ(grid.at(static_cast<std::size_t>((y * kWorldMapTiles) + x)),
                      world::sample(kSeed, x, y).terrain)
                << x << "," << y;
        }
    }
}

} // namespace
} // namespace protekt::game
