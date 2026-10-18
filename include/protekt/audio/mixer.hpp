#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "protekt/audio/synth.hpp"

namespace protekt::audio {

/// One sound to play, where in the stereo field, and how loud.
struct Voice {
    SoundKind kind{};
    float gain{1.0F}; ///< 0..1
    float pan{0.0F};  ///< -1 hard left, 0 centre, +1 hard right
};

/// Mixes voices into interleaved stereo: `frames * 2` samples, L,R,L,R... Every voice starts at
/// frame 0 of the buffer; a voice longer than `frames` is cut off, a shorter one is followed by
/// silence. No voices gives an all-zero buffer of the right length.
///
/// PANNING IS EQUAL-POWER: the left and right gains are cos and sin of a quarter turn swept by the
/// pan, so L^2 + R^2 is constant. A linear pan dips 3 dB in the middle, which makes a sound seem
/// to get quieter as it crosses in front of you -- the one position a threat should not sound weak.
/// A centred voice therefore sits at 0.707 on each side, not 1.
///
/// The output NEVER leaves [-1, 1]. If the sum would, the WHOLE buffer is divided by its peak
/// rather than each sample being clipped: clipping flattens the loudest moments (a shot and an
/// explosion landing together) into a buzz of added harmonics, where a single scale keeps every
/// waveform's shape and only turns it down. It is deterministic, and it costs the quiet parts of
/// that buffer some level, which is the right sacrifice when something loud has just happened.
[[nodiscard]] std::vector<float> mix(std::span<const Voice> voices, std::size_t frames,
                                     int sample_rate = kSampleRate) noexcept;

/// The frames needed to hear every voice to its end: the longest of them, 0 for none.
[[nodiscard]] std::size_t frames_to_play(std::span<const Voice> voices,
                                         int sample_rate = kSampleRate) noexcept;

} // namespace protekt::audio
