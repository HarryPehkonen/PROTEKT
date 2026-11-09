#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/route.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

/// A SCAVENGER MUST NEVER BE STUCK FOREVER. The sim plans a route (A*, `plan_route`) but a goal
/// that is not connected to the unit gets none, and `steer` then heads straight at it: against the
/// ring of craters round a one-tile ISLAND that is a unit pressed against the rim for the rest of
/// the battle. These tests build that island and watch a scavenger not be stuck on it.
namespace {

using protekt::sim::Fixed;
using protekt::sim::Scenario;
using protekt::sim::Scrap;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::Stance;
using protekt::sim::TileXY;
using protekt::sim::Unit;
using protekt::sim::UnitKind;
using protekt::world::Terrain;

bool crater(std::uint64_t seed, int x, int y) {
    return protekt::world::sample(seed, x, y).terrain == Terrain::Crater;
}

/// A standable tile whose four neighbours are craters: no route leads onto it.
struct Island {
    bool found{false};
    std::uint64_t seed{};
    int x{};
    int y{};
    int sx{}; ///< where the scavenger stands: open ground, nearer the island than the far pile
    int sy{};
    int fx{}; ///< a pile it CAN drive to, farther off than the island
    int fy{};
    int wx{}; ///< a wreck tile within two of the island that a route joins to the scavenger
    int wy{};
};

struct Ground {
    std::uint64_t seed;
    [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
        return protekt::sim::tile_passable(seed, x, y) ? Fixed::kOne : 0;
    }
};

bool connected(std::uint64_t seed, int ax, int ay, int bx, int by) {
    protekt::sim::Route route{};
    return protekt::sim::plan_route(
        Ground{seed}, TileXY{static_cast<std::uint8_t>(ax), static_cast<std::uint8_t>(ay)},
        TileXY{static_cast<std::uint8_t>(bx), static_cast<std::uint8_t>(by)}, route);
}

/// The first island, over seeds in order, with a scavenger spot 4 to 9 tiles from it and a second
/// spot 12 to 18 tiles from THAT which a route joins to it: the island pile is the nearer one.
Island find_island_uncached() {
    for (std::uint64_t seed = 1; seed < 400; ++seed) {
        for (int y = 50; y < 190; ++y) {
            for (int x = 50; x < 190; ++x) {
                if (crater(seed, x, y) || !crater(seed, x - 1, y) || !crater(seed, x + 1, y)
                    || !crater(seed, x, y - 1) || !crater(seed, x, y + 1)) {
                    continue;
                }
                for (int sy = y - 9; sy <= y + 9; ++sy) {
                    for (int sx = x - 9; sx <= x + 9; ++sx) {
                        const int apart = std::max(std::abs(sx - x), std::abs(sy - y));
                        if (apart < 4 || !protekt::sim::tile_passable(seed, sx, sy)) {
                            continue;
                        }
                        for (int fy = sy - 14; fy <= sy + 14; fy += 7) {
                            for (int fx = sx - 14; fx <= sx + 14; fx += 7) {
                                const int far = std::max(std::abs(fx - sx), std::abs(fy - sy));
                                const int to_island = ((sx - x) * (sx - x)) + ((sy - y) * (sy - y));
                                const int to_far
                                    = ((fx - sx) * (fx - sx)) + ((fy - sy) * (fy - sy));
                                if (far < 12 || to_island >= to_far
                                    || !protekt::sim::tile_passable(seed, fx, fy)) {
                                    continue;
                                }
                                if (!connected(seed, sx, sy, fx, fy)) {
                                    continue;
                                }
                                for (int wy = y - 2; wy <= y + 2; ++wy) {
                                    for (int wx = x - 2; wx <= x + 2; ++wx) {
                                        if (protekt::sim::tile_passable(seed, wx, wy)
                                            && (wx != x || wy != y)
                                            && connected(seed, sx, sy, wx, wy)) {
                                            return Island{true, seed, x, y, sx, sy, fx, fy, wx, wy};
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return Island{};
}

Island find_island() {
    static const Island found = find_island_uncached();
    return found;
}

Fixed centre(int tile) { return Fixed::from_raw((tile * Fixed::kOne) + (Fixed::kOne / 2)); }

/// A scavenger at the island's east side, a pile ON the island, and optionally a pile it can reach.
Scenario island_rig(const Island& island, bool with_reachable_pile) {
    Scenario s;
    s.seed = island.seed;
    s.objectives = {};
    s.spawns.push_back(Spawn{UnitKind::Recycler, Side::Player, island.sx, island.sy - 5});
    s.spawns.push_back(Spawn{UnitKind::Scavenger, Side::Player, island.sx, island.sy});
    s.scrap.push_back(Scrap{centre(island.x), centre(island.y)});
    if (with_reachable_pile) {
        s.scrap.push_back(Scrap{centre(island.fx), centre(island.fy)});
    }
    return s;
}

const Unit& scavenger(const Sim& sim) { return sim.units().at(1); }

int ticks(int seconds) { return seconds * protekt::sim::kTicksPerSecond; }

} // namespace

TEST(StuckScavenger, TheRigIsReallyAnIslandNoRouteLeadsOnto) {
    const Island island = find_island();
    ASSERT_TRUE(island.found) << "no seed in 1..399 has an island with open ground beside it";
    EXPECT_FALSE(connected(island.seed, island.sx, island.sy, island.x, island.y));
    EXPECT_TRUE(connected(island.seed, island.sx, island.sy, island.fx, island.fy));
    Sim sim = Sim::create(island_rig(island, true));
    EXPECT_EQ(sim.scraps().size(), 2U) << "the island pile is where the scenario put it";
}

TEST(StuckScavenger, ItGivesUpThePileItCannotReachAndTakesTheOneItCan) {
    const Island island = find_island();
    ASSERT_TRUE(island.found);
    Sim sim = Sim::create(island_rig(island, true));
    // The island pile is nearer (a few tiles against a dozen): that is the one it sets out for.
    for (int i = 0; i < ticks(40) && sim.ore(Side::Player) < 1; ++i) {
        sim.step();
    }
    EXPECT_EQ(sim.ore(Side::Player), 1) << "stuck on the island pile, it never took the other";
    EXPECT_EQ(sim.scraps().size(), 1U) << "the island pile is the one left";
}

TEST(StuckScavenger, WithOnlyTheUnreachablePileItEndsUpIdleAndStaysIdle) {
    const Island island = find_island();
    ASSERT_TRUE(island.found);
    Sim sim = Sim::create(island_rig(island, false));
    for (int i = 0; i < ticks(20); ++i) {
        sim.step();
    }
    EXPECT_EQ(scavenger(sim).stance, Stance::Idle) << "it is still fetching a pile it cannot reach";
    // And it does not take it up again: the answer is the same a long while later.
    const std::int32_t x = scavenger(sim).x.raw;
    const std::int32_t y = scavenger(sim).y.raw;
    for (int i = 0; i < ticks(100); ++i) {
        sim.step();
        ASSERT_NE(scavenger(sim).stance, Stance::Fetch)
            << "it went back for the same pile, tick " << i;
    }
    EXPECT_EQ(scavenger(sim).x.raw, x);
    EXPECT_EQ(scavenger(sim).y.raw, y);
    EXPECT_EQ(sim.ore(Side::Player), 0);
    EXPECT_EQ(sim.scraps().size(), 1U);
}

TEST(StuckScavenger, LitterIsNeverDroppedOnAnIslandNoHaulerCanReach) {
    const Island island = find_island();
    ASSERT_TRUE(island.found);
    Sim sim = Sim::create(island_rig(island, false));
    // A wreck beside the island, by many ids: with the check, none of their pieces
    // may end up on it; without it (home < 0) the control shows the island IS hit.
    int hits_unchecked = 0;
    int hits_checked = 0;
    for (std::uint32_t id = 10; id < 400; ++id) {
        for (const int checked : {0, 1}) {
            std::vector<Scrap> pieces;
            protekt::sim::scatter(pieces, island.seed, protekt::sim::UnitId{id}, centre(island.wx),
                                  centre(island.wy));
            protekt::sim::settle_litter(pieces, 0, island.seed, sim.units(),
                                        checked != 0 ? island.wx : -1, island.wy);
            for (const Scrap& piece : pieces) {
                if (piece.x.floor_int() == island.x && piece.y.floor_int() == island.y) {
                    (checked != 0 ? hits_checked : hits_unchecked) += 1;
                }
            }
        }
    }
    EXPECT_GT(hits_unchecked, 0)
        << "the control never put a piece on the island: the probe is blind";
    EXPECT_EQ(hits_checked, 0) << "a piece was settled on a tile no route leads onto";
}

TEST(StuckScavenger, TheLongestTripInTheRadarIsFinishedWellBeforeTheGiveUpBeats) {
    // The give-up count must not cut off an honest trip. The same open rig as the trip tests
    // (scavenger at tile (40, 70), the pile due east), with the pile as far as the radar sees.
    Scenario s;
    s.seed = 20261023;
    s.objectives = {};
    s.spawns.push_back(Spawn{UnitKind::Recycler, Side::Player, 34, 70});
    s.spawns.push_back(Spawn{UnitKind::Scavenger, Side::Player, 40, 70});
    s.scrap.push_back(Scrap{centre(40 + 38), centre(70)});
    Sim sim = Sim::create(s);
    int took_at = -1;
    for (int i = 0; i < ticks(protekt::sim::kFetchGiveUpBeats) && took_at < 0; ++i) {
        sim.step();
        if (sim.ore(Side::Player) == 1) {
            took_at = i;
        }
    }
    ASSERT_GE(took_at, 0) << "a 38-tile trip over open ground took the whole give-up budget";
    EXPECT_LT(took_at, ticks(protekt::sim::kFetchGiveUpBeats / 2))
        << "a long honest trip must use well under half the give-up budget";
}
