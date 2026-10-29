// The world is an infinite chunk field: every chunk coordinate an int can hold has a chunk, and
// asking for it must not be undefined behaviour. The extreme-coordinate tests are red under the
// gate's UBSan build (build-asan) and silent otherwise. The pinned values are VALUE-PRESERVATION
// tests: they were recorded from the code as it was BEFORE the arithmetic was widened, so a fix
// that quietly moves the world cannot pass them.
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "protekt/world/world.hpp"

namespace {

using namespace protekt::world;

constexpr int kMax = std::numeric_limits<int>::max();
constexpr int kMin = std::numeric_limits<int>::min();

std::uint64_t digest(const Chunk& chunk) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const Tile& tile : chunk.tiles) {
        h = (h ^ static_cast<std::uint16_t>(tile.height)) * 0x100000001B3ULL;
        h = (h ^ static_cast<std::uint64_t>(tile.terrain)) * 0x100000001B3ULL;
    }
    return h;
}

struct Pin {
    int x;
    int y;
    std::uint64_t value;
};

TEST(WorldWide, ChunksThatWorkedBeforeAreUnchanged) {
    // Seed 42, pre-fix. Negative and far-out coordinates included.
    constexpr Pin pins[] = {
        {0, 0, 0xADD3A21375C612E8ULL},
        {-1, -1, 0x4A2493E1DF5EF9FEULL},
        {-3, 5, 0x92D4BD3CC786528CULL},
        {7, -2, 0x94FD20C0A669EF85ULL},
        {100000, -100000, 0xCE6229C55C58F01AULL},
        {5000000, -5000000, 0xFE33F2CD7B8645DDULL},
        {-5000000, 5000000, 0xD33132E20112847CULL},
    };
    for (const Pin& pin : pins) {
        EXPECT_EQ(digest(generate(42, pin.x, pin.y)), pin.value) << pin.x << "," << pin.y;
    }
}

TEST(WorldWide, RegionHashesThatWorkedBeforeAreUnchanged) {
    constexpr Pin pins[] = {
        {0, 0, 0x2F69B4B4B921FEAFULL},
        {1, 1, 0xDCFD6A8B3D9E5DE1ULL}, // protekt-map --seed 42 --size 8
        {3, 2, 0x62BB5D07999D3632ULL},
        {-2, 3, 0x3222A6129BBB5BC5ULL},
        {4, -1, 0xBBBB8C6FDC660A23ULL},
        {-5, -5, 0x90D6E8CE39A4EC1FULL},
    };
    for (const Pin& pin : pins) {
        EXPECT_EQ(region_hash(42, pin.x, pin.y), pin.value) << pin.x << "," << pin.y;
    }
}

TEST(WorldWide, SamplesThatWorkedBeforeAreUnchanged) {
    struct SamplePin {
        int x;
        int y;
        int height;
        Terrain terrain;
    };
    constexpr SamplePin pins[] = {
        {0, 0, 238, Terrain{2}},
        {-1, -1, 240, Terrain{2}},
        {63, 64, 145, Terrain{0}},
        {-100000, 3000000, 147, Terrain{0}},
        {300000000, -300000000, 173, Terrain{2}},
    };
    for (const SamplePin& pin : pins) {
        const Tile tile = sample(42, pin.x, pin.y);
        EXPECT_EQ(tile.height, pin.height) << pin.x << "," << pin.y;
        EXPECT_EQ(tile.terrain, pin.terrain) << pin.x << "," << pin.y;
    }
}

TEST(WorldWide, ChunksAtTheEdgeOfIntAreGeneratedWithoutOverflow) {
    for (const int cx : {kMax, kMin, kMax - 1}) {
        for (const int cy : {kMax, kMin, 0}) {
            const Chunk a = generate(42, cx, cy);
            EXPECT_EQ(digest(a), digest(generate(42, cx, cy))) << cx << "," << cy;
        }
    }
}

TEST(WorldWide, SamplingTheEdgeOfIntIsDefined) {
    // sample() looks one tile right and one tile down for the slope, so world tile kMax is the
    // overflow case. The values are not pinned (there is no pre-fix value to record: it was UB),
    // only that the call is defined and repeatable.
    for (const int x : {kMax, kMin}) {
        for (const int y : {kMax, kMin}) {
            const Tile a = sample(42, x, y);
            const Tile b = sample(42, x, y);
            EXPECT_EQ(a.height, b.height) << x << "," << y;
            EXPECT_EQ(a.terrain, b.terrain) << x << "," << y;
        }
    }
}

TEST(WorldWide, RegionHashOfAnEmptyRegionAtTheEdgeOfIntIsDefined) {
    // chunks_x/chunks_y <= 0 means no chunk is hashed: only the header is mixed, and that must
    // not depend on overflow.
    EXPECT_EQ(region_hash(42, kMin, kMin), region_hash(42, kMin, kMin));
    EXPECT_NE(region_hash(42, kMin, kMin), region_hash(42, kMin, kMin + 1));
    EXPECT_NE(region_hash(42, kMax, 0), region_hash(42, kMax - 1, 0));
}

} // namespace
