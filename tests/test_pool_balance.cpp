// A FAIR MAP (Harri, 2026-11-02): "Balance the count -- each side's half gets its share of the
// pools, so no seed can hand one side the economy, while the positions stay random inside each
// half." A pool's side is whichever base is NEARER; `count / 2` per side, and the odd one goes to
// the side the seed picks.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

#include "protekt/sim/pool.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::sim::Pool;

constexpr std::array<Pool, 2> kBases{Pool{30, 30}, Pool{200, 190}};

/// How many of `pools` are nearer base 0 and how many nearer base 1: the perpendicular bisector,
/// one comparison. A tie is base 0's.
std::array<int, 2> split(const std::vector<Pool>& pools, const std::array<Pool, 2>& bases) {
    std::array<int, 2> sides{};
    for (const Pool& pool : pools) {
        const auto d2 = [&pool](const Pool& base) {
            const std::int64_t dx = pool.tile_x - base.tile_x;
            const std::int64_t dy = pool.tile_y - base.tile_y;
            return (dx * dx) + (dy * dy);
        };
        ++sides.at(d2(bases.at(1)) < d2(bases.at(0)) ? 1U : 0U);
    }
    return sides;
}

TEST(PoolBalance, AnEvenCountSplitsExactlyInHalfForEverySeed) {
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        for (const int count : {4, 6, 10}) {
            const std::vector<Pool> pools = protekt::sim::lay_pools(seed, count, kBases);
            ASSERT_EQ(static_cast<int>(pools.size()), count) << "seed " << seed;
            const std::array<int, 2> sides = split(pools, kBases);
            EXPECT_EQ(sides.at(0), count / 2) << "seed " << seed << " count " << count;
            EXPECT_EQ(sides.at(1), count / 2) << "seed " << seed << " count " << count;
        }
    }
}

TEST(PoolBalance, AnOddCountGivesTheExtraPoolToOneSideOrTheOther) {
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        for (const int count : {3, 7, 9}) {
            const std::array<int, 2> sides
                = split(protekt::sim::lay_pools(seed, count, kBases), kBases);
            EXPECT_EQ(sides.at(0) + sides.at(1), count) << "seed " << seed;
            EXPECT_EQ(std::min(sides.at(0), sides.at(1)), count / 2)
                << "seed " << seed << " count " << count;
        }
    }
}

TEST(PoolBalance, TheOddPoolGoesToWhicheverSideTheSeedPicksSoNeitherAlwaysWins) {
    std::set<int> extra_side;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        const std::array<int, 2> sides = split(protekt::sim::lay_pools(seed, 5, kBases), kBases);
        extra_side.insert(sides.at(0) > sides.at(1) ? 0 : 1);
    }
    EXPECT_EQ(extra_side.size(), 2U) << "across seeds the odd one lands on both sides";
}

TEST(PoolBalance, ThePositionsStillVaryWithTheSeedInsideEachHalf) {
    std::set<std::pair<int, int>> first_of_side_zero;
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        for (const Pool& pool : protekt::sim::lay_pools(seed, 6, kBases)) {
            first_of_side_zero.insert({pool.tile_x, pool.tile_y});
        }
    }
    EXPECT_GT(first_of_side_zero.size(), 60U) << "180 placements over 30 seeds are mostly distinct";
}

TEST(PoolBalance, EveryOldRejectionStillApplies) {
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        const std::vector<Pool> pools = protekt::sim::lay_pools(seed, 10, kBases);
        for (std::size_t i = 0; i < pools.size(); ++i) {
            const Pool& pool = pools.at(i);
            EXPECT_TRUE(protekt::sim::tile_passable(seed, pool.tile_x, pool.tile_y));
            for (const Pool& base : kBases) {
                const int dx = pool.tile_x - base.tile_x;
                const int dy = pool.tile_y - base.tile_y;
                EXPECT_GE((dx * dx) + (dy * dy),
                          protekt::sim::kPoolBaseClearTiles * protekt::sim::kPoolBaseClearTiles)
                    << "seed " << seed << " pool " << i << " is on a base";
            }
            for (std::size_t j = i + 1; j < pools.size(); ++j) {
                EXPECT_FALSE(pool.tile_x == pools.at(j).tile_x && pool.tile_y == pools.at(j).tile_y)
                    << "seed " << seed << ": two pools on one tile";
            }
        }
    }
}

TEST(PoolBalance, ASkirmishMapIsBalancedAboutItsOwnBases) {
    for (const std::uint64_t seed : {1ULL, 7ULL, 42ULL, 20261023ULL}) {
        const protekt::sim::Scenario scenario = protekt::sim::skirmish(seed);
        std::vector<Pool> factories;
        for (const protekt::sim::Spawn& spawn : scenario.spawns) {
            if (spawn.kind == protekt::sim::UnitKind::Factory) {
                factories.push_back(Pool{spawn.tile_x, spawn.tile_y});
            }
        }
        ASSERT_EQ(factories.size(), 2U);
        const std::array<int, 2> sides
            = split(scenario.pools, std::array<Pool, 2>{factories.at(0), factories.at(1)});
        EXPECT_LE(std::abs(sides.at(0) - sides.at(1)), 1) << "seed " << seed;
    }
}

} // namespace
