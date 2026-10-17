// Properties, not values: M1's contract is that the world is a pure, integer, reproducible
// function of (seed, chunk). The golden hash is recorded after the first implementation and
// from then on it protects against drift.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>

#include "protekt/world/world.hpp"

namespace {

using namespace protekt::world;

// Member-wise: Tile has padding bytes, so comparing the raw representation would read
// indeterminate memory (clang-tidy caught exactly that in the first version of this helper).
bool same(const Chunk& a, const Chunk& b) {
    for (std::size_t i = 0; i < kChunkTiles; ++i) {
        if (a.tiles[i].height != b.tiles[i].height || a.tiles[i].terrain != b.tiles[i].terrain) {
            return false;
        }
    }
    return true;
}

TEST(World, SameSeedSameChunk) {
    EXPECT_TRUE(same(generate(7, 0, 0), generate(7, 0, 0)));
    EXPECT_TRUE(
        same(generate(0xFFFFFFFFFFFFFFFFULL, -3, 5), generate(0xFFFFFFFFFFFFFFFFULL, -3, 5)));
}

TEST(World, NeighbouringChunksDiffer) {
    // Catches a per-chunk seed: a field keyed on the chunk coordinate, not the world's.
    EXPECT_FALSE(same(generate(7, 0, 0), generate(7, 1, 0)));
    EXPECT_FALSE(same(generate(7, 0, 0), generate(7, 0, 1)));
}

TEST(World, GenerationIsPure) {
    const Chunk first = generate(11, 2, -1);
    (void)generate(99, -4, 4); // a different call in between must not move anything
    EXPECT_TRUE(same(first, generate(11, 2, -1)));
}

TEST(World, SeedsDiffer) { EXPECT_NE(region_hash(1, 2, 2), region_hash(2, 2, 2)); }

TEST(World, RegionHashIsStableAndDependsOnDimensions) {
    EXPECT_EQ(region_hash(5, 3, 2), region_hash(5, 3, 2));
    EXPECT_NE(region_hash(5, 3, 2), region_hash(5, 2, 3));
}

TEST(World, HeightsAndTerrainAreInRange) {
    for (const std::uint64_t seed : {0ULL, 1ULL, 12345ULL, 0xDEADBEEFULL}) {
        for (int cy = -1; cy <= 1; ++cy) {
            for (int cx = -1; cx <= 1; ++cx) {
                const Chunk chunk = generate(seed, cx, cy);
                for (const Tile& tile : chunk.tiles) {
                    EXPECT_GE(tile.height, kMinHeight);
                    EXPECT_LE(tile.height, kMaxHeight);
                    EXPECT_LT(static_cast<int>(tile.terrain), kTerrainCount);
                }
            }
        }
    }
}

TEST(World, TheWorldIsNotFlat) {
    // A generation that returns a constant would satisfy every in-range check above.
    const Chunk chunk = generate(3, 0, 0);
    const Tile first = chunk.tiles.front();
    bool varied = false;
    for (const Tile& tile : chunk.tiles) {
        varied = varied || tile.height != first.height;
    }
    EXPECT_TRUE(varied) << "height is constant across the chunk";
}

TEST(World, GoldenRegionHash) {
    // Recorded from the first committed implementation (M1). This is the drift alarm, not a
    // correctness proof: if the world changes for a given seed, this fails, and re-recording it
    // is then a deliberate act in the same commit.
    EXPECT_EQ(region_hash(42, 2, 2), 0x8aace81e659ba3f5ULL);
}

TEST(World, TerrainIsContinuousAcrossChunkBorders) {
    // A chunk-local field steps at the border. Compare the mean step ACROSS the border with the
    // mean step just inside it: a seam makes the ratio blow up.
    const Chunk left = generate(42, 0, 0);
    const Chunk right = generate(42, 1, 0);
    long across = 0;
    long inside = 0;
    for (int y = 0; y < kChunkSize; ++y) {
        const auto row = static_cast<std::size_t>(y) * kChunkSize;
        across += std::abs(left.tiles[row + kChunkSize - 1].height - right.tiles[row].height);
        inside += std::abs(left.tiles[row + kChunkSize - 3].height
                           - left.tiles[row + kChunkSize - 2].height);
    }
    EXPECT_LT(across, inside * 4) << "a step at the chunk border suggests a chunk-local field";
}

TEST(World, InvariantsHoldAcrossManySeeds) {
    // The input space here is (seed, chunk) -- a deterministic sweep over adversarial seeds
    // checks these invariants harder than random bytes would. The libFuzzer target belongs on
    // byte-level inputs (map files, replays), which arrive in M2.
    std::uint64_t seed = 0;
    for (int step = 0; step < 500; ++step) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const Chunk chunk = generate(seed, step % 7 - 3, step % 5 - 2);
        for (const Tile& tile : chunk.tiles) {
            ASSERT_GE(tile.height, kMinHeight) << "seed " << seed;
            ASSERT_LE(tile.height, kMaxHeight) << "seed " << seed;
            ASSERT_LT(static_cast<int>(tile.terrain), kTerrainCount) << "seed " << seed;
        }
    }
}

TEST(World, SpawnCandidatesExistAndAreSpawnable) {
    int seeds_with_candidates = 0;
    for (const std::uint64_t seed : {0ULL, 1ULL, 42ULL, 777ULL, 0xDEADBEEFULL, 99ULL}) {
        const std::vector<Coord> candidates = spawn_candidates(seed, 0, 0);
        if (!candidates.empty()) {
            ++seeds_with_candidates;
        }
        const Chunk chunk = generate(seed, 0, 0);
        for (const Coord& c : candidates) {
            ASSERT_GE(c.x, 0);
            ASSERT_LT(c.x, kChunkSize);
            ASSERT_GE(c.y, 0);
            ASSERT_LT(c.y, kChunkSize);
            EXPECT_TRUE(is_spawnable(chunk, c.x, c.y));
        }
    }
    EXPECT_GE(seeds_with_candidates, 5) << "most seeds must offer somewhere to land";

    const Chunk chunk = generate(42, 0, 0);
    EXPECT_FALSE(is_spawnable(chunk, -1, 0));
    EXPECT_FALSE(is_spawnable(chunk, kChunkSize, 0));
    EXPECT_FALSE(is_spawnable(chunk, 0, kChunkSize));
}

} // namespace
