// world::sample() is what the sim uses to ask "what is the ground here?". It must be the same
// world generate() builds, and it must be pure.
#include <gtest/gtest.h>

#include <set>

#include "protekt/world/world.hpp"

namespace {

using namespace protekt::world;

TEST(WorldSample, AgreesWithGenerateForEveryTileOfSeveralChunks) {
    // sample() may compute one tile directly instead of generating a chunk -- a fair performance
    // choice that creates a SECOND copy of the noise arithmetic. This sweep is what stops the two
    // copies drifting apart: every tile of several chunks, negative coordinates included.
    for (const std::uint64_t seed : {7ULL, 0ULL, 0xDEADBEEFULL}) {
        for (int cy = -1; cy <= 0; ++cy) {
            for (int cx = -1; cx <= 1; ++cx) {
                const Chunk chunk = generate(seed, cx, cy);
                for (int ty = 0; ty < kChunkSize; ++ty) {
                    for (int tx = 0; tx < kChunkSize; ++tx) {
                        const Tile expected
                            = chunk.tiles.at(static_cast<std::size_t>(ty) * kChunkSize + tx);
                        const Tile got = sample(seed, cx * kChunkSize + tx, cy * kChunkSize + ty);
                        ASSERT_EQ(got.height, expected.height)
                            << "seed " << seed << " tile " << tx << "," << ty;
                        ASSERT_EQ(got.terrain, expected.terrain)
                            << "seed " << seed << " tile " << tx << "," << ty;
                    }
                }
            }
        }
    }
}

TEST(WorldSample, IsPure) {
    const Tile first = sample(11, 130, 200);
    (void)sample(99, 5, 5);
    const Tile again = sample(11, 130, 200);
    EXPECT_EQ(first.height, again.height);
    EXPECT_EQ(first.terrain, again.terrain);
}

TEST(WorldSample, TheMapHasVariedTerrain) {
    // Without this, a generation that returned nothing but Flat would make the terrain tests
    // below pass for the wrong reason.
    std::set<int> seen;
    for (int y = 0; y < 256; y += 3) {
        for (int x = 0; x < 256; x += 3) {
            seen.insert(static_cast<int>(sample(42, x, y).terrain));
        }
    }
    EXPECT_GE(seen.size(), 3U) << "a 256x256 map should contain most terrain kinds";
}

} // namespace
