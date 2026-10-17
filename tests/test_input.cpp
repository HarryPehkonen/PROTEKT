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

TEST(InputFrame, ClearEmptiesEveryPartOfIt) {
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
