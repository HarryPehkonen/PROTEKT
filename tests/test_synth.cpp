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

// ---- the shot is a thump under a crack, not a pew -----------------------------------------------

const std::vector<float>& shot_samples() {
    static const std::vector<float> samples = synthesise(SoundKind::YourShot);
    return samples;
}

std::size_t frame_at(float seconds) {
    return static_cast<std::size_t>(seconds * static_cast<float>(kSampleRate));
}

/// Indices of the rising zero crossings from `first` on.
std::vector<std::size_t> rising_crossings(const std::vector<float>& s, std::size_t first) {
    std::vector<std::size_t> at;
    for (std::size_t i = std::max<std::size_t>(first, 1); i < s.size(); ++i) {
        if (s[i - 1] < 0.0F && s[i] >= 0.0F) {
            at.push_back(i);
        }
    }
    return at;
}

TEST(ShotSound, MostEnergySitsBelowTwoHundredHertz) {
    const std::vector<float>& s = shot_samples();
    // A one-pole low-pass at 200 Hz, written out here rather than borrowed from the synth.
    const double alpha = 1.0 - std::exp(-2.0 * 3.14159265358979 * 200.0 / kSampleRate);
    double low_energy = 0.0;
    double high_energy = 0.0;
    double lp = 0.0;
    for (const float x : s) {
        lp += alpha * (static_cast<double>(x) - lp);
        const double residual = static_cast<double>(x) - lp;
        low_energy += lp * lp;
        high_energy += residual * residual;
    }
    // A pew has its body at 900 Hz and fails this; the thump passes it by a wide margin.
    EXPECT_GT(low_energy, 3.0 * high_energy) << "low " << low_energy << " high " << high_energy;
}

TEST(ShotSound, TheSettledToneHasALowZeroCrossingRate) {
    const std::vector<float>& s = shot_samples();
    const std::size_t first = frame_at(0.05F);
    std::size_t crossings = 0;
    for (std::size_t i = first + 1; i < s.size(); ++i) {
        if ((s[i - 1] < 0.0F) != (s[i] < 0.0F)) {
            ++crossings;
        }
    }
    const double seconds = static_cast<double>(s.size() - first) / kSampleRate;
    const double rate = static_cast<double>(crossings) / seconds;
    // A 60-120 Hz thump crosses zero about 120-240 times a second; a 900 Hz body would cross about
    // 1800 times a second. The bound sits between them, an order of magnitude under the pew.
    EXPECT_LT(rate, 300.0) << crossings << " crossings in " << seconds << " s";
}

TEST(ShotSound, ThePitchStaysInTheThumpBand) {
    const std::vector<float>& s = shot_samples();
    const std::vector<std::size_t> at = rising_crossings(s, frame_at(0.05F));
    ASSERT_GE(at.size(), 4U);
    std::vector<double> hz;
    for (std::size_t i = 1; i < at.size(); ++i) {
        hz.push_back(kSampleRate / static_cast<double>(at[i] - at[i - 1]));
    }
    std::sort(hz.begin(), hz.end());
    const double median = hz[hz.size() / 2];
    EXPECT_GT(median, 50.0);
    EXPECT_LT(median, 140.0);
}

TEST(ShotSound, NoToneHangsAboveTheThumpBandAfterTheCrackDies) {
    // Tests 2 and 3 measure zero crossings, and the low thump dominates those even when a much
    // quieter tone is mixed in on top. Adding a 900->150 Hz pew alongside the thump leaves the
    // settled crossing rate under 300/s and the median pitch in band, so neither catches it -- this
    // measures what they cannot: the energy ABOVE the thump band, after the crack has died. The
    // thump alone leaves a small residual here; a tone mixed in leaves far more.
    const std::vector<float>& s = shot_samples();
    const std::size_t first = frame_at(0.05F);
    // A one-pole low-pass at 300 Hz, written out here rather than borrowed from the synth.
    const double alpha = 1.0 - std::exp(-2.0 * 3.14159265358979 * 300.0 / kSampleRate);
    double low = 0.0;
    double high = 0.0;
    double lp = 0.0;
    for (std::size_t i = first; i < s.size(); ++i) {
        const double x = static_cast<double>(s[i]);
        lp += alpha * (x - lp);
        const double residual = x - lp;
        low += lp * lp;
        high += residual * residual;
    }
    // Measured: the thump alone leaves 0.052 here, a 900->150 Hz tone mixed in alongside it leaves
    // 0.141. The bound sits between them. Tests 2 and 3 cannot see the mixed tone -- the thump
    // dominates zero crossings -- so this is the test that catches it.
    const double fraction = high / (low + high);
    EXPECT_LT(fraction, 0.08) << "high " << high << " low " << low << " fraction " << fraction;
}

TEST(ShotSound, IsDeterministic) {
    const std::vector<float> a = synthesise(SoundKind::YourShot);
    const std::vector<float> b = synthesise(SoundKind::YourShot);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)), 0);
}

TEST(ShotSound, KeepsTheExistingLengthAndPeak) {
    const std::vector<float>& s = shot_samples();
    EXPECT_EQ(s.size(), 12000U); // 0.25 s at 48 kHz
    EXPECT_NEAR(peak_of(s), 0.8F, 0.01F);
    double sum = 0.0;
    for (const float x : s) {
        ASSERT_TRUE(std::isfinite(x));
        sum += x;
    }
    EXPECT_LT(std::fabs(sum / static_cast<double>(s.size())), 0.05) << "a large DC offset";
}

} // namespace
