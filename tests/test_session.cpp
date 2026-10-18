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

/// Screen position of a world point, through the SAME matrix the renderer uses. `x < 0.5` means the
/// left half of the frame. This is how a test can say "to the left" without trusting a sign
/// convention: it asks the projection.
struct ScreenPoint {
    float x{0.0F};
    float y{0.0F};
    float depth{0.0F};
};

[[nodiscard]] ScreenPoint project(const Camera& camera, float extent_width, float extent_height,
                                  const std::array<float, 3>& world) {
    const std::array<float, 16> m = view_projection(camera, extent_width / extent_height);
    const std::array<float, 4> p{world[0], world[1], world[2], 1.0F};
    const float clip_x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12] * p[3];
    const float clip_y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13] * p[3];
    const float clip_w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15] * p[3];
    ScreenPoint out;
    if (clip_w == 0.0F) {
        return out;
    }
    out.x = (clip_x / clip_w) * 0.5F + 0.5F;
    out.y = (clip_y / clip_w) * 0.5F + 0.5F;
    out.depth = clip_w;
    return out;
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

/// A click with the MARK on `unit`: the mark selects, not the click's own pixel (see game/aim.hpp).
[[nodiscard]] Click click_on(Rig& rig, sim::UnitId unit, MouseButton button, bool shift = false) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
    return Click{static_cast<int>(p.x), static_cast<int>(p.y), button, true, shift};
}

/// The mark on the ground well below every unit, and a right click there.
[[nodiscard]] Click ground_click(Rig& rig) {
    rig.options.aim = Aim{640.0F, 700.0F};
    return Click{640, 700, MouseButton::Right, true, false};
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
    EXPECT_EQ(orders.front().kind(), sim::OrderKind::MoveTo);

    std::vector<Order> idle;
    session.frame(InputFrame{}, rig.sim, rig.options, idle);
    EXPECT_EQ(idle.size(), 1U) << "a tank with no throttle still reports where it means to be";
}

// ---------------------------------------------------------------- commanding

TEST(Session, AClickSelectsAndARightClickOrdersAnotherTank) {
    Rig rig = make_rig();
    Session session{rig.tank_a};
    // One frame at a time, because the MARK moves between the two clicks and moving it is what
    // building the click does: building both frames up front would leave both of them aimed at
    // wherever the mark ended up last.
    run_frames(rig, session, {with_click(click_on(rig, rig.tank_b, MouseButton::Left))});
    run_frames(rig, session, {with_click(ground_click(rig))});

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
    session.frame(with_click(ground_click(rig)), rig.sim, rig.options, orders);

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

TEST(Session, AKeyTurnsTheViewToScreenLeft) {
    // The report: "A and D seem to be reversed -- I would expect A to turn me to the left."
    // Measured through the projection rather than argued about: hold A, and a point that was dead
    // ahead must end up drawn on the RIGHT half, because the view swung left under it.
    Rig rig = make_rig();
    Session session(rig.tank_a);
    const std::uint32_t start_tick = rig.sim.tick();

    Camera camera{};
    float ahead_x = 0.0F;
    float ahead_z = 0.0F;
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.id == rig.tank_a) {
            const float metres = static_cast<float>(unit.x.raw) / 65536.0F * kMetresPerTile;
            const float metres_y = static_cast<float>(unit.y.raw) / 65536.0F * kMetresPerTile;
            camera.x = metres;
            camera.z = metres_y;
            camera.y
                = height_metres(rig.options.seed, unit.x.floor_int(), unit.y.floor_int()) + 3.0F;
            // Dead ahead at the START heading (yaw 0 looks down +Z in scene coordinates).
            ahead_x = metres;
            ahead_z = metres_y + 8.0F * kMetresPerTile;
        }
    }

    const ScreenPoint before = project(camera, 1280.0F, 720.0F, {ahead_x, camera.y, ahead_z});
    ASSERT_GT(before.depth, 0.0F) << "the reference point must be in front of the camera";

    InputFrame hold_a;
    hold_a.keys.push_back(KeyEvent{input::kKeyA, true, false});
    for (int frame = 0; frame < 30; ++frame) {
        run_frames(rig, session, {hold_a});
    }
    ASSERT_GT(rig.sim.tick(), start_tick) << "the frames must have advanced the world";

    camera.yaw = session.heading();
    const ScreenPoint after = project(camera, 1280.0F, 720.0F, {ahead_x, camera.y, ahead_z});
    EXPECT_GT(after.x, 0.5F)
        << "A must turn the view to screen-left, so a point dead ahead swings RIGHT: x=" << after.x
        << " heading=" << session.heading();
}

TEST(Session, ADestroyedTankSaysSoAndStopsTakingInput) {
    // The report: "after a few seconds I could not move forwards or backwards, only turn around."
    // The hull was gone and the heading kept turning, which reads as a game that has half stopped.
    Rig rig = make_rig();
    Session session(rig.tank_a);
    EXPECT_FALSE(session.destroyed());

    // A unit id that is not in the world stands in for the destroyed hull: the session's rule is
    // "the unit I drive is not there any more", which is exactly what death leaves behind.
    Session gone(sim::UnitId{9999});
    std::vector<Order> orders;
    InputFrame hold_w;
    hold_w.keys.push_back(KeyEvent{input::kKeyW, true, false});
    gone.frame(hold_w, rig.sim, rig.options, orders);
    EXPECT_TRUE(gone.destroyed());
    EXPECT_TRUE(orders.empty()) << "a destroyed tank must not be given orders";
    EXPECT_FALSE(gone.message().empty()) << "and the player must be told, not left guessing";
}

} // namespace protekt::game
