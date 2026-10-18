// The device. The gate box has none, and that is the case that matters: opening must fail with a
// reason rather than crash or hang, and a device that does open must never block the caller.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "protekt/app/audio.hpp"
#include "protekt/audio/synth.hpp"

namespace {

using protekt::app::AudioOut;

TEST(AudioOut, OpenEitherGivesADeviceOrAReasonForSilence) {
    // The dummy driver never touches hardware, so this runs the same on a machine with a sound
    // card as on one without; where SDL has no dummy driver, or the build has no SDL, it is the
    // failure path that is exercised.
    ::setenv("SDL_AUDIODRIVER", "dummy", 1);
    std::string error;
    const std::unique_ptr<AudioOut> out = AudioOut::open(error);
    if (out == nullptr) {
        EXPECT_FALSE(error.empty()) << "silence must say why";
        return;
    }
    EXPECT_TRUE(out->ok());
}

TEST(AudioOut, PlayingMoreThanTheQueueHoldsNeverBlocks) {
    ::setenv("SDL_AUDIODRIVER", "dummy", 1);
    std::string error;
    const std::unique_ptr<AudioOut> out = AudioOut::open(error);
    if (out == nullptr) {
        GTEST_SKIP() << "no audio device here: " << error;
    }
    // A minute of audio offered at once. The device drains in real time, so a blocking or
    // unbounded play() would take a minute or a gigabyte; a capped one drops and returns.
    const std::vector<float> second(static_cast<std::size_t>(protekt::audio::kSampleRate) * 2,
                                    0.1F);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
        out->play(second, protekt::audio::kSampleRate);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    // The bound that matters: what is waiting is the cap plus at most the one buffer that crossed
    // it, not the minute that was offered.
    EXPECT_LE(out->queued_seconds(), AudioOut::kMaxQueuedSeconds + 2.0F + 0.1F);
    EXPECT_GT(out->queued_seconds(), 0.0F) << "the first buffers must have been accepted";
    EXPECT_TRUE(out->ok());
}

TEST(AudioOut, AnEmptyBufferAndAWrongRateAreIgnored) {
    ::setenv("SDL_AUDIODRIVER", "dummy", 1);
    std::string error;
    const std::unique_ptr<AudioOut> out = AudioOut::open(error);
    if (out == nullptr) {
        GTEST_SKIP() << "no audio device here: " << error;
    }
    out->play({}, protekt::audio::kSampleRate);
    const std::vector<float> some(200, 0.1F);
    out->play(some, 11025);
    EXPECT_TRUE(out->ok());
}

} // namespace
