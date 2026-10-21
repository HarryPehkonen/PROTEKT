// The standard skirmish is a pure function of the seed. If it is not, a replay stops being
// self-describing and every determinism guarantee downstream is void.
#include <gtest/gtest.h>

#include <limits>
#include <set>
#include <vector>

#include "protekt/sim/scenario.hpp"
#include "protekt/world/world.hpp"

namespace {

using namespace protekt::sim;
namespace world = protekt::world;

int count_of(const Scenario& s, Side side, UnitKind kind) {
    int n = 0;
    for (const Spawn& sp : s.spawns) {
        if (sp.side == side && sp.kind == kind) {
            ++n;
        }
    }
    return n;
}

TEST(Scenario, BothSidesGetTheFrozenComposition) {
    const Scenario s = skirmish(42);
    EXPECT_EQ(s.seed, 42U);
    ASSERT_EQ(s.spawns.size(), 14U) << "7 units a side, the hover tank included";
    EXPECT_EQ(count_of(s, Side::Player, UnitKind::HoverTank), 1);
    EXPECT_EQ(count_of(s, Side::Enemy, UnitKind::HoverTank), 1);
    EXPECT_EQ(s.spawns.at(3).kind, UnitKind::HoverTank) << "the player's fourth unit";
    EXPECT_EQ(s.spawns.at(3).side, Side::Player);
    for (const Side side : {Side::Player, Side::Enemy}) {
        EXPECT_EQ(count_of(s, side, UnitKind::Tank), 3);
        EXPECT_EQ(count_of(s, side, UnitKind::Factory), 1);
        EXPECT_EQ(count_of(s, side, UnitKind::Turret), 2);
    }
}

TEST(Scenario, ThePlayersFourthUnitIsAHoverTankDeterministicallyFromTheSeed) {
    for (const std::uint64_t seed : {7ULL, 42ULL}) {
        const Scenario a = skirmish(seed);
        const Scenario b = skirmish(seed);
        ASSERT_EQ(a.spawns.size(), 14U);
        EXPECT_EQ(a.spawns.at(3).kind, UnitKind::HoverTank);
        EXPECT_EQ(a.spawns.at(3).side, Side::Player);
        EXPECT_EQ(a.spawns.at(3).tile_x, b.spawns.at(3).tile_x);
        EXPECT_EQ(a.spawns.at(3).tile_y, b.spawns.at(3).tile_y);
        EXPECT_EQ(count_of(a, Side::Player, UnitKind::Tank), 3);
    }
}

TEST(Scenario, UnitsStartOnPassableGround) {
    for (const std::uint64_t seed : {1ULL, 42ULL, 777ULL, 0xDEADBEEFULL}) {
        const Scenario s = skirmish(seed);
        for (const Spawn& sp : s.spawns) {
            ASSERT_GE(sp.tile_x, 0);
            ASSERT_GE(sp.tile_y, 0);
            ASSERT_LT(sp.tile_x, kMapTiles);
            ASSERT_LT(sp.tile_y, kMapTiles);
            const world::Terrain ground = world::sample(seed, sp.tile_x, sp.tile_y).terrain;
            EXPECT_NE(ground, world::Terrain::Crater)
                << "seed " << seed << " spawns inside a crater at " << sp.tile_x << ","
                << sp.tile_y;
        }
    }
}

TEST(Scenario, IsAPureFunctionOfTheSeed) {
    const Scenario a = skirmish(42);
    const Scenario b = skirmish(42);
    ASSERT_EQ(a.spawns.size(), b.spawns.size());
    for (std::size_t i = 0; i < a.spawns.size(); ++i) {
        EXPECT_EQ(a.spawns[i].kind, b.spawns[i].kind);
        EXPECT_EQ(a.spawns[i].side, b.spawns[i].side);
        EXPECT_EQ(a.spawns[i].tile_x, b.spawns[i].tile_x);
        EXPECT_EQ(a.spawns[i].tile_y, b.spawns[i].tile_y);
    }
    const Scenario other = skirmish(43);
    bool differs = other.spawns.size() != a.spawns.size();
    for (std::size_t i = 0; !differs && i < other.spawns.size(); ++i) {
        differs = other.spawns[i].tile_x != a.spawns[i].tile_x
                  || other.spawns[i].tile_y != a.spawns[i].tile_y;
    }
    EXPECT_TRUE(differs) << "two seeds must not produce the same battlefield run for run";
}

TEST(Scenario, IdsAreAscendingAndUnique) {
    Sim sim = Sim::create(skirmish(42));
    ASSERT_EQ(sim.units().size(), 14U);
    std::set<std::uint32_t> seen;
    for (const Unit& u : sim.units()) {
        EXPECT_TRUE(u.id.valid());
        EXPECT_TRUE(seen.insert(u.id.value).second) << "duplicate id";
    }
    EXPECT_EQ(*seen.begin(), 1U) << "ids are handed out from 1 in creation order";
}

TEST(Scenario, TheTwoSidesStartWellApart) {
    const Scenario s = skirmish(42);
    int closest = std::numeric_limits<int>::max(); // the earlier value (512) capped every result
    for (const Spawn& p : s.spawns) {
        if (p.side != Side::Player) {
            continue;
        }
        for (const Spawn& e : s.spawns) {
            if (e.side != Side::Enemy) {
                continue;
            }
            const int dx = p.tile_x - e.tile_x;
            const int dy = p.tile_y - e.tile_y;
            const int d = dx * dx + dy * dy;
            closest = std::min(closest, d);
        }
    }
    EXPECT_GT(closest, 100 * 100) << "there must be ground to cross before the fight starts";
}

} // namespace
