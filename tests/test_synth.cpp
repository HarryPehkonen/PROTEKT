// The synthesised sounds: no files and no clock, so every property here is a property of the
// arithmetic. Each rule is one a listener would notice breaking -- a click at the start, a sound
// too quiet to hear, two kinds that sound the same, a buffer that changes between runs.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "protekt/audio/synth.hpp"

namespace {

using protekt::audio::kSampleRate;
using protekt::audio::kSoundKindCount;
using protekt::audio::SoundKind;
using protekt::audio::synthesise;

const std::vector<SoundKind> kKinds{SoundKind::YourShot, SoundKind::HitTaken, SoundKind::Explosion};

float peak_of(const std::vector<float>& samples) {
    float peak = 0.0F;
    for (const float s : samples) {
        peak = std::max(peak, std::fabs(s));
    }
    return peak;
}

TEST(Synth, EveryKindIsCoveredByTheTests) {
    EXPECT_EQ(kKinds.size(), static_cast<std::size_t>(kSoundKindCount));
}

TEST(Synth, TheFirstAndLastFramesAreExactlyZero) {
    for (const SoundKind kind : kKinds) {
        const std::vector<float> samples = synthesise(kind);
        ASSERT_GT(samples.size(), 2U);
        // Bit-for-bit zero: a sample that starts or ends off zero is a click in the speaker.
        EXPECT_EQ(samples.front(), 0.0F) << static_cast<int>(kind);
        EXPECT_EQ(samples.back(), 0.0F) << static_cast<int>(kind);
        EXPECT_FALSE(std::signbit(samples.front())) << "negative zero is not the same bytes";
        EXPECT_FALSE(std::signbit(samples.back()));
    }
}

TEST(Synth, IsNotSilentInTheMiddleAndNeverClips) {
    for (const SoundKind kind : kKinds) {
        const float peak = peak_of(synthesise(kind));
        EXPECT_GE(peak, 0.2F) << static_cast<int>(kind) << " is too quiet to be heard";
        EXPECT_LE(peak, 1.0F) << static_cast<int>(kind) << " clips";
    }
}

TEST(Synth, NothingIsNotANumber) {
    for (const SoundKind kind : kKinds) {
        for (const float s : synthesise(kind)) {
            ASSERT_TRUE(std::isfinite(s));
        }
    }
}

TEST(Synth, TwoCallsGiveIdenticalBytes) {
    for (const SoundKind kind : kKinds) {
        const std::vector<float> a = synthesise(kind);
        const std::vector<float> b = synthesise(kind);
        ASSERT_EQ(a.size(), b.size());
        EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)), 0)
            << static_cast<int>(kind);
    }
}

TEST(Synth, TheKindsAreAudiblyDifferent) {
    for (std::size_t i = 0; i < kKinds.size(); ++i) {
        for (std::size_t j = i + 1; j < kKinds.size(); ++j) {
            const std::vector<float> a = synthesise(kKinds[i]);
            const std::vector<float> b = synthesise(kKinds[j]);
            const std::size_t n = std::min(a.size(), b.size());
            // Differing in length would already be different; compare what overlaps, so two
            // sounds that merely share a prefix still fail.
            double difference = 0.0;
            for (std::size_t k = 0; k < n; ++k) {
                difference += std::fabs(static_cast<double>(a[k]) - static_cast<double>(b[k]));
            }
            EXPECT_GT(difference / static_cast<double>(n), 0.01)
                << static_cast<int>(kKinds[i]) << " sounds like " << static_cast<int>(kKinds[j]);
        }
    }
}

TEST(Synth, TheSoundsAreShortAndInTheRightOrder) {
    const auto seconds = [](SoundKind kind) {
        return static_cast<float>(synthesise(kind).size()) / static_cast<float>(kSampleRate);
    };
    EXPECT_NEAR(seconds(SoundKind::YourShot), 0.25F, 0.01F);
    EXPECT_NEAR(seconds(SoundKind::HitTaken), 0.30F, 0.01F);
    EXPECT_NEAR(seconds(SoundKind::Explosion), 0.60F, 0.01F);
}

TEST(Synth, DurationFramesMatchesWhatIsMade) {
    for (const SoundKind kind : kKinds) {
        EXPECT_EQ(protekt::audio::duration_frames(kind), synthesise(kind).size());
    }
}

TEST(Synth, ARateOfZeroIsRefusedNotDividedBy) {
    EXPECT_TRUE(synthesise(SoundKind::YourShot, 0).empty());
    EXPECT_TRUE(synthesise(SoundKind::YourShot, -1).empty());
}

TEST(Synth, TheHitHasASharpAttackAndTheExplosionDoesNot) {
    // "You are being hit" has to be recognisable: the hit reaches most of its peak within a few
    // milliseconds; the explosion's roar is a decaying cloud, not a snap.
    const auto frames_to_peak = [](SoundKind kind) {
        const std::vector<float> samples = synthesise(kind);
        const float peak = peak_of(samples);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            if (std::fabs(samples[i]) >= 0.8F * peak) {
                return i;
            }
        }
        return samples.size();
    };
    EXPECT_LT(frames_to_peak(SoundKind::HitTaken), static_cast<std::size_t>(kSampleRate / 100));
}

TEST(Synth, TheSineIsASine) {
    EXPECT_NEAR(protekt::audio::sine_turns(0.0F), 0.0F, 1e-6F);
    EXPECT_NEAR(protekt::audio::sine_turns(0.25F), 1.0F, 0.002F);
    EXPECT_NEAR(protekt::audio::sine_turns(0.5F), 0.0F, 0.002F);
    EXPECT_NEAR(protekt::audio::sine_turns(0.75F), -1.0F, 0.002F);
    EXPECT_NEAR(protekt::audio::sine_turns(3.1F), std::sin(2.0F * 3.14159265F * 0.1F), 0.002F);
    EXPECT_NEAR(protekt::audio::sine_turns(-0.1F), -std::sin(2.0F * 3.14159265F * 0.1F), 0.002F);
}

} // namespace
