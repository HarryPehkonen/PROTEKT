#include "protekt/input/input.hpp"

#include <gtest/gtest.h>

namespace protekt::input {
namespace {

TEST(InputFrame, StartsEmpty) {
    const InputFrame frame{};
    EXPECT_TRUE(frame.clicks.empty());
    EXPECT_TRUE(frame.keys.empty());
    EXPECT_FLOAT_EQ(frame.mouse_dx, 0.0F);
    EXPECT_FLOAT_EQ(frame.mouse_dy, 0.0F);
    EXPECT_FALSE(frame.quit);
}

TEST(InputFrame, ClearEmptiesTheEvents) {
    // The contract the game loop relies on: pump the queue, handle the frame, and the next pump
    // starts from nothing -- so no click is handled twice and no key repeats forever.
    InputFrame frame{};
    frame.clicks.push_back(Click{100, 200, MouseButton::Left, true});
    frame.clicks.push_back(Click{300, 400, MouseButton::Right, false});
    frame.keys.push_back(KeyEvent{42, true});
    frame.mouse_dx = 12.5F;
    frame.mouse_dy = -3.0F;
    frame.quit = true;

    frame.clear();

    EXPECT_TRUE(frame.clicks.empty());
    EXPECT_TRUE(frame.keys.empty());
    EXPECT_FLOAT_EQ(frame.mouse_dx, 0.0F);
    EXPECT_FLOAT_EQ(frame.mouse_dy, 0.0F);
    EXPECT_FALSE(frame.quit);
}

// N17: the window may deliver a flick of the mouse as many SDL motion events inside one frame, and
// the game spends the frame's total once. The sum is the window's job, so it lives HERE, where a
// test can reach it without a display: `render/window.cpp` calls `add_motion` for every event.
TEST(InputFrame, MotionArrivingInSeveralEventsIsTheSameTurnAsOneEvent) {
    InputFrame in_pieces{};
    for (int i = 0; i < 5; ++i) {
        in_pieces.add_motion(8.0F, -2.0F);
    }
    InputFrame at_once{};
    at_once.add_motion(40.0F, -10.0F);
    EXPECT_FLOAT_EQ(in_pieces.mouse_dx, at_once.mouse_dx);
    EXPECT_FLOAT_EQ(in_pieces.mouse_dy, at_once.mouse_dy);
    EXPECT_FLOAT_EQ(in_pieces.mouse_dx, 40.0F);
}

TEST(InputFrame, MotionIsSpentOnceAndTheNextFrameStartsFromNothing) {
    InputFrame frame{};
    frame.add_motion(30.0F, 4.0F);
    frame.clear();
    frame.add_motion(-5.0F, 1.0F);
    EXPECT_FLOAT_EQ(frame.mouse_dx, -5.0F) << "last frame's motion must not be counted twice";
    EXPECT_FLOAT_EQ(frame.mouse_dy, 1.0F);
}

TEST(InputFrame, ClearKeepsThePointerWhereItIs) {
    // The pointer is a POSITION, not an event: a frame in which the mouse did not move must not
    // forget where it is, or the reticle would blink off every time the player held still. That is
    // also why a run with no pointer at all -- headless, or --autodrive -- never claims to have
    // one.
    InputFrame frame{};
    EXPECT_FALSE(frame.cursor_known);

    frame.cursor_x = 640;
    frame.cursor_y = 360;
    frame.cursor_known = true;
    frame.clear();

    EXPECT_TRUE(frame.cursor_known);
    EXPECT_EQ(frame.cursor_x, 640);
    EXPECT_EQ(frame.cursor_y, 360);
}

TEST(InputFrame, AClearFrameCanBeFilledAgain) {
    InputFrame frame{};
    frame.clicks.push_back(Click{1, 2, MouseButton::Middle, true});
    frame.clear();
    frame.clicks.push_back(Click{3, 4, MouseButton::Left, true});
    frame.keys.push_back(KeyEvent{7, false});
    ASSERT_EQ(frame.clicks.size(), 1U);
    EXPECT_EQ(frame.clicks.front().x, 3);
    EXPECT_EQ(frame.clicks.front().button, MouseButton::Left);
    ASSERT_EQ(frame.keys.size(), 1U);
    EXPECT_FALSE(frame.keys.front().pressed);
}

} // namespace
} // namespace protekt::input
