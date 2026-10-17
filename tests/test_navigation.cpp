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
/// route; what it does about a CONCAVE corner -- where the goal direction and both axis components
/// are blocked at once -- is a short remembered detour, chosen in sim/steering.hpp.
///
/// The failure mode is silent: a unit that cannot get through keeps its order, keeps its stance and
/// does nothing, which from outside looks exactly like a unit nobody has ordered yet. That is why
/// the terrain in the corner case is ASSERTED here rather than described -- if the world changes,
/// this file stops testing what it names.
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

TEST(Navigation, AUnitInAConcaveCraterCornerFindsItsWayOut) {
    // The corner that held a tank for 3000 ticks: at (43,68) the tiles east, south and the diagonal
    // are all crater, with open ground to the north and west. The terrain is asserted rather than
    // described, so this cannot quietly become a record of different ground after a world change.
    EXPECT_EQ(world::sample(42, kTrapX + 1, kTrapY).terrain, world::Terrain::Crater) << "east";
    EXPECT_EQ(world::sample(42, kTrapX, kTrapY + 1).terrain, world::Terrain::Crater) << "south";
    EXPECT_EQ(world::sample(42, kTrapX + 1, kTrapY + 1).terrain, world::Terrain::Crater)
        << "the diagonal";
    EXPECT_NE(world::sample(42, kTrapX - 1, kTrapY).terrain, world::Terrain::Crater)
        << "there is a way out to the west";

    const Advance advance = drive(kTrapX, kTrapY, 1200);
    EXPECT_TRUE(advance.alive)
        << "nothing here can kill it; if it died, this measures something else";
    EXPECT_EQ(advance.stance, Stance::Attack) << "the order WAS accepted";
    const int moved = (advance.end_x - advance.start_x) + (advance.end_y - advance.start_y);
    EXPECT_GT(moved, 30) << "it must leave the pocket: it made " << moved
                         << " tiles of net progress";
}

TEST(Navigation, ATankOrderedOntoAFactoryReachesItAndDamagesIt) {
    // The measurement that started this: in fifty battles no unit ever damaged a base, because no
    // unit could get there. This is the end-to-end statement of the fix.
    Sim sim = Sim::create(one_tank_at(kTrapX, kTrapY));
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

    std::uint16_t lowest = sim.find(base)->hp;
    int first_damage = 0;
    for (int tick = 1; tick <= 2400; ++tick) {
        sim.step();
        const std::uint16_t hp = sim.find(base)->hp;
        if (hp < lowest) {
            lowest = hp;
            if (first_damage == 0) {
                first_damage = tick;
            }
        }
    }
    EXPECT_TRUE(sim.find(tank)->alive) << "it died on the way, so this measures something else";
    EXPECT_LT(lowest, 400) << "the base took no damage at all";
    EXPECT_EQ(first_damage, 965)
        << "and it takes about a thousand ticks to get there and open fire";
}

} // namespace protekt::sim
