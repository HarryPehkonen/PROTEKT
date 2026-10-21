// Ground variants (D6): each terrain type paints from three variant tiles of its own family,
// chosen per world tile by a pure hash of (type, x, y), so a field of one type is not a grid of
// identical stamps. Pure data and the scene mesh, no GPU.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "protekt/game/atlas.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace {

using protekt::game::kGroundVariantCount;
using protekt::game::terrain_variant_name;
using protekt::game::variant_index;
using protekt::world::Terrain;

constexpr std::array<Terrain, 4> kTypes{Terrain::Flat, Terrain::Rough, Terrain::Ridge,
                                        Terrain::Crater};

/// THE SAMPLE: a 64x64 block of tiles either side of the origin, so negative coordinates count.
constexpr int kLo = -32;
constexpr int kHi = 31;

/// Of the adjacent pairs in the sample, at least this share pick different variants (a fair hash
/// gives 2/3) ...
constexpr double kMinAdjacentDiffer = 0.60;
/// ... and no row or column of the sample holds a run of one variant longer than this. A fair
/// three-way hash over the sample's ~8000 runs makes one of about 9 by chance; 10 is the cap.
constexpr int kMaxRun = 10;

TEST(GroundVariant, TheSameTileGetsTheSameVariantEveryTime) {
    for (const Terrain type : kTypes) {
        for (int y = kLo; y <= kHi; y += 7) {
            for (int x = kLo; x <= kHi; x += 5) {
                const int v = variant_index(type, x, y);
                EXPECT_GE(v, 0);
                EXPECT_LT(v, kGroundVariantCount);
                EXPECT_EQ(v, variant_index(type, x, y)) << x << "," << y;
            }
        }
    }
    static_assert(variant_index(Terrain::Flat, 3, -4) == variant_index(Terrain::Flat, 3, -4));
}

TEST(GroundVariant, EveryVariantOccursForEveryType) {
    for (const Terrain type : kTypes) {
        std::array<int, 3> count{};
        for (int y = kLo; y <= kHi; ++y) {
            for (int x = kLo; x <= kHi; ++x) {
                ++count.at(static_cast<std::size_t>(variant_index(type, x, y)));
            }
        }
        for (std::size_t v = 0; v < count.size(); ++v) {
            // 4096 tiles: each variant near a third of them, none starved.
            EXPECT_GT(count.at(v), 4096 / 4) << "type " << static_cast<int>(type) << " v " << v;
        }
    }
}

TEST(GroundVariant, AdjacentTilesMostlyDifferAndNoLongRunOfOneVariant) {
    for (const Terrain type : kTypes) {
        int pairs = 0;
        int differ = 0;
        int longest = 0;
        for (int a = kLo; a <= kHi; ++a) {
            int row_run = 1;
            int column_run = 1;
            for (int b = kLo + 1; b <= kHi; ++b) {
                const bool row_same = variant_index(type, b, a) == variant_index(type, b - 1, a);
                const bool column_same = variant_index(type, a, b) == variant_index(type, a, b - 1);
                pairs += 2;
                differ += (row_same ? 0 : 1) + (column_same ? 0 : 1);
                row_run = row_same ? row_run + 1 : 1;
                column_run = column_same ? column_run + 1 : 1;
                longest = std::max({longest, row_run, column_run});
            }
        }
        EXPECT_GE(static_cast<double>(differ) / pairs, kMinAdjacentDiffer)
            << "type " << static_cast<int>(type);
        EXPECT_LE(longest, kMaxRun) << "type " << static_cast<int>(type);
    }
}

TEST(GroundVariant, TheManifestListsTwelveVariantSlotsAfterTheGroundTiles) {
    const auto& atlas = protekt::game::atlas_manifest();
    std::set<int> slots;
    for (const Terrain type : kTypes) {
        for (int v = 0; v < kGroundVariantCount; ++v) {
            const std::string name{terrain_variant_name(type, v)};
            EXPECT_EQ(name.rfind(std::string{protekt::game::terrain_tile_name(type)} + "_", 0), 0U)
                << name << " is named after its family";
            slots.insert(atlas.tile(name));
        }
    }
    EXPECT_EQ(slots, (std::set<int>{21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32}));
}

/// The atlas texel of `tile` at (x, y).
std::array<std::uint8_t, 3> texel(const std::vector<std::uint8_t>& px, int tile, int x, int y) {
    const auto cell = protekt::game::tile_cell(tile);
    const std::size_t row = std::size_t{cell.row} * protekt::game::kTileSide;
    const std::size_t col = std::size_t{cell.col} * protekt::game::kTileSide;
    const std::size_t at = (((row + static_cast<std::size_t>(y)) * protekt::game::kAtlasSide) + col
                            + static_cast<std::size_t>(x))
                           * 4U;
    return {px.at(at), px.at(at + 1), px.at(at + 2)};
}

TEST(GroundVariant, EachVariantIsPaintedInItsFamilysTonesButIsItsOwnPattern) {
    // SUBTLE: a variant uses exactly its family tile's tones in near the same shares, so the
    // ground's colour does not shift between variants; only where the patches lie does.
    const auto px = protekt::game::atlas_pixels();
    const auto& atlas = protekt::game::atlas_manifest();
    for (const Terrain type : kTypes) {
        const int family = atlas.tile(protekt::game::terrain_tile_name(type));
        std::map<std::array<std::uint8_t, 3>, int> family_tones;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                ++family_tones[texel(px, family, x, y)];
            }
        }
        std::vector<int> tiles{family};
        for (int v = 0; v < kGroundVariantCount; ++v) {
            const int tile = atlas.tile(terrain_variant_name(type, v));
            std::map<std::array<std::uint8_t, 3>, int> tones;
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    ++tones[texel(px, tile, x, y)];
                }
            }
            ASSERT_EQ(tones.size(), family_tones.size()) << "tile " << tile;
            for (const auto& [rgb, n] : tones) {
                ASSERT_TRUE(family_tones.contains(rgb)) << "tile " << tile << " has a new tone";
                EXPECT_NEAR(n, family_tones.at(rgb), 4096 * 0.08) << "tile " << tile;
            }
            for (const int other : tiles) {
                int same = 0;
                for (int y = 0; y < 64; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        same += texel(px, tile, x, y) == texel(px, other, x, y) ? 1 : 0;
                    }
                }
                EXPECT_LT(same, 4096 * 9 / 10) << "tiles " << tile << " and " << other;
            }
            tiles.push_back(tile);
        }
    }
}

TEST(GroundVariant, EveryTerrainQuadSamplesTheVariantItsTileHashesTo) {
    // The scene mapping: the TYPE still decides the family, the hash the variant inside it.
    constexpr std::uint64_t kSeed = 42;
    constexpr int kRadius = 12;
    protekt::game::SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = kRadius;
    options.camera.x = 60.0F;
    options.camera.z = 60.0F;
    options.camera.y = 8.0F;
    const auto scene = protekt::game::build_scene(
        protekt::sim::Sim::create(protekt::sim::skirmish(kSeed)), options);
    const auto& atlas = protekt::game::atlas_manifest();
    const std::size_t side = (2U * kRadius) + 2U;
    std::map<Terrain, std::set<int>> slots_hit;
    for (std::size_t q = 0; q < side * side * 6U; q += 6) {
        const auto& p00 = scene.vertices.at(q);
        const int tx
            = static_cast<int>(std::lround(p00.position[0] / protekt::game::kMetresPerTile));
        const int ty
            = static_cast<int>(std::lround(p00.position[2] / protekt::game::kMetresPerTile));
        const Terrain type = protekt::world::sample(kSeed, tx, ty).terrain;
        const std::string_view name = terrain_variant_name(type, variant_index(type, tx, ty));
        const auto r = atlas.rect(name);
        ASSERT_EQ(p00.u, r.u0) << tx << "," << ty;
        ASSERT_EQ(p00.v, r.v0) << tx << "," << ty;
        ASSERT_EQ(scene.vertices.at(q + 2).u, r.u1) << tx << "," << ty;
        ASSERT_EQ(scene.vertices.at(q + 2).v, r.v1) << tx << "," << ty;
        slots_hit[type].insert(atlas.tile(name));
    }
    ASSERT_TRUE(slots_hit.contains(Terrain::Flat));
    EXPECT_EQ(slots_hit.at(Terrain::Flat).size(), std::size_t{3}) << "grass shows all three";
}

} // namespace
