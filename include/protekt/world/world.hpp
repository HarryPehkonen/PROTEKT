#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace protekt::world {

inline constexpr int kChunkSize = 64; ///< tiles per side
inline constexpr int kChunkTiles = kChunkSize * kChunkSize;
inline constexpr int kMinHeight = -512;
inline constexpr int kMaxHeight = 512;

enum class Terrain : std::uint8_t { Flat = 0, Rough = 1, Ridge = 2, Crater = 3 };
inline constexpr int kTerrainCount = 4;

struct Tile {
    /// Integer on purpose. A float height would make the world depend on the compiler, the
    /// optimisation level and -ffast-math, which is exactly what a seed must not do.
    std::int16_t height{};
    Terrain terrain{};
};

struct Chunk {
    std::array<Tile, kChunkTiles> tiles{};
};

struct Coord {
    int x{};
    int y{};
};

/// The world is a PURE FUNCTION of (seed, chunk coordinate). Nothing is cached, nothing is
/// stored, no global or static state may be involved: the same arguments must produce
/// byte-identical results in any process, on any machine, at any optimisation level.
/// Tiles are indexed [y * kChunkSize + x]; the world is continuous across chunk borders
/// (the field is a function of WORLD coordinates, not chunk-local ones).
[[nodiscard]] Chunk generate(std::uint64_t seed, int chunk_x, int chunk_y) noexcept;

/// A stable hash over the chunks in [0, chunks_x) x [0, chunks_y) of the world for `seed`.
/// Used as the determinism oracle: it must not change between runs or machines.
[[nodiscard]] std::uint64_t region_hash(std::uint64_t seed, int chunks_x, int chunks_y) noexcept;

/// The tile at a WORLD tile coordinate, for runtimes that need to look up a single spot
/// (the sim does). Pure, no cache, no global state: it generates the chunk that contains the
/// coordinate. Must agree with generate() tile for tile.
[[nodiscard]] Tile sample(std::uint64_t seed, int world_x, int world_y) noexcept;

/// True when a tile can host a spawning unit: its terrain must be passable and its height
/// within the band the implementation documents. Out-of-range coordinates return false.
[[nodiscard]] bool is_spawnable(const Chunk& chunk, int x, int y) noexcept;

/// Deterministic spawn candidates for one chunk, in scan order.
[[nodiscard]] std::vector<Coord> spawn_candidates(std::uint64_t seed, int chunk_x, int chunk_y);

} // namespace protekt::world
