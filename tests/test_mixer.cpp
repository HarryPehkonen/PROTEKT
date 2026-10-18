// The mixer: voices in, interleaved stereo out. The rules are the ones a listener hears --
// silence is silence, left is on the left, two guns are louder than one, and nothing ever clips.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "protekt/audio/mixer.hpp"

namespace {

using protekt::audio::kSampleRate;
using protekt::audio::mix;
using protekt::audio::SoundKind;
using protekt::audio::Voice;

constexpr std::size_t kFrames = 24000;

float channel_energy(const std::vector<float>& stereo, std::size_t channel) {
    float sum = 0.0F;
    for (std::size_t i = channel; i < stereo.size(); i += 2) {
        sum += stereo[i] * stereo[i];
    }
    return sum;
}

float peak_of(const std::vector<float>& samples) {
    float peak = 0.0F;
    for (const float s : samples) {
        peak = std::max(peak, std::fabs(s));
    }
    return peak;
}

TEST(Mixer, NoVoicesIsSilenceOfTheRightLength) {
    const std::vector<float> out = mix({}, kFrames);
    ASSERT_EQ(out.size(), kFrames * 2);
    EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](float s) { return s == 0.0F; }));
}

TEST(Mixer, AnEmptySpanIsTheSameAsNone) {
    const std::vector<Voice> none;
    const std::vector<float> out = mix(none, 100);
    ASSERT_EQ(out.size(), 200U);
    EXPECT_EQ(peak_of(out), 0.0F);
}

TEST(Mixer, ZeroFramesIsAnEmptyBuffer) {
    const std::vector<Voice> one{Voice{SoundKind::YourShot, 1.0F, 0.0F}};
    EXPECT_TRUE(mix(one, 0).empty());
}

TEST(Mixer, OneCentredVoiceIsHeardAndBothChannelsAgree) {
    const std::vector<Voice> one{Voice{SoundKind::YourShot, 1.0F, 0.0F}};
    const std::vector<float> out = mix(one, kFrames);
    ASSERT_EQ(out.size(), kFrames * 2);
    EXPECT_GT(peak_of(out), 0.1F) << "a gain-1 voice must be audible";
    for (std::size_t i = 0; i < kFrames; ++i) {
        ASSERT_NEAR(out[2 * i], out[(2 * i) + 1], 1e-6F) << "frame " << i;
    }
}

TEST(Mixer, AHardLeftVoiceIsLouderOnTheLeft) {
    const std::vector<Voice> left{Voice{SoundKind::Explosion, 1.0F, -1.0F}};
    const std::vector<float> out = mix(left, kFrames);
    EXPECT_GT(channel_energy(out, 0), 100.0F * channel_energy(out, 1));
    EXPECT_GT(channel_energy(out, 0), 0.0F);
}

TEST(Mixer, AHardRightVoiceIsLouderOnTheRight) {
    const std::vector<Voice> right{Voice{SoundKind::Explosion, 1.0F, 1.0F}};
    const std::vector<float> out = mix(right, kFrames);
    EXPECT_GT(channel_energy(out, 1), 100.0F * channel_energy(out, 0));
}

TEST(Mixer, PanningIsEqualPowerSoACrossingSoundDoesNotDip) {
    // Left^2 + right^2 is the same at every pan, so a sound crossing in front of you holds its
    // loudness. A linear pan would lose half the power in the middle.
    const auto power = [](float pan) {
        const std::vector<Voice> v{Voice{SoundKind::HitTaken, 1.0F, pan}};
        const std::vector<float> out = mix(v, kFrames);
        return channel_energy(out, 0) + channel_energy(out, 1);
    };
    const float centre = power(0.0F);
    EXPECT_NEAR(power(-1.0F) / centre, 1.0F, 0.02F);
    EXPECT_NEAR(power(0.5F) / centre, 1.0F, 0.02F);
    EXPECT_NEAR(power(1.0F) / centre, 1.0F, 0.02F);
}

TEST(Mixer, GainScalesTheVoice) {
    const std::vector<Voice> loud{Voice{SoundKind::YourShot, 0.8F, 0.0F}};
    const std::vector<Voice> quiet{Voice{SoundKind::YourShot, 0.2F, 0.0F}};
    EXPECT_NEAR(peak_of(mix(loud, kFrames)) / peak_of(mix(quiet, kFrames)), 4.0F, 0.05F);
    const std::vector<Voice> mute{Voice{SoundKind::YourShot, 0.0F, 0.0F}};
    EXPECT_EQ(peak_of(mix(mute, kFrames)), 0.0F);
}

TEST(Mixer, TwoCopiesOfAVoiceAreLouderThanOne) {
    // Gain 0.4 each, so the sum is well below 1 and the limiter has nothing to do: what is
    // measured is the summing itself.
    const std::vector<Voice> one{Voice{SoundKind::YourShot, 0.4F, 0.0F}};
    const std::vector<Voice> two{Voice{SoundKind::YourShot, 0.4F, 0.0F},
                                 Voice{SoundKind::YourShot, 0.4F, 0.0F}};
    const std::vector<float> a = mix(one, kFrames);
    const std::vector<float> b = mix(two, kFrames);
    EXPECT_GT(peak_of(b), peak_of(a) * 1.9F);
    EXPECT_GT(channel_energy(b, 0), channel_energy(a, 0) * 3.5F);
}

TEST(Mixer, TheOutputNeverLeavesMinusOneToOne) {
    std::vector<Voice> pile;
    pile.reserve(12);
    for (int i = 0; i < 12; ++i) {
        pile.push_back(Voice{static_cast<SoundKind>(i % 3), 1.0F, (i % 2 == 0) ? -1.0F : 1.0F});
    }
    const std::vector<float> out = mix(pile, 30000);
    EXPECT_LE(peak_of(out), 1.0F);
    EXPECT_GT(peak_of(out), 0.9F) << "the limiter scales down to the edge, not to nothing";
    EXPECT_TRUE(std::none_of(out.begin(), out.end(), [](float s) { return std::isnan(s); }));
}

TEST(Mixer, ALimitedBufferKeepsTheShapeOfTheWaveform) {
    // One scale for the whole buffer, not a clip per sample: the ratio between two samples
    // survives the limiter.
    const std::vector<Voice> one{Voice{SoundKind::YourShot, 1.0F, 0.0F}};
    std::vector<Voice> many(8, Voice{SoundKind::YourShot, 1.0F, 0.0F});
    const std::vector<float> a = mix(one, kFrames);
    const std::vector<float> b = mix(many, kFrames);
    std::size_t big = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i]) > std::fabs(a[big])) {
            big = i;
        }
    }
    std::size_t other = (big + 2000) % a.size();
    other -= other % 2; // the same channel
    big -= big % 2;
    ASSERT_GT(std::fabs(a[other]), 1e-3F);
    EXPECT_NEAR(b[other] / b[big], a[other] / a[big], 0.01F);
}

TEST(Mixer, BadGainAndPanAreContainedNotPropagated) {
    const std::vector<Voice> odd{Voice{SoundKind::YourShot, 7.0F, 9.0F},
                                 Voice{SoundKind::HitTaken, std::nanf(""), std::nanf("")},
                                 Voice{SoundKind::Explosion, -3.0F, -9.0F}};
    const std::vector<float> out = mix(odd, kFrames);
    for (const float s : out) {
        ASSERT_TRUE(std::isfinite(s));
        ASSERT_LE(std::fabs(s), 1.0F);
    }
}

TEST(Mixer, IsDeterministic) {
    const std::vector<Voice> v{Voice{SoundKind::YourShot, 0.7F, -0.3F},
                               Voice{SoundKind::Explosion, 0.9F, 0.6F}};
    EXPECT_EQ(mix(v, kFrames), mix(v, kFrames));
}

TEST(Mixer, ASoundIsCutAtTheEndOfTheBufferAndSilenceFollowsAShortOne) {
    const std::size_t shot_frames = protekt::audio::duration_frames(SoundKind::YourShot);
    const std::vector<Voice> one{Voice{SoundKind::YourShot, 1.0F, 0.0F}};
    const std::vector<float> longer = mix(one, shot_frames + 1000);
    EXPECT_EQ(peak_of(std::vector<float>(longer.end() - 2000, longer.end())), 0.0F);
    const std::vector<float> shorter = mix(one, 100);
    EXPECT_EQ(shorter.size(), 200U);
}

TEST(Mixer, FramesToPlayIsTheLongestVoice) {
    const std::vector<Voice> v{Voice{SoundKind::YourShot, 1.0F, 0.0F},
                               Voice{SoundKind::Explosion, 1.0F, 0.0F}};
    EXPECT_EQ(protekt::audio::frames_to_play(v),
              protekt::audio::duration_frames(SoundKind::Explosion));
    EXPECT_EQ(protekt::audio::frames_to_play({}), 0U);
    EXPECT_EQ(kSampleRate, 48000);
}

} // namespace
