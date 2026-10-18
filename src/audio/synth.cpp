#include "protekt/audio/synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace protekt::audio {

namespace {

constexpr float kAttackSeconds = 0.001F; ///< long enough to start at 0, short enough to be a snap
constexpr float kReleaseSeconds
    = 0.005F; ///< the tail is folded to 0 over this, so the end is silent

struct Spec {
    float seconds;
    float target_peak; ///< what the buffer is scaled to; loud enough to hear, short of clipping
    std::uint32_t seed;
};

Spec spec_of(SoundKind kind) noexcept {
    switch (kind) {
    case SoundKind::YourShot:
        return Spec{0.25F, 0.8F, 0x2545F491U};
    case SoundKind::HitTaken:
        return Spec{0.30F, 0.95F, 0x9E3779B9U};
    case SoundKind::Explosion:
        return Spec{0.60F, 0.9F, 0x6C078965U};
    }
    return Spec{0.25F, 0.8F, 1U};
}

/// A fixed integer xorshift32. Seeded per kind (and never zero, which it would stay at forever), so
/// the noise of a shot is the same noise every time: a crack you can learn to recognise.
class Noise {
public:
    explicit Noise(std::uint32_t seed) noexcept : state_(seed == 0 ? 1U : seed) {}
    /// White noise in [-1, 1).
    [[nodiscard]] float next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return static_cast<float>(static_cast<std::int32_t>(state_)) / 2147483648.0F;
    }

private:
    std::uint32_t state_;
};

/// The per-sample factor of an exponential decay with time constant `tau`. Applying it by
/// multiplication, sample after sample, is an exponential envelope with no exp() in it.
float decay_factor(float tau_seconds, int rate) noexcept {
    return 1.0F - (1.0F / (tau_seconds * static_cast<float>(rate)));
}

/// Advances a phase in turns by `hz`, wrapped into [0, 1).
float advance(float phase, float hz, int rate) noexcept {
    phase += hz / static_cast<float>(rate);
    return phase >= 1.0F ? phase - 1.0F : phase;
}

std::size_t frames_of(float seconds, int rate) noexcept {
    return static_cast<std::size_t>(seconds * static_cast<float>(rate));
}

/// Shapes the ends, then scales the whole buffer to the kind's peak. The start is shaped by the
/// generators' own attack ramp; this folds the tail to zero, normalises, and pins both end frames
/// to an exact +0.0F (a multiplication by zero can leave -0.0F, which compares equal but is not the
/// same bytes).
void finish(std::vector<float>& samples, float target_peak, int rate) noexcept {
    const std::size_t n = samples.size();
    const std::size_t release = std::max<std::size_t>(frames_of(kReleaseSeconds, rate), 1);
    for (std::size_t i = 0; i < release && i < n; ++i) {
        samples[n - 1 - i] *= static_cast<float>(i) / static_cast<float>(release);
    }
    float peak = 0.0F;
    for (const float s : samples) {
        peak = std::max(peak, std::fabs(s));
    }
    if (peak > 0.0F) {
        const float scale = target_peak / peak;
        for (float& s : samples) {
            s *= scale;
        }
    }
    samples.front() = 0.0F;
    samples.back() = 0.0F;
}

float attack(std::size_t i, int rate) noexcept {
    const float ramp = static_cast<float>(i) / (kAttackSeconds * static_cast<float>(rate));
    return std::min(ramp, 1.0F);
}

/// A crack (short white noise) over a tone that falls from 900 Hz towards 150 Hz: the noise is the
/// report, the falling tone is the "pew" that tells a shot from a click.
void shot(std::vector<float>& out, int rate, Noise& noise) noexcept {
    const float crack_decay = decay_factor(0.025F, rate);
    const float tone_decay = decay_factor(0.07F, rate);
    const float fall = decay_factor(0.09F, rate);
    float crack = 1.0F;
    float tone = 1.0F;
    float hz = 900.0F;
    float phase = 0.0F;
    for (std::size_t i = 0; i < out.size(); ++i) {
        phase = advance(phase, std::max(hz, 150.0F), rate);
        const float body = 0.6F * tone * sine_turns(phase);
        out[i] = attack(i, rate) * ((0.8F * crack * noise.next()) + body);
        crack *= crack_decay;
        tone *= tone_decay;
        hz *= fall;
    }
}

/// A thud: a low sine whose pitch sags, a click of noise on the attack, and a short high ping on
/// top. The sag and the click give it weight; the ping is what makes it an alarm rather than a bump
/// in the floor.
void hit(std::vector<float>& out, int rate, Noise& noise) noexcept {
    const float thud_decay = decay_factor(0.09F, rate);
    const float click_decay = decay_factor(0.006F, rate);
    const float ping_decay = decay_factor(0.05F, rate);
    const float sag = decay_factor(0.2F, rate);
    float thud = 1.0F;
    float click = 1.0F;
    float ping = 1.0F;
    float hz = 110.0F;
    float thud_phase = 0.0F;
    float ping_phase = 0.0F;
    for (std::size_t i = 0; i < out.size(); ++i) {
        thud_phase = advance(thud_phase, std::max(hz, 45.0F), rate);
        ping_phase = advance(ping_phase, 660.0F, rate);
        out[i] = attack(i, rate)
                 * ((1.0F * thud * sine_turns(thud_phase)) + (0.5F * click * noise.next())
                    + (0.3F * ping * sine_turns(ping_phase)));
        thud *= thud_decay;
        click *= click_decay;
        ping *= ping_decay;
        hz *= sag;
    }
}

/// White noise through a one-pole low-pass (the dull roar of something large), a 50 Hz rumble under
/// it, and a slow exponential decay. The low-pass is what separates it from a hiss.
void explosion(std::vector<float>& out, int rate, Noise& noise) noexcept {
    const float roar_decay = decay_factor(0.16F, rate);
    const float rumble_decay = decay_factor(0.25F, rate);
    float roar = 1.0F;
    float rumble = 1.0F;
    float filtered = 0.0F;
    float phase = 0.0F;
    for (std::size_t i = 0; i < out.size(); ++i) {
        filtered += 0.12F * (noise.next() - filtered);
        phase = advance(phase, 50.0F, rate);
        out[i] = attack(i, rate) * ((2.5F * roar * filtered) + (0.5F * rumble * sine_turns(phase)));
        roar *= roar_decay;
        rumble *= rumble_decay;
    }
}

} // namespace

float sine_turns(float turns) noexcept {
    // Fold into [-0.5, 0.5) turns, then the classic parabola with one correction pass. Only + - *
    // and a comparison, so every IEEE machine returns the same bits.
    float t = turns - static_cast<float>(static_cast<std::int32_t>(turns));
    if (t >= 0.5F) {
        t -= 1.0F;
    } else if (t < -0.5F) {
        t += 1.0F;
    }
    constexpr float kTwoPi = 6.28318530717958647692F;
    constexpr float kB = 1.27323954473516268615F;  // 4 / pi
    constexpr float kC = -0.40528473456935108578F; // -4 / pi^2
    const float a = t * kTwoPi;
    const float y = (kB * a) + (kC * a * (a < 0.0F ? -a : a));
    return (0.225F * ((y * (y < 0.0F ? -y : y)) - y)) + y;
}

std::size_t duration_frames(SoundKind kind, int sample_rate) noexcept {
    return sample_rate > 0 ? frames_of(spec_of(kind).seconds, sample_rate) : 0;
}

std::vector<float> synthesise(SoundKind kind, int sample_rate) noexcept {
    const std::size_t n = duration_frames(kind, sample_rate);
    if (n < 2) {
        return {};
    }
    const Spec spec = spec_of(kind);
    std::vector<float> samples(n, 0.0F);
    Noise noise(spec.seed);
    switch (kind) {
    case SoundKind::YourShot:
        shot(samples, sample_rate, noise);
        break;
    case SoundKind::HitTaken:
        hit(samples, sample_rate, noise);
        break;
    case SoundKind::Explosion:
        explosion(samples, sample_rate, noise);
        break;
    }
    finish(samples, spec.target_peak, sample_rate);
    return samples;
}

} // namespace protekt::audio
