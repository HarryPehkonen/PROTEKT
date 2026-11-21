// A map is a document (M6): written as text, read back whole, edited by hand, drawn as a picture.
#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "protekt/app/map_file.hpp"
#include "protekt/game/png.hpp"
#include "protekt/sim/steering.hpp"

namespace {

namespace fs = std::filesystem;
using namespace protekt::app;
using protekt::world::Terrain;

fs::path temp_file(const std::string& name) {
    static int counter = 0;
    return fs::path(::testing::TempDir())
           / ("protekt-mapfile-" + std::to_string(::getpid()) + "-" + std::to_string(++counter)
              + "-" + name);
}

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// A small map that uses every part of the format.
MapDoc rich_map() {
    MapDoc map = generate_map(42, 12);
    map.name = "rich \"quoted\" map";
    map.heights.at(map.index(3, 4)) = -512;
    map.heights.at(map.index(5, 6)) = 512;
    map.terrain.at(map.index(3, 4)) = Terrain::Crater;
    map.terrain.at(map.index(5, 6)) = Terrain::Ridge;
    map.solid.at(map.index(7, 7)) = 1;
    map.solid.at(map.index(0, 11)) = 1;
    map.spawns = {{"player", 1, 2}, {"enemy", 10, 9}};
    map.places = {{"the ambush", 2, 3, 4, 2}};
    map.volumes = {{6, 2, false, 0, 96, 3, 2, 0b110101U}, {1, 8, true, 40, 200, 2, 2, 0b1001U}};
    return map;
}

std::string replace_first(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << "no '" << from << "' in:\n" << text;
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

TEST(MapFile, AGeneratedMapIsTheSeedsWorldAndRecordsTheSeed) {
    const MapDoc map = generate_map(42, 16);
    EXPECT_EQ(map.seed, 42U);
    EXPECT_EQ(map.width, 16);
    EXPECT_EQ(map.height, 16);
    const protekt::world::Chunk chunk = protekt::world::generate(42, 0, 0);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const protekt::world::Tile& tile = chunk.tiles.at(
                static_cast<std::size_t>(y) * static_cast<std::size_t>(protekt::world::kChunkSize)
                + static_cast<std::size_t>(x));
            EXPECT_EQ(map.heights.at(map.index(x, y)), tile.height);
            EXPECT_EQ(map.terrain.at(map.index(x, y)), tile.terrain);
        }
    }
    EXPECT_EQ(map.spawns.size(), 2U);
}

TEST(MapFile, ARoundTripLosesNothing) {
    const MapDoc written = rich_map();
    const MapParse back = parse_map(write_map(written));
    ASSERT_TRUE(back.ok) << back.error;
    // Field by field, so a failure names the field that was lost.
    EXPECT_EQ(back.map.name, written.name);
    EXPECT_EQ(back.map.seed, written.seed);
    EXPECT_EQ(back.map.width, written.width);
    EXPECT_EQ(back.map.height, written.height);
    EXPECT_EQ(back.map.terrain, written.terrain);
    EXPECT_EQ(back.map.heights, written.heights);
    EXPECT_EQ(back.map.solid, written.solid);
    EXPECT_EQ(back.map.spawns, written.spawns);
    EXPECT_EQ(back.map.places, written.places);
    EXPECT_EQ(back.map.volumes, written.volumes);
    EXPECT_TRUE(back.map == written);
    // And the text of the loaded map is the text it came from.
    EXPECT_EQ(write_map(back.map), write_map(written));
    EXPECT_EQ(back.map.heights.at(back.map.index(3, 4)), -512);
    EXPECT_EQ(back.map.solid.at(back.map.index(7, 7)), 1);
    EXPECT_EQ(back.map.volumes.at(1).mask_bits, 0b1001U);
}

TEST(MapFile, TwoGenerationsOfOneSeedAreByteIdenticalAndTwoSeedsAreNot) {
    EXPECT_EQ(write_map(generate_map(7, 32)), write_map(generate_map(7, 32)));
    EXPECT_NE(write_map(generate_map(7, 32)), write_map(generate_map(8, 32)));
}

TEST(MapFile, AHandEditedMapLoadsAndTheEditIsWhatTheGameSees) {
    const MapDoc original = generate_map(42, 8);
    std::string text = write_map(original);
    // His workflow: open the file, change one number and one letter, save.
    const int old_height = original.heights.at(original.index(0, 0));
    const std::string first_cell = [&] {
        std::ostringstream cell;
        cell.width(4);
        cell << old_height;
        return cell.str();
    }();
    const std::size_t heights_at = text.find("[height]");
    ASSERT_NE(heights_at, std::string::npos);
    const std::size_t cell_at = text.find(first_cell, heights_at);
    ASSERT_NE(cell_at, std::string::npos);
    text.replace(cell_at, 4, " 301");
    const std::size_t terrain_at = text.find("[terrain]");
    const std::size_t rows_at = text.find("\"\"\"\n", terrain_at) + 4;
    const char was = text.at(rows_at);
    text.at(rows_at) = was == 'R' ? 'c' : 'R';
    const MapParse edited = parse_map(text);
    ASSERT_TRUE(edited.ok) << edited.error;
    EXPECT_EQ(edited.map.heights.at(0), 301);
    EXPECT_EQ(edited.map.terrain.at(0), was == 'R' ? Terrain::Crater : Terrain::Ridge);
    // Everything else is as it was.
    MapDoc expected = original;
    expected.heights.at(0) = 301;
    expected.terrain.at(0) = edited.map.terrain.at(0);
    EXPECT_TRUE(edited.map == expected);
    EXPECT_EQ(edited.map.seed, 42U) << "a hand edit still says where the map came from";
}

TEST(MapFile, AHandWrittenSolidTileAndVolumeBecomeTheSimsOwn) {
    std::string text = write_map(generate_map(3, 4));
    text += "\n[[volume]]\nx = 1\ny = 2\nturned = false\nbase = 0\ntop = 96\nmask = "
            "\"\"\"\n#.\n##\n\"\"\"\n";
    const MapParse parsed = parse_map(text);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    const auto placed = placed_volumes(parsed.map);
    ASSERT_EQ(placed.size(), 1U);
    EXPECT_TRUE(protekt::sim::placed_solid_at(placed.at(0), 1, 2));
    EXPECT_FALSE(protekt::sim::placed_solid_at(placed.at(0), 2, 2));
    EXPECT_TRUE(protekt::sim::placed_solid_at(placed.at(0), 2, 3));
    EXPECT_EQ(placed.at(0).volume.top.raw, 96 * protekt::sim::Fixed::kOne);
}

TEST(MapFile, ABadFileIsRefusedWithItsLine) {
    const std::string good = write_map(generate_map(5, 4));
    const MapParse short_row = parse_map(
        replace_first(good, "[solid]\nrows = \"\"\"\n....", "[solid]\nrows = \"\"\"\n..."));
    EXPECT_FALSE(short_row.ok);
    EXPECT_NE(short_row.error.find("line "), std::string::npos) << short_row.error;
    const MapParse bad_letter = parse_map(
        replace_first(good, "[terrain]\nrows = \"\"\"\n", "[terrain]\nrows = \"\"\"\nZ"));
    EXPECT_FALSE(bad_letter.ok);
    const MapParse unknown = parse_map(good + "\n[wat]\n");
    EXPECT_FALSE(unknown.ok);
    EXPECT_FALSE(parse_map("").ok);
    const MapParse off_map = parse_map(good + "\n[[spawn]]\nside = \"player\"\nx = 99\ny = 0\n");
    EXPECT_FALSE(off_map.ok);
    const MapParse bad_height = parse_map(
        replace_first(good, "[height]\nrows = \"\"\"\n", "[height]\nrows = \"\"\"\n9999"));
    EXPECT_FALSE(bad_height.ok);
    EXPECT_FALSE(load_map_file("/nonexistent/protekt.map").ok);
}

TEST(MapFile, DrawWritesAnImageOfTheExpectedSizeShowingTheMap) {
    const MapDoc map = rich_map();
    const fs::path out = temp_file("draw.png");
    ASSERT_TRUE(draw_map(map, out));
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    ASSERT_TRUE(protekt::game::read_png_rgba(out, rgba, w, h));
    EXPECT_EQ(w, static_cast<std::uint32_t>(map.width * kMapDrawScale));
    EXPECT_EQ(h, static_cast<std::uint32_t>(map.height * kMapDrawScale));
    // Not pixel values: only that impassable ground and spawns change the picture.
    MapDoc bare = map;
    bare.solid.assign(bare.solid.size(), 0);
    bare.spawns.clear();
    bare.volumes.clear();
    const fs::path bare_out = temp_file("bare.png");
    ASSERT_TRUE(draw_map(bare, bare_out));
    EXPECT_NE(slurp(out), slurp(bare_out));
    MapDoc no_spawns = map;
    no_spawns.spawns.clear();
    const fs::path no_spawn_out = temp_file("nospawn.png");
    ASSERT_TRUE(draw_map(no_spawns, no_spawn_out));
    EXPECT_NE(slurp(out), slurp(no_spawn_out));
    // A pure function of the map.
    const fs::path again = temp_file("again.png");
    ASSERT_TRUE(draw_map(map, again));
    EXPECT_EQ(slurp(out), slurp(again));
    std::error_code ignored;
    fs::remove(out, ignored);
    fs::remove(bare_out, ignored);
    fs::remove(no_spawn_out, ignored);
    fs::remove(again, ignored);
}

/// The five items on his list, each read off the shipped map's own heights with the SIM's slope
/// constants (`kClimbLimit`, `kSlideThreshold`): the map is as drawn, by the rules the game plays.
TEST(MapFile, TheShippedMapLoadsAndUsesEveryItemOnTheList) {
    const MapParse parsed
        = load_map_file(std::string{PROTEKT_SOURCE_DIR} + "/assets/maps/proving_ground.map");
    ASSERT_TRUE(parsed.ok) << parsed.error;
    const MapDoc& map = parsed.map;
    EXPECT_EQ(map.spawns.size(), 2U);
    EXPECT_GE(map.places.size(), 5U);
    EXPECT_FALSE(map.volumes.empty());
    const auto h = [&](int x, int y) { return map.heights.at(map.index(x, y)); };
    const auto climbable = [&](int x0, int y0, int x1, int y1) {
        return h(x1, y1) - h(x0, y0) <= protekt::sim::kClimbLimit;
    };
    // 1. A VALLEY THAT LIMITS MOVEMENT: a floor walled on both sides by cliffs you cannot climb.
    EXPECT_FALSE(climbable(30, 20, 29, 20)) << "west wall";
    EXPECT_GT(h(29, 20) - h(30, 20), protekt::sim::kClimbLimit);
    EXPECT_GT(h(38, 20) - h(37, 20), protekt::sim::kClimbLimit);
    // 2. A RAMP UP: every step along the row is climbable and the sum reaches the plateau.
    for (int x = 15; x < 22; ++x) {
        EXPECT_TRUE(climbable(x, 30, x + 1, 30)) << "ramp step at x=" << x;
        EXPECT_LE(h(x, 30) - h(x + 1, 30), protekt::sim::kSlideThreshold) << "no slide on the ramp";
    }
    EXPECT_GE(h(22, 30) - h(15, 30), 100);
    // 3. HIGH GROUND THAT BLOCKS WEAPONS: a plateau higher than the ground either side of it,
    // by more than a body's height (a shot has to go through it, not over it).
    EXPECT_GT(h(26, 20) - h(10, 20), 100);
    EXPECT_GT(h(26, 20) - h(33, 20), 100);
    // 4. GROUND FOR AN AMBUSH: a named pocket, below its rim on three sides, open on the fourth.
    const auto ambush = std::find_if(map.places.begin(), map.places.end(),
                                     [](const MapPlace& p) { return p.name == "the ambush"; });
    ASSERT_NE(ambush, map.places.end());
    EXPECT_LT(h(ambush->x + 1, ambush->y + 1), h(ambush->x + 1, ambush->y - 1) - 100);
    EXPECT_LT(h(ambush->x + 1, ambush->y + 1), h(ambush->x + ambush->w, ambush->y + 1) - 100);
    EXPECT_LT(h(ambush->x + 1, ambush->y + 1), h(ambush->x + 1, ambush->y + ambush->h) - 100);
    EXPECT_TRUE(climbable(ambush->x - 1, ambush->y + 1, ambush->x, ambush->y + 1)
                || h(ambush->x - 1, ambush->y + 1) == h(ambush->x, ambush->y + 1));
    // 5. A PLACE YOU CAN FALL DOWN BUT NOT DRIVE BACK UP: stepping east off the plateau is a drop
    // (the slope wins), stepping back is a cliff (a wall).
    EXPECT_GT(h(45, 24) - h(46, 24), protekt::sim::kSlideThreshold);
    EXPECT_GT(h(45, 24) - h(46, 24), protekt::sim::kClimbLimit) << "going back up is a wall";
    EXPECT_FALSE(climbable(46, 24, 45, 24));
    // and the world-frame solids and the volume are in the file.
    EXPECT_NE(std::count(map.solid.begin(), map.solid.end(), std::uint8_t{1}), 0);
}

} // namespace
