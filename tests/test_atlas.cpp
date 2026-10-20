// The atlas layout and its manifest (D2b): pure data, no GPU.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "atlas_manifest.hpp"
#include "protekt/game/atlas.hpp"

namespace {

using protekt::game::AtlasManifest;
using protekt::game::kFallbackTile;
using protekt::game::tile_rect;
using protekt::game::UvRect;

TEST(AtlasManifest, KnownNamesGiveTheirTileAndUnknownGiveTheFallback) {
    const AtlasManifest m = AtlasManifest::parse("tank_hull 0\ntank_tracks 1\nturret_body 6\n");
    EXPECT_EQ(m.tile("tank_hull"), 0);
    EXPECT_EQ(m.tile("tank_tracks"), 1);
    EXPECT_EQ(m.tile("turret_body"), 6);
    EXPECT_EQ(m.tile("no_such_part"), kFallbackTile);
    EXPECT_EQ(kFallbackTile, 0);
}

TEST(AtlasManifest, ABadLineIsSkippedAndTheRestSurvive) {
    const std::string text = "tank_hull 3\n"
                             "this has too many words 4\n"
                             "no_index\n"
                             "bad_index abc\n"
                             "negative -1\n"
                             "too_big 16\n"
                             "trailing 5x\n"
                             "\n"
                             "# a comment 9\n"
                             "   \t \n"
                             "tank_barrel 7\r\n"
                             "  spaced   \t 2  \n"
                             "last_line_no_newline 15";
    const AtlasManifest m = AtlasManifest::parse(text);
    EXPECT_EQ(m.tile("tank_hull"), 3);
    EXPECT_EQ(m.tile("tank_barrel"), 7);
    EXPECT_EQ(m.tile("spaced"), 2);
    EXPECT_EQ(m.tile("last_line_no_newline"), 15);
    EXPECT_EQ(m.size(), std::size_t{4});
    EXPECT_EQ(m.tile("bad_index"), kFallbackTile);
    EXPECT_EQ(m.tile("negative"), kFallbackTile);
    EXPECT_EQ(m.tile("too_big"), kFallbackTile);
    EXPECT_EQ(m.tile("trailing"), kFallbackTile);
}

TEST(AtlasManifest, NeverThrowsOnGarbage) {
    EXPECT_NO_THROW({
        const AtlasManifest a = AtlasManifest::parse("");
        const AtlasManifest b = AtlasManifest::parse(std::string(5000, '9'));
        const AtlasManifest c
            = AtlasManifest::parse(std::string("\0\0 \0\n 99999999999999999999 1", 27));
        const AtlasManifest d = AtlasManifest::parse("x 99999999999999999999\n");
        EXPECT_EQ(a.size(), std::size_t{0});
        EXPECT_EQ(d.tile("x"), kFallbackTile);
    });
}

TEST(AtlasManifest, ALaterLineForTheSameNameWins) {
    EXPECT_EQ(AtlasManifest::parse("a 1\na 2\n").tile("a"), 2);
}

TEST(AtlasManifest, TheCommittedManifestAssignsTheDocumentedTiles) {
    const AtlasManifest& m = protekt::game::atlas_manifest();
    EXPECT_EQ(m.tile("tank_hull"), 0);
    EXPECT_EQ(m.tile("tank_tracks"), 1);
    EXPECT_EQ(m.tile("tank_turret"), 2);
    EXPECT_EQ(m.tile("tank_barrel"), 3);
    EXPECT_EQ(m.tile("factory_walls"), 4);
    EXPECT_EQ(m.tile("factory_roof"), 5);
    EXPECT_EQ(m.tile("turret_body"), 6);
    EXPECT_EQ(m.size(), std::size_t{7});
}

TEST(AtlasManifest, TheEmbeddedTextIsTheCommittedFile) {
    std::ifstream in(std::string{PROTEKT_SOURCE_DIR} + "/assets/atlas_manifest.txt");
    ASSERT_TRUE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    EXPECT_EQ(text.str(), std::string{kAtlasManifestText});
}

TEST(AtlasLayout, ATilesRectIsExactlyItsAtlasRectangle) {
    const UvRect first = tile_rect(0);
    EXPECT_EQ(first.u0, 0.0F);
    EXPECT_EQ(first.v0, 0.0F);
    EXPECT_EQ(first.u1, 0.25F);
    EXPECT_EQ(first.v1, 0.25F);
    const UvRect one = tile_rect(1);
    EXPECT_EQ(one.u0, 0.25F);
    EXPECT_EQ(one.v0, 0.0F);
    EXPECT_EQ(one.u1, 0.5F);
    EXPECT_EQ(one.v1, 0.25F);
    const UvRect six = tile_rect(6);
    EXPECT_EQ(six.u0, 0.5F);
    EXPECT_EQ(six.v0, 0.25F);
    EXPECT_EQ(six.u1, 0.75F);
    EXPECT_EQ(six.v1, 0.5F);
    const UvRect last = tile_rect(15);
    EXPECT_EQ(last.u0, 0.75F);
    EXPECT_EQ(last.v0, 0.75F);
    EXPECT_EQ(last.u1, 1.0F);
    EXPECT_EQ(last.v1, 1.0F);
}

TEST(AtlasLayout, AnOutOfRangeIndexGetsTheFallbackRect) {
    for (const int bad : {-1, 16, 1000}) {
        const UvRect r = tile_rect(bad);
        EXPECT_EQ(r.u1, 0.25F);
        EXPECT_EQ(r.v1, 0.25F);
    }
}

TEST(AtlasPixels, IsA256SquareRgbaImageOfSolidTiles) {
    const auto px = protekt::game::atlas_pixels();
    ASSERT_EQ(px.size(), std::size_t{256} * 256 * 4);
    const auto at = [&](std::size_t x, std::size_t y, std::size_t c) {
        return px.at(((y * 256) + x) * 4 + c);
    };
    for (std::size_t tile = 0; tile < 16; ++tile) {
        const std::size_t x0 = (tile % 4) * 64;
        const std::size_t y0 = (tile / 4) * 64;
        for (std::size_t c = 0; c < 4; ++c) {
            const std::uint8_t want = at(x0, y0, c);
            EXPECT_EQ(at(x0 + 63, y0 + 63, c), want) << "tile " << tile;
            EXPECT_EQ(at(x0 + 31, y0 + 7, c), want) << "tile " << tile;
        }
        EXPECT_EQ(at(x0, y0, 3), 255) << "opaque";
    }
    // Tile 0 is white: it is the identity for everything that sets no UV.
    EXPECT_EQ(at(0, 0, 0), 255);
    EXPECT_EQ(at(0, 0, 1), 255);
    EXPECT_EQ(at(0, 0, 2), 255);
    // The track tile is dark; the tiles differ from each other where they should.
    EXPECT_LT(at(64, 0, 0), 100);
}

} // namespace
