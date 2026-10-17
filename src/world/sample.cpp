#include <algorithm>

#include "protekt/rng.hpp"
#include "protekt/world/world.hpp"

namespace protekt::world {

namespace {

// A single-tile evaluation of the field generate() builds. It mirrors the arithmetic in
// world.cpp exactly (same constants, same order); AgreesWithGenerate pins the two together.
// Generating a whole chunk to read one tile would cost ~12k tile evaluations per lookup.
constexpr std::uint64_t kWorldStream = 0;
constexpr int kOctaves = 3;
constexpr int kFixedBits = 10;
constexpr int kFixedOne = 1 << kFixedBits;
constexpr int kRoughSlope = 24;
constexpr int kRidgeHeight = 160;
constexpr int kCraterHeight = -160;

int lattice(std::uint64_t seed, int octave, int ix, int iy) noexcept {
    const std::uint64_t key
        = seed ^ (0xC2B2AE3D27D4EB4FULL * static_cast<std::uint64_t>(octave + 1))
          ^ (0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(static_cast<std::int64_t>(ix)))
          ^ (0xD6E8FEB86659FD93ULL * static_cast<std::uint64_t>(static_cast<std::int64_t>(iy)));
    const std::uint64_t h = rng::SplitMix64(key).next();
    return static_cast<int>((h >> 32) % static_cast<std::uint64_t>(kMaxHeight - kMinHeight))
           + kMinHeight;
}

int smooth(int t) noexcept { return (t * t * (3 * kFixedOne - 2 * t)) >> (2 * kFixedBits); }

int lerp(int a, int b, int weight) noexcept { return a + (((b - a) * weight) >> kFixedBits); }

int value_noise(std::uint64_t seed, int octave, int shift, int wx, int wy) noexcept {
    const int ix = wx >> shift;
    const int iy = wy >> shift;
    const int mask = (1 << shift) - 1;
    const int tx = smooth(((wx & mask) << kFixedBits) >> shift);
    const int ty = smooth(((wy & mask) << kFixedBits) >> shift);
    const int top = lerp(lattice(seed, octave, ix, iy), lattice(seed, octave, ix + 1, iy), tx);
    const int bottom
        = lerp(lattice(seed, octave, ix, iy + 1), lattice(seed, octave, ix + 1, iy + 1), tx);
    return lerp(top, bottom, ty);
}

int height_at(std::uint64_t seed, int wx, int wy) noexcept {
    constexpr int kShifts[kOctaves] = {5, 4, 3};
    constexpr int kWeights[kOctaves] = {4, 2, 1};
    int sum = 0;
    for (int o = 0; o < kOctaves; ++o) {
        sum += kWeights[o] * value_noise(seed, o, kShifts[o], wx, wy);
    }
    return std::clamp(sum / 7, kMinHeight, kMaxHeight);
}

int abs_diff(int a, int b) noexcept { return a > b ? a - b : b - a; }

} // namespace

Tile sample(std::uint64_t seed, int world_x, int world_y) noexcept {
    const std::uint64_t world_seed = rng::stream_seed(seed, kWorldStream);
    const int h = height_at(world_seed, world_x, world_y);
    const int slope = abs_diff(h, height_at(world_seed, world_x + 1, world_y))
                      + abs_diff(h, height_at(world_seed, world_x, world_y + 1));
    Tile tile{};
    tile.height = static_cast<std::int16_t>(h);
    if (h >= kRidgeHeight) {
        tile.terrain = Terrain::Ridge;
    } else if (h <= kCraterHeight) {
        tile.terrain = Terrain::Crater;
    } else if (slope > kRoughSlope) {
        tile.terrain = Terrain::Rough;
    } else {
        tile.terrain = Terrain::Flat;
    }
    return tile;
}

} // namespace protekt::world
