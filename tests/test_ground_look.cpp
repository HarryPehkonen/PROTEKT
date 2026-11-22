// The ground as material (slice A1): the large-scale tint that breaks the tile repeat, the
// weathered variants, and the site tiles. A LOOK only: nothing here may reach the sim.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "protekt/game/atlas.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace {

using Rgb = std::array<std::uint8_t, 3>;
using protekt::game::ground_tint;

Rgb texel(const std::vector<std::uint8_t>& px, int tile, int x, int y) {
    const auto cell = protekt::game::tile_cell(tile);
    const int ax = (static_cast<int>(cell.col) * 64) + x;
    const int ay = (static_cast<int>(cell.row) * 64) + y;
    const std::size_t at
        = ((static_cast<std::size_t>(ay) * 512U) + static_cast<std::size_t>(ax)) * 4U;
    return {px.at(at), px.at(at + 1), px.at(at + 2)};
}

std::set<Rgb> colours(const std::vector<std::uint8_t>& px, int tile) {
    std::set<Rgb> seen;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            seen.insert(texel(px, tile, x, y));
        }
    }
    return seen;
}

double mean_sum(const std::vector<std::uint8_t>& px, int tile) {
    double sum = 0.0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const Rgb c = texel(px, tile, x, y);
            sum += (c.at(0) + c.at(1) + c.at(2)) / 4096.0;
        }
    }
    return sum;
}

TEST(GroundTint, IsAFunctionOfSeedAndPositionAlone_TheNumberTwice) {
    // The same corner, the same seed, two separate evaluations: the same number, and it is THIS
    // one.
    const auto first = ground_tint(1, 10, -7);
    const auto second = ground_tint(1, 10, -7);
    EXPECT_EQ(first, second);
    EXPECT_NEAR(first[0], 0.9458F, 1e-3F);
    EXPECT_NEAR(first[1], 0.9413F, 1e-3F);
    EXPECT_NEAR(first[2], 0.9368F, 1e-3F);
    EXPECT_NE(ground_tint(2, 10, -7), first) << "another world, another weather";
}

TEST(GroundTint, VariesOverTensOfTilesAndNeverStepsBetweenNeighbours) {
    float lo = 10.0F;
    float hi = 0.0F;
    double near_step = 0.0;
    double far_step = 0.0;
    int n = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const float here = ground_tint(1, x, y)[1];
            lo = std::min(lo, here);
            hi = std::max(hi, here);
            EXPECT_LE(std::fabs(here - ground_tint(1, x + 1, y)[1]), 0.05F) << x << "," << y;
            near_step += std::fabs(here - ground_tint(1, x + 1, y)[1]);
            far_step += std::fabs(here - ground_tint(1, x + 24, y)[1]);
            ++n;
        }
    }
    EXPECT_GE(hi - lo, 0.15F) << "a visible swing across one map";
    EXPECT_LE(hi, 1.25F);
    EXPECT_GE(lo, 0.75F);
    EXPECT_LT(near_step * 4.0, far_step) << "slow: tiles 24 apart differ far more than neighbours";
    EXPECT_GT(n, 0);
}

TEST(GroundTint, TheSceneCarriesItOnEveryTerrainVertex) {
    constexpr std::uint64_t kSeed = 7;
    protekt::game::SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 12;
    o.camera.x = 40.0F;
    o.camera.z = 40.0F;
    o.camera.y = 30.0F;
    static const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const auto scene = protekt::game::build_scene(sim, o);
    const auto base = protekt::game::terrain_base_colour();
    // The mesh opens with the terrain: (2r+2)^2 tiles of six vertices.
    const std::size_t terrain = std::size_t{26} * 26U * 6U;
    ASSERT_GT(scene.vertices.size(), terrain);
    std::size_t checked = 0;
    float lo = 10.0F;
    float hi = 0.0F;
    for (std::size_t i = 0; i < terrain; ++i) {
        const auto& v = scene.vertices.at(i);
        const int cx = static_cast<int>(std::lround(v.position[0] / protekt::game::kMetresPerTile));
        const int cz = static_cast<int>(std::lround(v.position[2] / protekt::game::kMetresPerTile));
        const auto tint = ground_tint(kSeed, cx, cz);
        for (std::size_t c = 0; c < 3; ++c) {
            ASSERT_NEAR(v.colour.at(c), base.at(c) * tint.at(c), 1e-4F) << "vertex " << i;
        }
        lo = std::min(lo, tint[1]);
        hi = std::max(hi, tint[1]);
        ++checked;
    }
    EXPECT_EQ(checked, terrain) << "the terrain's vertices are base x tint of their own corner";
    EXPECT_GT(hi - lo, 0.03F);
}

TEST(GroundLook, EveryDrawnVariantIsListedWeatheredAndNeitherWhiteNorMagenta) {
    const auto px = protekt::game::atlas_pixels();
    const auto& manifest = protekt::game::atlas_manifest();
    for (int t = 0; t < protekt::world::kTerrainCount; ++t) {
        std::set<int> tiles;
        std::vector<double> means;
        for (int v = 0; v < protekt::game::kGroundVariantCount; ++v) {
            const auto name
                = protekt::game::terrain_variant_name(static_cast<protekt::world::Terrain>(t), v);
            const int tile = manifest.tile(name);
            ASSERT_NE(tile, protekt::game::kFallbackTile)
                << name << " must list, not fall to white";
            tiles.insert(tile);
            const auto seen = colours(px, tile);
            EXPECT_GT(seen.size(), std::size_t{20}) << name << ": a material, not three flat tones";
            EXPECT_EQ(seen.count(Rgb{255, 255, 255}), 0U) << name;
            EXPECT_EQ(seen.count(Rgb{255, 0, 255}), 0U) << name;
            means.push_back(mean_sum(px, tile));
        }
        EXPECT_EQ(tiles.size(), std::size_t{protekt::game::kGroundVariantCount});
        const auto [lo, hi] = std::minmax_element(means.begin(), means.end());
        EXPECT_GE(*hi - *lo, 6.0) << "type " << t << ": the six variants span a tonal range";
    }
}

TEST(GroundLook, TheThreeSiteTilesAreNotFlatAndDifferFromEachOther) {
    const auto px = protekt::game::atlas_pixels();
    for (const int tile : {45, 46, 47}) {
        EXPECT_GT(colours(px, tile).size(), std::size_t{8}) << "site tile " << tile << " is flat";
    }
    EXPECT_NE(mean_sum(px, 45), mean_sum(px, 46));
    EXPECT_NE(mean_sum(px, 46), mean_sum(px, 47));
    EXPECT_NE(mean_sum(px, 45), mean_sum(px, 47));
    for (int y = 0; y < 64; ++y) {
        EXPECT_NE(texel(px, 45, 20, y), texel(px, 47, 20, y));
    }
}

TEST(GroundLook, TheSameSeedPaintsByteIdenticalPixelsAndAnotherSeedDoesNot) {
    const auto a = protekt::game::atlas_pixels(1);
    const auto b = protekt::game::atlas_pixels(1);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, protekt::game::atlas_pixels(2));
    EXPECT_EQ(a.size(), std::size_t{512} * 512U * 4U);
}

} // namespace
