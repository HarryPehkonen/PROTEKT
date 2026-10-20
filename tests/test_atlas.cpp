// The atlas layout and its manifest (D2b), and its paint (D2e): pure data, no GPU.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "atlas_manifest.hpp"
#include "protekt/game/atlas.hpp"

namespace {

using protekt::game::AtlasManifest;
using protekt::game::kFallbackTile;
using protekt::game::kHalfTexel;
using protekt::game::tile_rect;
using protekt::game::UvRect;

using Rgb = std::array<std::uint8_t, 3>;

constexpr std::array<int, 3> kHullTiles{7, 8, 9};

/// Texel (x, y) of `tile` in a whole-atlas RGBA buffer.
Rgb texel(const std::vector<std::uint8_t>& px, int tile, int x, int y) {
    const int ax = ((tile % 4) * 64) + x;
    const int ay = ((tile / 4) * 64) + y;
    const std::size_t at
        = ((static_cast<std::size_t>(ay) * 256U) + static_cast<std::size_t>(ax)) * 4U;
    return {px.at(at), px.at(at + 1), px.at(at + 2)};
}

int lightness(const Rgb& rgb) { return rgb.at(0) + rgb.at(1) + rgb.at(2); }

/// Each distinct colour in `tile` and the share of the tile it covers.
std::map<Rgb, double> tone_shares(const std::vector<std::uint8_t>& px, int tile) {
    std::map<Rgb, double> shares;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            shares[texel(px, tile, x, y)] += 1.0 / 4096.0;
        }
    }
    return shares;
}

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
    EXPECT_EQ(m.tile("tank_hull"), 7) << "an alias of tank_hull_a";
    EXPECT_EQ(m.tile("tank_tracks"), 1);
    EXPECT_EQ(m.tile("tank_turret"), 2);
    EXPECT_EQ(m.tile("tank_barrel"), 3);
    EXPECT_EQ(m.tile("factory_walls"), 4);
    EXPECT_EQ(m.tile("factory_roof"), 5);
    EXPECT_EQ(m.tile("turret_body"), 6);
    EXPECT_EQ(m.tile("tank_hull_a"), 7);
    EXPECT_EQ(m.tile("tank_hull_b"), 8);
    EXPECT_EQ(m.tile("tank_hull_c"), 9);
    for (const auto name : protekt::game::kHullVariantNames) {
        EXPECT_NE(m.tile(name), kFallbackTile) << name << " is listed";
    }
    EXPECT_EQ(m.size(), std::size_t{10});
}

TEST(AtlasManifest, TheEmbeddedTextIsTheCommittedFile) {
    std::ifstream in(std::string{PROTEKT_SOURCE_DIR} + "/assets/atlas_manifest.txt");
    ASSERT_TRUE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    EXPECT_EQ(text.str(), std::string{kAtlasManifestText});
}

TEST(AtlasLayout, ATilesRectIsExactlyItsAtlasRectangleInsetByHalfATexel) {
    const UvRect first = tile_rect(0);
    EXPECT_EQ(first.u0, 0.0F + kHalfTexel);
    EXPECT_EQ(first.v0, 0.0F + kHalfTexel);
    EXPECT_EQ(first.u1, 0.25F - kHalfTexel);
    EXPECT_EQ(first.v1, 0.25F - kHalfTexel);
    const UvRect one = tile_rect(1);
    EXPECT_EQ(one.u0, 0.25F + kHalfTexel);
    EXPECT_EQ(one.v0, 0.0F + kHalfTexel);
    EXPECT_EQ(one.u1, 0.5F - kHalfTexel);
    EXPECT_EQ(one.v1, 0.25F - kHalfTexel);
    const UvRect six = tile_rect(6);
    EXPECT_EQ(six.u0, 0.5F + kHalfTexel);
    EXPECT_EQ(six.v0, 0.25F + kHalfTexel);
    EXPECT_EQ(six.u1, 0.75F - kHalfTexel);
    EXPECT_EQ(six.v1, 0.5F - kHalfTexel);
    const UvRect last = tile_rect(15);
    EXPECT_EQ(last.u0, 0.75F + kHalfTexel);
    EXPECT_EQ(last.v0, 0.75F + kHalfTexel);
    EXPECT_EQ(last.u1, 1.0F - kHalfTexel);
    EXPECT_EQ(last.v1, 1.0F - kHalfTexel);
}

TEST(AtlasLayout, AnOutOfRangeIndexGetsTheFallbackRect) {
    for (const int bad : {-1, 16, 1000}) {
        const UvRect r = tile_rect(bad);
        EXPECT_EQ(r.u1, 0.25F - kHalfTexel);
        EXPECT_EQ(r.v1, 0.25F - kHalfTexel);
    }
}

TEST(AtlasPixels, IsA256SquareOpaqueImageWithAWhiteIdentityTileAndMagentaSpares) {
    const auto px = protekt::game::atlas_pixels();
    ASSERT_EQ(px.size(), std::size_t{256} * 256 * 4);
    for (std::size_t i = 3; i < px.size(); i += 4) {
        ASSERT_EQ(px.at(i), 255) << "opaque";
    }
    // Tile 0 is solid white: it is the identity for everything that sets no UV. Tiles 10 to 15 are
    // unassigned, solid magenta, so a stray UV is loud.
    for (const int tile : {0, 10, 15}) {
        const auto rgb = [&](int x, int y) { return texel(px, tile, x, y); };
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                ASSERT_EQ(rgb(x, y), rgb(0, 0)) << "tile " << tile << " is solid";
            }
        }
    }
    EXPECT_EQ(texel(px, 0, 0, 0), (Rgb{255, 255, 255}));
    EXPECT_EQ(texel(px, 10, 0, 0), (Rgb{255, 0, 255}));
}

TEST(AtlasPaint, TheSameSeedPaintsTheSameAtlasAndAnotherSeedADifferentOne) {
    EXPECT_EQ(protekt::game::atlas_pixels(),
              protekt::game::atlas_pixels(protekt::game::kAtlasSeed));
    EXPECT_EQ(protekt::game::atlas_pixels(7), protekt::game::atlas_pixels(7));
    EXPECT_NE(protekt::game::atlas_pixels(7), protekt::game::atlas_pixels());
}

TEST(AtlasPaint, EachHullVariantIsABaseADarkAndASandToneAndEachCoversARealShare) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : kHullTiles) {
        const auto shares = tone_shares(px, tile);
        ASSERT_EQ(shares.size(), std::size_t{3}) << "tile " << tile << ": exactly three tones";
        std::vector<Rgb> tones;
        for (const auto& [rgb, share] : shares) {
            EXPECT_GE(share, 0.12) << "tile " << tile << ": every tone is a real patch, not specks";
            EXPECT_LE(share, 0.70) << "tile " << tile << ": no tone swamps the others";
            tones.push_back(rgb);
        }
        // Told apart by lightness, which is what camo reads by: dark < base < sand by a margin.
        std::sort(tones.begin(), tones.end(),
                  [](const Rgb& a, const Rgb& b) { return lightness(a) < lightness(b); });
        EXPECT_GT(lightness(tones.at(1)), lightness(tones.at(0)) + 150) << "tile " << tile;
        EXPECT_GT(lightness(tones.at(2)), lightness(tones.at(1)) + 100) << "tile " << tile;
    }
}

TEST(AtlasPaint, TheThreeHullVariantsAreVisiblyDifferent) {
    const auto px = protekt::game::atlas_pixels();
    // Texel by texel, any two variants disagree over a large share of the tile.
    for (std::size_t a = 0; a < kHullTiles.size(); ++a) {
        for (std::size_t b = a + 1; b < kHullTiles.size(); ++b) {
            int differ = 0;
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    differ += texel(px, kHullTiles.at(a), x, y) != texel(px, kHullTiles.at(b), x, y)
                                  ? 1
                                  : 0;
                }
            }
            EXPECT_GT(differ, 4096 * 35 / 100)
                << "tiles " << kHullTiles.at(a) << " and " << kHullTiles.at(b);
        }
    }
    // And by STYLE. C is blocks: every tone change, across or down, falls on the 4-texel grid.
    // A (splodges) and B (stripes) are curves and diagonals: most of theirs do not.
    const auto off_grid_share = [&](int tile) {
        int changes = 0;
        int off_grid = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 1; x < 64; ++x) {
                if (texel(px, tile, x, y) != texel(px, tile, x - 1, y)) {
                    ++changes;
                    off_grid += (x % 4) != 0 ? 1 : 0;
                }
                if (texel(px, tile, y, x) != texel(px, tile, y, x - 1)) {
                    ++changes;
                    off_grid += (x % 4) != 0 ? 1 : 0;
                }
            }
        }
        return changes == 0 ? 0.0 : static_cast<double>(off_grid) / changes;
    };
    EXPECT_EQ(off_grid_share(9), 0.0) << "C: blocks on the 4-texel grid";
    EXPECT_GT(off_grid_share(7), 0.5) << "A: splodges are round";
    EXPECT_GT(off_grid_share(8), 0.5) << "B: stripes are diagonal";
    // B's edges run diagonally: a stripe edge seen along a row moves as you go down. Its tone along
    // a row and along a column change about equally often, unlike a horizontal or vertical band.
    int across = 0;
    int down = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 1; x < 64; ++x) {
            across += texel(px, 8, x, y) != texel(px, 8, x - 1, y) ? 1 : 0;
            down += texel(px, 8, y, x) != texel(px, 8, y, x - 1) ? 1 : 0;
        }
    }
    EXPECT_GT(across, 200) << "B has many stripes";
    EXPECT_LT(std::abs(across - down), (across + down) / 4) << "B's stripes are diagonal";
}

TEST(AtlasPaint, TheTrackTileIsDarkWithLighterLinkStripesAcrossItsHeight) {
    const auto px = protekt::game::atlas_pixels();
    int light_rows = 0;
    for (int y = 0; y < 64; ++y) {
        const Rgb row = texel(px, 1, 0, y);
        for (int x = 1; x < 64; ++x) {
            ASSERT_EQ(texel(px, 1, x, y), row) << "a stripe runs the whole width";
        }
        light_rows += lightness(row) > lightness(texel(px, 1, 0, 0)) ? 1 : 0;
    }
    EXPECT_LT(lightness(texel(px, 1, 0, 0)), 3 * 100) << "the base is dark";
    // Eight links, each a stripe of two light rows, an 8-row period.
    EXPECT_EQ(light_rows, 16);
    for (int y = 0; y < 56; ++y) {
        EXPECT_EQ(texel(px, 1, 0, y), texel(px, 1, 0, y + 8)) << "row " << y;
    }
}

TEST(AtlasPaint, MetalAndBuildingTilesAreTheirBaseToneWithOnlyAFewPercentOfVariation) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : {2, 3, 4, 5, 6}) {
        int lo = 255;
        int hi = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const Rgb rgb = texel(px, tile, x, y);
                EXPECT_EQ(rgb.at(0), rgb.at(1)) << "near-neutral, so the side colour shows";
                lo = std::min(lo, static_cast<int>(rgb.at(0)));
                hi = std::max(hi, static_cast<int>(rgb.at(0)));
            }
        }
        EXPECT_GT(hi, lo) << "tile " << tile << " is not flat";
        EXPECT_LE(hi - lo, 20) << "tile " << tile << ": subtle, under 8% of full scale";
        EXPECT_GT(lo, 160) << "tile " << tile << " is light metal";
        // Slow, not noise: neighbouring texels differ by at most one step.
        for (int y = 0; y < 64; ++y) {
            for (int x = 1; x < 64; ++x) {
                EXPECT_LE(std::abs(texel(px, tile, x, y).at(0) - texel(px, tile, x - 1, y).at(0)),
                          2)
                    << "tile " << tile;
            }
        }
    }
}

} // namespace
