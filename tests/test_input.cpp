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
