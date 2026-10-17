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

} // namespace
} // namespace protekt::input
