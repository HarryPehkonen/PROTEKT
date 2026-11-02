#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// A BIOMETAL POOL (Harri, 2026-10-31): an INFINITE source at a fixed place. It stores no amount --
/// it is a position, and the tile is all there is of it. A scavenger deployed on one extracts there
/// (`Sim`'s store step); the pool itself never changes, so it is scenario data and is not hashed.
struct Pool {
    int tile_x{};
    int tile_y{};
};

/// A map lays this many pools, from its terrain seed: `kMinPools` to `kMaxPools` inclusive. A
/// designed map names its own count (`ScenarioData::pool_count`).
inline constexpr int kMinPools = 3;
inline constexpr int kMaxPools = 10;

/// How a pool is placed: no nearer than `kPoolEdgeTiles` to the map's edge, no nearer than
/// `kPoolBaseClearTiles` to a base centre (the opening scrap rings reach 12), and, while the map
/// has room, `kPoolSpacingTiles` from every other pool. Placement is a sequence of hashed
/// candidates, so a pool is a pure function of the seed.
inline constexpr int kPoolEdgeTiles = 8;
inline constexpr int kPoolBaseClearTiles = 14;
inline constexpr int kPoolSpacingTiles = 24;

/// How many pools the seed lays: a number in [kMinPools, kMaxPools].
[[nodiscard]] int pool_count_for(std::uint64_t seed) noexcept;

/// `count` pools on passable ground, none within `kPoolBaseClearTiles` of a tile in `bases`. A
/// PURE FUNCTION of (seed, count, bases). The spacing relaxes by halves until `count` fit, so the
/// count asked for is the count returned unless the ground has no room at all.
[[nodiscard]] std::vector<Pool> lay_pools(std::uint64_t seed, int count,
                                          std::span<const Pool> bases);

/// The pool whose tile is (`tile_x`, `tile_y`), or nullptr.
[[nodiscard]] const Pool* pool_at(std::span<const Pool> pools, int tile_x, int tile_y) noexcept;

/// True when a living deployed scavenger of EITHER side stands on the pool: ONE SCAVENGER PER POOL.
[[nodiscard]] bool pool_taken(std::span<const Unit> units, const Pool& pool) noexcept;

/// The height of a side's GOLD band: the room its deployed scavengers added, `capacity - base`.
/// The bottom of the store is the band, the base is the green above it.
[[nodiscard]] std::uint16_t store_fast_band(std::span<const Unit> units, Side side,
                                            std::uint16_t base) noexcept;

/// The store a side's scavengers have built: `base` plus `kStoreCapacityPerDeployed` for every
/// living DEPLOYED scavenger the side has. DERIVED from the unit table, never stored, so it falls
/// the tick a deployed scavenger dies and cannot disagree with the units.
[[nodiscard]] std::uint16_t store_capacity(std::span<const Unit> units, Side side,
                                           std::uint16_t base) noexcept;

} // namespace protekt::sim
