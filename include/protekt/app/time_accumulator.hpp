#pragma once

#include <chrono>
#include <cstdint>

namespace protekt::app {

/// Wall time in, whole sim ticks out: how PLAY mode advances the sim at a fixed rate whatever the
/// renderer manages. The caller reads the clock; this only does the arithmetic, so it can be tested
/// with made-up time points.
///
/// A stall (a dragged window, a suspended laptop) must not turn into a catch-up burst that
/// teleports the tank, so the time owed from ONE call is capped at `max_catch_up`. The cap applies
/// per call; what is left over after the whole ticks are taken is carried to the next.
class TimeAccumulator {
public:
    using Clock = std::chrono::steady_clock;

    TimeAccumulator(std::chrono::nanoseconds period, std::chrono::nanoseconds max_catch_up,
                    Clock::time_point start)
        : period_(period), max_catch_up_(max_catch_up), last_(start) {}

    /// The whole ticks that have come due as of `now`.
    std::uint32_t steps_at(Clock::time_point now) {
        auto elapsed = now - last_;
        last_ = now;
        if (elapsed > max_catch_up_) {
            elapsed = max_catch_up_;
        }
        backlog_ += elapsed;
        std::uint32_t steps = 0;
        while (backlog_ >= period_) {
            backlog_ -= period_;
            ++steps;
        }
        return steps;
    }

private:
    std::chrono::nanoseconds period_;
    std::chrono::nanoseconds max_catch_up_;
    Clock::time_point last_;
    std::chrono::nanoseconds backlog_{0};
};

} // namespace protekt::app
