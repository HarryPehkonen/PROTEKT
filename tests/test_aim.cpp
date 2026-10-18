// Where the gun is pointed.
//
// The tank's gun is on its centre line and the player turns with A/D, so the middle of the screen
// is the only place a shot can come from; the mouse adds elevation within that. These tests pin the
// rule as a pure function of the frame and the extent, so it needs no window and no world.
#include <gtest/gtest.h>

#include <optional>

#include "protekt/game/aim.hpp"
#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"

namespace {

using protekt::game::Aim;
using protekt::game::aim_from;
using protekt::input::InputFrame;
using protekt::render::Extent;

constexpr Extent kExtent{1280, 720};

InputFrame pointer_at(int x, int y) {
    InputFrame frame{};
    frame.cursor_x = x;
    frame.cursor_y = y;
    frame.cursor_known = true;
    return frame;
}

/// The mark the rule produces, or a default after failing the test.
///
/// Deliberately not `EXPECT_TRUE(aim.has_value()); return *aim`: clang-tidy cannot see through
/// gtest's assertion macros, so it reads that dereference as unchecked access and fails the gate.
/// `value_or` has no dereference to misread, and the failure is already recorded.
Aim mark_for(int x, int y, Extent extent = kExtent) {
    const std::optional<Aim> aim = aim_from(pointer_at(x, y), extent);
    EXPECT_TRUE(aim.has_value()) << "a known pointer must produce a mark";
    return aim.value_or(Aim{});
}

TEST(AimPoint, IsHorizontallyTheMiddleOfTheScreenWhateverTheMouseDoes) {
    // The mark cannot be dragged sideways: turning the tank is what moves it across the world, and
    // a mark that could slide to the edge would promise a shot the middle of the screen cannot
    // take.
    for (const int x : {0, 1, 100, 639, 640, 641, 1200, 1279}) {
        EXPECT_FLOAT_EQ(mark_for(x, 360).x, 640.0F) << "mouse at x=" << x;
    }
}

TEST(AimPoint, FollowsTheMouseUpAndDown) {
    for (const int y : {0, 120, 360, 719}) {
        EXPECT_FLOAT_EQ(mark_for(640, y).y, static_cast<float>(y)) << "mouse at y=" << y;
    }
}

TEST(AimPoint, CannotLeaveTheFrame) {
    // A click cannot land outside the frame, so the mark must not pretend that it can: a mouse
    // event from before a resize can carry a height the frame no longer has.
    EXPECT_FLOAT_EQ(mark_for(640, -50).y, 0.0F);
    EXPECT_FLOAT_EQ(mark_for(640, 5000).y, 719.0F);
}

TEST(AimPoint, NoPointerMeansNoMark) {
    // A headless run and a scripted one never see a pointer, and must therefore draw no mark and
    // pick nothing -- which is what leaves their frames identical to before there was a mark at
    // all.
    const InputFrame none{};
    EXPECT_FALSE(aim_from(none, kExtent).has_value());

    InputFrame stale = pointer_at(640, 360);
    stale.cursor_known = false;
    EXPECT_FALSE(aim_from(stale, kExtent).has_value());
}

TEST(AimPoint, ADegenerateFrameHasNoMark) {
    EXPECT_FALSE(aim_from(pointer_at(640, 360), Extent{0, 0}).has_value());
    EXPECT_FALSE(aim_from(pointer_at(640, 360), Extent{1280, 0}).has_value());
}

} // namespace
