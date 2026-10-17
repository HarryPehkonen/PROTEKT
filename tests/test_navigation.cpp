#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

/// Movement over terrain that is in the way.
///
/// The simulation steers in a STRAIGHT LINE at the current goal and, when a step is refused,
/// retries along each axis separately so a unit slides along a crater rim. It does not plan a
/// route. That is enough to cross open ground and enough to slide along a straight edge -- and it
/// is not enough to leave a concave corner, where the goal direction and both axis components are
/// all blocked at once.
///
/// These tests exist because that failure is silent: the unit keeps its order, keeps its stance,
/// and does nothing forever, which from outside looks exactly like a unit that has not been ordered
/// yet.
/// **They record measured behaviour, not requirements.** The stuck test is expected to be DELETED
/// and replaced by its opposite (a unit that gets out) when navigation lands; until then it is here
/// so the gap is measured rather than remembered.
namespace protekt::sim {
namespace {

/// A base far to the south-east of a chosen trap tile, and one tank. The base is unarmed, so
/// nothing can kill the tank: whatever it does, it does on its own.
[[nodiscard]] Scenario one_tank_at(int tile_x, int tile_y) {
    Scenario scenario{};
    scenario.seed = 42;
    Spawn tank{};
    tank.side = Side::Player;
    tank.kind = UnitKind::Tank;
    tank.tile_x = tile_x;
    tank.tile_y = tile_y;
    scenario.spawns.push_back(tank);
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 147;
    base.tile_y = 166;
    scenario.spawns.push_back(base);
    return scenario;
}

struct Advance {
    int start_x{};
    int start_y{};
    int end_x{};
    int end_y{};
    Stance stance{};
    bool alive{};
};

/// Orders the tank onto the enemy base and reports where it got to.
[[nodiscard]] Advance drive(int tile_x, int tile_y, int ticks) {
    Sim sim = Sim::create(one_tank_at(tile_x, tile_y));
    UnitId tank{};
    UnitId base{};
    for (const Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            tank = unit.id;
        } else {
            base = unit.id;
        }
    }
    Order order{};
    order.kind = OrderKind::Attack;
    order.unit = tank;
    order.target = base;
    sim.enqueue(order);

    Advance result{};
    result.start_x = sim.find(tank)->x.floor_int();
    result.start_y = sim.find(tank)->y.floor_int();
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
    }
    const Unit* self = sim.find(tank);
    result.end_x = self->x.floor_int();
    result.end_y = self->y.floor_int();
    result.stance = self->stance;
    result.alive = self->alive;
    return result;
}

constexpr int kTrapX = 43;
constexpr int kTrapY = 68;

} // namespace

TEST(Navigation, AUnitOnOpenGroundClosesOnItsTarget) {
    // The control. Without it, the stuck test below could pass for the wrong reason -- an order
    // that was never accepted looks identical to a unit that cannot move. Here it is accepted and
    // works.
    const Advance advance = drive(kTrapX, kTrapY - 4, 600);
    EXPECT_TRUE(advance.alive);
    EXPECT_EQ(advance.stance, Stance::Attack) << "the order was accepted";
    const int moved = (advance.end_x - advance.start_x) + (advance.end_y - advance.start_y);
    EXPECT_GT(moved, 30) << "600 ticks at 6 tiles/s is 120 tiles of travel available; it made "
                         << moved << " tiles of net progress";
}

TEST(Navigation, AUnitInAConcaveCraterCornerNeverMoves_DOCUMENTED_GAP) {
    // Measured 2026-10-17, and the reason "no unit ever attacks a factory" was true in every one of
    // fifty measured battles. At (43,68) the tiles east, south AND the diagonal are all craters --
    // the concave corner of a rim with open ground to the north and west only. The steering tries
    // the straight-line direction and then each axis separately, all three blocked, and the unit
    // waits there forever with its order intact.
    const Advance advance = drive(kTrapX, kTrapY, 1200);
    EXPECT_TRUE(advance.alive)
        << "nothing here can kill it; if it died, this measures something else";
    EXPECT_EQ(advance.stance, Stance::Attack) << "the order WAS accepted: the unit is not idle";
    EXPECT_EQ(advance.end_x, advance.start_x);
    EXPECT_EQ(advance.end_y, advance.start_y);
    // The terrain that does it, asserted rather than described, so this test cannot quietly become
    // a record of different terrain after a world change.
    EXPECT_EQ(world::sample(42, kTrapX + 1, kTrapY).terrain, world::Terrain::Crater) << "east";
    EXPECT_EQ(world::sample(42, kTrapX, kTrapY + 1).terrain, world::Terrain::Crater) << "south";
    EXPECT_EQ(world::sample(42, kTrapX + 1, kTrapY + 1).terrain, world::Terrain::Crater)
        << "the diagonal";
    EXPECT_NE(world::sample(42, kTrapX - 1, kTrapY).terrain, world::Terrain::Crater)
        << "there is a way out to the west, which the steering cannot see";
}

} // namespace protekt::sim
