// The player, as a commander.
//
// The claim under test is architectural: a human enters the simulation through the same interface
// and the same orders as the scripted opponent, and the sim does not know the difference.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/game/player.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace {

using protekt::game::InputState;
using protekt::game::kLookaheadTiles;
using protekt::game::kTurnEasePerTick;
using protekt::game::kTurnRatePerTick;
using protekt::game::PlayerCommander;

constexpr std::uint64_t kSeed = 42;

/// How long the helm takes to reach full lock, in ticks, TAKEN FROM THE CONSTANT rather than
/// restated: the ease is a feel value that will be tuned, and a test that hardcodes "12 ticks"
/// stops testing the thing the moment somebody tunes it.
constexpr int kTicksToFullLock = static_cast<int>(1.0F / kTurnEasePerTick) + 2;

std::vector<protekt::sim::Order> orders_for(const protekt::sim::Sim& sim, PlayerCommander& player) {
    std::vector<protekt::sim::Order> out;
    player.think(protekt::sim::observe(sim, protekt::sim::Side::Player), out);
    return out;
}

/// One sim tick with whatever input is set: the commander thinks, its orders are enqueued, the sim
/// steps. The helm is clocked by the SIM TICK, so a test that never steps the sim is not testing
/// the steering at all -- which is exactly what this file did before the helm was clocked.
void tick_once(protekt::sim::Sim& sim, PlayerCommander& player) {
    for (const auto& order : orders_for(sim, player)) {
        sim.enqueue(order);
    }
    sim.step();
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
    EXPECT_EQ(order.kind(), protekt::sim::OrderKind::MoveTo);
    // Yaw 0 points at +y in tile space; the waypoint must be ahead, not on top of the tank.
    // Raw Q16.16, because the simulation is fixed-point and the test has no reason to float it.
    EXPECT_GT(std::get<protekt::sim::orders::MoveTo>(order.payload).y.raw, tank.y.raw)
        << "full ahead pointed backwards";
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
    EXPECT_LT(std::get<protekt::sim::orders::MoveTo>(orders.front().payload).y.raw, tank.y.raw)
        << "reverse pointed forwards";
}

TEST(PlayerCommander, TurningMovesTheWaypointOffTheOldHeading) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.throttle = 1.0F;
    player.set_input(input);
    const auto straight_orders = orders_for(sim, player);
    ASSERT_FALSE(straight_orders.empty()) << "full ahead must produce an order";
    const auto straight = straight_orders.front();

    input.turn = 1.0F;
    player.set_input(input);
    // A tick has to run for the helm to move: a turn is a tick's worth of turn, not a call's.
    tick_once(sim, player);
    const auto turned_orders = orders_for(sim, player);
    ASSERT_FALSE(turned_orders.empty());
    const auto turned = turned_orders.front();
    EXPECT_NE(std::get<protekt::sim::orders::MoveTo>(straight.payload).x.raw,
              std::get<protekt::sim::orders::MoveTo>(turned.payload).x.raw)
        << "turning changed nothing: the heading is not reaching the waypoint";
}

TEST(PlayerCommander, TheHelmEasesUpToTheTankRate) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.turn = 1.0F;
    player.set_input(input);

    float previous = 0.0F;
    float last = player.heading();
    for (int tick = 0; tick < kTicksToFullLock; ++tick) {
        tick_once(sim, player);
        const float delta = player.heading() - last;
        last = player.heading();
        EXPECT_GE(delta, previous) << "tick " << tick << ": the helm jumped instead of easing in";
        EXPECT_LE(delta, kTurnRatePerTick + 1e-6F)
            << "tick " << tick << ": faster than full lock, which easing must not allow";
        if (tick == 0) {
            // Half the rate, not "less than the rate": `wrap_angle` loses a few bits folding the
            // heading into range, so a delta that is a hair under full lock is rounding, not
            // easing.
            EXPECT_LT(delta, 0.5F * kTurnRatePerTick)
                << "the first tick was already near full lock: that is a snap, not an ease";
        }
        previous = delta;
    }
    EXPECT_NEAR(player.turn_rate(), 1.0F, 1e-5F) << "full lock was never reached";
    EXPECT_NEAR(previous, kTurnRatePerTick, 1e-5F)
        << "full lock is still kTurnRatePerTick radians per tick";
}

TEST(PlayerCommander, TheHelmEasesBackToCentre) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.turn = 1.0F;
    player.set_input(input);
    for (int tick = 0; tick < kTicksToFullLock; ++tick) {
        tick_once(sim, player);
    }
    ASSERT_NEAR(player.turn_rate(), 1.0F, 1e-5F) << "the test needs the helm at full lock first";

    input.turn = 0.0F;
    player.set_input(input);
    float previous = kTurnRatePerTick + 1.0F;
    for (int tick = 0; tick < kTicksToFullLock; ++tick) {
        const float last = player.heading();
        tick_once(sim, player);
        const float delta = player.heading() - last;
        EXPECT_LE(delta, previous + 1e-6F) << "tick " << tick << ": the helm snapped off the stop";
        previous = delta;
    }
    EXPECT_NEAR(player.turn_rate(), 0.0F, 1e-5F) << "the helm never came off the stop";
    EXPECT_NEAR(previous, 0.0F, 1e-6F) << "still turning after letting go";
}

TEST(PlayerCommander, TheThrottleEasesAndTopsOutAtTheLookahead) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    const auto id = player_unit(sim);
    PlayerCommander player(id);
    InputState input;
    input.throttle = 1.0F;
    player.set_input(input);

    constexpr float kRaw = 1.0F / 65536.0F;
    float previous = 0.0F;
    for (int tick = 0; tick < kTicksToFullLock; ++tick) {
        const auto* tank = sim.find(id);
        ASSERT_NE(tank, nullptr);
        // The commander builds its waypoint from the unit's TILE-floor position, so that is what
        // the distance is measured against; the sub-tile offset would put a tile of noise in the
        // number.
        const float tank_x = static_cast<float>(tank->x.floor_int());
        const float tank_y = static_cast<float>(tank->y.floor_int());

        std::vector<protekt::sim::Order> orders;
        player.think(protekt::sim::observe(sim, protekt::sim::Side::Player), orders);
        ASSERT_EQ(orders.size(), 1U);
        const auto& move = std::get<protekt::sim::orders::MoveTo>(orders.front().payload);
        const float dx = (static_cast<float>(move.x.raw) * kRaw) - tank_x;
        const float dy = (static_cast<float>(move.y.raw) * kRaw) - tank_y;
        const float distance = std::sqrt((dx * dx) + (dy * dy));

        EXPECT_GE(distance, previous - 1e-3F) << "tick " << tick << ": the throttle dropped";
        EXPECT_LE(distance, kLookaheadTiles + 1e-3F)
            << "tick " << tick << ": further ahead than the lookahead, which easing must not allow";
        if (tick == 0) {
            EXPECT_LT(distance, 0.5F * kLookaheadTiles)
                << "the first tick was already near full lookahead: that is a snap, not an ease";
        }
        previous = distance;

        for (const auto& order : orders) {
            sim.enqueue(order);
        }
        sim.step();
    }
    EXPECT_NEAR(player.throttle_rate(), 1.0F, 1e-5F);
    EXPECT_NEAR(previous, kLookaheadTiles, 1e-3F) << "full throttle is still kLookaheadTiles";
}

TEST(PlayerCommander, TheHelmIsClockedByTheSimTickAndNotByTheFrame) {
    // The bug this test exists for: the heading advanced once per CALL, and the frame loop makes
    // the same number of calls whether it then steps the sim once or three times -- so at 120
    // frames per second the tank turned four times as fast as at 30, which is the "aiming is
    // difficult, the turn happens so quickly" report. A frame worth two ticks must land the tank
    // EXACTLY where two frames worth one tick each would.
    auto by_tick = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    auto by_frame = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander ticked(player_unit(by_tick));
    PlayerCommander framed(player_unit(by_frame));
    InputState input;
    input.turn = 1.0F;
    ticked.set_input(input);
    framed.set_input(input);

    // A: a think at ticks 0, 1, 2, 3, stepping one tick at a time.
    for (int frame = 0; frame < 3; ++frame) {
        tick_once(by_tick, ticked);
    }
    std::vector<protekt::sim::Order> ignored;
    ticked.think(protekt::sim::observe(by_tick, protekt::sim::Side::Player), ignored);

    // B: a think at ticks 0, 1 and 3, with the middle frame stepping TWO ticks.
    tick_once(by_frame, framed);
    framed.think(protekt::sim::observe(by_frame, protekt::sim::Side::Player), ignored);
    by_frame.step();
    by_frame.step();
    framed.think(protekt::sim::observe(by_frame, protekt::sim::Side::Player), ignored);

    ASSERT_EQ(by_tick.tick(), by_frame.tick()) << "the two runs must have run the same ticks";
    EXPECT_FLOAT_EQ(framed.heading(), ticked.heading())
        << "the frame rate is reaching the helm: two ticks in one frame did not land where two "
           "single-tick frames did";

    // And a frame in which no tick ran turns nothing at all.
    const float before = framed.heading();
    framed.think(protekt::sim::observe(by_frame, protekt::sim::Side::Player), ignored);
    EXPECT_FLOAT_EQ(framed.heading(), before) << "a frame with no tick in it turned the tank";
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
        // The tick must run as well as the think, or the heading never moves and this loop spins
        // until the guard trips -- which is how it behaved when the helm was clocked per call.
        std::vector<protekt::sim::Order> ignored;
        player.think(protekt::sim::observe(sim, protekt::sim::Side::Player), ignored);
        for (const auto& order : ignored) {
            sim.enqueue(order);
        }
        sim.step();
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
            EXPECT_EQ(order.kind(), protekt::sim::OrderKind::MoveTo)
                << "the player issued an Attack, which outranks auto-engagement and would make "
                   "the tank pursue instead of obey the steering wheel";
            sim.enqueue(order);
        }
        sim.step();
    }
}

/// The BODY turns on A and D: the hull's heading, at the eased rate, in opposite directions, and
/// with the throttle at zero -- a tank turns on the spot.
TEST(PlayerCommanderBody, ATurnsOneWayAndDTheOtherWithNoThrottleAtAll) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander left(player_unit(sim));
    PlayerCommander right(player_unit(sim));
    InputState input;
    input.throttle = 0.0F;
    input.turn = 1.0F;
    left.set_input(input);
    input.turn = -1.0F;
    right.set_input(input);
    for (int tick = 0; tick < kTicksToFullLock; ++tick) {
        tick_once(sim, left);
        std::vector<protekt::sim::Order> ignored;
        right.think(protekt::sim::observe(sim, protekt::sim::Side::Player), ignored);
    }
    EXPECT_GT(left.heading(), 0.1F) << "A did not turn the hull with the throttle at zero";
    EXPECT_LT(right.heading(), -0.1F) << "D did not turn the hull the other way";
    EXPECT_NEAR(left.heading(), -right.heading(), 1e-4F) << "A and D are not equal and opposite";
}

/// The turret is the mouse's: looking around moves the view and leaves the hull exactly where it
/// was, while the tank is being driven.
TEST(PlayerCommanderTurret, TheMouseMovesTheViewAndNotTheHull) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    player.set_heading(0.0F);
    InputState input;
    input.throttle = 1.0F;
    player.set_input(input);
    for (int tick = 0; tick < 5; ++tick) {
        tick_once(sim, player);
    }
    const float hull = player.heading();
    const float view = player.view_yaw();
    // A second of looking East: a quarter turn spread over 30 ticks.
    for (int tick = 0; tick < 30; ++tick) {
        input.look_yaw = -1.5707964F / 30.0F;
        player.set_input(input);
        tick_once(sim, player);
    }
    EXPECT_NE(player.view_yaw(), view) << "the mouse did not move the view";
    EXPECT_EQ(player.heading(), hull) << "looking around turned the hull";
}

/// The turret is bolted to the body: turning the body carries the view round by the same angle and
/// leaves the turret's angle relative to the hull alone.
TEST(PlayerCommanderTurret, TurningTheBodyCarriesTheTurretWithIt) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.look_yaw = 0.7F;
    player.set_input(input);
    tick_once(sim, player);
    const float turret = player.turret_yaw();
    ASSERT_NEAR(turret, 0.7F, 1e-5F);
    const float hull = player.heading();
    const float view = player.view_yaw();

    input = InputState{};
    input.turn = 1.0F;
    player.set_input(input);
    for (int tick = 0; tick < kTicksToFullLock; ++tick) {
        tick_once(sim, player);
    }
    const float hull_moved = protekt::game::Camera::wrap_angle(player.heading() - hull);
    ASSERT_GT(hull_moved, 0.1F);
    EXPECT_NEAR(protekt::game::Camera::wrap_angle(player.view_yaw() - view), hull_moved, 1e-4F)
        << "the view did not move with the body";
    EXPECT_FLOAT_EQ(player.turret_yaw(), turret) << "turning the body moved the turret on it";
}

/// The tank drives where the BODY points: with the throttle held, the mouse does not change the
/// ordered destination, and A/D do.
TEST(PlayerCommanderTurret, TheMouseDoesNotChangeWhereTheTankIsOrdered) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.throttle = 1.0F;
    player.set_input(input);
    for (int tick = 0; tick < 40; ++tick) {
        tick_once(sim, player);
    }
    const auto destination = [&] {
        const auto orders = orders_for(sim, player);
        EXPECT_FALSE(orders.empty());
        const auto& move = std::get<protekt::sim::orders::MoveTo>(orders.front().payload);
        return std::pair{move.x.raw, move.y.raw};
    };
    const auto before = destination();
    input.look_yaw = 1.2F;
    input.look_pitch = 0.3F;
    player.set_input(input);
    EXPECT_EQ(destination(), before) << "the mouse changed where the tank is going";

    input = InputState{};
    input.throttle = 1.0F;
    input.turn = 1.0F;
    player.set_input(input);
    tick_once(sim, player);
    EXPECT_NE(destination(), before) << "A did not change where the tank is going";
}

/// The turret may be turned all the way round, and further: it wraps and never clamps.
TEST(PlayerCommanderTurret, TheTurretWrapsAndNeverClamps) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    const float quarter = 1.5707964F;
    InputState input;
    float last = player.view_yaw();
    for (int step = 1; step <= 8; ++step) {
        input.look_yaw = quarter;
        player.set_input(input);
        tick_once(sim, player);
        EXPECT_NEAR(std::abs(protekt::game::Camera::wrap_angle(player.view_yaw() - last)), quarter,
                    1e-4F)
            << "step " << step << ": the view stopped turning";
        last = player.view_yaw();
        if (step == 2) {
            EXPECT_NEAR(std::abs(player.turret_yaw()), 3.14159265F, 1e-4F)
                << "the turret cannot look straight behind";
        }
    }
    EXPECT_NEAR(player.turret_yaw(), 0.0F, 1e-4F) << "two full circles did not come back to centre";
}

TEST(PlayerCommanderTurret, TheTurretDoesNotDecayBackToCentre) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander player(player_unit(sim));
    InputState input;
    input.look_yaw = 0.9F;
    player.set_input(input);
    tick_once(sim, player);
    for (int tick = 0; tick < 200; ++tick) {
        tick_once(sim, player);
    }
    EXPECT_NEAR(player.turret_yaw(), 0.9F, 1e-5F);
}

/// A destroyed tank's body cannot turn; its turret still looks.
TEST(PlayerCommanderDeath, TheBodyStaysPutAndTheTurretStillLooks) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    PlayerCommander wreck(protekt::sim::UnitId{9999});
    wreck.set_heading(0.4F);
    InputState input;
    input.turn = 1.0F;
    input.look_yaw = 0.3F;
    wreck.set_input(input);
    const float view = wreck.view_yaw();
    for (int tick = 0; tick < kTicksToFullLock; ++tick) {
        std::vector<protekt::sim::Order> out;
        wreck.think(protekt::sim::observe(sim, protekt::sim::Side::Player), out);
        EXPECT_TRUE(out.empty());
        sim.step();
        input.look_yaw = 0.0F;
        wreck.set_input(input);
    }
    EXPECT_FLOAT_EQ(wreck.heading(), protekt::game::Camera::wrap_angle(0.4F))
        << "a wreck's body turned";
    EXPECT_NEAR(protekt::game::Camera::wrap_angle(wreck.view_yaw() - view), 0.3F, 1e-5F)
        << "the wreck's turret did not look";
}

} // namespace
