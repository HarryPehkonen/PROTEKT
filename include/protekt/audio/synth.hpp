#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/// Sound made out of arithmetic: no assets, no files, nothing to license, and nothing that can go
/// missing from a checkout. Every sample is a function of the kind and the sample rate and of
/// nothing else -- no clock, no `std::rand`, no libm -- so a buffer is the same bytes on every
/// machine, and a test can hold it to that.
///
/// This layer knows nothing about the game. It does not include the sim, and the sim does not
/// include it: the simulation never learns that sound exists.
namespace protekt::audio {

inline constexpr int kSampleRate = 48000; ///< frames per second of every buffer by default

/// What a sound is, not where it came from: the rule in `game/sounds.hpp` decides which of these a
/// battle event is worth.
enum class SoundKind : std::uint8_t {
    YourShot = 0,  ///< a short noise crack over a low thump that sags from 120 Hz to 60 Hz
    HitTaken = 1,  ///< a low thud with a sharp attack: the "you are being hit" alarm
    Explosion = 2, ///< decaying, filtered noise
};
inline constexpr int kSoundKindCount = 3;

/// One channel of samples, K frames. Deterministic: same kind, same bytes, everywhere. The first
/// and last frame are exactly 0.0F (a sample that starts or ends off zero clicks), and the peak is
/// normalised into [0.2, 1.0] so no kind is lost under another. Empty for a non-positive rate.
[[nodiscard]] std::vector<float> synthesise(SoundKind kind, int sample_rate = kSampleRate) noexcept;

/// How many frames `synthesise(kind, sample_rate)` returns, without making them.
[[nodiscard]] std::size_t duration_frames(SoundKind kind, int sample_rate = kSampleRate) noexcept;

/// sin(2 pi * turns), by a polynomial built from +, - and * only. libm's sine may differ in the
/// last bit between C libraries, and the guarantee here is bytes, not "close". Accurate to about
/// 0.1% of full scale, which is far below what an ear can tell on a gunshot.
[[nodiscard]] float sine_turns(float turns) noexcept;

} // namespace protekt::audio
