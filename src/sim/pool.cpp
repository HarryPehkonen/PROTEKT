#include "protekt/sim/pool.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "protekt/rng.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::sim {

namespace {

constexpr std::uint64_t kPoolCountStream = 4; ///< world 0, sim 1, ai 2, scenario 3 are taken
constexpr std::uint64_t kPoolSiteStream = 5;
constexpr int kCandidatesPerPass = 4000; ///< hashed candidates tried at one spacing

[[nodiscard]] std::int64_t distance2(int ax, int ay, int bx, int by) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(ax) - bx;
    const std::int64_t dy = static_cast<std::int64_t>(ay) - by;
    return (dx * dx) + (dy * dy);
}

/// A tile a vehicle can stand on and drive out of: it and all eight neighbours are passable ground.
[[nodiscard]] bool open_ground(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (!tile_passable(seed, tile_x + dx, tile_y + dy)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int pool_count_for(std::uint64_t seed) noexcept {
    rng::SplitMix64 stream(rng::stream_seed(seed, kPoolCountStream));
    return kMinPools + static_cast<int>(stream.bounded(kMaxPools - kMinPools + 1));
}

std::vector<Pool> lay_pools(std::uint64_t seed, int count, std::span<const Pool> bases) {
    std::vector<Pool> pools;
    const auto span = static_cast<std::uint32_t>(kMapTiles - (2 * kPoolEdgeTiles));
    // Spacing relaxes by halves (24, 12, 6, 3, 1) until the whole count fits.
    for (int spacing = kPoolSpacingTiles; spacing >= 1; spacing /= 2) {
        pools.clear();
        rng::SplitMix64 stream(rng::stream_seed(seed, kPoolSiteStream));
        for (int attempt = 0; attempt < kCandidatesPerPass && std::cmp_less(pools.size(), count);
             ++attempt) {
            const int x = kPoolEdgeTiles + static_cast<int>(stream.bounded(span));
            const int y = kPoolEdgeTiles + static_cast<int>(stream.bounded(span));
            if (!open_ground(seed, x, y)) {
                continue;
            }
            const std::int64_t clear
                = static_cast<std::int64_t>(kPoolBaseClearTiles) * kPoolBaseClearTiles;
            const std::int64_t apart = static_cast<std::int64_t>(spacing) * spacing;
            const bool near_base = std::any_of(bases.begin(), bases.end(), [&](const Pool& base) {
                return distance2(x, y, base.tile_x, base.tile_y) < clear;
            });
            const bool crowded = std::any_of(pools.begin(), pools.end(), [&](const Pool& other) {
                return distance2(x, y, other.tile_x, other.tile_y) < apart;
            });
            if (!near_base && !crowded) {
                pools.push_back(Pool{x, y});
            }
        }
        if (std::cmp_equal(pools.size(), count)) {
            break;
        }
    }
    return pools;
}

const Pool* pool_at(std::span<const Pool> pools, int tile_x, int tile_y) noexcept {
    const auto found = std::find_if(pools.begin(), pools.end(), [&](const Pool& pool) {
        return pool.tile_x == tile_x && pool.tile_y == tile_y;
    });
    return found == pools.end() ? nullptr : &*found;
}

bool pool_taken(std::span<const Unit> units, const Pool& pool) noexcept {
    return std::any_of(units.begin(), units.end(), [&](const Unit& unit) {
        return unit.alive && unit.kind == UnitKind::DeployedScavenger
               && unit.x.floor_int() == pool.tile_x && unit.y.floor_int() == pool.tile_y;
    });
}

std::uint16_t store_capacity(std::span<const Unit> units, Side side, std::uint16_t base) noexcept {
    int capacity = base;
    for (const Unit& unit : units) {
        if (unit.alive && unit.side == side && unit.kind == UnitKind::DeployedScavenger) {
            capacity += kStoreCapacityPerDeployed;
        }
    }
    return static_cast<std::uint16_t>(std::min(capacity, 0xFFFF));
}

std::uint16_t store_fast_band(std::span<const Unit> units, Side side, std::uint16_t base) noexcept {
    return static_cast<std::uint16_t>(store_capacity(units, side, base) - base);
}

} // namespace protekt::sim
