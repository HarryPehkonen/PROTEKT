#include "protekt/world/world.hpp"

#include <algorithm>
#include <cstddef>

#include "protekt/rng.hpp"

namespace protekt::world {

namespace {

constexpr std::uint64_t kWorldStream = 0; // the worldgen subsystem's RNG stream
constexpr int kOctaves = 3;
constexpr int kFixedBits = 10; // interpolation weights are fixed-point, 1.0 == 1024
constexpr int kFixedOne = 1 << kFixedBits;

constexpr int kRoughSlope = 24;     // |dh/dx| + |dh/dy| above this is Rough
constexpr int kRidgeHeight = 160;   // at or above: Ridge
constexpr int kCraterHeight = -160; // at or below: Crater
constexpr int kSpawnMinHeight = -128;
constexpr int kSpawnMaxHeight = 128;

// Lattice value in [kMinHeight, kMaxHeight) for an integer lattice point; a pure hash of
// (seed, octave, ix, iy). The int -> uint64 conversions wrap, which is well defined.
int lattice(std::uint64_t seed, int octave, int ix, int iy) noexcept {
    const std::uint64_t key
        = seed ^ (0xC2B2AE3D27D4EB4FULL * static_cast<std::uint64_t>(octave + 1))
          ^ (0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(static_cast<std::int64_t>(ix)))
          ^ (0xD6E8FEB86659FD93ULL * static_cast<std::uint64_t>(static_cast<std::int64_t>(iy)));
    const std::uint64_t h = rng::SplitMix64(key).next();
    return static_cast<int>((h >> 32) % static_cast<std::uint64_t>(kMaxHeight - kMinHeight))
           + kMinHeight;
}

// Smoothstep on a fixed-point t in [0, 1024]; result in [0, 1024].
int smooth(int t) noexcept { return (t * t * (3 * kFixedOne - 2 * t)) >> (2 * kFixedBits); }

int lerp(int a, int b, int weight) noexcept { return a + (((b - a) * weight) >> kFixedBits); }

// One octave of value noise at world coordinates; cells are (1 << shift) tiles wide.
int value_noise(std::uint64_t seed, int octave, int shift, int wx, int wy) noexcept {
    const int ix = wx >> shift; // arithmetic shift == floor division for negatives
    const int iy = wy >> shift;
    const int mask = (1 << shift) - 1;
    const int tx = smooth(((wx & mask) << kFixedBits) >> shift);
    const int ty = smooth(((wy & mask) << kFixedBits) >> shift);
    const int top = lerp(lattice(seed, octave, ix, iy), lattice(seed, octave, ix + 1, iy), tx);
    const int bottom
        = lerp(lattice(seed, octave, ix, iy + 1), lattice(seed, octave, ix + 1, iy + 1), tx);
    return lerp(top, bottom, ty);
}

// Height at WORLD coordinates: octaves of cell width 32, 16, 8 weighted 4:2:1.
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

Terrain classify(int height, int slope) noexcept {
    if (height >= kRidgeHeight) {
        return Terrain::Ridge;
    }
    if (height <= kCraterHeight) {
        return Terrain::Crater;
    }
    if (slope > kRoughSlope) {
        return Terrain::Rough;
    }
    return Terrain::Flat;
}

std::uint64_t fnv_step(std::uint64_t h, std::uint64_t byte) noexcept {
    return (h ^ (byte & 0xFFU)) * 0x100000001B3ULL;
}

std::uint64_t fnv_u64(std::uint64_t h, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        h = fnv_step(h, value >> shift);
    }
    return h;
}

} // namespace

Chunk generate(std::uint64_t seed, int chunk_x, int chunk_y) noexcept {
    const std::uint64_t world_seed = rng::stream_seed(seed, kWorldStream);
    Chunk chunk{};
    for (int y = 0; y < kChunkSize; ++y) {
        for (int x = 0; x < kChunkSize; ++x) {
            const int wx = chunk_x * kChunkSize + x;
            const int wy = chunk_y * kChunkSize + y;
            const int h = height_at(world_seed, wx, wy);
            const int slope = abs_diff(h, height_at(world_seed, wx + 1, wy))
                              + abs_diff(h, height_at(world_seed, wx, wy + 1));
            Tile& tile
                = chunk.tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(kChunkSize)
                              + static_cast<std::size_t>(x)];
            tile.height = static_cast<std::int16_t>(h);
            tile.terrain = classify(h, slope);
        }
    }
    return chunk;
}

std::uint64_t region_hash(std::uint64_t seed, int chunks_x, int chunks_y) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL; // FNV-1a offset basis
    h = fnv_u64(h, seed);
    h = fnv_u64(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(chunks_x)));
    h = fnv_u64(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(chunks_y)));
    for (int cy = 0; cy < chunks_y; ++cy) {
        for (int cx = 0; cx < chunks_x; ++cx) {
            const Chunk chunk = generate(seed, cx, cy);
            for (const Tile& tile : chunk.tiles) {
                const auto height = static_cast<std::uint16_t>(tile.height);
                h = fnv_step(h, height);
                h = fnv_step(h, static_cast<std::uint64_t>(height >> 8));
                h = fnv_step(h, static_cast<std::uint64_t>(tile.terrain));
            }
        }
    }
    return h;
}

// Spawnable: Flat terrain with height in [-128, 128].
bool is_spawnable(const Chunk& chunk, int x, int y) noexcept {
    if (x < 0 || x >= kChunkSize || y < 0 || y >= kChunkSize) {
        return false;
    }
    const Tile& tile
        = chunk.tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(kChunkSize)
                      + static_cast<std::size_t>(x)];
    return tile.terrain == Terrain::Flat && tile.height >= kSpawnMinHeight
           && tile.height <= kSpawnMaxHeight;
}

std::vector<Coord> spawn_candidates(std::uint64_t seed, int chunk_x, int chunk_y) {
    const Chunk chunk = generate(seed, chunk_x, chunk_y);
    std::vector<Coord> out;
    for (int y = 0; y < kChunkSize; ++y) {
        for (int x = 0; x < kChunkSize; ++x) {
            if (is_spawnable(chunk, x, y)) {
                out.push_back(Coord{x, y});
            }
        }
    }
    return out;
}

} // namespace protekt::world
