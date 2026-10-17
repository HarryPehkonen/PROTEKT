#include "protekt/game/session.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::Click;
using input::InputFrame;
using input::KeyEvent;
using input::MouseButton;
using sim::Order;
using sim::Scenario;
using sim::Side;
using sim::Spawn;
using sim::Stance;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr render::Extent kExtent{1280, 720};

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId tank_a{};
    sim::UnitId tank_b{};
};

[[nodiscard]] Rig make_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(Scenario{kSeed,
                                        {{UnitKind::Tank, Side::Player, 40, 40},
                                         {UnitKind::Tank, Side::Player, 42, 40},
                                         {UnitKind::Tank, Side::Enemy, 40, 60}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.side == Side::Player) {
            rig.tank_b = unit.id;
        }
    }
    const float ground = height_metres(kSeed, 40, 40);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, ground + 3.0F, 60.0F, 0.0F, -0.12F};
    return rig;
}

/// One frame of the game loop, exactly as `app/game.cpp` runs it: input in, orders out, tick, and
/// the camera re-derived from where the tank ended up.
void run_frames(Rig& rig, Session& session, const std::vector<InputFrame>& frames,
                std::uint32_t ticks_per_frame = 1) {
    for (const InputFrame& frame : frames) {
        std::vector<Order> orders;
        session.frame(frame, rig.sim, rig.options, orders);
        for (const Order& order : orders) {
            rig.sim.enqueue(order);
        }
        for (std::uint32_t t = 0; t < ticks_per_frame; ++t) {
            rig.sim.step();
        }
        rig.options.camera = Camera{rig.options.camera.x, rig.options.camera.y,
                                    rig.options.camera.z, session.heading(), -0.12F};
        const sim::Unit* self = rig.sim.find(rig.tank_a);
        rig.options.camera.x = static_cast<float>(self->x.raw) / 65536.0F * kMetresPerTile;
        rig.options.camera.z
            = (static_cast<float>(self->y.raw) / 65536.0F * kMetresPerTile) - 20.0F;
    }
}

[[nodiscard]] InputFrame holding(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

[[nodiscard]] InputFrame releasing(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, false, false});
    return frame;
}

[[nodiscard]] Click click_on(const Rig& rig, sim::UnitId unit, MouseButton button,
                             bool shift = false) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    return Click{static_cast<int>(p.x), static_cast<int>(p.y), button, true, shift};
}

[[nodiscard]] InputFrame with_click(const Click& click) {
    InputFrame frame{};
    frame.clicks.push_back(click);
    return frame;
}

} // namespace

// ---------------------------------------------------------------- driving
//
// These are the tests that were missing when the window opened onto a tank that never moved: the
// only thing between a keystroke and the simulation is this class, and now it is proved, headless.

TEST(Session, WDrivesTheTankForward) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    const int start_y = rig.sim.find(rig.tank_a)->y.floor_int();

    std::vector<InputFrame> frames;
    frames.push_back(holding(input::kKeyW));
    for (int i = 1; i < 60; ++i) {
        frames.push_back(InputFrame{});
    }
    run_frames(rig, session, frames);

    const int end_y = rig.sim.find(rig.tank_a)->y.floor_int();
    EXPECT_GT(end_y, start_y + 2) << "sixty ticks with W held moved the tank from y=" << start_y
                                  << " to y=" << end_y;
}

TEST(Session, AtTheRealSpawnHoldingWActuallyMovesTheTank) {
    // The bug this milestone exists for, in the scenario that HAS the bug: at seed 42 the player's
    // tank spawns facing four crater tiles. Before the spawn heading was chosen from the terrain,
    // holding W here moved the tank one tile and stopped against the wall.
    auto sim = protekt::sim::Sim::create(sim::skirmish(kSeed));
    sim::UnitId tank{};
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == Side::Player && unit.kind == UnitKind::Tank) {
            tank = unit.id;
            break;
        }
    }
    ASSERT_TRUE(tank.valid());
    SceneOptions options{};
    options.seed = kSeed;
    options.extent = kExtent;
    options.camera = Camera{0.0F, 10.0F, 0.0F, 0.0F, -0.12F};

    Session session{tank};
    const int start_x = sim.find(tank)->x.floor_int();
    const int start_y = sim.find(tank)->y.floor_int();
    std::vector<InputFrame> frames;
    frames.push_back(holding(input::kKeyW));
    for (int i = 1; i < 60; ++i) {
        frames.push_back(InputFrame{});
    }
    for (const InputFrame& frame : frames) {
        std::vector<Order> orders;
        session.frame(frame, sim, options, orders);
        for (const Order& order : orders) {
            sim.enqueue(order);
        }
        sim.step();
    }
    const int dx = sim.find(tank)->x.floor_int() - start_x;
    const int dy = sim.find(tank)->y.floor_int() - start_y;
    EXPECT_GT((dx * dx) + (dy * dy), 9)
        << "sixty ticks of W at the real spawn moved the tank (" << dx << ", " << dy
        << ") tiles: it must not be staring at a wall";
}

TEST(Session, ReleasingWStopsTheTank) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    std::vector<InputFrame> frames;
    frames.push_back(holding(input::kKeyW));
    for (int i = 1; i < 40; ++i) {
        frames.push_back(InputFrame{});
    }
    frames.push_back(releasing(input::kKeyW));
    for (int i = 0; i < 40; ++i) {
        frames.push_back(InputFrame{});
    }
    run_frames(rig, session, frames);

    const sim::Unit* tank = rig.sim.find(rig.tank_a);
    EXPECT_EQ(tank->vx.raw, 0) << "a tank with no throttle must not still be rolling";
    EXPECT_EQ(tank->vy.raw, 0);
}

TEST(Session, AAndDTurnTheTankOppositeWays) {
    Rig left = make_rig();
    Session left_session{left.tank_a};
    std::vector<InputFrame> frames{holding(input::kKeyA)};
    run_frames(left, left_session, frames);
    const float left_heading = left_session.heading();

    Rig right = make_rig();
    Session right_session{right.tank_a};
    std::vector<InputFrame> frames_right{holding(input::kKeyD)};
    run_frames(right, right_session, frames_right);
    const float right_heading = right_session.heading();

    EXPECT_NE(left_heading, right_heading);
    EXPECT_NE(left_heading, 0.0F);
    EXPECT_NE(right_heading, 0.0F);
}

TEST(Session, TheDrivenTankGetsOneOrderPerFrameWhileDriving) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    std::vector<Order> orders;
    session.frame(holding(input::kKeyW), rig.sim, rig.options, orders);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().unit, rig.tank_a);
    EXPECT_EQ(orders.front().kind, sim::OrderKind::MoveTo);

    std::vector<Order> idle;
    session.frame(InputFrame{}, rig.sim, rig.options, idle);
    EXPECT_EQ(idle.size(), 1U) << "a tank with no throttle still reports where it means to be";
}

// ---------------------------------------------------------------- commanding

TEST(Session, AClickSelectsAndARightClickOrdersAnotherTank) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    std::vector<InputFrame> frames;
    frames.push_back(with_click(click_on(rig, rig.tank_b, MouseButton::Left)));
    frames.push_back(with_click(Click{640, 700, MouseButton::Right, true, false}));
    run_frames(rig, session, frames);

    EXPECT_EQ(rig.sim.find(rig.tank_b)->stance, Stance::MoveTo)
        << "the commanded tank should be driving to the tile that was clicked";
}

TEST(Session, TheDrivenTankIsNeverGivenAClickOrder) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    // Select the driven tank, then right click the ground. The only order for it must be the one
    // the throttle produces -- the click has to be refused, not quietly obeyed.
    std::vector<Order> orders;
    session.frame(with_click(click_on(rig, rig.tank_a, MouseButton::Left)), rig.sim, rig.options,
                  orders);
    orders.clear();
    session.frame(with_click(Click{640, 700, MouseButton::Right, true, false}), rig.sim,
                  rig.options, orders);

    int for_driven = 0;
    for (const Order& order : orders) {
        if (order.unit == rig.tank_a) {
            ++for_driven;
        }
    }
    EXPECT_EQ(for_driven, 1) << "one order: the throttle's. The click must not add a second";
    EXPECT_FALSE(session.message().empty()) << "and the player must be told why";
}

TEST(Session, EscapeClearsTheSelection) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    std::vector<InputFrame> frames;
    frames.push_back(with_click(click_on(rig, rig.tank_b, MouseButton::Left)));
    InputFrame escape{};
    escape.keys.push_back(KeyEvent{input::kKeyEscape, true, false});
    frames.push_back(escape);
    run_frames(rig, session, frames);
    EXPECT_TRUE(session.selection().empty());
}

TEST(Session, TheCameraFollowsTheTankSoTheWorldMovesUnderIt) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    const float start_z = rig.options.camera.z;
    std::vector<InputFrame> frames;
    frames.push_back(holding(input::kKeyW));
    for (int i = 1; i < 45; ++i) {
        frames.push_back(InputFrame{});
    }
    run_frames(rig, session, frames);
    EXPECT_GT(rig.options.camera.z, start_z) << "the eye must travel with the hull";
}

} // namespace protekt::game
