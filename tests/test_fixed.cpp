// Fixed point is the floor the whole simulation stands on: if these are wrong, every movement
// test is wrong in a way that only shows up on negative coordinates.
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>

#include "protekt/sim/fixed.hpp"

namespace {

using protekt::sim::Fixed;

TEST(Fixed, FloorIsFloorNotTruncation) {
    EXPECT_EQ(Fixed::from_int(-3).floor_int(), -3);
    EXPECT_EQ(Fixed::from_raw(-1).floor_int(), -1) << "any negative raw value floors to -1";
    EXPECT_EQ(Fixed::from_raw(Fixed::kOne - 1).floor_int(), 0);
    EXPECT_EQ(Fixed::from_raw(-Fixed::kOne).floor_int(), -1);
}

TEST(Fixed, IntRoundTrip) {
    for (int i = -1000; i <= 1000; i += 7) {
        EXPECT_EQ(Fixed::from_int(i).floor_int(), i);
    }
}

TEST(Fixed, MultiplyIsExact) {
    EXPECT_EQ(protekt::sim::fixed_mul(Fixed::from_int(2), Fixed::from_int(3)), Fixed::from_int(6));
    EXPECT_EQ(protekt::sim::fixed_mul(Fixed::from_int(-1), Fixed::from_int(3)),
              Fixed::from_int(-3));
    EXPECT_EQ(
        protekt::sim::fixed_mul(Fixed::from_raw(Fixed::kOne / 2), Fixed::from_raw(Fixed::kOne / 2)),
        Fixed::from_raw(Fixed::kOne / 4));
}

TEST(Fixed, MultiplySaturatesInsteadOfOverflowing) {
    const Fixed big = Fixed::from_int(30000);
    const Fixed product = protekt::sim::fixed_mul(big, big);
    EXPECT_LE(product.raw, Fixed::kMaxRaw) << "overflow must saturate, never wrap";
    EXPECT_GT(product.raw, 0);
    EXPECT_EQ(protekt::sim::fixed_mul(Fixed::max(), Fixed::max()), Fixed::max());
}

TEST(Fixed, DivideByZeroIsDefined) {
    // A total function: the fuzzer is allowed to hand this anything.
    EXPECT_EQ(protekt::sim::fixed_div(Fixed::from_int(1), Fixed::zero()), Fixed::max());
    EXPECT_EQ(protekt::sim::fixed_div(Fixed::from_int(1), Fixed::from_int(4)),
              Fixed::from_raw(Fixed::kOne / 4));
    EXPECT_EQ(protekt::sim::fixed_div(Fixed::from_int(-6), Fixed::from_int(3)),
              Fixed::from_int(-2));
}

TEST(Fixed, DivideAndMultiplyAgree) {
    for (int i = -100; i <= 100; ++i) {
        const Fixed x = Fixed::from_int(i);
        const Fixed back = protekt::sim::fixed_mul(protekt::sim::fixed_div(x, Fixed::from_int(3)),
                                                   Fixed::from_int(3));
        EXPECT_LE(std::abs(back.raw - x.raw), 4) << "i=" << i;
    }
}

TEST(Fixed, AbsSaturatesAtTheMinimum) {
    EXPECT_EQ(protekt::sim::fixed_abs(Fixed::from_int(-5)), Fixed::from_int(5));
    EXPECT_EQ(protekt::sim::fixed_abs(Fixed::from_raw(Fixed::kMinRaw)), Fixed::max());
}

TEST(Fixed, AddAndSubSaturate) {
    EXPECT_EQ(protekt::sim::fixed_add(Fixed::max(), Fixed::from_int(1)), Fixed::max());
    EXPECT_EQ(protekt::sim::fixed_sub(Fixed::from_raw(Fixed::kMinRaw), Fixed::from_int(1)),
              Fixed::from_raw(Fixed::kMinRaw));
    EXPECT_EQ(protekt::sim::fixed_add(Fixed::from_int(2), Fixed::from_int(-5)),
              Fixed::from_int(-3));
}

} // namespace
