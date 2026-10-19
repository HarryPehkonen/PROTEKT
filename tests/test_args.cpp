// The one number parser all three command lines share. It is strict: the whole argument must be
// a number, and a value that does not fit the type is refused rather than wrapped or clamped.
#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include "protekt/app/args.hpp"

namespace {

using protekt::app::parse_number;

TEST(Args, ParsesAWholeNumber) {
    std::uint64_t value = 0;
    EXPECT_TRUE(parse_number(std::string_view{"42"}, value));
    EXPECT_EQ(value, 42U);
}

TEST(Args, RejectsEmpty) {
    std::uint64_t value = 7;
    EXPECT_FALSE(parse_number(std::string_view{""}, value));
    EXPECT_EQ(value, 7U) << "a refused value must leave the destination untouched";
}

TEST(Args, RejectsTrailingGarbage) {
    std::uint64_t value = 7;
    EXPECT_FALSE(parse_number(std::string_view{"42x"}, value));
    EXPECT_EQ(value, 7U);
}

TEST(Args, RejectsOverflow) {
    std::uint8_t value = 7;
    EXPECT_FALSE(parse_number(std::string_view{"256"}, value));
    EXPECT_EQ(value, 7U);
    EXPECT_TRUE(parse_number(std::string_view{"255"}, value));
    EXPECT_EQ(value, 255U);
}

TEST(Args, AcceptsANegativeForASignedType) {
    int value = 0;
    EXPECT_TRUE(parse_number(std::string_view{"-7"}, value));
    EXPECT_EQ(value, -7);
}

TEST(Args, RejectsANegativeForAnUnsignedType) {
    std::uint64_t value = 7;
    EXPECT_FALSE(parse_number(std::string_view{"-1"}, value));
    EXPECT_EQ(value, 7U);
}

} // namespace
