// Pins the RNG to its specification. The expected values were computed from the algorithm
// written in rng.hpp by an independent implementation (Python), not from this C++.
#include <gtest/gtest.h>

#include <array>

#include "protekt/rng.hpp"

namespace {

using protekt::rng::SplitMix64;

TEST(Rng, KnownAnswersSeedZero) {
    SplitMix64 rng(0);
    EXPECT_EQ(rng.next(), 0xE220A8397B1DCDAFULL);
    EXPECT_EQ(rng.next(), 0x6E789E6AA1B965F4ULL);
    EXPECT_EQ(rng.next(), 0x06C45D188009454FULL);
    EXPECT_EQ(rng.next(), 0xF88BB8A8724C81ECULL);
}

TEST(Rng, KnownAnswersSeedDeadBeef) {
    SplitMix64 rng(0xDEADBEEFULL);
    EXPECT_EQ(rng.next(), 0x4ADFB90F68C9EB9BULL);
    EXPECT_EQ(rng.next(), 0xDE586A3141A10922ULL);
    EXPECT_EQ(rng.next(), 0x021FBC2F8E1CFC1DULL);
    EXPECT_EQ(rng.next(), 0x7466CE737BE16790ULL);
}

TEST(Rng, BoundedStaysInRangeAndMatchesSpec) {
    EXPECT_EQ(SplitMix64(0).bounded(1), 0U);
    SplitMix64 rng(0);
    const std::array<std::uint32_t, 8> expected{33, 634, 184, 344, 562, 452, 798, 226};
    for (const std::uint32_t want : expected) {
        EXPECT_EQ(rng.bounded(1000), want);
    }
    for (std::uint32_t step = 0; step < 200; ++step) {
        EXPECT_LT(rng.bounded(97), 97U);
    }
}

TEST(Rng, StreamSeedsAreDistinctAndStable) {
    EXPECT_EQ(protekt::rng::stream_seed(0, 0), 0XE220A8397B1DCDAFULL);
    EXPECT_NE(protekt::rng::stream_seed(0, 0), protekt::rng::stream_seed(0, 1));
    EXPECT_NE(protekt::rng::stream_seed(0, 1), protekt::rng::stream_seed(1, 0));
    EXPECT_EQ(protekt::rng::stream_seed(42, 7), protekt::rng::stream_seed(42, 7));
}

} // namespace
