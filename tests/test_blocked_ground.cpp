// NO INVISIBLE WALLS (G9). Harri, driving the Proving Ground, twice: "And there is also something
// blocking me in a straight passage. I don't see anything odd in the map." / "There is still some
// invisible wall in the middle of the proving-grounds map." G5 found it (test_slope_drive.cpp,
// StraightPassage): the map's kerb (a terrain VOLUME) and rockfall (SOLID GROUND) stop a tank, the
// map tool hatches both in red, and the game drew neither. His rule: an impassable tile with
// nothing drawn on it is an invisible wall. These pin that the game now draws every one, asked from
// the very data the sim stops a unit by -- not from a list of where the walls are today.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

#include "protekt/app/map_file.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/blocked_ground.hpp"
#include "protekt/game/png.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/volume.hpp"
#include "protekt/world/ground.hpp"

namespace protekt {
namespace {

using Tiles = std::set<std::pair<int, int>>;

[[nodiscard]] app::MapDoc proving_ground() {
    const app::MapParse parsed
        = app::load_map_file(std::string{PROTEKT_SOURCE_DIR} + "/assets/maps/proving_ground.map");
    EXPECT_TRUE(parsed.ok) << parsed.error;
    return parsed.map;
}

/// The battle the game starts on `map` (`game.cpp`, `--mapfile`): a skirmish on the map's data.
[[nodiscard]] sim::Sim battle_on(const app::MapDoc& map) {
    return sim::Sim::create(
        sim::skirmish(map.seed, app::map_scenario_data(map, sim::default_data())));
}

/// A scene centred on world tile (`x`, `y`) that meshes `radius` tiles round it.
[[nodiscard]] game::SceneOptions looking_at(std::uint64_t seed, int x, int y, int radius) {
    game::SceneOptions o;
    o.seed = seed;
    o.radius_tiles = radius;
    o.camera.x = (static_cast<float>(x) + 0.5F) * game::kMetresPerTile;
    o.camera.z = (static_cast<float>(y) + 0.5F) * game::kMetresPerTile;
    o.camera.y = 8.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

/// The world tiles whose ground quad is painted from atlas tile `name` (`test_site_ground.cpp`'s
/// reading): a triangle whose three vertices all sample inside that tile, by its centroid.
[[nodiscard]] Tiles painted_with(const game::Scene& scene, std::string_view name) {
    const game::UvRect rect = game::atlas_manifest().rect(name);
    const auto inside = [&](const game::SceneVertex& v) {
        return v.u >= rect.u0 && v.u <= rect.u1 && v.v >= rect.v0 && v.v <= rect.v1;
    };
    Tiles found;
    for (std::size_t i = 0; i + 2 < scene.vertices.size(); i += 3) {
        const game::SceneVertex& a = scene.vertices.at(i);
        const game::SceneVertex& b = scene.vertices.at(i + 1);
        const game::SceneVertex& c = scene.vertices.at(i + 2);
        if (!inside(a) || !inside(b) || !inside(c)) {
            continue;
        }
        const float x = (a.position[0] + b.position[0] + c.position[0]) / 3.0F;
        const float z = (a.position[2] + b.position[2] + c.position[2]) / 3.0F;
        found.insert({static_cast<int>(std::floor(x / game::kMetresPerTile)),
                      static_cast<int>(std::floor(z / game::kMetresPerTile))});
    }
    return found;
}

/// Every tile of `map` the sim will not let a GROUNDED unit into, asked the sim's own way: solid
/// ground (`world::ground_solid`, the mover's first refusal) or a volume of the battle's that is
/// solid there and stops a unit at lift zero (`placed_solid_at` and `volume_stops`, the mover's
/// second).
[[nodiscard]] Tiles stops_a_unit(const sim::Sim& sim, const app::MapDoc& map) {
    Tiles stops;
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            bool stopped = world::ground_solid(x, y);
            for (const sim::PlacedVolume& v : sim.volumes()) {
                stopped = stopped
                          || (sim::placed_solid_at(v, x, y)
                              && sim::volume_stops(v.volume, sim::Fixed::zero()));
            }
            if (stopped) {
                stops.insert({x, y});
            }
        }
    }
    return stops;
}

[[nodiscard]] std::string listed(const Tiles& tiles) {
    std::ostringstream out;
    for (const auto& [x, y] : tiles) {
        out << " (" << x << "," << y << ")";
    }
    return out.str();
}

TEST(InvisibleWall, EveryTileThatStopsAUnitOnTheProvingGroundIsDrawnBlocked) {
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Sim sim = battle_on(map);
    const Tiles stops = stops_a_unit(sim, map);
    // The map has walls of BOTH kinds, or this would prove nothing about one of them.
    ASSERT_FALSE(sim.volumes().empty()) << "the Proving Ground's volumes reach the sim";
    bool solid_ground = false;
    for (const auto& [x, y] : stops) {
        solid_ground = solid_ground || world::ground_solid(x, y);
    }
    ASSERT_TRUE(solid_ground) << "the Proving Ground has solid ground";

    // One scene that meshes the whole 64x64 map.
    const game::Scene scene = game::build_scene(
        sim, looking_at(map.seed, map.width / 2, map.height / 2, map.width / 2 + 8));
    const Tiles drawn = painted_with(scene, game::kBlockedGroundTile);
    Tiles invisible;
    for (const auto& tile : stops) {
        if (!drawn.contains(tile)) {
            invisible.insert(tile);
        }
    }
    EXPECT_TRUE(invisible.empty()) << invisible.size() << " of " << stops.size()
                                   << " tiles that stop a unit are drawn as open ground (an "
                                      "INVISIBLE WALL):"
                                   << listed(invisible);
    // And nothing is drawn blocked that the sim lets a unit into at some height: the marking
    // means "this stops something", never "this is decoration".
    for (const auto& [x, y] : drawn) {
        EXPECT_TRUE(game::ground_blocked(sim.volumes(), x, y)) << x << "," << y;
    }
}

TEST(InvisibleWall, TheTilesThatTurnedHimAsideInTheValleyAreDrawn) {
    // His tank, driven down valley column 33, is turned off it at the kerb (row 24) and the
    // rockfall turns column 31 aside at row 32 (StraightPassage, test_slope_drive.cpp). The scene
    // as he sees it from there, with the camera on column 33 just north of the kerb.
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Sim sim = battle_on(map);
    const game::Scene scene = game::build_scene(sim, looking_at(map.seed, 33, 20, 24));
    const Tiles drawn = painted_with(scene, game::kBlockedGroundTile);
    EXPECT_TRUE(drawn.contains({33, 24})) << "the kerb across his column";
    EXPECT_TRUE(drawn.contains({31, 32})) << "the rockfall";
    EXPECT_FALSE(drawn.contains({33, 23})) << "the open valley north of the kerb";
}

TEST(InvisibleWall, ABattleWithNoWallsDrawsNone) {
    // A skirmish on the seed's own ground has no solid ground and no volumes: nothing is hatched.
    const sim::Sim sim = sim::Sim::create(sim::skirmish(42));
    ASSERT_TRUE(sim.volumes().empty());
    const game::Scene scene = game::build_scene(sim, looking_at(42, 60, 60, 24));
    EXPECT_TRUE(painted_with(scene, game::kBlockedGroundTile).empty());
}

/// How many of `scene`'s vertices are the world map's blocked red: the hatch's bright tone, flat.
/// Nothing else in a frame is that colour as a vertex colour (the ground's red is in its texture).
[[nodiscard]] std::size_t blocked_map_vertices(const game::Scene& scene) {
    std::size_t n = 0;
    for (const game::SceneVertex& v : scene.vertices) {
        n += v.colour[0] == static_cast<float>(game::kBlockedHatchBright[0]) / 255.0F
                     && v.colour[1] == static_cast<float>(game::kBlockedHatchBright[1]) / 255.0F
                     && v.colour[2] == static_cast<float>(game::kBlockedHatchBright[2]) / 255.0F
                 ? 1U
                 : 0U;
    }
    return n;
}

TEST(InvisibleWall, TheWorldMapShowsTheWallsInTheMapToolsRed) {
    // R's square map of the world is the other place he looks for what is ahead: the kerb and the
    // rockfall are red there too, and only while the map is up.
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    const sim::Sim sim = battle_on(map);
    game::SceneOptions options = looking_at(map.seed, 33, 20, 24);
    options.extent = game::Extent{1280, 720};
    options.viewer_unit = sim.units().front().id;
    options.show_world_map = false;
    EXPECT_EQ(blocked_map_vertices(game::build_scene(sim, options)), 0U);
    options.show_world_map = true;
    EXPECT_GT(blocked_map_vertices(game::build_scene(sim, options)), 0U);
}

TEST(InvisibleWall, AWorldMapWithNoWallsShowsNoRed) {
    const sim::Sim sim = sim::Sim::create(sim::skirmish(42));
    game::SceneOptions options = looking_at(42, 60, 60, 24);
    options.extent = game::Extent{1280, 720};
    options.viewer_unit = sim.units().front().id;
    options.show_world_map = true;
    EXPECT_EQ(blocked_map_vertices(game::build_scene(sim, options)), 0U);
}

TEST(InvisibleWall, TheGameAndTheMapToolHatchAlike) {
    // The map tool draws a tile 8 pixels across, the atlas 64 texels: map pixel (a, b) of a wall
    // tile is atlas texel (8a, 8b) of the blocked tile, colour for colour. One look, not two.
    const app::MapDoc map = proving_ground();
    const std::filesystem::path png = std::filesystem::temp_directory_path()
                                      / ("protekt-blocked-" + std::to_string(::getpid()) + ".png");
    ASSERT_TRUE(app::draw_map(map, png));
    std::vector<std::uint8_t> drawing;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    ASSERT_TRUE(game::read_png_rgba(png, drawing, w, h));
    std::filesystem::remove(png);
    const std::vector<std::uint8_t> atlas = game::atlas_pixels(game::kAtlasSeed);
    const game::TileCell cell
        = game::tile_cell(game::atlas_manifest().tile(game::kBlockedGroundTile));
    // The rockfall's north-west tile, solid ground in the map.
    constexpr int kTx = 30;
    constexpr int kTy = 32;
    ASSERT_NE(map.solid[map.index(kTx, kTy)], 0);
    constexpr int kScale = static_cast<int>(game::kTileSide) / app::kMapDrawScale;
    // The interior only: the map tool rims a tile that looks into a cliff with a 2-pixel black
    // line (its cliff layer, drawn over the hatch), which is not the hatch.
    constexpr int kRim = 2;
    for (int b = kRim; b < app::kMapDrawScale - kRim; ++b) {
        for (int a = kRim; a < app::kMapDrawScale - kRim; ++a) {
            const std::size_t mp = ((static_cast<std::size_t>((kTy * app::kMapDrawScale) + b) * w)
                                    + static_cast<std::size_t>((kTx * app::kMapDrawScale) + a))
                                   * 4U;
            const std::size_t ax
                = (std::size_t{cell.col} * game::kTileSide) + static_cast<std::size_t>(a * kScale);
            const std::size_t ay
                = (std::size_t{cell.row} * game::kTileSide) + static_cast<std::size_t>(b * kScale);
            const std::size_t ap = ((ay * game::kAtlasSide) + ax) * 4U;
            for (std::size_t k = 0; k < 3; ++k) {
                EXPECT_EQ(drawing.at(mp + k), atlas.at(ap + k))
                    << a << "," << b << " channel " << k;
            }
        }
    }
}

} // namespace
} // namespace protekt
