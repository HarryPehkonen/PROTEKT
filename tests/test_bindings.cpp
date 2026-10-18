#include "protekt/input/bindings.hpp"

#include <gtest/gtest.h>

namespace protekt::input {
namespace {

TEST(HeldKeys, StartsWithNothingHeld) {
    const HeldKeys keys{};
    EXPECT_FALSE(keys.any());
}

TEST(HeldKeys, WMeansForwardAndReleasingItMeansNot) {
    HeldKeys keys{};
    apply(keys, KeyEvent{kKeyW, true});
    EXPECT_TRUE(keys.forward);
    EXPECT_TRUE(keys.any());
    apply(keys, KeyEvent{kKeyW, false});
    EXPECT_FALSE(keys.forward);
    EXPECT_FALSE(keys.any());
}

TEST(HeldKeys, EachMovementKeyIsSeparate) {
    HeldKeys keys{};
    apply(keys, KeyEvent{kKeyW, true});
    apply(keys, KeyEvent{kKeyS, true});
    apply(keys, KeyEvent{kKeyA, true});
    apply(keys, KeyEvent{kKeyD, true});
    EXPECT_TRUE(keys.forward);
    EXPECT_TRUE(keys.back);
    EXPECT_TRUE(keys.left);
    EXPECT_TRUE(keys.right);

    apply(keys, KeyEvent{kKeyA, false});
    EXPECT_FALSE(keys.left) << "releasing one key must not release the others";
    EXPECT_TRUE(keys.forward);
}

TEST(HeldKeys, RepeatingAKeyDownChangesNothing) {
    // The OS repeats key-down events while a key is held. A tank must not accelerate on each one.
    HeldKeys keys{};
    for (int i = 0; i < 20; ++i) {
        apply(keys, KeyEvent{kKeyW, true});
    }
    EXPECT_TRUE(keys.forward);
    for (int i = 0; i < 20; ++i) {
        apply(keys, KeyEvent{kKeyW, false});
    }
    EXPECT_FALSE(keys.forward) << "and a single release is enough";
}

TEST(HeldKeys, KeysTheGameDoesNotUseAreIgnored) {
    HeldKeys keys{};
    apply(keys, KeyEvent{kKeyTab, true});
    apply(keys, KeyEvent{kKeyEscape, false});
    apply(keys, KeyEvent{999, true});
    EXPECT_FALSE(keys.any());
}

// The keys that are commands, not states: press one and something happens once.

TEST(CommandKeys, CtrlQAsksForQuit) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{kKeyQ, true, false, true, false});
    EXPECT_TRUE(quit_requested(frame));
}

TEST(CommandKeys, ABareQDoesNotQuit) {
    // A slipped hand must not end the game: the modifier is the whole safety of this key.
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{kKeyQ, true});
    EXPECT_FALSE(quit_requested(frame));

    InputFrame shifted{};
    shifted.keys.push_back(KeyEvent{kKeyQ, true, true, false, false});
    EXPECT_FALSE(quit_requested(shifted));
}

TEST(CommandKeys, ReleasingQDoesNotQuit) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{kKeyQ, false, false, true, false});
    EXPECT_FALSE(quit_requested(frame));
}

TEST(CommandKeys, F11AndAltEnterBothAskForFullScreen) {
    InputFrame f11{};
    f11.keys.push_back(KeyEvent{kKeyF11, true});
    EXPECT_TRUE(fullscreen_requested(f11));

    InputFrame alt_enter{};
    alt_enter.keys.push_back(KeyEvent{kKeyEnter, true, false, false, true});
    EXPECT_TRUE(fullscreen_requested(alt_enter));
}

TEST(CommandKeys, EnterAloneIsNotFullScreen) {
    // Enter is a common key for other things, so it only asks when Alt is held with it.
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{kKeyEnter, true});
    EXPECT_FALSE(fullscreen_requested(frame));

    InputFrame released{};
    released.keys.push_back(KeyEvent{kKeyF11, false});
    EXPECT_FALSE(fullscreen_requested(released));
}

TEST(CommandKeys, NothingPressedAsksForNothing) {
    const InputFrame empty{};
    EXPECT_FALSE(quit_requested(empty));
    EXPECT_FALSE(fullscreen_requested(empty));
}

} // namespace
} // namespace protekt::input
