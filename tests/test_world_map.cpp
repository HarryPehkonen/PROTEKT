// N4 + BZ2R: the square world map. R turns the radar into a square map of the whole world, your own
// assets and your nav marks on it (drawn as the radar draws them) and NOTHING of the enemy's, and
// it is a pure view.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/radar.hpp"
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
    ASSERT_EQ(marks.place(0, 0).outcome, NavMarks::Outcome::Placed);
    ASSERT_EQ(marks.place(255, 255).outcome, NavMarks::Outcome::Placed);
    ASSERT_EQ(marks.place(128, 77).outcome, NavMarks::Outcome::Placed);
    const WorldMap map = map_of(rig, &marks);
    const std::vector<NavMarks::Mark> dropped = marks.marks();
    ASSERT_EQ(map.marks.size(), 3U);
    for (std::size_t i = 0; i < dropped.size(); ++i) {
        EXPECT_EQ(map.marks.at(i).slot, dropped.at(i).slot);
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
        // It fits inside the frame, in the BOTTOM-LEFT corner: BZ2's minimap footprint, where the
        // radar also sits (the two swap into the same corner).
        const float margin
            = protekt::game::kPanelMarginPixels * (static_cast<float>(extent.height) / 720.0F);
        EXPECT_NEAR(layout.x, margin, 0.01F);
        EXPECT_NEAR(layout.y + layout.size, static_cast<float>(extent.height) - margin, 0.01F);
        EXPECT_GE(layout.y, 0.0F);
        EXPECT_LE(layout.x + layout.size, static_cast<float>(extent.width) + 0.01F);
    }
}

TEST(WorldMap, TheSquareIsTheRadarsMajorDiameter) {
    // The map takes the elliptical radar's long axis, so its side IS the radar's width. The owner's
    // call, 2026-10-23: "The map size should match the major diameter of the elliptical radar
    // display" -- the earlier 12.7% of the frame width (BZ2's minimap footprint) read "a tiny bit
    // smaller than what I expected".
    for (const Extent extent : {Extent{1280, 720}, Extent{1920, 1080}, Extent{2560, 1440}}) {
        const WorldMapLayout layout = world_map_layout(extent);
        const RadarPanel panel = radar_panel(extent);
        EXPECT_NEAR(layout.size, panel.width, 0.01F) << extent.width << "x" << extent.height;
    }
    // A frame too short for the square shrinks it to fit between the two margins, not off the top.
    const WorldMapLayout short_frame = world_map_layout(Extent{2400, 200});
    EXPECT_LE(short_frame.size, 200.0F);
    EXPECT_GE(short_frame.y, 0.0F);
}

TEST(WorldMap, FurnitureScalesWithThePanelNotTheFrame) {
    // The furniture constants are drawn for BZ2's 162.56 px reference panel. The square is now the
    // radar's diameter -- about 330 px at 1280 wide -- so the factor sits at its CEILING there, and
    // the blobs keep a share of the bigger panel instead of staying frame-fixed.
    const float reference = map_furniture_scale(world_map_layout(Extent{1280, 720}));
    EXPECT_NEAR(reference, protekt::game::kMapMaxFurnitureScale, 0.01F);
    // A smaller frame's panel is smaller than the reference, and its blobs shrink in proportion.
    const float smaller = map_furniture_scale(world_map_layout(Extent{640, 360}));
    EXPECT_LT(smaller, reference);
    EXPECT_GE(smaller, protekt::game::kMapMinFurnitureScale);
    // The floor and ceiling keep the dots readable and the blobs blobs at the extremes.
    const float huge = map_furniture_scale(world_map_layout(Extent{3840, 2160}));
    EXPECT_LE(huge, protekt::game::kMapMaxFurnitureScale);
}

TEST(WorldMap, NoEnemyAssetIsEverDrawn) {
    Rig rig = make_rig();
    ASSERT_GE(rig.enemy.size(), 3U) << "enemies certainly exist on this seed";
    ASSERT_GE(rig.own.size(), 3U);
    NavMarks marks;
    (void)marks.place(60, 60);
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

TEST(WorldMap, MarksAppearInSlotOrderWithTheirHighlightAndAnOverwriteMovesOne) {
    Rig rig = make_rig();
    NavMarks marks;
    EXPECT_TRUE(map_of(rig, &marks).marks.empty()) << "no phantom marks before any is placed";
    EXPECT_TRUE(map_of(rig, nullptr).marks.empty());
    ASSERT_EQ(marks.place(10, 20).slot, 0);
    ASSERT_EQ(marks.place(30, 40).slot, 1);
    ASSERT_EQ(marks.place(50, 60).slot, 2);
    ASSERT_TRUE(marks.toggle(1));
    WorldMap map = map_of(rig, &marks);
    ASSERT_EQ(map.marks.size(), 3U);
    EXPECT_EQ(map.marks.at(0).slot, 0);
    EXPECT_EQ(map.marks.at(1).slot, 1);
    EXPECT_EQ(map.marks.at(2).slot, 2);
    EXPECT_FALSE(map.marks.at(0).highlighted);
    EXPECT_TRUE(map.marks.at(1).highlighted);
    EXPECT_FALSE(map.marks.at(2).highlighted);
    // Seven more fill the table, and the eleventh overwrites slot 0: still ten, one moved.
    for (int p = 4; p <= 11; ++p) {
        (void)marks.place(p * 10, 5);
    }
    map = map_of(rig, &marks);
    ASSERT_EQ(map.marks.size(), 10U);
    EXPECT_EQ(map_cell(map.layout, MapPoint{map.marks.at(0).px, map.marks.at(0).py}),
              (MapCell{110, 5}));
    // The drawn marks add vertices, and one more mark is more.
    SceneOptions open = rig.options;
    open.show_world_map = true;
    NavMarks two;
    (void)two.place(10, 20);
    (void)two.place(30, 40);
    const std::size_t none = build_scene(rig.sim, open).vertices.size();
    open.nav_marks = &two;
    const std::size_t with_two = build_scene(rig.sim, open).vertices.size();
    (void)two.place(70, 80);
    const std::size_t with_three = build_scene(rig.sim, open).vertices.size();
    EXPECT_GT(with_two, none);
    EXPECT_GT(with_three, with_two);
}

TEST(WorldMap, AMarkIsTheRadarsSize) {
    // His ruling, 2026-10-26: the mark is "EXACTLY the same as in the radar" -- the plus has no
    // number on the map either, and its size is the radar's own.
    Rig rig = make_rig();
    NavMarks marks;
    (void)marks.place(100, 100);
    const WorldMap map = map_of(rig, &marks);
    ASSERT_EQ(map.marks.size(), 1U);
    EXPECT_FLOAT_EQ(map.marks.at(0).half, radar_nav_half(radar_panel(kExtent)));
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
        (void)marks.place(60, 60);
        (void)marks.place(0, 0);
        (void)marks.place(255, 255);
        (void)marks.toggle(1);
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

namespace protekt::game {
namespace {

// THE NEEDLE POINTS WHERE THE TANK IS GOING. A heading is the sim's yaw: 0 is world forward and a
// tank drives along (sin h, cos h) in world x/y (player.cpp). The map flips the world's y into the
// frame's y (map_point), so the same motion on the map is (sin h, -cos h). Get that sign wrong and
// the needle points south while the tank drives north -- a compass that lies is worse than none.
TEST(WorldMap, TheNeedlePointsAlongTheTanksHeadingNotAwayFromIt) {
    MapAsset blip{};
    blip.px = 500.0F;
    blip.py = 300.0F;
    blip.half = kMapUnitHalf; // the reference blip, so the needle is its nominal 3 mm
    const float reach = kMapUnitHalf + kMapHeadingLinePixels;

    // The midpoint of the base edge and of the tip edge; the needle's direction is tip - base.
    const auto direction = [&blip](float heading) {
        const std::array<MapPoint, 4> needle = map_heading_needle(blip, heading);
        const MapPoint base{(needle.at(0).x + needle.at(1).x) * 0.5F,
                            (needle.at(0).y + needle.at(1).y) * 0.5F};
        const MapPoint tip{(needle.at(2).x + needle.at(3).x) * 0.5F,
                           (needle.at(2).y + needle.at(3).y) * 0.5F};
        return std::array<float, 2>{tip.x - base.x, tip.y - base.y};
    };

    // NORTH: world forward, yaw 0. The tank drives toward +y in the world; the map's y grows DOWN,
    // so the needle must point UP the frame -- negative y, and no x.
    const std::array<float, 2> north = direction(0.0F);
    EXPECT_NEAR(north.at(0), 0.0F, 0.01F);
    EXPECT_NEAR(north.at(1), -kMapHeadingLinePixels, 0.01F);

    // EAST: yaw pi/2, the tank drives toward +x. The needle points RIGHT, and no y.
    const std::array<float, 2> east = direction(static_cast<float>(M_PI) * 0.5F);
    EXPECT_NEAR(east.at(0), kMapHeadingLinePixels, 0.01F);
    EXPECT_NEAR(east.at(1), 0.0F, 0.01F);

    // SOUTH: yaw pi. Down the frame, not up: the needle must NOT point north for a tank reversing.
    const std::array<float, 2> south = direction(static_cast<float>(M_PI));
    EXPECT_NEAR(south.at(0), 0.0F, 0.01F);
    EXPECT_NEAR(south.at(1), kMapHeadingLinePixels, 0.01F);

    // WEST: yaw 3pi/2, pointing left.
    const std::array<float, 2> west = direction(static_cast<float>(M_PI) * 1.5F);
    EXPECT_NEAR(west.at(0), -kMapHeadingLinePixels, 0.01F);
    EXPECT_NEAR(west.at(1), 0.0F, 0.01F);

    // The needle starts at the blip's edge and ends `reach` out along it: it is a needle FROM the
    // blip, not a cross through it. Checked on the north case, where both edges sit on one axis.
    const std::array<MapPoint, 4> needle = map_heading_needle(blip, 0.0F);
    const float base_y = (needle.at(0).y + needle.at(1).y) * 0.5F;
    const float tip_y = (needle.at(2).y + needle.at(3).y) * 0.5F;
    EXPECT_NEAR(base_y, blip.py - kMapUnitHalf, 0.01F);
    EXPECT_NEAR(tip_y, blip.py - reach, 0.01F);
    // And its width is the nominal 1.8 px at the reference blip: 0.9 either side of the centre
    // line.
    EXPECT_NEAR(std::abs(needle.at(0).x - needle.at(1).x) * 0.5F, kMapHeadingLineHalfPixels, 0.01F);
}

} // namespace
} // namespace protekt::game
