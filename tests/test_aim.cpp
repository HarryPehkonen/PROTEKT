// Where the gun is pointed.
//
// The pointer is captured and the player aims by looking, so the mark is the middle of the frame.
// These tests pin the rule as a pure function of the frame and the extent, so it needs no window
// and no world.
#include <gtest/gtest.h>

#include <optional>

#include "protekt/game/aim.hpp"
#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"

namespace {

using protekt::game::Aim;
using protekt::game::aim_from;
using protekt::game::Extent;
using protekt::input::InputFrame;

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

TEST(AimPoint, IsTheCentreOfTheFrameInBothAxesWhateverThePointerSays) {
    // The pointer is captured for look and the player aims by looking, so the mark is the middle
    // of the frame and the cursor fields mean nothing to it.
    for (const int x : {0, 100, 640, 1279}) {
        for (const int y : {0, 360, 719}) {
            const Aim aim = mark_for(x, y);
            EXPECT_FLOAT_EQ(aim.x, 640.0F) << "pointer at " << x << "," << y;
            EXPECT_FLOAT_EQ(aim.y, 360.0F) << "pointer at " << x << "," << y;
        }
    }
}

TEST(AimPoint, DoesNotNeedAKnownPointer) {
    const InputFrame none{};
    const std::optional<Aim> aim = aim_from(none, kExtent);
    ASSERT_TRUE(aim.has_value());
    EXPECT_FLOAT_EQ(aim.value_or(Aim{}).x, 640.0F);
    EXPECT_FLOAT_EQ(aim.value_or(Aim{}).y, 360.0F);
}

TEST(AimPoint, FollowsTheFrameSize) {
    const Aim aim = mark_for(0, 0, Extent{800, 600});
    EXPECT_FLOAT_EQ(aim.x, 400.0F);
    EXPECT_FLOAT_EQ(aim.y, 300.0F);
}

TEST(AimPoint, ADegenerateFrameHasNoMark) {
    EXPECT_FALSE(aim_from(pointer_at(640, 360), Extent{0, 0}).has_value());
    EXPECT_FALSE(aim_from(pointer_at(640, 360), Extent{1280, 0}).has_value());
}

} // namespace
