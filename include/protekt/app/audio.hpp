#pragma once

#include <memory>
#include <span>
#include <string>

namespace protekt::app {

/// An audio output that can always fail and never blocks the game loop.
///
/// There is no mixing in here and no callback: the game thread hands over finished stereo samples
/// and the device drains them. A machine with no sound hardware, no SDL, or a driver that falls
/// over yields no AudioOut at all, and the game runs silent.
class AudioOut {
public:
    /// Opens the default output. nullptr means silence, and `error` says why.
    [[nodiscard]] static std::unique_ptr<AudioOut> open(std::string& error);

    /// False once the device has failed; after that `play` does nothing.
    [[nodiscard]] bool ok() const noexcept;

    /// Queues interleaved stereo float samples. Never blocks. If the device already has about
    /// `kMaxQueuedSeconds` waiting the buffer is DROPPED: a late sound is worse than a missing one,
    /// and an unbounded queue would turn one slow frame into seconds of stale gunfire.
    void play(std::span<const float> interleaved_stereo, int sample_rate) noexcept;

    /// Seconds of audio waiting in the device. 0 when it has failed.
    [[nodiscard]] float queued_seconds() const noexcept;

    /// How much the device may be holding before `play` starts dropping.
    static constexpr float kMaxQueuedSeconds = 0.75F;

    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    AudioOut(AudioOut&&) = delete;
    AudioOut& operator=(AudioOut&&) = delete;
    ~AudioOut();

private:
    struct Impl;
    explicit AudioOut(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace protekt::app
