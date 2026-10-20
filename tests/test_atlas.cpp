// The atlas layout and its manifest (D2b), and its paint (D2e): pure data, no GPU.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
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
#include "protekt/game/scene.hpp"

namespace {

using protekt::game::AtlasManifest;
using protekt::game::kFallbackTile;
using protekt::game::kHalfTexel;
using protekt::game::tile_rect;
using protekt::game::UvRect;

using Rgb = std::array<std::uint8_t, 3>;

/// Every camo tile: the neutral set (7, 8, 9), the player's (10, 11, 12), the enemy's (13, 14, 15).
constexpr std::array<int, 9> kHullTiles{7, 8, 9, 10, 11, 12, 13, 14, 15};
/// The camo sets, each a, b, c: splodges, stripes, blocks.
constexpr std::array<std::array<int, 3>, 3> kHullSets{{{7, 8, 9}, {10, 11, 12}, {13, 14, 15}}};
/// The two team sets, which tanks wear.
constexpr std::array<int, 6> kTeamHullTiles{10, 11, 12, 13, 14, 15};

/// Texel (x, y) of `tile` in a whole-atlas RGBA buffer.
Rgb texel(const std::vector<std::uint8_t>& px, int tile, int x, int y) {
    const int ax = ((tile % 4) * 64) + x;
    const int ay = ((tile / 4) * 64) + y;
    const std::size_t at
        = ((static_cast<std::size_t>(ay) * 256U) + static_cast<std::size_t>(ax)) * 4U;
    return {px.at(at), px.at(at + 1), px.at(at + 2)};
}

int lightness(const Rgb& rgb) { return rgb.at(0) + rgb.at(1) + rgb.at(2); }

/// Rec. 709 luma of the encoded values, in [0, 1].
double luma(const Rgb& rgb) {
    return ((0.2126 * rgb.at(0)) + (0.7152 * rgb.at(1)) + (0.0722 * rgb.at(2))) / 255.0;
}

/// HSV saturation, (max - min) / max, in [0, 1]: how much hue a colour has to read by.
double saturation(const Rgb& rgb) {
    const int hi = std::max({rgb.at(0), rgb.at(1), rgb.at(2)});
    const int lo = std::min({rgb.at(0), rgb.at(1), rgb.at(2)});
    return hi == 0 ? 0.0 : static_cast<double>(hi - lo) / hi;
}

/// HSV hue in degrees, [0, 360).
double hue(const Rgb& rgb) {
    const double r = rgb.at(0);
    const double g = rgb.at(1);
    const double b = rgb.at(2);
    const double hi = std::max({r, g, b});
    const double span = hi - std::min({r, g, b});
    if (span == 0.0) {
        return 0.0;
    }
    double h = 0.0;
    if (hi == r) {
        h = 60.0 * std::fmod((g - b) / span, 6.0);
    } else if (hi == g) {
        h = 60.0 * (((b - r) / span) + 2.0);
    } else {
        h = 60.0 * (((r - g) / span) + 4.0);
    }
    return h < 0.0 ? h + 360.0 : h;
}

/// The shorter way round the hue circle from `a` to `b`, in degrees, [0, 180].
double hue_distance(double a, double b) {
    const double d = std::fabs(a - b);
    return d > 180.0 ? 360.0 - d : d;
}

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
    // D2f: the six team camo tiles took the six spare tiles, so the manifest is 16 names.
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
    EXPECT_EQ(m.tile("tank_hull_player_a"), 10);
    EXPECT_EQ(m.tile("tank_hull_player_b"), 11);
    EXPECT_EQ(m.tile("tank_hull_player_c"), 12);
    EXPECT_EQ(m.tile("tank_hull_enemy_a"), 13);
    EXPECT_EQ(m.tile("tank_hull_enemy_b"), 14);
    EXPECT_EQ(m.tile("tank_hull_enemy_c"), 15);
    for (const auto& team : protekt::game::kTeamHullNames) {
        for (const auto name : team) {
            EXPECT_NE(m.tile(name), kFallbackTile) << name << " is listed";
        }
    }
    EXPECT_EQ(m.size(), std::size_t{16});
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

TEST(AtlasPixels, IsA256SquareOpaqueImageWithAWhiteIdentityTileAndNoSpareLeft) {
    const auto px = protekt::game::atlas_pixels();
    ASSERT_EQ(px.size(), std::size_t{256} * 256 * 4);
    for (std::size_t i = 3; i < px.size(); i += 4) {
        ASSERT_EQ(px.at(i), 255) << "opaque";
    }
    // Tile 0 is solid white: it is the identity for everything that sets no UV. Since D2f the team
    // camo fills tiles 10 to 15, so no tile is left at the unassigned magenta.
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            ASSERT_EQ(texel(px, 0, x, y), (Rgb{255, 255, 255})) << "tile 0 is solid white";
        }
    }
    for (int tile = 0; tile < protekt::game::kTileCount; ++tile) {
        EXPECT_NE(texel(px, tile, 0, 0), (Rgb{255, 0, 255})) << "tile " << tile << " is assigned";
    }
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
        // Told apart by lightness, which camo reads by: dark < base < light (sand) by a margin.
        std::sort(tones.begin(), tones.end(),
                  [](const Rgb& a, const Rgb& b) { return lightness(a) < lightness(b); });
        EXPECT_GT(lightness(tones.at(1)), lightness(tones.at(0)) + 150) << "tile " << tile;
        EXPECT_GT(lightness(tones.at(2)), lightness(tones.at(1)) + 100) << "tile " << tile;
    }
}

TEST(AtlasPaint, TheThreeHullVariantsAreVisiblyDifferent) {
    const auto px = protekt::game::atlas_pixels();
    // Texel by texel, any two variants of a set disagree over a large share of the tile.
    for (const auto& set : kHullSets) {
        for (std::size_t a = 0; a < set.size(); ++a) {
            for (std::size_t b = a + 1; b < set.size(); ++b) {
                int differ = 0;
                for (int y = 0; y < 64; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        differ += texel(px, set.at(a), x, y) != texel(px, set.at(b), x, y) ? 1 : 0;
                    }
                }
                EXPECT_GT(differ, 4096 * 35 / 100) << "tiles " << set.at(a) << " and " << set.at(b);
            }
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
    for (const auto& set : kHullSets) {
        EXPECT_EQ(off_grid_share(set.at(2)), 0.0) << set.at(2) << " C: blocks on the 4-texel grid";
        EXPECT_GT(off_grid_share(set.at(0)), 0.5) << set.at(0) << " A: splodges are round";
        EXPECT_GT(off_grid_share(set.at(1)), 0.5) << set.at(1) << " B: stripes are diagonal";
        // B's edges run diagonally: a stripe edge seen along a row moves as you go down. Its tone
        // along a row and along a column change about equally often, unlike a straight band.
        const int stripes = set.at(1);
        int across = 0;
        int down = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 1; x < 64; ++x) {
                across += texel(px, stripes, x, y) != texel(px, stripes, x - 1, y) ? 1 : 0;
                down += texel(px, stripes, y, x) != texel(px, stripes, y, x - 1) ? 1 : 0;
            }
        }
        EXPECT_GT(across, 200) << stripes << " B has many stripes";
        EXPECT_LT(std::abs(across - down), (across + down) / 4)
            << stripes << " B's stripes are diagonal";
    }
}

/// THE READABILITY GATE (D2f). Camo multiplied by a saturated side colour collapsed into one hue in
/// several brightnesses, which the eye reads as shading, not camo. So each TEAM hull tile must read
/// by HUE and by LIGHTNESS: its darkest and lightest tones are distinguishable hue families, and
/// its luminance range is wide. Contrast is a requirement here (the user's eyesight needs it), so
/// the thresholds are named, to be argued with rather than nudged.
/// Hue distance, in degrees, the darkest and lightest tones must be apart: 30 is past the width of
/// one named hue family (a "blue" or a "red" spans about 30 degrees), so they are two families.
constexpr double kMinHueSeparationDegrees = 30.0;
/// A tone's HSV saturation must reach this for its hue to count: below it a colour is grey and its
/// hue is noise.
constexpr double kMinHueSaturation = 0.12;
/// Luma range, lightest tone minus darkest, the tile must EXCEED.
constexpr double kMinLuminanceRange = 0.4;
/// The tone targets of the brief: dark near 0.30-0.35 luma, light near 0.85.
constexpr double kMaxDarkLuma = 0.38;
constexpr double kMinLightLuma = 0.80;

/// The darkest and lightest texels of `tile`, by luma.
std::array<Rgb, 2> tone_extremes(const std::vector<std::uint8_t>& px, int tile) {
    Rgb darkest{255, 255, 255};
    Rgb lightest{0, 0, 0};
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const Rgb rgb = texel(px, tile, x, y);
            darkest = luma(rgb) < luma(darkest) ? rgb : darkest;
            lightest = luma(rgb) > luma(lightest) ? rgb : lightest;
        }
    }
    return {darkest, lightest};
}

/// `rgb` times a vertex colour, as the shader multiplies them.
Rgb tinted(const Rgb& rgb, const std::array<float, 3>& tint) {
    Rgb out{};
    for (std::size_t c = 0; c < 3; ++c) {
        out.at(c)
            = static_cast<std::uint8_t>(std::lround(static_cast<float>(rgb.at(c)) * tint.at(c)));
    }
    return out;
}

/// The side colours (the scene's), and the hull's pale tint of one.
constexpr std::array<std::array<float, 3>, 2> kSideColours{
    {{0.15F, 0.35F, 0.85F}, {0.85F, 0.15F, 0.15F}}};
std::array<float, 3> hull_tint(const std::array<float, 3>& side) {
    std::array<float, 3> out = side;
    for (float& c : out) {
        c += (1.0F - c) * protekt::game::kHullPaleMix;
    }
    return out;
}

/// Whether a dark and a light tone pass the gate: two hue families, a wide luma range.
bool reads_as_camo(const Rgb& dark, const Rgb& light) {
    return luma(light) - luma(dark) > kMinLuminanceRange && saturation(dark) >= kMinHueSaturation
           && saturation(light) >= kMinHueSaturation
           && hue_distance(hue(dark), hue(light)) >= kMinHueSeparationDegrees;
}

TEST(AtlasPaint, EachTeamHullTileReadsByHueAndByLightness) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : kTeamHullTiles) {
        const auto [darkest, lightest] = tone_extremes(px, tile);
        EXPECT_GT(luma(lightest) - luma(darkest), kMinLuminanceRange) << "tile " << tile;
        EXPECT_LE(luma(darkest), kMaxDarkLuma) << "tile " << tile;
        EXPECT_GE(luma(lightest), kMinLightLuma) << "tile " << tile;
        EXPECT_GE(saturation(darkest), kMinHueSaturation) << "tile " << tile << ": dark has a hue";
        EXPECT_GE(saturation(lightest), kMinHueSaturation)
            << "tile " << tile << ": light has a hue";
        EXPECT_GE(hue_distance(hue(darkest), hue(lightest)), kMinHueSeparationDegrees)
            << "tile " << tile << ": dark hue " << hue(darkest) << ", light hue " << hue(lightest);
        // And AS DRAWN: times the hull's pale tint of its own side colour, it still reads.
        const auto tint = hull_tint(kSideColours.at(tile < 13 ? 0 : 1));
        EXPECT_TRUE(reads_as_camo(tinted(darkest, tint), tinted(lightest, tint)))
            << "tile " << tile << " under its hull tint";
    }
    // The failure this gate exists for, pinned: the neutral camo under the FULL saturated blue
    // collapses into one hue and must not pass.
    const auto [dark, light] = tone_extremes(px, 7);
    EXPECT_FALSE(
        reads_as_camo(tinted(dark, kSideColours.at(0)), tinted(light, kSideColours.at(0))));
}

TEST(AtlasPaint, EachTeamsCamoIsInItsOwnHueFamily) {
    const auto px = protekt::game::atlas_pixels();
    // The darkest tone carries the team: blue (around 225) for the player, red (around 5) for the
    // enemy, so a hull is the team's even before the side tint multiplies it.
    for (const int tile : kTeamHullTiles) {
        Rgb darkest{255, 255, 255};
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const Rgb rgb = texel(px, tile, x, y);
                darkest = luma(rgb) < luma(darkest) ? rgb : darkest;
            }
        }
        const double want = tile < 13 ? 225.0 : 5.0;
        EXPECT_LT(hue_distance(hue(darkest), want), 20.0) << "tile " << tile;
    }
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
