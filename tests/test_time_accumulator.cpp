// The play-mode clock: wall time in, whole sim ticks out. The catch-up cap is a real behaviour --
// a stall must not turn into a burst of ticks that teleports the tank -- so it is pinned here.
#include <gtest/gtest.h>

#include <chrono>

#include "protekt/app/time_accumulator.hpp"

namespace {

using protekt::app::TimeAccumulator;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using Clock = std::chrono::steady_clock;

constexpr nanoseconds kPeriod{1'000'000'000LL / 30};
constexpr nanoseconds kCap{250'000'000LL};

TEST(TimeAccumulator, NoTimeNoSteps) {
    const Clock::time_point start{};
    TimeAccumulator clock{kPeriod, kCap, start};
    EXPECT_EQ(clock.steps_at(start), 0U);
}

TEST(TimeAccumulator, OnePeriodIsOneStep) {
    const Clock::time_point start{};
    TimeAccumulator clock{kPeriod, kCap, start};
    EXPECT_EQ(clock.steps_at(start + kPeriod), 1U);
}

TEST(TimeAccumulator, TheRemainderIsCarriedNotDropped) {
    const Clock::time_point start{};
    TimeAccumulator clock{kPeriod, kCap, start};
    // 20 ms frames against a 33.3 ms tick: 0, 1, 1, 0, 1, 1 ... -- 6 frames is 120 ms = 3 ticks.
    std::uint32_t total = 0;
    for (int frame = 1; frame <= 6; ++frame) {
        total += clock.steps_at(start + milliseconds{20 * frame});
    }
    EXPECT_EQ(total, 3U);
}

TEST(TimeAccumulator, AStallIsCappedAtTheCatchUpLimit) {
    const Clock::time_point start{};
    TimeAccumulator clock{kPeriod, kCap, start};
    // Ten seconds of nothing: 250 ms is 7 whole 33.3 ms periods, not 300 ticks.
    EXPECT_EQ(clock.steps_at(start + std::chrono::seconds{10}), 7U);
}

TEST(TimeAccumulator, TheCapAppliesPerCallNotInTotal) {
    const Clock::time_point start{};
    TimeAccumulator clock{kPeriod, kCap, start};
    EXPECT_EQ(clock.steps_at(start + std::chrono::seconds{10}), 7U);
    // The leftover of the capped call (250 ms - 7 periods = 16.7 ms) is carried: with one more
    // period of wall time that is 50 ms owed, one tick, and 16.7 ms still carried. The other
    // 9.75 s of the stall is gone.
    EXPECT_EQ(clock.steps_at(start + std::chrono::seconds{10} + kPeriod), 1U);
}

TEST(TimeAccumulator, ExactlyTheCapIsNotClipped) {
    const Clock::time_point start{};
    TimeAccumulator clock{kPeriod, kCap, start};
    EXPECT_EQ(clock.steps_at(start + kCap), 7U);
}

} // namespace
