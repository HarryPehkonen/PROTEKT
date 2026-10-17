// The player, as a commander.
//
// The claim under test is architectural: a human enters the simulation through the same interface
// and the same orders as the scripted opponent, and the sim does not know the difference.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "protekt/game/player.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace {

using protekt::game::InputState;
using protekt::game::PlayerCommander;

constexpr std::uint64_t kSeed = 42;

std::vector<protekt::sim::Order> orders_for(const protekt::sim::Sim& sim, PlayerCommander& player) {
    std::vector<protekt::sim::Order> out;
    player.think(protekt::ai::observe(sim, protekt::sim::Side::Player), out);
    return out;
}

protekt::sim::UnitId player_unit(const protekt::sim::Sim& sim) {
    for (const auto& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Player && unit.kind == protekt::sim::UnitKind::Tank) {
            return unit.id;
        }
    }
    return {};
}

TEST(PlayerCommander, OnlyEverOrdersItsOwnUnit) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    const auto id = player_unit(sim);
    ASSERT_TRUE(id.valid());
    PlayerCommander player(id);
    InputState input;
    input.throttle = 1.0F;
    input.turn = 0.4F;
    player.set_input(input);
    const auto orders = orders_for(sim, player);
    ASSERT_FALSE(orders.empty()) << "full ahead must produce an order";
    for (const auto& order : orders) {
        EXPECT_EQ(order.unit, id) << "a commander that orders other people's units is the bug "
                                     "this project already shipped once";
    }
}

TEST(PlayerCommander, FullAheadSendsTheTankWhereItIsPointing) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    const auto id = player_unit(sim);
    PlayerCommander player(id);
    const auto& tank = sim.units().front();
    InputState input;
    input.throttle = 1.0F;
    player.set_input(input);
    const auto orders = orders_for(sim, player);
    ASSERT_FALSE(orders.empty());
    const auto& order = orders.front();
    EXPECT_EQ(order.kind, protekt::sim::OrderKind::MoveTo);
    // Yaw 0 points at +y in tile space; the waypoint must be ahead, not on top of the tank.
    // Raw Q16.16, because the simulation is fixed-point and the test has no reason to float it.
    EXPECT_GT(order.y.raw, tank.y.raw) << "full ahead pointed backwards";
}

TEST(PlayerCommander, ReverseSendsItBackwards) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    const auto id = player_unit(sim);
    PlayerCommander player(id);
    const auto& tank = sim.units().front();
    InputState input;
    input.throttle = -1.0F;
    player.set_input(input);
    const auto orders = orders_for(sim, player);
    ASSERT_FALSE(orders.empty());
    EXPECT_LT(orders.front().y.raw, tank.y.raw) << "reverse pointed forwards";
}

TEST(PlayerCommander, TurningMovesTheWaypointOffTheOldHeading) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.throttle = 1.0F;
    player.set_input(input);
    const auto straight_orders = orders_for(sim, player);
    ASSERT_FALSE(straight_orders.empty()) << "full ahead must produce an order";
    const auto straight = straight_orders.front();

    input.turn = 1.0F;
    player.set_input(input);
    const auto turned_orders = orders_for(sim, player);
    ASSERT_FALSE(turned_orders.empty());
    const auto turned = turned_orders.front();
    EXPECT_NE(straight.x.raw, turned.x.raw)
        << "turning changed nothing: the heading is not reaching the waypoint";
}

TEST(PlayerCommander, TurnRateIsPerTickAndClamped) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.turn = 1.0F;
    player.set_input(input);
    (void)orders_for(sim, player);
    const float once = player.heading();
    (void)orders_for(sim, player);
    EXPECT_NEAR(player.heading(), 2.0F * once, 0.001F) << "turning must be per tick, not per frame";
    EXPECT_LT(std::abs(once), 0.5F) << "a whole half-radian per tick is not a tank, it is a spin";
}

TEST(PlayerCommander, IdleDoesNotDriveAnywhere) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    const auto id = player_unit(sim);
    const std::int32_t start_x = sim.units().front().x.raw;
    const std::int32_t start_y = sim.units().front().y.raw;
    for (int i = 0; i < 60; ++i) {
        for (const auto& order : orders_for(sim, player)) {
            sim.enqueue(order);
        }
        sim.step();
    }
    const auto& unit = *std::find_if(sim.units().begin(), sim.units().end(),
                                     [id](const auto& u) { return u.id == id; });
    const std::int64_t moved_x = unit.x.raw - start_x;
    const std::int64_t moved_y = unit.y.raw - start_y;
    const std::int64_t moved
        = std::llround(std::sqrt(static_cast<double>(moved_x * moved_x + moved_y * moved_y)));
    EXPECT_LT(moved, static_cast<std::int64_t>(1) << 16)
        << "with no input the tank wandered " << (static_cast<double>(moved) / 65536.0) << " tiles";
}

TEST(PlayerCommander, DrivingForwardActuallyMovesTheTankThroughTheSimulation) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    const auto id = player_unit(sim);
    PlayerCommander player(id);
    InputState input;
    input.throttle = 1.0F;

    // Terrain can legitimately veto a straight line: with seed 42 this spawn faces FOUR crater
    // tiles, and the sim correctly refuses to drive into them (measured; the first version of
    // this test demanded 30 tiles and got 1). So turn until the heading has a clear run, then
    // drive -- which also exercises turning.
    const auto& tank = sim.units().front();
    const int tx = tank.x.floor_int();
    const int ty = tank.y.floor_int();
    // Only a Crater blocks movement (terrain_speed_factor is zero there; Ridge is merely slow).
    const auto clear_run = [tx, ty](float heading) {
        for (int d = 1; d <= 10; ++d) {
            const auto tile = protekt::world::sample(
                kSeed, tx + static_cast<int>(std::sin(heading) * static_cast<float>(d)),
                ty + static_cast<int>(std::cos(heading) * static_cast<float>(d)));
            if (tile.terrain == protekt::world::Terrain::Crater) {
                return false;
            }
        }
        return true;
    };
    int guard = 0;
    while (!clear_run(player.heading()) && guard < 400) {
        input.turn = 1.0F;
        player.set_input(input);
        std::vector<protekt::sim::Order> ignored;
        player.think(protekt::ai::observe(sim, protekt::sim::Side::Player), ignored);
        ++guard;
    }
    ASSERT_LT(guard, 400) << "no clear heading in a full turn: the spawn is boxed in";
    input.turn = 0.0F;
    player.set_input(input);

    const std::int32_t start_x = sim.units().front().x.raw;
    const std::int32_t start_y = sim.units().front().y.raw;
    for (int i = 0; i < 150; ++i) {
        for (const auto& order : orders_for(sim, player)) {
            sim.enqueue(order);
        }
        sim.step();
    }
    const auto& unit = *std::find_if(sim.units().begin(), sim.units().end(),
                                     [id](const auto& u) { return u.id == id; });
    const std::int32_t dx = unit.x.raw - start_x;
    const std::int32_t dy = unit.y.raw - start_y;
    const double distance = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
    EXPECT_GT(distance, 3.0 * 65536.0) << "150 ticks at 0.2 tiles/tick is 30 tiles; the tank moved "
                                       << (distance / 65536.0) << " tiles";
    EXPECT_GT(distance, 5.0 * 65536.0)
        << "150 ticks at 0.2 tiles/tick is 30 tiles of clear run; the tank managed "
        << (distance / 65536.0) << " tiles";
}

TEST(PlayerCommander, DoesNotHijackTheDrivingToChaseEnemies) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    const auto id = player_unit(sim);
    PlayerCommander player(id);
    // Walk the sim forward until the sides are close, driving the player straight at the enemy.
    InputState input;
    input.throttle = 1.0F;
    player.set_input(input);
    for (int i = 0; i < 3000; ++i) {
        for (const auto& order : orders_for(sim, player)) {
            EXPECT_EQ(order.kind, protekt::sim::OrderKind::MoveTo)
                << "the player issued an Attack, which outranks auto-engagement and would make "
                   "the tank pursue instead of obey the steering wheel";
            sim.enqueue(order);
        }
        sim.step();
    }
}

} // namespace
