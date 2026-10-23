#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

namespace protekt::game {

/// The playable map, in tiles. The cache covers exactly the map the route planner walks; its side
/// is taken from the sim the same way game/world_map.hpp takes its own.
inline constexpr int kTerrainCacheSide = sim::kMapTiles;

/// How many tiles the table holds: the whole map, 65,536.
inline constexpr std::size_t kTerrainCacheTiles
    = static_cast<std::size_t>(kTerrainCacheSide) * static_cast<std::size_t>(kTerrainCacheSide);

/// The world's answer for one tile, remembered.
///
/// WHY THIS EXISTS. `world::sample` is pure and cheap to describe, but it is not cheap to RUN: it
/// evaluates three octaves of value noise on every call, measured at 0.77 us on a 2012 i5. The
/// terrain mesh asks it five times per tile -- four corners plus the tile's own type -- and the
/// mesh is rebuilt every frame, so a radius-24 frame spent 12.1 ms re-deriving 2,401 tiles it had
/// already derived the frame before (measured: 5.0 us a tile, 78% of that in sample calls). The
/// same primitive is what route planning leans on, where 2,318 calls cost 1.8 ms.
///
/// This is a MEMO, never a second source of truth: an entry is written only by `world::sample` and
/// read back unchanged, so a frame that uses the cache is byte-identical to one that does not.
/// TerrainCache.MatchesTheWorld and TerrainCache.ChangesNothingAboutAFrame pin exactly that.
///
/// LAZY, so nothing pays for tiles nobody looks at, and PER SEED: the seed is a constructor
/// argument and cannot be changed afterwards, because a cache that answered for two seeds is the
/// one bug worth fearing here. Callers that cannot state the seed simply pass no cache and get the
/// old path.
///
/// OWNED BY THE CALLER -- the app holds one for its session and hands it to build_scene through
/// SceneOptions as a non-owned pointer, the way `headings` and `nav_marks` are passed. The two
/// accessors are const and the table is `mutable` because a memo IS a write behind a read: the
/// answers never change, only the work does.
///
/// Coordinates outside the map are answered but not remembered: the noise field is defined
/// everywhere (the mesh samples one tile past its far edge), the table is not.
class TerrainCache final {
public:
    explicit TerrainCache(std::uint64_t seed) noexcept
        : seed_(seed), tiles_(kTerrainCacheTiles), derived_(kTerrainCacheTiles, 0) {}

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

    /// The tile's height in the WORLD's own units: the conversion to metres belongs to the picture,
    /// not here, so the sim could reuse this table without dragging math.hpp into the sim.
    [[nodiscard]] std::int16_t height_units(int tile_x, int tile_y) const noexcept {
        return tile_at(tile_x, tile_y).height;
    }

    /// The tile's terrain type, which picks the mesh's texture and its walkability.
    [[nodiscard]] world::Terrain terrain(int tile_x, int tile_y) const noexcept {
        return tile_at(tile_x, tile_y).terrain;
    }

    /// How many tiles have been derived so far: the measurement the probes and the tests read, and
    /// the number that says the memo is being used rather than the world.
    [[nodiscard]] std::size_t tiles_derived() const noexcept { return derived_count_; }

private:
    [[nodiscard]] static bool in_map(int tile_x, int tile_y) noexcept {
        return tile_x >= 0 && tile_y >= 0 && tile_x < kTerrainCacheSide
               && tile_y < kTerrainCacheSide;
    }

    [[nodiscard]] static std::size_t index_of(int tile_x, int tile_y) noexcept {
        return (static_cast<std::size_t>(tile_y) * static_cast<std::size_t>(kTerrainCacheSide))
               + static_cast<std::size_t>(tile_x);
    }

    /// The memo itself: one `world::sample` per tile, and only on the first ask.
    [[nodiscard]] const world::Tile& tile_at(int tile_x, int tile_y) const noexcept {
        if (!in_map(tile_x, tile_y)) {
            outside_ = world::sample(seed_, tile_x, tile_y);
            return outside_;
        }
        const std::size_t at = index_of(tile_x, tile_y);
        if (derived_.at(at) == 0) {
            tiles_.at(at) = world::sample(seed_, tile_x, tile_y);
            derived_.at(at) = 1;
            ++derived_count_;
        }
        return tiles_.at(at);
    }

    std::uint64_t seed_{};
    mutable std::vector<world::Tile> tiles_{};
    mutable std::vector<std::uint8_t> derived_{};
    mutable world::Tile outside_{}; ///< the scratch tile an off-map coordinate is answered with
    mutable std::size_t derived_count_{0};
};

} // namespace protekt::game
