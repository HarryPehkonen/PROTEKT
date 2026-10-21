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

/// The tiles painted so far: the first quadrant, the hover skirt and the four ground tiles.
constexpr int kAssignedTiles = 21;

/// Texel (x, y) of `tile` in a whole-atlas RGBA buffer.
Rgb texel(const std::vector<std::uint8_t>& px, int tile, int x, int y) {
    const auto cell = protekt::game::tile_cell(tile);
    const int ax = (static_cast<int>(cell.col) * 64) + x;
    const int ay = (static_cast<int>(cell.row) * 64) + y;
    const std::size_t at
        = ((static_cast<std::size_t>(ay) * 512U) + static_cast<std::size_t>(ax)) * 4U;
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
                             "too_big 64\n"
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
    // D2f: the six team camo tiles took the six spare tiles, so the manifest is 16 names; E9b's
    // hover skirt opened the second quadrant, so it is 17; D3's four ground tiles make it 21.
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
    EXPECT_EQ(m.tile("hover_skirt"), 16);
    EXPECT_EQ(m.tile("terrain_grass"), 17);
    EXPECT_EQ(m.tile("terrain_dirt"), 18);
    EXPECT_EQ(m.tile("terrain_rock"), 19);
    EXPECT_EQ(m.tile("terrain_crater"), 20);
    for (const auto& team : protekt::game::kTeamHullNames) {
        for (const auto name : team) {
            EXPECT_NE(m.tile(name), kFallbackTile) << name << " is listed";
        }
    }
    EXPECT_EQ(m.size(), std::size_t{21});
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
    EXPECT_EQ(first.u1, 0.125F - kHalfTexel);
    EXPECT_EQ(first.v1, 0.125F - kHalfTexel);
    const UvRect one = tile_rect(1);
    EXPECT_EQ(one.u0, 0.125F + kHalfTexel);
    EXPECT_EQ(one.v0, 0.0F + kHalfTexel);
    EXPECT_EQ(one.u1, 0.25F - kHalfTexel);
    EXPECT_EQ(one.v1, 0.125F - kHalfTexel);
    const UvRect six = tile_rect(6);
    EXPECT_EQ(six.u0, 0.25F + kHalfTexel);
    EXPECT_EQ(six.v0, 0.125F + kHalfTexel);
    EXPECT_EQ(six.u1, 0.375F - kHalfTexel);
    EXPECT_EQ(six.v1, 0.25F - kHalfTexel);
    const UvRect last = tile_rect(15);
    EXPECT_EQ(last.u0, 0.375F + kHalfTexel);
    EXPECT_EQ(last.v0, 0.375F + kHalfTexel);
    EXPECT_EQ(last.u1, 0.5F - kHalfTexel);
    EXPECT_EQ(last.v1, 0.5F - kHalfTexel);
}

TEST(AtlasLayout, AnOutOfRangeIndexGetsTheFallbackRect) {
    for (const int bad : {-1, 64, 1000}) {
        const UvRect r = tile_rect(bad);
        EXPECT_EQ(r.u1, 0.125F - kHalfTexel);
        EXPECT_EQ(r.v1, 0.125F - kHalfTexel);
    }
}

TEST(AtlasLayout, TheEightByEightIsFourQuadrantsAndTheFirstIsTheOldFourByFour) {
    using protekt::game::tile_cell;
    EXPECT_EQ(protekt::game::kTilesPerRow, 8U);
    EXPECT_EQ(protekt::game::kAtlasSide, 512U);
    EXPECT_EQ(protekt::game::kTileCount, 64);
    // Tiles 0-15 are where the 4x4 atlas had them: row-major in the top-left quadrant.
    for (int index = 0; index < 16; ++index) {
        EXPECT_EQ(tile_cell(index).col, static_cast<std::uint32_t>(index % 4)) << index;
        EXPECT_EQ(tile_cell(index).row, static_cast<std::uint32_t>(index / 4)) << index;
    }
    // Then the top-right, bottom-left and bottom-right quadrants, row-major inside each.
    EXPECT_EQ(tile_cell(16).col, 4U);
    EXPECT_EQ(tile_cell(16).row, 0U);
    EXPECT_EQ(tile_cell(21).col, 5U);
    EXPECT_EQ(tile_cell(21).row, 1U);
    EXPECT_EQ(tile_cell(32).col, 0U);
    EXPECT_EQ(tile_cell(32).row, 4U);
    EXPECT_EQ(tile_cell(48).col, 4U);
    EXPECT_EQ(tile_cell(48).row, 4U);
    EXPECT_EQ(tile_cell(63).col, 7U);
    EXPECT_EQ(tile_cell(63).row, 7U);
    const UvRect skirt = tile_rect(16);
    EXPECT_EQ(skirt.u0, 0.5F + kHalfTexel);
    EXPECT_EQ(skirt.v0, 0.0F + kHalfTexel);
    EXPECT_EQ(skirt.u1, 0.625F - kHalfTexel);
    EXPECT_EQ(skirt.v1, 0.125F - kHalfTexel);
}

TEST(AtlasPaint, TheHoverSkirtIsABrightBandUnderADarkLip) {
    const auto px = protekt::game::atlas_pixels();
    constexpr int kSkirt = 16;
    for (int x = 0; x < 64; ++x) {
        EXPECT_LT(lightness(texel(px, kSkirt, x, 0)), 3 * 128)
            << "the lip, where it meets the hull";
    }
    int bright = 0;
    for (int y = 4; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const Rgb rgb = texel(px, kSkirt, x, y);
            EXPECT_EQ(rgb.at(0), rgb.at(1)) << "grey: the raw skirt colour is the hue";
            EXPECT_EQ(rgb.at(1), rgb.at(2));
            bright += luma(rgb) >= 0.6 ? 1 : 0;
        }
    }
    EXPECT_EQ(bright, 60 * 64) << "every texel under the lip is bright";
    // It brightens toward the ground, and paints the same with any seed: it has no pattern.
    EXPECT_LT(lightness(texel(px, kSkirt, 3, 4)), lightness(texel(px, kSkirt, 3, 63)));
    EXPECT_EQ(texel(px, kSkirt, 9, 40), texel(protekt::game::atlas_pixels(7), kSkirt, 9, 40));
}

TEST(AtlasPixels, IsA512SquareOpaqueImageWithAWhiteIdentityTileAndNoAssignedTileLeftMagenta) {
    const auto px = protekt::game::atlas_pixels();
    ASSERT_EQ(px.size(), std::size_t{512} * 512 * 4);
    for (std::size_t i = 3; i < px.size(); i += 4) {
        ASSERT_EQ(px.at(i), 255) << "opaque";
    }
    // Tile 0 is solid white: it is the identity for everything that sets no UV. Since D2f the team
    // camo fills tiles 10 to 15, so no tile of the first quadrant is left at the unassigned
    // magenta; E9b's 8x8 growth assigns the hover skirt (16), D3 the ground (17 to 20), and the
    // rest stay magenta, the loud budget.
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            ASSERT_EQ(texel(px, 0, x, y), (Rgb{255, 255, 255})) << "tile 0 is solid white";
        }
    }
    for (int tile = 0; tile < kAssignedTiles; ++tile) {
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

/// D2g: the camo must read ON THE RENDERED TANK, where a hull face shows the whole tile in about 40
/// screen pixels. At D2f the tile passed its gate but the seed-7 tank box was 3261 of 3804 pixels
/// blue-family: sand covered ~10-15% of the tile in a dozen small specks. So the sand -- the tone a
/// hue apart from the team's dark -- carries the pattern, in a few big patches.
/// The share of the tile the sand (lightest) tone must cover, at least and at most.
constexpr double kMinSandCoverage = 0.30;
constexpr double kMaxSandCoverage = 0.45;
/// A sand patch counts as a PATCH, not a speck, if it spans this many texels across or down...
constexpr int kMinPatchSpan = 16;
/// ...and nearly all the sand must lie in such patches (a sliver clipped by the tile's corner may
/// not), and there are only a few of them: bold, not speckle.
constexpr double kMinSandInPatches = 0.90;
constexpr int kMinSandPatches = 2;
constexpr int kMaxSandPatches = 4;

TEST(AtlasPaint, EachHullTilesSandIsAFewBoldPatchesCoveringAThirdOfIt) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : kHullTiles) {
        const auto [darkest, sand] = tone_extremes(px, tile);
        std::array<bool, 4096> seen{};
        int sand_total = 0;
        int sand_in_patches = 0;
        int patches = 0;
        for (int start = 0; start < 4096; ++start) {
            if (seen.at(static_cast<std::size_t>(start))
                || texel(px, tile, start % 64, start / 64) != sand) {
                continue;
            }
            // Flood the 4-connected sand patch from `start`, keeping its size and bounding box.
            std::vector<int> stack{start};
            seen.at(static_cast<std::size_t>(start)) = true;
            int size = 0;
            int x0 = 64;
            int x1 = -1;
            int y0 = 64;
            int y1 = -1;
            while (!stack.empty()) {
                const int at = stack.back();
                stack.pop_back();
                const int x = at % 64;
                const int y = at / 64;
                ++size;
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
                for (const auto& [nx, ny] : std::array<std::array<int, 2>, 4>{
                         {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}}) {
                    const int next = (ny * 64) + nx;
                    if (nx >= 0 && ny >= 0 && nx < 64 && ny < 64
                        && !seen.at(static_cast<std::size_t>(next))
                        && texel(px, tile, nx, ny) == sand) {
                        seen.at(static_cast<std::size_t>(next)) = true;
                        stack.push_back(next);
                    }
                }
            }
            sand_total += size;
            if (std::max(x1 - x0, y1 - y0) + 1 >= kMinPatchSpan) {
                sand_in_patches += size;
                ++patches;
            }
        }
        const double coverage = sand_total / 4096.0;
        EXPECT_GE(coverage, kMinSandCoverage) << "tile " << tile;
        EXPECT_LE(coverage, kMaxSandCoverage) << "tile " << tile;
        EXPECT_GE(sand_in_patches, kMinSandInPatches * sand_total)
            << "tile " << tile << ": the sand is patches, not specks";
        EXPECT_GE(patches, kMinSandPatches) << "tile " << tile;
        EXPECT_LE(patches, kMaxSandPatches) << "tile " << tile;
        // The pattern is two-tone dark-and-sand: the mid tone is only an accent.
        const auto shares = tone_shares(px, tile);
        EXPECT_GT(shares.at(darkest), shares.at(sand) / 2) << "tile " << tile;
        for (const auto& [rgb, share] : shares) {
            if (rgb != darkest && rgb != sand) {
                EXPECT_LT(share, shares.at(sand)) << "tile " << tile << ": the mid is an accent";
                EXPECT_LT(share, shares.at(darkest)) << "tile " << tile << ": the mid is an accent";
            }
        }
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

TEST(AtlasPaint, TankMetalTilesAreTheirBaseToneWithOnlyAFewPercentOfVariation) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : {2, 3}) {
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

/// The structure tiles -- factory walls (4), factory roof (5), turret body (6) -- carry PAINTED
/// detail, read by luminance steps so it survives the side-colour multiply. The gate's thresholds:
/// a luma range of at least kMinStructureRange, at least kMinSeamTexels seam texels (luma under
/// kSeamLuma), at least kMinPlateTexels light-plate texels (luma over kPlateLuma), and, after a
/// saturated side tint multiplies them, a luma range of at least kMinTintedRange. Measured before
/// the paint: range 0.07 at most, no seams, which is what these fail on.
constexpr double kMinStructureRange = 0.55;
constexpr int kMinSeamTexels = 150;
constexpr double kSeamLuma = 0.45;
constexpr int kMinPlateTexels = 600;
constexpr double kPlateLuma = 0.80;
constexpr double kMinTintedRange = 0.25; ///< E11b: was 0.10, which a flat deep tint just met
constexpr double kNearBlackLuma = 0.15;  ///< the door frame and the chevrons

TEST(AtlasPaint, TheTurretAndFactoryTilesCarryVisibleLuminanceDetail) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : {4, 5, 6}) {
        double lo = 1.0;
        double hi = 0.0;
        int seams = 0;
        int plates = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const Rgb rgb = texel(px, tile, x, y);
                EXPECT_EQ(rgb.at(0), rgb.at(1)) << "near-neutral, so the side colour shows";
                EXPECT_EQ(rgb.at(1), rgb.at(2)) << "luminance steps, not hue tricks";
                const double l = luma(rgb);
                lo = std::min(lo, l);
                hi = std::max(hi, l);
                seams += l < kSeamLuma ? 1 : 0;
                plates += l > kPlateLuma ? 1 : 0;
            }
        }
        EXPECT_GE(hi - lo, kMinStructureRange) << "tile " << tile;
        EXPECT_GE(seams, kMinSeamTexels) << "tile " << tile;
        EXPECT_GE(plates, kMinPlateTexels) << "tile " << tile;
        // AS DRAWN: the structures take the hull's pale tint of their side colour.
        const auto tinted_range = [&](const std::array<float, 3>& tint) {
            double tlo = 1.0;
            double thi = 0.0;
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const double l = luma(tinted(texel(px, tile, x, y), tint));
                    tlo = std::min(tlo, l);
                    thi = std::max(thi, l);
                }
            }
            return thi - tlo;
        };
        for (const auto& side : kSideColours) {
            EXPECT_GE(tinted_range(hull_tint(side)), kMinTintedRange)
                << "tile " << tile << " under the pale tint";
        }
    }
}

TEST(AtlasPaint, TheFactoryWallsDoorFrameAndChevronsAreNearBlack) {
    const auto px = protekt::game::atlas_pixels();
    int dark = 0;
    for (int y = 13; y < 64; ++y) {
        for (int x = 4; x < 52; ++x) {
            dark += luma(texel(px, 4, x, y)) <= kNearBlackLuma ? 1 : 0;
        }
    }
    EXPECT_GE(dark, 300);
    for (int x = 4; x <= 9; ++x) { // the frame's left side is six texels of near-black
        EXPECT_LE(luma(texel(px, 4, x, 40)), kNearBlackLuma) << "x " << x;
    }
}

// ---------------------------------------------------------------- the ground (D3)

/// The ground tiles, in world::Terrain order (Flat, Rough, Ridge, Crater), and the flat colour each
/// terrain type was drawn in before it had a tile, in 0-255.
constexpr std::array<int, 4> kGroundTiles{17, 18, 19, 20};
constexpr std::array<std::array<double, 3>, 4> kOldFlatColours{
    {{0.25 * 255, 0.55 * 255, 0.20 * 255},
     {0.45 * 255, 0.42 * 255, 0.18 * 255},
     {0.55 * 255, 0.50 * 255, 0.48 * 255},
     {0.15 * 255, 0.12 * 255, 0.10 * 255}}};

/// THE VARIANCE AND READABILITY GATE, in named thresholds.
/// A ground tile's luma spans at least this, texel to texel: real detail, not a flat colour.
constexpr double kMinGroundLumaRange = 0.20;
/// Its patches and bands average at least this many texels along a row and down a column, so the
/// detail survives gameplay distance instead of collapsing into sub-pixel speckle.
constexpr double kMinGroundPatchTexels = 8.0;
/// Its mean colour stays within this of the flat colour the type used to be (per channel, 0-255),
/// so the ground still reads as the terrain it was.
constexpr double kMaxGroundMeanDrift = 0.08 * 255;
/// Any two types' mean colours are at least this far apart (RGB distance, 0-255): grass, dirt, rock
/// and crater stay distinguishable at a glance.
constexpr double kMinGroundTypeSeparation = 0.12 * 255;
/// A tile wraps: at least this share of its edge texels equal the texel across the seam.
constexpr double kMinGroundSeamMatch = 0.80;

std::array<double, 3> mean_colour(const std::vector<std::uint8_t>& px, int tile) {
    std::array<double, 3> sum{};
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const Rgb rgb = texel(px, tile, x, y);
            for (std::size_t c = 0; c < 3; ++c) {
                sum.at(c) += rgb.at(c) / 4096.0;
            }
        }
    }
    return sum;
}

/// The mean length of a run of one colour along the rows (`down` false) or the columns.
double mean_run(const std::vector<std::uint8_t>& px, int tile, bool down) {
    int runs = 0;
    for (int a = 0; a < 64; ++a) {
        ++runs;
        for (int b = 1; b < 64; ++b) {
            const Rgb here = down ? texel(px, tile, a, b) : texel(px, tile, b, a);
            const Rgb before = down ? texel(px, tile, a, b - 1) : texel(px, tile, b - 1, a);
            runs += here != before ? 1 : 0;
        }
    }
    return 4096.0 / runs;
}

TEST(AtlasGround, EachGroundTileHasALumaRangeOfAtLeastKMinGroundLumaRange) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : kGroundTiles) {
        double lo = 1.0;
        double hi = 0.0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                lo = std::min(lo, luma(texel(px, tile, x, y)));
                hi = std::max(hi, luma(texel(px, tile, x, y)));
            }
        }
        EXPECT_GE(hi - lo, kMinGroundLumaRange) << "tile " << tile;
    }
}

TEST(AtlasGround, EachGroundTilesDetailIsAtLeastKMinGroundPatchTexelsAcross) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : kGroundTiles) {
        EXPECT_GE(mean_run(px, tile, false), kMinGroundPatchTexels) << "tile " << tile << " rows";
        EXPECT_GE(mean_run(px, tile, true), kMinGroundPatchTexels) << "tile " << tile << " columns";
        EXPECT_LE(tone_shares(px, tile).size(), std::size_t{3})
            << "tile " << tile << ": three tones";
    }
}

TEST(AtlasGround, EachGroundTileMeansTheFlatColourItsTypeUsedToBe) {
    const auto px = protekt::game::atlas_pixels();
    for (std::size_t t = 0; t < kGroundTiles.size(); ++t) {
        const auto mean = mean_colour(px, kGroundTiles.at(t));
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(mean.at(c), kOldFlatColours.at(t).at(c), kMaxGroundMeanDrift)
                << "tile " << kGroundTiles.at(t) << " channel " << c;
        }
    }
}

TEST(AtlasGround, TheFourGroundTypesStayKMinGroundTypeSeparationApart) {
    const auto px = protekt::game::atlas_pixels();
    for (std::size_t a = 0; a < kGroundTiles.size(); ++a) {
        for (std::size_t b = a + 1; b < kGroundTiles.size(); ++b) {
            const auto ma = mean_colour(px, kGroundTiles.at(a));
            const auto mb = mean_colour(px, kGroundTiles.at(b));
            double d2 = 0.0;
            for (std::size_t c = 0; c < 3; ++c) {
                d2 += (ma.at(c) - mb.at(c)) * (ma.at(c) - mb.at(c));
            }
            EXPECT_GE(std::sqrt(d2), kMinGroundTypeSeparation)
                << "tiles " << kGroundTiles.at(a) << " and " << kGroundTiles.at(b);
        }
    }
    // The crater is the darkest ground by a wide margin: a hole, not another field.
    const auto mean_luma = [&](int tile) {
        const auto m = mean_colour(px, tile);
        return ((0.2126 * m.at(0)) + (0.7152 * m.at(1)) + (0.0722 * m.at(2))) / 255.0;
    };
    for (const int tile : {17, 18, 19}) {
        EXPECT_LT(mean_luma(20) + 0.15, mean_luma(tile)) << "tile " << tile;
    }
}

TEST(AtlasGround, EachGroundTileWrapsSoNeighbouringQuadsShowNoSeam) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : kGroundTiles) {
        int match = 0;
        for (int i = 0; i < 64; ++i) {
            match += texel(px, tile, 0, i) == texel(px, tile, 63, i) ? 1 : 0;
            match += texel(px, tile, i, 0) == texel(px, tile, i, 63) ? 1 : 0;
        }
        EXPECT_GE(match / 128.0, kMinGroundSeamMatch) << "tile " << tile;
    }
}

TEST(AtlasGround, TheGroundIsPaintedDeterministicallyFromTheSeed) {
    const auto a = protekt::game::atlas_pixels(7);
    const auto b = protekt::game::atlas_pixels(7);
    const auto c = protekt::game::atlas_pixels();
    for (const int tile : kGroundTiles) {
        bool differs = false;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                ASSERT_EQ(texel(a, tile, x, y), texel(b, tile, x, y)) << "tile " << tile;
                differs = differs || texel(a, tile, x, y) != texel(c, tile, x, y);
            }
        }
        EXPECT_TRUE(differs) << "tile " << tile << ": another seed paints another pattern";
    }
}

} // namespace
