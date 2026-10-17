// The version test pins the shape, not the number: duplicating "0.1.0" here would be a
// second place to forget when bumping (CMakeLists.txt is the single source).
#include <gtest/gtest.h>

#include <string_view>

#include "protekt/version.hpp"

namespace {

bool is_semver(std::string_view text) {
    int dots = 0;
    bool digit_since_dot = false;
    for (const char c : text) {
        if (c == '.') {
            if (!digit_since_dot) {
                return false;
            }
            ++dots;
            digit_since_dot = false;
        } else if (c >= '0' && c <= '9') {
            digit_since_dot = true;
        } else {
            return false;
        }
    }
    return dots == 2 && digit_since_dot;
}

TEST(Version, IsGeneratedAndSemver) {
    EXPECT_FALSE(protekt::version().empty());
    EXPECT_TRUE(is_semver(protekt::version())) << "got: " << protekt::version();
}

} // namespace
