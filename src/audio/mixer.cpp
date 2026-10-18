#include "protekt/audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace protekt::audio {

namespace {

float clamp_gain(float gain) noexcept {
    // `!(gain > 0)` is also true for NaN, which becomes silence rather than poisoning the buffer.
    return !(gain > 0.0F) ? 0.0F : std::min(gain, 1.0F);
}

float clamp_pan(float pan) noexcept { return !(pan > -1.0F) ? -1.0F : std::min(pan, 1.0F); }

} // namespace

std::size_t frames_to_play(std::span<const Voice> voices, int sample_rate) noexcept {
    std::size_t longest = 0;
    for (const Voice& voice : voices) {
        longest = std::max(longest, duration_frames(voice.kind, sample_rate));
    }
    return longest;
}

std::vector<float> mix(std::span<const Voice> voices, std::size_t frames,
                       int sample_rate) noexcept {
    std::vector<float> out(frames * 2, 0.0F);
    // Each kind is made once per call however many voices ask for it.
    std::vector<std::vector<float>> cache(kSoundKindCount);
    for (const Voice& voice : voices) {
        const auto slot = static_cast<std::size_t>(voice.kind);
        if (slot >= cache.size()) {
            continue;
        }
        if (cache[slot].empty()) {
            cache[slot] = synthesise(voice.kind, sample_rate);
        }
        // A quarter turn swept by the pan: 0 turns is hard left (all cosine), 0.25 hard right.
        const float swept = (clamp_pan(voice.pan) + 1.0F) * 0.125F;
        const float gain = clamp_gain(voice.gain);
        const float left = gain * sine_turns(0.25F - swept);
        const float right = gain * sine_turns(swept);
        const std::vector<float>& samples = cache[slot];
        const std::size_t n = std::min(samples.size(), frames);
        for (std::size_t i = 0; i < n; ++i) {
            out[2 * i] += samples[i] * left;
            out[(2 * i) + 1] += samples[i] * right;
        }
    }
    float peak = 0.0F;
    for (const float s : out) {
        peak = std::max(peak, std::fabs(s));
    }
    if (peak > 1.0F) {
        for (float& s : out) {
            s /= peak; // division, not multiplication by 1/peak: the peak sample lands on exactly 1
        }
    }
    return out;
}

} // namespace protekt::audio
